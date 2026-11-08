/**
 * @file daemon/manager.cpp
 * @brief 常驻 root Manager 的实现
 * @details Manager 一生只做四件特权事：建 configfs gadget、mount FunctionFS、
 *          往 ep0 写描述符、bind 到 UDC。做完就把三个端点 fd 与一份生效配置
 *          快照交给降权子进程，自己不再碰任何数据。每只手柄一站：gadget 实例名与
 *          FunctionFS 挂载点都按 MAC 派生，所以这些特权动作按槽位重复 N 次，
 *          各站互不共享状态。
 *          启动时它还负责读两份 YAML 配置、从 logind 的活跃图形会话选出 worker
 *          的目标身份。
 * @details 不负责：HID/音频数据面（worker 进程内的 bt/bridge/audio/usb）、
 *          交接机制本身（handoff 模块）、配置解析与身份策略（config /
 *          identity 模块）、设备发现（discovery 模块）。
 * @note 层次：daemon。线程模型：主循环单线程（loguru 的日志线程除外）。
 * @warning fork 之后、execv 之前的子进程路径不得分配内存或打日志——父进程已有
 *          其他线程跑过 malloc，锁状态不可信；见 handoff.h 的无分配重载。
 */

#include "daemon/manager.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/debug.h"
#include "core/device/descriptors.h"
#include "daemon/config.h"
#include "daemon/discovery.h"
#include "daemon/handoff.h"
#include "daemon/identity.h"
#include "daemon/options.h"
#include "daemon/privdrop.h"
#include "daemon/shutdown.h"
#include "loguru.hpp"
#include "usb/usb.h"

