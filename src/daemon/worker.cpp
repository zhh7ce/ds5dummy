/**
 * @file daemon/worker.cpp
 * @brief 降权后跑在自己身份上的数据流进程
 * @details 一个 worker 周期 = 认领 fd → 连 BT → 起三侧线程（bt 传输、
 *          audio 下行、usb 三条阻塞线）→ 主循环轮询退出条件并打统计 → 反向
 *          关停。桥接逻辑本身不在这个文件里，这里只管进程与线程生命周期。
 * @details 不负责：USB gadget 的建立（Manager）、协议翻译（bridge）、
 *          传输细节（bt/usb/audio 各自模块）。
 * @note 层次：daemon。多线程：本文件是各模块回调与统计的汇聚点，主循环只
 *       sleep 与读原子标志。
 */

#include "daemon/worker.h"

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include "audio/audio.h"
#include "bridge/bridge.h"
#include "bt/bt.h"
#include "core/debug.h"
#include "daemon/config.h"
#include "daemon/shutdown.h"
#include "loguru.hpp"
#include "usb/usb.h"

namespace ds5::daemon {
namespace {

/**
 * @brief 音频链失效后整条下行重新拉起的间隔（秒）。
 * @details 音频链内部先按指数退避重建（上限 5 s、连续 5 次失败才放弃），
 *          那之后再交到这里兜底。PipeWire 重启通常在秒级完成，30 s 一轮既
 *          能接上它，又不会在 PipeWire 长期缺席时把日志刷满。
 */
constexpr int kAudioRetrySeconds = 30;

/**
 * @brief 给定目录下的 PipeWire unix socket 是否可写。
 * @param dir 运行时目录路径；nullptr 与空串一律视为不可用。
 * @return 目录里存在 pipewire-0 且当前 uid 可写时 true。
 * @note 只做 access(W_OK)，不建立连接：目的是判断"这个目录值不值得用"。
 * @see RestoreXdgRuntimeDir。
 */
bool PipewireSocketReachable(const char* dir) {
  if (dir == nullptr || *dir == '\0') return false;
  const std::string socket = std::string(dir) + "/pipewire-0";
  return ::access(socket.c_str(), W_OK) == 0;
}

/**
 * @brief 按当前 uid 把 XDG_RUNTIME_DIR 重建到能连上 PipeWire 的值。
 * @details 降权后的进程不一定还拿得到桌面会话的环境：sudo 会剥离
 *          XDG_RUNTIME_DIR，而 `sudo -i` 留下的 /run/user/0 普通用户又写不了。
 *          libpipewire 用 `$XDG_RUNTIME_DIR/pipewire-0` 找 socket，变量缺失时
 *          connect 直接 EHOSTDOWN，所以在这里把它修好。
 * @note 副作用：可能 setenv 覆盖 XDG_RUNTIME_DIR。只在现值不可用时才动它。
 * @warning 不改也没关系：audio::Start 失败时 worker 继续以 HID-only 运行。
 * @see audio::Start。
 */
void RestoreXdgRuntimeDir() {
  if (PipewireSocketReachable(::getenv("XDG_RUNTIME_DIR"))) return;

  uid_t target = ::getuid();
  if (target == 0) {
    // 还没降权（直接跑 worker 的调试路径）：sudo 记着原始桌面用户，用它兜底。
    if (const char* sudo_uid = ::getenv("SUDO_UID"); sudo_uid && *sudo_uid) {
      target = static_cast<uid_t>(::atoi(sudo_uid));
    }
  }
  if (target == 0) {
    DS5DUMMY_LOG(WARNING, "[worker] no writable PipeWire socket and no desktop uid");
    return;
  }

  // /run/user/<uid> 是 systemd-logind 的既定布局，不是本程序创建的。
  const std::string dir = "/run/user/" + std::to_string(target);
  if (!PipewireSocketReachable(dir.c_str())) {
    DS5DUMMY_LOG(WARNING,
          "[worker] {}/pipewire-0 not writable: {} — audio will be skipped",
          dir, strerror(errno));
    return;
  }
  ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
  DS5DUMMY_LOG(INFO, "[worker] restored XDG_RUNTIME_DIR={} for uid {}", dir,
        static_cast<unsigned>(target));
}

#if DS5DUMMY_DEBUG
/**
 * @brief 5 秒统计窗口的上一次快照，用来把累计计数器差分成窗口值。
 * @details 计数器各自待在自己的模块里（usb 管 IN 写、bt 管链路与发送队列），
 *          这里只存"上次读数 + 上次时刻"，worker 是唯一把它们拼成一行的人。
 */
struct StatsWindow {
  uint64_t last_bt_rx = 0;   ///< 上次读到的 BT 输入报告累计数。
  uint64_t last_usb_tx = 0;  ///< 上次读到的 USB IN 写累计数。
  std::chrono::steady_clock::time_point last_time =
      std::chrono::steady_clock::now();  ///< 上次成窗时刻，单调钟。
};
#endif

/**
 * @brief 一个 worker 进程的生命周期持有者。
 * @details 角色：把 bt / bridge / audio / usb 四个模块按依赖顺序接起来，并在
 *          退出信号或链路断开时反向拆掉。协作者：Options（配置）、四个模块的
 *          全局入口、shutdown 的退出标志。
 * @note 生命周期：构造只存参数，Run() 才动资源；Run() 返回时线程已全部 join、
 *       fd 已 Close。线程模型：本对象的主循环线程 + 各模块起的线程，成员里
 *       跨线程的标志（stop_all_ / audio_failed_，以及诊断构建里的
 *       usb_rx_count_）都是 std::atomic，三个 *_started_ 只在主线程读写。
 * @see RunWorker。
 */
class WorkerSession {
 public:
  /**
   * @brief 记住配置与交接来的 fd 号，不申请任何资源。
   * @param options 配置快照的引用，必须比本对象活得久。
   * @param endpoints 本进程里的三个 fd 号（正常为 3/4/5），是否已挪到位
   *                  由 Adopt 校验。
   */
  WorkerSession(const Options& options, usb::Endpoints endpoints)
      : options_(options), endpoints_(endpoints) {}

  /**
   * @brief 主入口：认领 fd，跑一次桥接数据面，返回前收尾。
   * @return 0 正常退出；1 认领失败或缺桥接必需的 bt_addr。
   * @note 副作用：起线程、开 socket、连 PipeWire；全部在本函数内成对回收。
   */
  int Run() {
    if (!usb::Adopt(endpoints_)) {
      DS5DUMMY_LOG(ERROR,
            "[worker] Cannot adopt inherited FunctionFS fds — this process "
            "must be started by the root Manager "
            "(--worker --ep0/--ep-in/--ep-out)");
      return 1;
    }

    if (options_.bt_addr.empty()) {
      DS5DUMMY_LOG(ERROR,
            "[worker] 'bt_addr' is missing: there is nothing to bridge "
            "without a controller. Set it in ~/{}/{} and restart the service "
            "(use 'bluetoothctl devices' to find the controller's classic "
            "BD_ADDR).",
            kUserConfigDir, kUserConfigFileName);
      usb::Close();
      return 1;
    }

    // Start 可能在任意一步失败，而已起的那几步不会自己回滚：统一交给幂等的
    // Stop() 收尾，再返回错误码。
    if (!Start()) {
      Stop();
      return 1;
    }

    printf("Press Ctrl+C to exit.\n\n");
    // 主循环只做几件轻活：睡 100 ms、收音频失效通知、按需重拉音频，诊断构建
    // 下再每 5 秒打一行统计。真正的数据搬运都在各模块的线程里。
    // stop_all_ = 链路断/ep0 关（由别的线程置），ShutdownRequested = 用户按了
    // Ctrl-C：两条退出路径都在这里汇合。
    // 启动时音频就没起来（常见于 PipeWire 还没就绪）也排进同一条重试路径：
    // 音频缺席绝不能拖住 HID，但也不该让整个 worker 重启才能再试。
    std::optional<std::chrono::steady_clock::time_point> audio_retry_at;
    if (!options_.no_audio && !audio_started_) {
      audio_retry_at = std::chrono::steady_clock::now() +
                       std::chrono::seconds(kAudioRetrySeconds);
    }
    while (!stop_all_ && !ShutdownRequested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      // exchange 一次性取走通知：音频线程只会喊一次，重复置位不该重复停。
      // audio_started_ 保证只对"真的起过"的下行做 Stop。
      if (audio_failed_.exchange(false) && audio_started_) {
        DS5DUMMY_LOG(
            WARNING,
            "[worker] Audio lost after retries; retrying every {} s, HID stays up",
            kAudioRetrySeconds);
        audio::Stop();
        audio_started_ = false;
        audio_retry_at = std::chrono::steady_clock::now() +
                         std::chrono::seconds(kAudioRetrySeconds);
      }
      // 整条下行重试：audio 内部的退避已经放弃，这里每隔 kAudioRetrySeconds
      // 重新 Start 一次，直到这一轮退出。Stop 与 Start 都只在本线程上跑。
      if (audio_retry_at && std::chrono::steady_clock::now() >= *audio_retry_at) {
        if (StartAudio()) {
          DS5DUMMY_LOG(INFO, "[worker] Audio downlink recovered");
          audio_retry_at.reset();
        } else {
          audio_retry_at = std::chrono::steady_clock::now() +
                           std::chrono::seconds(kAudioRetrySeconds);
        }
      }
#if DS5DUMMY_DEBUG
      LogStatsWindow();
#endif
    }
    DS5DUMMY_LOG(INFO, "[worker] Stopping — signal={} bt_lost_or_ep0_closed={}",
          static_cast<long>(ShutdownRequested()), stop_all_.load());

    Stop();
    return 0;
  }