namespace ds5::daemon {
namespace {

/// @brief 一个槽位失败后到下一次尝试之间的等待毫秒数。经验值：够 udev/内核把
///        上一站的残骸清干净，又不至于让"改错了配置重跑"变成几分钟的等待。
constexpr int kRestartBackoffMs = 2000;

/// @brief 可中断等待与拆站回收循环共用的睡眠切片毫秒数。
/// @details 100 ms 是响应性与唤醒次数的折中：信号最多晚这么一点被看到，回收
///          循环也不会以整拍（默认 3 s）为粒度才发现 worker 已经退出。
constexpr int kSleepSliceMs = 100;

/**
 * @brief 可中断的退避等待：Ctrl-C 到来时 Manager 可能正睡在这里。
 * @param ms 总等待毫秒数；<=0 时立即返回。
 * @note 切成 @ref kSleepSliceMs 一段是为了响应性：usleep 整段睡下去会让信号
 *       最多晚 ms 毫秒才被看到。
 * @see ShutdownRequested。
 */
void BackoffSleep(int ms) {
  for (int slept = 0; slept < ms && !ShutdownRequested();
       slept += kSleepSliceMs) {
    usleep(kSleepSliceMs * 1000);
  }
}

/**
 * @brief 读回当前可执行文件的绝对路径，供 worker 的 execv 使用。
 * @return 路径字符串；readlink 失败时返回空串（调用方据此放弃启动）。
 * @details 用 /proc/self/exe 而不是 argv[0]：argv[0] 可以是任意字符串，而
 *          worker 必须重新 execv 同一个镜像。
 * @note 4096 是 PATH_MAX 量级的缓冲；超过这个长度的路径会被判为失败。
 */
std::string SelfExePath() {
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  buf[n] = '\0';
  return buf;
}

/**
 * @brief 关掉 Manager 手里那份端点 fd 副本并把结构复位为 -1；可重复调用。
 * @param endpoints 本次 Provision 成功时填好的 fd 三元组。
 * @note fork 之后父进程必须立刻调用它：从那一刻起数据流只归 worker。逐个
 *       close 而不是交给某个对象的析构，是因为 Provision 把 fd 所有权交给了
 *       调用方（见 usb.h）。
 */
void CloseEndpoints(usb::Endpoints& endpoints) {
  int* const fds[] = {&endpoints.ep0, &endpoints.ep_in, &endpoints.ep_out};
  for (int* fd : fds) {
    if (*fd >= 0) ::close(*fd);
    *fd = -1;
  }
}

/**
 * @brief 把 worker 的退出状态翻译成一行日志。
 * @param status waitpid 原样报告的 status。
 * @details execv 之前的失败带在 120+ExecStage 区间里，这类退出意味着"worker
 *          根本没跑起来"，要和它跑起来后的正常退出码区分开——前者的根因在
 *          Manager 这一侧（fd 交接、降权），后者的根因在数据流那一侧。
 * @note 无副作用之外的行为：只打日志。
 * @see ExecStageName, PrepareAndExec。
 */
void ReportChildExit(int status) {
  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    // 交接失败退出码的区间端点，与 handoff.h 的枚举一一对应。
    constexpr int kFirstStage =
        kExecExitBase + static_cast<int>(ExecStage::kBadPlan);
    constexpr int kLastStage =
        kExecExitBase + static_cast<int>(ExecStage::kExec);
    if (code >= kFirstStage && code <= kLastStage) {
      DS5DUMMY_LOG(ERROR, "[manager] worker died before exec: {} (exit {})",
            ExecStageName(code), code);
    } else {
      DS5DUMMY_LOG(INFO, "[manager] worker exited with code {}", code);
    }
  } else if (WIFSIGNALED(status)) {
    DS5DUMMY_LOG(INFO, "[manager] worker killed by signal {}", WTERMSIG(status));
  } else {
    DS5DUMMY_LOG(INFO, "[manager] worker status={}", status);
  }
}

/**
 * @brief 把插件配置写成一行面向横幅的文本。
 * @param settings 本站生效的扁平插件配置。
 * @return 形如 "idle.off_after_minutes=30, other.key=1" 的单行文本；空配置返回
 *         "(none)"。
 * @details 只列用户在配置里写了的键，不列插件自己的默认值：Manager 不认识任何
 *          键名，也不知道哪个插件用了什么。生效值的另一半在插件自己的启动日志
 *          里（worker 侧的 [idle] 那行）。Settings 有序，遍历顺序即横幅顺序。
 */
std::string PluginSettingsText(const plugin::Settings& settings) {
  std::string text;
  for (const auto& [key, value] : settings) {
    if (!text.empty()) text += ", ";
    text += key + "=" + value;
  }
  return text.empty() ? "(none)" : text;
}

/**
 * @brief 把一组手柄级设置打进启动横幅。
 * @param label 行的标签："defaults"（global 层）或具体 MAC。
 * @param settings 生效设置。
 * @note worker 的日志里不会回显这些字段，"为什么声音偏小/手柄反应慢"的第一
 *       个怀疑对象就是这里，所以横幅要逐项列全。
 */
void PrintControllerSettings(const std::string& label,
                             const ControllerConfig& settings) {
  // 0 不是间隔而是"只在新样本到达时发"：横幅写 real-time，避免出现一个会被
  // 读成 0 ms 的数字。
  char interval_text[16];
  if (settings.usb_report_interval_ms == 0) {
    std::snprintf(interval_text, sizeof interval_text, "real-time");
  } else {
    std::snprintf(interval_text, sizeof interval_text, "%d ms",
                  settings.usb_report_interval_ms);
  }
  printf("  %s: audio %s, haptics %.2f, speaker %d, USB %s, model %s, "
         "bt_wait %d s\n",
         label.c_str(), settings.audio ? "on" : "off",
         static_cast<double>(settings.haptics_gain), settings.speaker_gain,
         interval_text, UsbModelName(settings.usb_model), settings.bt_wait_s);
  // 插件的键单列一行：它是用户写的，Manager 只负责原样搬下去，横幅把这站最终
  // 会交给 worker 的那些键摊开，配置写没写中一眼就能看出来。
  printf("  %s plugins: %s\n", label.c_str(),
         PluginSettingsText(settings.plugins).c_str());
}

/**
 * @brief 多槽监督者：每只手柄一站虚拟 USB + 一个 worker，各自独立重试。
 * @details 角色：唯一持 root 的循环。协作者：usb（每站的 configfs/FunctionFS）、
 *          discovery（找出手柄 MAC）、handoff+privdrop（子进程的准备）、
 *          options（传给 worker 的配置）。
 * @note 生命周期：构造只做参数保存，特权动作全部推迟到 Run()；单个 worker
 *       退出时立刻拆掉它那一站，Run() 返回时所有站都已拆过。线程模型：单线程
 *       主循环，数据面线程都在 worker 进程里。
 * @note "发现"只用来决定何时起 worker：节点消失不会拆掉仍在跑的 worker——
 *       L2CAP 后端根本不用 hidraw 节点，手柄断链由 worker 自己发现并退出。
 * @see RunManager。
 */
class ManagerSupervisor {
 public:
  /**
   * @brief 保存配置骨架、用户配置快照与降权目标，不做任何特权动作。
   * @param base_options worker 配置骨架：各站只在此基础上覆盖 MAC 与五个
   *                     手柄级字段，进程级的日志级别等由这里继承。
   * @param user 用户配置快照；controllers 里已是"内置默认 ← global ← 专属"
   *             叠加后的最终值。按值接管，本对象不依赖调用方的存储期。
   * @param ffs_base FunctionFS 挂载根（来自系统配置，调用方已过白名单校验）；
   *                 每站再按 MAC 派生自己的挂载点。
   * @param self_path 可执行文件绝对路径，execv 用。
   * @param drop_uid 降权目标 uid，须非 0。
   * @param drop_gid 降权目标 gid，须非 0。
   * @param tick_ms 监督循环的节拍毫秒数，即发现扫描与 worker 回收的间隔；
   *                由系统配置的 discovery_interval_s 换算，须 >= 1000（配置层
   *                按秒收口，这里不做二次校验）。
   */
  ManagerSupervisor(Options base_options, UserConfig user,
                    std::string ffs_base, std::string self_path,
                    uid_t drop_uid, gid_t drop_gid, int tick_ms)
      : base_options_(std::move(base_options)),
        user_(std::move(user)),
        ffs_base_(std::move(ffs_base)),
        self_path_(std::move(self_path)),
        drop_uid_(drop_uid),
        drop_gid_(drop_gid),
        tick_ms_(tick_ms) {}