 private:
  /**
   * @brief 按数据流顺序把三个模块起起来，任一步失败即返回 false 交给 Stop。
   * @return 全链路就绪 true；BT 没连上或线程起不来 false。
   * @note 调用顺序有硬约束：bridge::Init 必须早于任何会回调进 bridge 的线程；
   *       消费方必须先于生产方就绪（bt 传输 → audio → usb）。
   * @see Stop。
   */
  bool Start() {
    DS5DUMMY_LOG(INFO,
          "[worker] Waiting up to {} s for the DualSense at {} (press the PS "
          "button to connect it now)",
          options_.bt_wait_s, options_.bt_addr.c_str());

    // 设备模型 + 0x36 影子的初始化，必须早于任何线程回调进 bridge。
    bridge::Init(options_.bt_addr,
                 static_cast<uint8_t>(options_.speaker_gain));
    // 上行麦克风的去处：bridge 在 bt 读线程上把 Opus 包交给 audio 的有界
    // 队列。它只在主机开始录音后被调用，所以 audio 没起来时这条路径根本
    // 不会触发（开关永远为 false）。
    bridge::SetMicSink(audio::OnMicPacket);

    if (!OpenBtWithWait()) {
      if (ShutdownRequested()) {
        DS5DUMMY_LOG(INFO,
              "[worker] Shutdown requested before the controller appeared");
      } else {
#if defined(BT_BACKEND_L2CAP)
        // 三类成因各对应一条可执行命令，按实测出现频率排列。
        DS5DUMMY_LOG(ERROR, "[worker] No L2CAP channel to {} within {} s.",
              options_.bt_addr.c_str(), options_.bt_wait_s);
        DS5DUMMY_LOG(ERROR, "[worker] Common causes:");
        DS5DUMMY_LOG(ERROR,
              "[worker]   - Controller has an active LE link: 'bluetoothctl "
              "disconnect' first");
        DS5DUMMY_LOG(ERROR,
              "[worker]   - hidp must stay loaded: this backend releases the "
              "BlueZ HID session and then claims the PSM");
        DS5DUMMY_LOG(ERROR,
              "[worker]   - Link key missing: re-pair with 'bluetoothctl' "
              "(agent on)");
#else
        DS5DUMMY_LOG(ERROR, "[worker] No hidraw node for {} within {} s.",
              options_.bt_addr.c_str(), options_.bt_wait_s);
        DS5DUMMY_LOG(ERROR, "[worker] Common causes:");
        DS5DUMMY_LOG(ERROR,
              "[worker]   - Controller is connected: 'bluetoothctl connect "
              "{}'", options_.bt_addr.c_str());
        DS5DUMMY_LOG(ERROR,
              "[worker]   - DKMS module is loaded: 'sudo modprobe "
              "hid_playstation_ds5dummy'");
        DS5DUMMY_LOG(ERROR,
              "[worker]   - Node visible with HID_ID bus 0005: check "
              "/sys/class/hidraw/*/device/uevent");
#endif
      }
      return false;
    }

    DS5DUMMY_LOG(INFO, "[worker] Connected — bridging BT ↔ USB");
    // 起线程的顺序照着数据流排，让每个生产方开始产出时它的消费方已经在跑：
    // bt 传输线程 → audio（产出经 bridge 进发送队列）→ usb 三条阻塞线程。
    bt::Handlers bt_handlers;
    // 以下回调跑在 bt 的两条传输线程上：on_input 固定在读线程，on_disconnected
    // 读写两侧都可能触发、甚至各一次。只做转发与置标志，不许阻塞、不许打高频日志。
    bt_handlers.on_input = [this](const uint8_t* data, size_t len) {
      bridge::OnBtInput(data, len);
    };
    bt_handlers.on_disconnected = [this] { stop_all_ = true; };  // 断链 = 退出请求
    if (!bt::StartThreads(bt_handlers)) {
      DS5DUMMY_LOG(ERROR, "[worker] Cannot start BT transport thread");
      return false;
    }
    bt_started_ = true;

    // 音频是可降级的一路：起不来只降级成 HID-only，不影响已经就绪的 HID 桥。
    // 先清通知位，避免上一轮残留让本次运行立刻误判成"音频已失效"。
    audio_failed_ = false;
    if (!options_.no_audio && !StartAudio()) {
      DS5DUMMY_LOG(WARNING,
            "[worker] Audio downlink unavailable, continuing HID-only");
    }

    // 以下四个回调跑在 usb 数据面的三条线程上（ep0 / IN / OUT），bridge 侧
    // 入口都是非阻塞的：要么直接回 STALL/ack，要么把报告投进 bt 的发送队列。
    usb::Handlers usb_handlers;
    usb_handlers.on_class_request =
        [](const usb_ctrlrequest& request, usb::Ep0Responder& ep0) {
          bridge::OnClassRequest(request, ep0);
        };
    usb_handlers.on_output_report = [this](const uint8_t* data, size_t len) {
      bridge::OnUsbOutput(data, len);
#if DS5DUMMY_DEBUG
      usb_rx_count_++;  // 只服务统计行，不参与任何判定
#endif
    };
    // 端点文件被关闭 = 主机侧解绑，fd 已不可用：请求整轮退出，由 Manager 重建。
    usb_handlers.on_ep0_closed = [this] { stop_all_ = true; };
    usb_handlers.on_output_closed = [this] { stop_all_ = true; };
    if (!usb::StartThreads(
            usb_handlers,
            std::chrono::milliseconds(options_.usb_report_interval_ms))) {
      DS5DUMMY_LOG(ERROR, "[worker] Cannot start USB data-plane threads");
      return false;
    }
    usb_started_ = true;
    return true;
  }