  /**
   * @brief 跑监督循环，直到收到退出信号。
   * @return 恒为 0：启动条件的失败由 RunManager 提前拦下，循环内的失败一律
   *         退避重试，不向上冒。
   * @details 每轮先补建该服务的站、再回收已退出的 worker，然后睡到下一个节拍
   *          （tick_ms_，来自系统配置的 discovery_interval_s）。单手柄模式不扫描
   *          sysfs（保留"先起站、worker 等手柄连上"的老语义）；auto/list 模式以
   *          发现结果为起站依据。
   * @warning 无限循环：worker 反复崩溃时本函数不返回，只有信号能结束它。
   */
  int Run() {
    while (!ShutdownRequested()) {
      if (user_.bt_mode == BtTargetMode::kSingle) {
        EnsureSingleSlot();
      } else {
        DiscoverAndSpawn();
      }
      ReapWorkers();
      BackoffSleep(tick_ms_);
    }

    // Ctrl-C：每个 worker 只发一次 SIGTERM，它自己会走 Stop 流程；随后的
    // 回收循环负责拆掉每一站，直到所有 worker 都退出。
    TerminateWorkers();
    while (!slots_.empty()) {
      ReapWorkers();
      // 走到这里时退出标志已经置起，BackoffSleep 会立即返回，回收循环就成了
      // 对 waitpid 的忙等。拆站必须走完，没有"提前醒"的语义，所以固定切片睡。
      if (!slots_.empty()) usleep(kSleepSliceMs * 1000);
    }
    DS5DUMMY_LOG(INFO, "[manager] Shutting down");
    return 0;
  }

 private:
  /// @brief 一个正在服务的站：一只手柄的 gadget 坐标与它的 worker。
  struct Slot {
    usb::GadgetId gadget;  ///< configfs 实例名 + FunctionFS 挂载点。
    pid_t pid = -1;        ///< worker 的进程号。
  };

  /**
   * @brief 取一只手柄的生效设置：有专属条目用条目，否则用 global 层。
   * @param mac 规范大写的 BD_ADDR。
   * @return 生效设置的副本。
   */
  ControllerConfig EffectiveSettings(const std::string& mac) const {
    const auto it = user_.controllers.find(mac);
    return it != user_.controllers.end() ? it->second : user_.defaults;
  }