  /**
   * @brief 起音频链（下行 encoder+sink，上行 decoder+source）。
   * @return 成功 true；no_audio 关闭了音频或 audio::Start 任一步失败 false。
   * @details 成功与失败都在 audio 内部成对收尾（失败路径会回滚已建的部分），
   *          所以本函数可以反复调用；成功时置 audio_started_，由 Stop() 负责收。
   *          本站的主机侧 USB 接口目录（options_.host_sysfs_path）由 Manager 建站时
   *          解析并经交接参数传下来，worker 只转交，不去 sysfs 里自己找——
   *          它已经降权，也不认识 configfs 坐标。
   *          三个回调的线程约束：
   *          - output 跑在 libpipewire 数据线程上，只把已编码的一段交给 bridge；
   *          - on_lost 也在音频线程上，只置一次性标志——audio::Stop 不能在音频
   *            线程上自调，交给主循环；
   *          - on_mic_open 跑在 audio 的 PipeWire 主循环线程（拆除时是主线程）上，
   *            把手柄麦克风开关打成 0x32 报告，经 bt 的发送队列出去。
   * @note 只在主线程调用。
   * @see Stop, audio::Start。
   */
  bool StartAudio() {
    if (options_.no_audio) return false;
    if (!audio::Start(
            options_.bt_addr, options_.haptics_gain,
            [](const uint8_t* haptics, const uint8_t* speaker) {
              bridge::OnAudioOutput(haptics, speaker);
            },
            options_.host_sysfs_path, [this] { audio_failed_ = true; },
            [](bool open) { bridge::OnMicOpen(open); })) {
      return false;
    }
    audio_started_ = true;
    return true;
  }

  /**
   * @brief 拆机：顺序与 Start 严格相反，先停生产方再停消费方。
   * @note 幂等：三个 started_ 标志决定各自是否要停，未起过的模块不会被误停。
   * @note 副作用：join 线程、关 socket 与 fd。返回后本进程不再有活动资源。
   * @see Start。
   */
  void Stop() {
    if (audio_started_) {
      audio::Stop();
      audio_started_ = false;
    }
    if (usb_started_) {
      usb::StopThreads();
      usb_started_ = false;
    }
    if (bt_started_) {
      bt::StopThreads();
      bt_started_ = false;
    }
    bt::Close();
    usb::Close();
  }