  /**
   * @brief 该 MAC 现在是否允许尝试建站（退避期是否已过）。
   * @param mac 目标手柄。
   * @return 没有退避记录或已到期 true。
   */
  bool RetryDue(const std::string& mac) const {
    const auto it = retry_after_.find(mac);
    return it == retry_after_.end() ||
           std::chrono::steady_clock::now() >= it->second;
  }

  /**
   * @brief 给一个 MAC 记上下一次最早尝试时刻。
   * @param mac 目标手柄。
   * @note 建站失败与 worker 退出都走这里，避免失败时以整个监督节拍为间隔反复
   *       fork/provision。
   */
  void ScheduleRetry(const std::string& mac) {
    retry_after_[mac] =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kRestartBackoffMs);
  }

  /**
   * @brief 单手柄模式：没有在跑的站就按配置里的固定 MAC 建一站。
   * @details 起站依据仍是配置而不是扫描结果（老语义是"站先起来，worker 用
   *          bt_wait_s 等手柄连上"），但型号默认从 HID 身份读出来，所以这里为
   *          那只 MAC 做一次窄查询：查得到就按它的型号建站，查不到（手柄还没
   *          连上）先按普通 DualSense 建站——这一站的呈现方式此后不再改。
   *          配置里写了 usb_model（非 auto）时，查询结果与回落都不影响呈现。
   */
  void EnsureSingleSlot() {
    if (!slots_.empty()) return;
    const std::string& mac = user_.bt_addr;
    if (!RetryDue(mac)) return;
    StartSlot(mac, ModelForMac(mac));
  }

  /**
   * @brief 在发现结果里查一只手柄的型号。
   * @param mac 规范大写 BD_ADDR。
   * @return 该 MAC 的型号；它不在 sysfs 里时按普通 DualSense 处理。
   * @note 这只是"发现结果"：配置里的 usb_model 不是 auto 时，StartSlot 会用
   *       它覆盖这里给出的型号。
   */
  static core::device::ControllerModel ModelForMac(const std::string& mac) {
    for (const DiscoveredController& found : ScanControllers(kHidrawSysfsBase)) {
      if (found.mac == mac) return found.model;
    }
    return core::device::ControllerModel::kDualSense;
  }

  /**
   * @brief auto/list 模式：扫描 sysfs，为每只"该服务且没在服务"的手柄建站。
   * @details list 模式把 controllers 的键集当白名单；auto 模式全收。已经
   *          在跑的 MAC 不重复建站，退避期内的也不碰。
   */
  void DiscoverAndSpawn() {
    for (const DiscoveredController& found :
         ScanControllers(kHidrawSysfsBase)) {
      if (user_.bt_mode == BtTargetMode::kList &&
          user_.controllers.count(found.mac) == 0) {
        continue;
      }
      if (slots_.count(found.mac) != 0) continue;
      if (!RetryDue(found.mac)) continue;
      DS5DUMMY_LOG(INFO, "[manager] discovered DualSense {} at {}",
                   found.mac.c_str(), found.node.c_str());
      StartSlot(found.mac, found.model);
    }
  }