  /**
   * @brief 在 bt_wait_s 秒内反复尝试打开到控制器的 L2CAP 通道。
   * @return 连上 true；超时、`bt_wait_s`（用户配置值经 argv 下发）为 0 或收到
   *         退出信号 false。
   * @note 阻塞调用，只在主循环线程上跑；每次失败后 Close 再 Open，确保不留
   *       半个 socket。
   * @see Start —— 唯一的调用方。
   */
  bool OpenBtWithWait() {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      if (ShutdownRequested()) return false;
      if (bt::Open(options_.bt_addr)) return true;
      bt::Close();
      // bt_wait_s == 0 表示"不等"：调试脚本要的是一次性判定。
      if (options_.bt_wait_s <= 0) return false;
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
      if (elapsed >= options_.bt_wait_s) return false;
      // 500 ms 一轮：等人按下 PS 键的间隔，经验值；再短也只是空转 connect。
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

#if DS5DUMMY_DEBUG
  /**
   * @brief 每 5 秒把两个模块的累计计数器差分成一行窗口统计。
   * @note 只在主循环线程调用；读的是各模块自己的原子计数器，不加锁。
   * @details 主循环每 100 ms 调一次（10 Hz），而窗口满 5 秒才成行，所以绝大多数
   *          调用直接返回。时间基准取单调钟，不受系统时间调整影响。
   * @see bt::StatsWindow, usb::InputWrites。
   */
  void LogStatsWindow() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed_s =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stats_.last_time)
                .count()) /
        1000.0;
    if (elapsed_s < 5.0) return;  // 统计窗口 5 s：一行日志够看清趋势，又不刷屏

    const bt::Stats bt_stats = bt::StatsWindow();
    const uint64_t usb_tx = usb::InputWrites();
    // 差分：模块给的是进程级累计，窗口要的是这一段的量。
    const uint64_t bt_rx_delta = bt_stats.rx_reports - stats_.last_bt_rx;
    const uint64_t usb_tx_delta = usb_tx - stats_.last_usb_tx;
    stats_.last_bt_rx = bt_stats.rx_reports;
    stats_.last_usb_tx = usb_tx;
    stats_.last_time = now;

    DS5DUMMY_LOG(INFO,
          "[worker] BT rx: {} ({:.0f}/s) | USB tx: {} ({:.0f}/s) | USB rx: {}",
          bt_rx_delta, static_cast<double>(bt_rx_delta) / elapsed_s,
          usb_tx_delta, static_cast<double>(usb_tx_delta) / elapsed_s,
          usb_rx_count_.load());
    DS5DUMMY_LOG(INFO,
          "[worker] BT tx 0x31: {} 0x36: {} | dropped: {} | truncated: {}",
          bt_stats.tx_31, bt_stats.tx_36, bt_stats.dropped, bt_stats.truncated);
    DS5DUMMY_LOG(INFO,
          "[worker] bt_write over {} items: avg {:.0f} us, max {} us | queue "
          "high water: {} of {}",
          bt_stats.write_count,
          bt_stats.write_count ? static_cast<double>(bt_stats.write_us_total) /
                                     static_cast<double>(bt_stats.write_count)
                               : 0.0,
          bt_stats.write_us_max, bt_stats.queue_high_water, bt::kQueueMax);
  }
#endif

  const Options& options_;
  usb::Endpoints endpoints_;
  std::atomic<bool> stop_all_{false};       ///< 链路断/ep0 关：主循环退出请求。
  std::atomic<bool> audio_failed_{false};   ///< 音频线程的一次性失效通知。
#if DS5DUMMY_DEBUG
  std::atomic<uint64_t> usb_rx_count_{0};   ///< 主机 OUT 报告累计（仅统计用）。
  StatsWindow stats_;                        ///< 上一窗口的读数与时刻。
#endif
  bool bt_started_ = false;     ///< bt 传输线程已起，Stop 时负责收。非原子：只在主线程改。
  bool audio_started_ = false;  ///< 音频下行已起，Stop 时负责收。同上只在主线程改。
  bool usb_started_ = false;    ///< usb 三条数据面线程已起，Stop 时负责收。同上。
};

}  // namespace

/**
 * @brief worker 进程主体：配置日志、修运行环境、跑一个 WorkerSession。
 * @param options worker 侧配置（ep*_fd 为本进程已就位的 fd 号）。
 * @return WorkerSession 的退出码。
 * @note 副作用：setenv XDG_RUNTIME_DIR、起线程、开 socket。
 * @see RunManager。
 */
int RunWorker(const Options& options) {
  DS5DUMMY_LOG(INFO, "[worker] uid={} euid={} gid={} egid={} pid={}",
        static_cast<long>(::getuid()), static_cast<long>(::geteuid()),
        static_cast<long>(::getgid()), static_cast<long>(::getegid()),
        static_cast<long>(::getpid()));

  // 身份已经落定，环境跟着身份走：先修 XDG_RUNTIME_DIR，后面任何模块连桌面
  // 服务都直接用现成的环境。
  RestoreXdgRuntimeDir();

  usb::Endpoints endpoints{options.ep0_fd, options.ep_in_fd, options.ep_out_fd};
  WorkerSession session(options, endpoints);
  return session.Run();
}

}  // namespace ds5::daemon