  /**
   * @brief 为一只手柄建站并把 worker 拉起来。
   * @param mac 规范大写的 BD_ADDR，同时决定 gadget 实例名、挂载点与 worker 的
   *            目标地址。
   * @param detected 发现阶段从真手柄 HID VID/PID 读出的型号；配置里的
   *                 usb_model 不是 auto 时会被它覆盖。
   * @details 三步依序：Provision（失败自带回滚）→ fork+exec → 父进程关掉自己
   *          的 fd 副本。任何一步失败都记退避并留给下一轮重试；失败的站点自己
   *          清干净，不影响别的站。
   */
  void StartSlot(const std::string& mac,
                 core::device::ControllerModel detected) {
    const usb::GadgetId gadget = usb::ResolveGadgetId(mac, ffs_base_);
    if (gadget.instance.empty()) {
      DS5DUMMY_LOG(ERROR, "[manager] '{}' cannot name a gadget", mac.c_str());
      ScheduleRetry(mac);
      return;
    }

    ControllerConfig settings = EffectiveSettings(mac);
    // usb_model 的 auto 就是"用发现结果"；写了型号则本站一律按它呈现，
    // 单 MAC 模式下手柄还没连上（发现不到型号）时也照样生效。在这里把 auto
    // 定下来，快照里因此只有具体型号，worker 不再重复判断。
    const core::device::ControllerModel model =
        settings.usb_model.value_or(detected);
    settings.usb_model = model;
    // 配置层已把间隔收口到 0/1/2/4 毫秒。0 是 real-time：描述符仍用最快的
    // 1 ms，但 worker 只在收到新蓝牙样本时才写；其余档位的数值本身就是
    // bInterval 的字节值（全速一帧 1 ms），不需要换算。这个字节是站点自己的
    // （两只手柄可以各用不同的节拍）。
    // bInterval 只是给主机看的声明——dummy_hcd 不兑现它，IN 线程按同一个
    // 数值自己排发送节拍（见 usb::StartThreads）。
    const auto interval_ms = static_cast<uint8_t>(
        settings.usb_report_interval_ms == 0 ? 1
                                             : settings.usb_report_interval_ms);

    usb::Station station;
    if (!usb::Provision(station, gadget, core::device::ProfileFor(model),
                        interval_ms)) {
      DS5DUMMY_LOG(WARNING, "[manager] Build failed for {}, retrying in {} ms",
                   mac.c_str(), kRestartBackoffMs);
      ScheduleRetry(mac);
      return;
    }

    // 本站的运行期事实也盖在设置里：交给 worker 的那一份因此是完整快照，
    // 不需要在交接前再逐字段补。
    settings.bt_addr = mac;
    settings.host_sysfs_path = station.host_sysfs_path;

    pid_t pid = -1;
    if (!StartWorker(station, settings, gadget.instance, pid)) {
      // worker 没起来：关掉父侧 fd 并把刚建好的站拆掉，坐标只属于这一站。
      CloseEndpoints(station.endpoints);
      usb::Teardown(gadget.ffs_path, gadget.instance);
      ScheduleRetry(mac);
      return;
    }

    retry_after_.erase(mac);
    slots_.emplace(mac, Slot{gadget, pid});
  }

  /**
   * @brief fork 出 worker 并把配置快照与 fd 交给它，随后父进程放掉自己的副本。
   * @param station 本站的 Provision 产出；其中三个端点 fd 成功时被就地关闭并
   *                复位。
   * @param settings 本站完整的生效设置：用户可写字段已叠加，bt_addr、
   *                host_sysfs_path 与 usb_model 也已由 StartSlot 填齐。
   * @param instance gadget 实例名，只用于日志。
   * @param out_pid 成功时写入子进程号；失败时不动。
   * @return 子进程已起来 true（不代表它成功 execv）；任何准备失败 false。
   * @details 两个硬约束：配置快照与它的 memfd 必须在 fork 之前写好（子进程不得
   *          分配内存，也不能再编码配置），argv 只含程序名与 `--worker`。
   *          worker 按固定号取用四条交接通道（3/4/5 端点、6 快照），不必知道
   *          Manager 手里原本是几号。设置整份拷贝，Options 与 ControllerConfig
   *          共用同一份字段定义，这里不需要逐项改名或取反。
   * @note 失败路径只负责关自己的 fd；拆站由调用方统一做。
   * @see PrepareAndExec, MakeConfigFd。
   */
  bool StartWorker(usb::Station& station, const ControllerConfig& settings,
                   const std::string& instance, pid_t& out_pid) {
    Options worker_options = base_options_;
    worker_options.controller = settings;

    // 生效配置编成一份自描述快照，经 memfd 交给 worker；命令行不再承载配置。
    std::string why;
    const std::vector<uint8_t> config_blob = worker_options.Encode();
    const int config_fd = MakeConfigFd(config_blob, why);
    if (config_fd < 0) {
      DS5DUMMY_LOG(ERROR, "[manager] Cannot hand config to worker for {}: {}",
                   settings.bt_addr.c_str(), why.c_str());
      return false;
    }

    // argv 的 char* 数组也必须在 fork 之前建好；只剩程序名与内部角色开关。
    const char* const argv[] = {self_path_.c_str(), "--worker", nullptr};

    const HandoffPlan plan{station.endpoints.ep0, station.endpoints.ep_in,
                           station.endpoints.ep_out, config_fd};
    if (!PlanOk(plan, why)) {
      DS5DUMMY_LOG(ERROR, "[manager] Cannot hand off fds for {}: {}",
                   settings.bt_addr.c_str(), why.c_str());
      ::close(config_fd);
      return false;
    }

    const pid_t child = ::fork();
    if (child < 0) {
      DS5DUMMY_LOG(ERROR, "[manager] fork: {}", strerror(errno));
      ::close(config_fd);
      return false;
    }
    if (child == 0) {
      // 子进程从这里开始不许分配内存、不许打日志：只走 syscall 和字面量。
      PrepareAndExec(plan, drop_uid_, drop_gid_, argv, self_path_.c_str());
    }

    out_pid = child;
    DS5DUMMY_LOG(INFO, "[manager] worker pid={} serves {} ({})",
                 static_cast<long>(child), settings.bt_addr.c_str(),
                 instance.c_str());
    // 父进程立刻放掉自己那份副本：从这一刻起 FunctionFS 数据流只在
    // uid=drop_uid 的 worker 进程里跑，配置快照也只剩 worker 那一份。
    ::close(config_fd);
    CloseEndpoints(station.endpoints);
    return true;
  }

  /**
   * @brief 回收所有已退出的 worker，并就地拆掉它们那一站。
   * @details waitpid(-1, WNOHANG) 一次收割一个，直到没有已退出的子进程。
   *          站点的拆除只用自己的坐标，别的站仍在跑。
   */
  void ReapWorkers() {
    for (;;) {
      int status = 0;
      const pid_t pid = ::waitpid(-1, &status, WNOHANG);
      if (pid == 0) return;  // 有子进程但都还在跑
      if (pid < 0) {
        if (errno == EINTR) continue;
        // ECHILD = 没有子进程（全部收完），是正常的终止条件。
        if (errno != ECHILD) {
          DS5DUMMY_LOG(ERROR, "[manager] waitpid: {}", strerror(errno));
        }
        return;
      }
      OnWorkerExit(pid, status);
    }
  }

  /**
   * @brief 一个 worker 退出后：报退出原因、拆站、记退避。
   * @param pid 退出的子进程号。
   * @param status waitpid 报告的原始状态。
   * @note 拆站后该 MAC 从 slots_ 里移除；若手柄还在 sysfs 里，下一轮扫描会
   *       在退避到期后重新建站。
   */
  void OnWorkerExit(pid_t pid, int status) {
    ReportChildExit(status);
    for (auto it = slots_.begin(); it != slots_.end(); ++it) {
      if (it->second.pid != pid) continue;
      usb::Teardown(it->second.gadget.ffs_path, it->second.gadget.instance);
      ScheduleRetry(it->first);
      slots_.erase(it);
      return;
    }
    // 每个 slot 的 pid 都由本进程 fork 而来；走到这里说明状态不一致。
    DS5DUMMY_LOG(WARNING, "[manager] reaped unknown pid {}",
                 static_cast<long>(pid));
  }

  /// @brief 向所有在跑的 worker 各发一次 SIGTERM；只用于收摊。
  void TerminateWorkers() {
    for (const auto& entry : slots_) {
      const Slot& slot = entry.second;
      if (slot.pid <= 0) continue;
      // ESRCH 说明它已经自己退了，不是错误；其余失败原因要留痕。
      if (::kill(slot.pid, SIGTERM) != 0 && errno != ESRCH) {
        DS5DUMMY_LOG(WARNING, "[manager] SIGTERM to worker {}: {}",
                     entry.first.c_str(), strerror(errno));
      }
    }
  }

  Options base_options_;  ///< worker 配置骨架；每站只覆盖 MAC 与手柄级字段。
  UserConfig user_;       ///< 用户配置快照，controllers 已叠加完各层。
  std::string ffs_base_;  ///< FunctionFS 挂载根；每站按 MAC 派生后缀。
  std::string self_path_;
  uid_t drop_uid_;
  gid_t drop_gid_;
  /// 监督循环的节拍（毫秒）：发现扫描与 worker 回收共用，来自系统配置。
  int tick_ms_;
  std::map<std::string, Slot> slots_;  ///< MAC → 正在服务的站。
  /// MAC → 最早可重试时刻；worker 退出或建站失败时写入，成功后清除。
  std::map<std::string, std::chrono::steady_clock::time_point> retry_after_;
};

}  // namespace

/**
 * @brief 读配置、选身份、校验启动条件，然后进入监督循环。
 * @param options 命令行解析出的内部参数。Manager 只把它当作 worker 配置的
 *                骨架：手柄级字段全部被配置文件里的生效值覆盖。
 * @return 0 正常收摊，或当前用户没有启用本服务；1 启动条件不成立（每条都有
 *         对应日志说明原因）。
 * @details 顺序是绑定的：root 校验 → 系统配置 → 会话/身份 → 家目录 → 用户配置
 *          → 生效日志级别 → gadget 前置校验。所有可能随登录状态变化的东西都
 *          在 provision 之前一次算清，循环里只重试建站与 worker。
 * @note 副作用：可能挂 Filesystem、写 configfs、fork 子进程。
 * @warning 四个特权动作之外不再碰数据面；循环里只在节拍休眠与 waitpid 之间
 *          往返。
 * @see ManagerSupervisor, RunWorker。
 */
int RunManager(const Options& options) {
  // 只有 root 能建 configfs、挂 FunctionFS；部署路径是 systemd 单元而不是 sudo。
  if (::geteuid() != 0) {
    DS5DUMMY_LOG(ERROR,
          "[manager] The Manager must run as root (it owns configfs and "
          "FunctionFS): start it through systemd (systemctl start ds5dummy)");
    return 1;
  }

  // 系统配置：文件缺失走内置默认（不是错误），存在但读不动/解析失败必须停下。
  SystemConfig sys;
  std::string why;
  const LoadResult sys_result = LoadSystemConfig(kSystemConfigPath, sys, why);
  if (sys_result == LoadResult::kError) {
    DS5DUMMY_LOG(ERROR, "[manager] {}", why.c_str());
    return 1;
  }
  if (sys_result == LoadResult::kAbsent) {
    DS5DUMMY_LOG(INFO, "[manager] {} is absent; using built-in defaults",
          kSystemConfigPath);
  }

  // 目标身份只来自 logind 的活跃图形会话；两种失败语义对应两种退出码。
  const IdentityDecision decision =
      SelectWorkerIdentity(sys, QueryGraphicalSessions());
  for (const std::string& warning : decision.warnings) {
    DS5DUMMY_LOG(WARNING, "[manager] {}", warning.c_str());
  }
  if (decision.outcome == IdentityOutcome::kRetry) {
    // 还没登录或候选不唯一：非零退出，交给 systemd 每 2 s 重拉。
    DS5DUMMY_LOG(ERROR, "[manager] {}", decision.why.c_str());
    return 1;
  }
  if (decision.outcome == IdentityOutcome::kDisabled) {
    // 登录用户不在白名单内：以 0 退出，服务停在 inactive 而不是反复重启。
    DS5DUMMY_LOG(WARNING, "[manager] {}", decision.why.c_str());
    return 0;
  }
  const WorkerIdentity& identity = decision.identity;

  // 用户配置在目标用户家目录里，家目录只能来自 passwd。
  std::string home;
  if (!LookupHome(identity.uid, home, why)) {
    DS5DUMMY_LOG(ERROR, "[manager] {}", why.c_str());
    return 1;
  }
  const std::string user_config =
      home + "/" + kUserConfigDir + "/" + kUserConfigFileName;

  // 文件存在但读不动/解析失败才是失败；显式 enabled: false 才等于"该用户
  // 没启用"。
  UserConfig user;
  const LoadResult user_result =
      LoadUserConfig(identity.uid, home, user, why);
  if (user_result == LoadResult::kError) {
    DS5DUMMY_LOG(ERROR, "[manager] {}", why.c_str());
    return 1;
  }
  if (user_result == LoadResult::kAbsent) {
    // 家目录里没有配置不再是"这个用户不启用"：LoadUserConfig 承诺 kAbsent 时
    // 不动 out，所以这里默认构造的 user 就是内置默认层，取值与发布的用户模板
    // 逐字一致（enabled: true、bt_addr: auto、其余全默认）。
    DS5DUMMY_LOG(INFO,
          "[manager] No config for uid {} at {}; using built-in defaults",
          static_cast<unsigned>(identity.uid), user_config.c_str());
  }
  if (!user.enabled) {
    DS5DUMMY_LOG(INFO, "[manager] uid {} disabled the service in {}",
          static_cast<unsigned>(identity.uid), user_config.c_str());
    return 0;
  }

  // 用户级别覆盖系统级别；两者都是 ParseVerbosity 的数值语义。
  const int log_level = user.log_level.value_or(sys.log_level);
  loguru::g_stderr_verbosity = static_cast<loguru::Verbosity>(log_level);

  // gadget 前置条件：ffs 路径来自 /etc 也仍要过白名单；至少要有空闲 UDC。
  if (!usb::IsSafeGadgetPath(sys.ffs_path)) {
    DS5DUMMY_LOG(ERROR, "[manager] Rejected ffs_path from {}: {}",
          kSystemConfigPath, sys.ffs_path.c_str());
    return 1;
  }
  if (!usb::HasDummyUdc()) {
    DS5DUMMY_LOG(ERROR,
          "[manager] No dummy_udc.N in /sys/class/udc — load it first: "
          "sudo modprobe dummy_hcd is_high_speed=0 num_dummy=<controllers>");
    return 1;
  }

  // worker 的配置骨架：手柄级字段由监督者按 MAC 逐站填入，命令行里的旧
  // 取值不再下发；这里只保留进程级字段（日志级别）。
  Options worker_options = options;
  worker_options.verbosity = log_level;

  const std::string self_path = SelfExePath();
  if (self_path.empty()) {
    DS5DUMMY_LOG(ERROR, "[manager] Cannot read /proc/self/exe: {}", strerror(errno));
    return 1;
  }

  printf("=== DS5Dummy Manager — virtual USB DualSense ===\n");
  printf("FunctionFS mount base: %s\n", sys.ffs_path.c_str());
  printf("Discovery: scans %s every %d s\n", kHidrawSysfsBase,
         sys.discovery_interval_s);
  printf("Worker will run as uid=%u gid=%u\n",
         static_cast<unsigned>(identity.uid),
         static_cast<unsigned>(identity.gid));
  printf("System config: %s\n", kSystemConfigPath);
  // 缺文件时把这个事实写进路径那一行：横幅里的路径此时并不存在，读 journal 的
  // 人需要知道当前生效的是内置默认值，而不是去找一个还没写的文件里找原因。
  const std::string user_config_shown =
      user_result == LoadResult::kAbsent
          ? user_config + " (absent, built-in defaults in use)"
          : user_config;
  printf("User config: %s\n", user_config_shown.c_str());
  // 横幅要能被 journal 直接读懂：调 DEBUG/VERBOSE 的人正是从这一行确认级别，
  // 只印数字等于没说。名字表与 ParseVerbosity 同源；整数调到枚举之外时没有
  // 对应名字，退回数值，避免把 nullptr 交给 %s。
  if (const char* const level_name = internal::VerbosityName(log_level)) {
    printf("Log level: %s\n", level_name);
  } else {
    printf("Log level: %d\n", log_level);
  }
  switch (user.bt_mode) {
    case BtTargetMode::kSingle:
      printf("Handles: %s\n", user.bt_addr.c_str());
      break;
    case BtTargetMode::kList:
      printf("Handles: listed controllers only\n");
      break;
    case BtTargetMode::kAuto:
      printf("Handles: auto (every DualSense in the system)\n");
      break;
  }
  PrintControllerSettings("defaults", user.defaults);
  for (const auto& [mac, settings] : user.controllers) {
    PrintControllerSettings(mac, settings);
  }
  printf("Press Ctrl+C to exit.\n\n");
  fflush(stdout);

  // 节拍按秒配置、内部用毫秒：配置层已把 discovery_interval_s 收口到
  // INT_MAX/1000 以内，这里的乘法不会回绕。
  ManagerSupervisor supervisor(std::move(worker_options), std::move(user),
                               sys.ffs_path, self_path, identity.uid,
                               identity.gid, sys.discovery_interval_s * 1000);
  return supervisor.Run();
}

}  // namespace ds5::daemon
