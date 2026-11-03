/**
 * @file
 * @brief bt 的裸 L2CAP 后端：自己走 HIDP，只实现 §A 接缝，BlueZ 只管底层链路
 *
 * 两条 AF_BLUETOOTH/L2CAP 通道按顺序打开，对齐上游 DS5Dongle 固件的做法：
 * - PSM 0x11 — HIDP 控制通道：GET_REPORT / SET_PROTOCOL；
 * - PSM 0x13 — HIDP 中断通道：INPUT / OUTPUT 报告数据报。
 *
 * 分工切在 L2CAP 这层：BlueZ 仍然负责 ACL 与配对，本项目只把两个 PSM 抢
 * 过来自己组帧。所以 Open() 的顺序是"确保 ACL 在 → 让 BlueZ 放开 HID
 * profile → 立刻 connect"，中间不能留间隙，否则 BlueZ 的 input plugin 会
 * 把 PSM 重新 claim 走；Close() 再把 profile 还回去。
 *
 * HIDP framing 全部在本文件内消化：Read() 剥 0xA1，Write() 补 0xA2，控制
 * 通道的事务头（0x43/0xA3）只在这里出现。建链先关后开、feature 缓存与预取
 * 都在门面那一层（bt_link.cpp）；本文件的 Write() 一步都不等，开链末尾的
 * 0x32 唤醒报告也由本文件自己补发。
 */

#include "bt/bt_detail.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "core/debug.h"
#include "core/protocol/report.h"
#include "loguru.hpp"

// 模块内部（常量、状态、辅助）收在匿名 namespace；文件末尾的 bt::detail
// 命名空间才是 bt_detail.h 承诺给门面的契约实现。
namespace {

// ---------------------------------------------------------------------------
// 后端私有常量
// ---------------------------------------------------------------------------

/// HIDP 控制通道 PSM（规范固定值：Human Interface Device Control）。
constexpr uint16_t kPsmHidControl = 0x11;
/// HIDP 中断通道 PSM（规范固定值：Human Interface Device Interrupt）。
constexpr uint16_t kPsmHidInterrupt = 0x13;
/// MTU 协商目标（经验值，对齐 DualSense 实际在用的帧尺寸）。connect 后读回
/// 真实协商值：装不下 0x36 音频帧（1 字节 HIDP 头 + 398 字节报告体 = 399B）
/// 时只打 WARNING，不改行为。
constexpr uint16_t kWantMtu = 672;
/// 0x36 报告在中断通道上的完整线尺寸：HIDP_DATA 帧头 + 报告体。
constexpr uint16_t kHidpAudioFrameBytes = 1 + core::protocol::kBtAudioReportSize;

/// 控制通道一次请求等回复的总上限：手柄不回应时不能让建链/ep0 线程无限等。
constexpr int kControlRecvTimeoutMs = 2000;
/// BlueZ 的 HID profile UUID（HID Service），DisconnectProfile/ConnectProfile
/// 用它点名要放/还的是哪个 profile。
constexpr const char* kHidProfileUuid = "00001124-0000-1000-8000-00805F9B34FB";
/// 单个 PSM 的 connect 重试次数。
constexpr int kL2capConnectRetries = 3;
/// connect 重试之间的间隔：给控制器状态（page、profile 释放）留出落定时间。
constexpr int kL2capConnectRetryMs = 500;
/// 发送缓冲上限：故意压小到 8KB，不让内核把好几秒的音频堆在链路前面，
/// 使上层看到的延迟全是 backlog。
constexpr int kL2capSndbufBytes = 8192;

// ---------------------------------------------------------------------------
// 进程级状态（每 worker 进程一份）
// ---------------------------------------------------------------------------

int ctrl_fd = -1;             ///< HIDP 控制通道 fd，-1 表示没开
int intr_fd = -1;             ///< HIDP 中断通道 fd，-1 表示没开
std::string hidp_stolen_mac;  ///< Close() 时要归还 HID profile 的 MAC（放开成功即记，建链失败也照还）

// ---------------------------------------------------------------------------
// BlueZ D-Bus 辅助
// ---------------------------------------------------------------------------

/**
 * @brief fork 一个 gdbus call 子进程执行一次 BlueZ D-Bus 调用，回收退出码。
 *
 * 走外部 gdbus 而不是库调用，是为了不为此链再引一份 GDBus 依赖；每次一个
 * 短命子进程，调用超时靠 gdbus --timeout 保证。output 非空时才建管道接收
 * stdout；不需要输出时不建管道，也就没有读端要伺候。
 *
 * @param path   BlueZ 设备对象的 D-Bus path，见 BluezDevicePath()。
 * @param method 方法名，例如 "org.bluez.Device1.Connect"。
 * @param arg1   第一个参数，nullptr 表示不传。
 * @param arg2   第二个参数，nullptr 表示不传（传了 arg2 就必须同时传 arg1）。
 * @param output 非空时收集子进程的 stdout（供属性读取类调用检查返回值）。
 * @return 子进程退出码；pipe/fork/waitpid 任一环节失败或子进程被信号打死
 *         返回 -1。子进程里 execlp 失败以 127 退出（"命令不存在"约定值）。
 */
int RunGdbus(const std::string& path, const char* method, const char* arg1,
             const char* arg2, std::string* output) {
  int pipefd[2] = {-1, -1};
  if (output != nullptr && ::pipe(pipefd) != 0) {
    DS5DUMMY_LOG(WARNING, "[bt] pipe() for gdbus failed: {}", strerror(errno));
    return -1;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    DS5DUMMY_LOG(WARNING, "[bt] cannot fork gdbus: {}", strerror(errno));
    if (pipefd[0] >= 0) ::close(pipefd[0]);
    if (pipefd[1] >= 0) ::close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    // 子进程：stdout 接进管道，只留写端。
    if (output != nullptr) {
      ::dup2(pipefd[1], STDOUT_FILENO);
      ::close(pipefd[0]);
      ::close(pipefd[1]);
    }
    // --timeout 5：D-Bus 调用最多等 5 秒，BlueZ 卡住不能把建链无限拖死。
    if (arg1 != nullptr && arg2 != nullptr) {
      ::execlp("gdbus", "gdbus", "call", "--system", "--timeout", "5", "--dest",
               "org.bluez", "--object-path", path.c_str(), "--method", method,
               arg1, arg2, static_cast<char*>(nullptr));
    } else if (arg1 != nullptr) {
      ::execlp("gdbus", "gdbus", "call", "--system", "--timeout", "5", "--dest",
               "org.bluez", "--object-path", path.c_str(), "--method", method,
               arg1, static_cast<char*>(nullptr));
    } else {
      ::execlp("gdbus", "gdbus", "call", "--system", "--timeout", "5", "--dest",
               "org.bluez", "--object-path", path.c_str(), "--method", method,
               static_cast<char*>(nullptr));
    }
    _exit(127);  // execlp 回来就是没找到 gdbus；_exit 跳过 atexit/缓冲区
  }

  if (output != nullptr) {
    ::close(pipefd[1]);  // 父进程关掉写端，读到 EOF 才有意义
    char buf[256];
    for (;;) {
      const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
      if (n > 0) {
        output->append(buf, static_cast<size_t>(n));
      } else if (n == 0) {
        break;  // 子进程退出、写端全关：输出收完
      } else if (errno != EINTR) {
        break;  // 真错误：放弃收集，退出码仍照收
      }
    }
    ::close(pipefd[0]);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;  // 被信号打断不算失败，接着收尸
    DS5DUMMY_LOG(WARNING, "[bt] waitpid(gdbus) failed: {}", strerror(errno));
    return -1;
  }
  if (!WIFEXITED(status)) {
    // 被信号打死（如超时后 SIGKILL）拿不到可用的退出码，按失败处理。
    DS5DUMMY_LOG(WARNING, "[bt] gdbus terminated by signal {}", WTERMSIG(status));
    return -1;
  }
  return WEXITSTATUS(status);
}

/**
 * @brief 把 "AA:BB:CC:DD:EE:FF" 拼成 BlueZ 的设备对象 path。
 *
 * 形如 /org/bluez/hci0/dev_0C_27_…：冒号换成下划线、字母转大写；hci0 写死
 * ——本项目只支持第一块蓝牙适配器。
 *
 * @param mac 经典蓝牙 BD_ADDR 字符串。
 * @return 拼好的 path；不做输入合法性校验，非法 MAC 会拼出一个不存在的
 *         path，后续 D-Bus 调用自然失败。
 */
std::string BluezDevicePath(const std::string& mac) {
  std::string path = "/org/bluez/hci0/dev_";
  for (char c : mac) {
    // toupper 前转 unsigned char：负 char 直接进 toupper 是 UB。
    path +=
        (c == ':')
            ? '_'
            : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return path;
}

/**
 * @brief 读 Device1 的 Connected 属性，判断 ACL 是否在。
 *
 * 判据仅仅是 gdbus 输出里出现 "true" 子串——属性值以 variant 文本形式回来，
 * 布尔属性只会在真值时带这个字。读失败/输出异常一律按未连接处理，调用方
 * 据此走 Connect 重试路径。
 */
bool BluezDeviceConnected(const std::string& path) {
  std::string out;
  const int rc = RunGdbus(path, "org.freedesktop.DBus.Properties.Get",
                          "org.bluez.Device1", "Connected", &out);
  if (rc != 0) return false;
  return out.find("true") != std::string::npos;
}

/**
 * @brief 让 BlueZ 把这只手柄的 ACL 拉起来。
 *
 * 抢 PSM 之前链路必须在：我们随后要让 BlueZ 放开 HID profile，之后没有
 * 任何东西替这条 ACL 保活，BlueZ 会把闲置链路断掉。
 *
 * @param mac 目标手柄 MAC。
 * @return 已连上 true；轮询预算（25 次 × 200ms ≈ 5s）耗尽 false——调用方
 *         不因此放弃，照样会试裸 L2CAP，失败会以更直接的 errno 报出来。
 */
bool BluezEnsureAcl(const std::string& mac) {
  const std::string path = BluezDevicePath(mac);

  // 已连上就绝不再调 Connect()：profile 被我们放开后，Connect 可能一直等
  // profile 直到超时，反而把一条本来好的 ACL 折腾掉。300ms 是给状态落定
  // 留的经验缓冲。
  if (BluezDeviceConnected(path)) {
    DS5DUMMY_LOG(INFO, "[bt] BlueZ ACL already connected for {}", mac);
    usleep(300 * 1000);
    return true;
  }

  // 只点火不看结果：Connect 的返回值没有下面轮询 Connected 属性可靠。
  RunGdbus(path, "org.bluez.Device1.Connect", nullptr, nullptr, nullptr);

  // Connect 可能早于 ACL 就绪就返回（或返回 InProgress），改为轮询属性，
  // 预算 25 次 × 200ms。
  for (int attempt = 0; attempt < 25; ++attempt) {
    if (BluezDeviceConnected(path)) {
      DS5DUMMY_LOG(INFO, "[bt] BlueZ ACL connected for {}", mac);
      // Connected=true 仍可能早于 link key/加密完成，留 500ms settle，
      // 太短会让紧随其后的 PSM connect 吃到 EHOSTDOWN。
      usleep(500 * 1000);
      return true;
    }
    usleep(200 * 1000);
  }
  DS5DUMMY_LOG(WARNING,
        "[bt] BlueZ ACL did not become Connected for {}; "
        "will still try raw L2CAP",
        mac);
  return false;
}

/**
 * @brief 让 BlueZ 放开这只手柄的 HID profile，腾出 PSM 0x11/0x13 给我们。
 *
 * 调用后必须立刻 connect（见 Open()），间隙一长 BlueZ input plugin 会把
 * PSM 抢回去。
 *
 * @return profile 确实被释放 true；本来就没占用 false——只影响日志措辞，
 *         不影响后续抢通道。
 */
bool BluezDisconnectHid(const std::string& mac) {
  const std::string path = BluezDevicePath(mac);
  const int rc = RunGdbus(path, "org.bluez.Device1.DisconnectProfile",
                          kHidProfileUuid, nullptr, nullptr);
  if (rc == 0) {
    DS5DUMMY_LOG(INFO, "[bt] BlueZ HID profile released for {}", mac);
    return true;
  }
  DS5DUMMY_LOG(INFO, "[bt] BlueZ HID profile was not active for {} (gdbus rc={})", mac,
        rc);
  return false;
}

/**
 * @brief Close() 时把 HID profile 还给 BlueZ，手柄重新由内核 HID 栈接管。
 *
 * 忘了还的话，这只手柄在 BlueZ 眼里永远没有 HID 服务，下次内核不会接它。
 *
 * @param mac 当初抢 profile 的那只手柄；空串表示没抢过，直接跳过。
 */
void BluezRestoreHid(const std::string& mac) {
  if (mac.empty()) return;
  const std::string path = BluezDevicePath(mac);
  const int rc = RunGdbus(path, "org.bluez.Device1.ConnectProfile",
                          kHidProfileUuid, nullptr, nullptr);
  if (rc == 0) {
    DS5DUMMY_LOG(INFO, "[bt] BlueZ HID profile restored for {}", mac);
  } else {
    DS5DUMMY_LOG(WARNING, "[bt] BlueZ HID profile restore failed for {} (gdbus rc={})",
          mac, rc);
  }
}

// ---------------------------------------------------------------------------
// L2CAP socket 管理
// ---------------------------------------------------------------------------

/// 把 "AA:BB:..." 解析成内核 bdaddr_t（字节序按蓝牙小端存放）。
bool ParseMac(const std::string& mac, bdaddr_t& out) {
  return str2ba(mac.c_str(), &out) == 0;
}

/**
 * @brief 建一条到 @p addr 指定 PSM 的 SEQPACKET L2CAP 通道。
 *
 * SEQPACKET 而不是 SOCK_RAW：保留消息边界，一次 send 就是一个 HIDP 帧，
 * 一次 recv 收完整一帧，上层不需要自己做重组。connect 之前先把参数定下：
 * imtu/omtu 按 kWantMtu 请求；SO_SNDBUF 压到 kL2capSndbufBytes。成功后读回
 * 协商到的 MTU，只记日志不改行为。
 *
 * 只有链路类 errno（EHOSTDOWN/ETIMEDOUT/ENETUNREACH/ECONNREFUSED/EALREADY/
 * EBUSY/ECONNRESET）才重试——它们都随控制器状态落定可能自愈；socket()
 * 本身失败是本地资源问题，重试没有意义，直接放弃。
 *
 * @param addr  控制器 BD_ADDR（ParseMac 的产物）。
 * @param psm   目标 PSM，取 kPsmHidControl 或 kPsmHidInterrupt。
 * @param label 日志用的通道名。
 * @return 建好的 fd；重试耗尽或 socket() 失败返回 -1。
 */
int L2capConnectOne(const bdaddr_t* addr, uint16_t psm, const char* label) {
  int last_errno = 0;
  for (int attempt = 0; attempt < kL2capConnectRetries; ++attempt) {
    int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (fd < 0) {
      DS5DUMMY_LOG(ERROR, "{}: socket() failed: {}", label, strerror(errno));
      return -1;
    }

    // MTU 请求要在 connect 前落下，连上之后协商结果就定了。
    struct l2cap_options opts = {};
    opts.imtu = kWantMtu;
    opts.omtu = kWantMtu;
    // 两处 setsockopt 的返回值都不看：它们只调参数，设不上就按内核默认值跑，
    // 不影响这条通道能否连开——真正的问题会在紧随其后的 connect 上以 errno
    // 现形，那才是需要分支的地方。
    ::setsockopt(fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts));

    int sndbuf = kL2capSndbufBytes;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_l2 sa = {};
    sa.l2_family = AF_BLUETOOTH;
    sa.l2_psm = psm;
    sa.l2_bdaddr = *addr;
    sa.l2_cid = 0;       // 0 = 走 PSM 动态建连，不绑定固定 CID
    sa.l2_bdaddr_type = 0;  // 0 = BDADDR_BREDR：经典蓝牙，不是 LE

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) ==
        0) {
      // 读回真实协商值：只用于诊断，不改发送行为。
      socklen_t olen = sizeof(opts);
      if (::getsockopt(fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &olen) == 0) {
        DS5DUMMY_LOG(INFO, "{} connected (imtu={} omtu={})", label, opts.imtu,
              opts.omtu);
        if (opts.imtu < kHidpAudioFrameBytes) {
          DS5DUMMY_LOG(WARNING,
                "{}: MTU < {} — 0x36 audio frames may not fit", label,
                kHidpAudioFrameBytes);
        }
      } else {
        DS5DUMMY_LOG(INFO, "{} connected (MTU unknown)", label);
      }
      return fd;
    }

    last_errno = errno;
    ::close(fd);  // 失败的这次尝试不留 fd

    // 链路类错误等一等可能自愈；其余（权限、参数）重试一百次也是同样的错。
    bool retryable = (last_errno == EHOSTDOWN || last_errno == ETIMEDOUT ||
                      last_errno == ENETUNREACH || last_errno == ECONNREFUSED ||
                      last_errno == EALREADY || last_errno == EBUSY ||
                      last_errno == ECONNRESET);
    if (!retryable || attempt == kL2capConnectRetries - 1) break;

    // 两类高频失败给出人话提示，省一次全网搜索。
    const char* hint = "";
    if (last_errno == EHOSTDOWN)
      hint =
          " (controller may be on LE only — try: bluetoothctl disconnect MAC "
          "then connect MAC)";
    if (last_errno == ECONNREFUSED)
      hint = " (PSM may be taken — is BlueZ input plugin disabled?)";
    DS5DUMMY_LOG(WARNING, "{}: connect() errno={} ({}){}, retrying ({}/{})...", label,
          last_errno, strerror(last_errno), hint, attempt + 1,
          kL2capConnectRetries);
    usleep(kL2capConnectRetryMs * 1000);
  }
  DS5DUMMY_LOG(ERROR, "{}: connect() failed after {} attempts: {} (errno={})", label,
        kL2capConnectRetries, strerror(last_errno), last_errno);
  return -1;
}

// ---------------------------------------------------------------------------
// 控制通道协议（HIDP SET_PROTOCOL / GET_REPORT）
// ---------------------------------------------------------------------------

/**
 * @brief 在控制通道上带超时地收一个数据报。
 *
 * 控制通道是 blocking 的，超时靠 poll 保证；一次 recv 就是一个 SEQPACKET
 * 数据报，不需要循环收。
 *
 * @param buf        接收缓冲。
 * @param size       buf 容量；超过它的部分会被 SEQPACKET 截断。
 * @param timeout_ms poll 等待上限（毫秒）。
 * @return > 0 收到的字节数；0 超时内没数据；-1 fd 没开或 poll/recv 出错
 *         （errno 保留现场）。
 */
ssize_t ControlRecvTimeout(uint8_t* buf, size_t size, int timeout_ms) {
  if (ctrl_fd < 0) {
    errno = EBADF;
    return -1;
  }

  struct pollfd pfd = {ctrl_fd, POLLIN, 0};
  int r = ::poll(&pfd, 1, timeout_ms);
  if (r < 0) return -1;   // poll 出错
  if (r == 0) return 0;   // 超时：预算内手柄没回
  return ::recv(ctrl_fd, buf, size, 0);
}

/**
 * @brief 把中断通道从 boot protocol 切到 report protocol。
 *
 * 裸 L2CAP 连上来可能停在 boot protocol，而 DS5 的 boot 报告里没有我们要
 * 的完整 0x31 状态；不切过去一份输入报告都收不到。
 *
 * @return 请求帧完整发出 true——不等回应（SET_PROTOCOL 无回复，手柄随后
 *         的输入报告就是生效证据）；fd 没开或短写 false。
 */
bool ControlSetProtocolReportMode() {
  if (ctrl_fd < 0) return false;

  // HIDP SET_PROTOCOL 请求帧 [0x71][0x01]：参数 0x01 = report protocol；
  // 头字节 0x71 是线上实际形态（事务码 0x70 叠加低位），按抓包/上游字节照抄。
  uint8_t req[2] = {0x71, 0x01};
  ssize_t n = ::send(ctrl_fd, req, sizeof(req), MSG_NOSIGNAL);
  if (n != static_cast<ssize_t>(sizeof(req))) {
    DS5DUMMY_LOG(WARNING, "control SET_PROTOCOL(report): send() failed: {} (errno={})",
          n < 0 ? strerror(errno) : "short write", errno);
    return false;
  }
  DS5DUMMY_LOG(INFO, "control SET_PROTOCOL(report) sent");
  return true;
}

/**
 * @brief 发 HIDP GET_REPORT(@p report_id)，等到 id 匹配的 feature 回包。
 *
 * 控制通道上的 0xA3 回包可能属于上一次请求（它超时后才到），所以按回包自带
 * 的 report id 配对：只有 reply_buf[1] == @p report_id 的那一份算本次的答案，
 * id 不符或首字节不是 0xA3 的包读掉丢弃、继续等。与上游 DS5Dongle 的归档
 * 方式一致——那边的 feature_data 按回包里 packet[1] 建键，条目永远只对应
 * 自己的 id。整体预算 kControlRecvTimeoutMs，每轮 recv 只按剩余时间等。这里
 * 只管把控制通道上的原始帧收回来，缓存一律不碰。
 *
 * @param report_id   目标 report id。
 * @param reply_buf   收包缓冲：成功时 reply_buf[0] 是事务头 0xA3，
 *                    reply_buf[1..] 就是 [report id][报告体]。
 * @param reply_size  reply_buf 容量。
 * @return > 0 收到的帧字节数（>= 2 才算真带回了报告）；超时、poll/recv
 *         出错或 fd 没开时 -1。
 * @note 同步阻塞调用线程（建链主线程或 ep0 线程），最长约 2s。
 */
ssize_t ControlGetReport(uint8_t report_id, uint8_t* reply_buf,
                         size_t reply_size) {
  if (ctrl_fd < 0) return -1;

  // GET_REPORT 请求只有 2 字节：事务头 + 目标 report id。
  uint8_t req[2] = {core::protocol::kHidpGetReport, report_id};
  ssize_t n = ::send(ctrl_fd, req, sizeof(req), MSG_NOSIGNAL);
  if (n != static_cast<ssize_t>(sizeof(req))) {
    DS5DUMMY_LOG(ERROR, "control GET 0x{:02X}: send() errno={}", report_id, errno);
    return -1;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kControlRecvTimeoutMs);
  for (;;) {
    // 每轮把 deadline 折成剩余预算传下去，多轮非目标包不会放大总等待时间。
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    if (remaining <= 0) {
      DS5DUMMY_LOG(ERROR, "control GET 0x{:02X}: timeout (no 0xA3 reply in {}ms)",
            report_id, kControlRecvTimeoutMs);
      return -1;
    }

    n = ControlRecvTimeout(reply_buf, reply_size, static_cast<int>(remaining));
    if (n <= 0) {
      // 0 = 这一轮就把整段预算用完了；<0 = poll/recv 出错。两种都不再等。
      if (n == 0)
        DS5DUMMY_LOG(ERROR, "control GET 0x{:02X}: timeout (no reply in {}ms)",
              report_id, kControlRecvTimeoutMs);
      else
        DS5DUMMY_LOG(ERROR, "control GET 0x{:02X}: recv error: {}", report_id,
              strerror(errno));
      return -1;
    }

    // 只有 [0xA3][本次请求的 id] 才算答案；其余包（SET_PROTOCOL 的回包、
    // 异步通知、其它 id 的迟到回复）读掉丢弃、继续等剩余预算。
    if (n >= 2 && reply_buf[0] == core::protocol::kHidpFeatureReply &&
        reply_buf[1] == report_id) {
      return n;
    }
    if (reply_buf[0] == core::protocol::kHidpFeatureReply) {
      DS5DUMMY_LOG(WARNING,
            "control GET 0x{:02X}: ignoring reply for report 0x{:02X} (len={})",
            report_id, n >= 2 ? reply_buf[1] : 0, n);
    } else {
      DS5DUMMY_LOG(WARNING,
            "control GET 0x{:02X}: ignoring HIDP control header 0x{:02X}",
            report_id, reply_buf[0]);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// §A 后端接缝：本文件唯一对外负责的东西（bt_detail.h 里那 7 个符号）
// ---------------------------------------------------------------------------

namespace bt::detail {

namespace {

/// 开链后补发的 0x32 唤醒报告，整份含尾 4 字节 CRC 冻结在这里。[0]=0x32、
/// [2]=0x90、[3]=0x3f 逐字节照抄上游形态，[1]=0x10 的高 4 位正是队列路径
/// 盖 seq 的那 4 位，这里等于把 seq 钉死成 1（与队列从 0 起计的编号不连续，
/// 控制器是否检查未实机验证）。[4..66] 是 SetStateData 的固件初值，与
/// bridge 的状态影子开局内容一致——byte[4]/byte[5] 的耳机/扬声器音量非零，
/// 是控制器接受后续 0x36 报告的前提，改这份数组会同时改掉开局的输出状态。
/// [67..137] 全零，[138..141] 是覆盖前 138 字节的 CRC（kOutputReportCrcSeed）。
constexpr uint8_t kWakeReport[core::protocol::kBtWakeReportSize] = {
    0x32, 0x10, 0x90, 0x3f, 0xfd, 0xf7, 0x00, 0x00, 0x7f, 0x64, 0xff, 0x09,
    0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x07, 0x00, 0x00, 0x02, 0x01, 0x00,
    0xff, 0xd7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8a, 0x01, 0xfa, 0x7f,
};

/// 唤醒直发的重试预算（经验值，跨过链路初建的不稳定窗口）：中断通道刚
/// 拿到时可能还没排到发送窗口，给它几轮 10ms 的余地。
constexpr int kWakeAttempts = 5;
constexpr int kWakePollMs = 10;
constexpr useconds_t kWakeRetryDelayUs = 10 * 1000;

/// 把 kWakeReport 发出去，失败只记 WARNING 不判建链失败：主机随后的第一份
/// 真实输出报告同样能把手柄推起来，而这里判死会让 worker 白扔掉刚建好的链路。
void SendWakeReport() {
  for (int attempt = 0; attempt < kWakeAttempts; ++attempt) {
    const IoResult waited = WaitWritable(intr_fd, kWakePollMs);
    if (waited == kOk) {
      const IoResult sent = Write(kWakeReport, sizeof(kWakeReport));
      if (sent == kOk) {
        DS5DUMMY_LOG(INFO, "sent 0x32 wake report ({} bytes)", sizeof(kWakeReport));
        return;
      }
      if (sent != kAgain) {
        DS5DUMMY_LOG(WARNING, "0x32 wake report failed (result={})",
              static_cast<int>(sent));
        return;
      }
    } else if (waited != kAgain) {
      DS5DUMMY_LOG(WARNING, "0x32 wake report wait failed (result={})",
            static_cast<int>(waited));
      return;
    }
    usleep(kWakeRetryDelayUs);
  }
  DS5DUMMY_LOG(WARNING, "0x32 wake report not sent after {} attempts", kWakeAttempts);
}

}  // namespace

/**
 * @brief 抢下两个 HIDP PSM，把控制通道切到 report protocol。
 *
 * 顺序是刻意的：BluezEnsureAcl 先把 ACL 弄上来 → 反复"放开 HID profile
 * 并立刻 connect"（中间留空隙 BlueZ input plugin 会把 PSM 抢回去）→
 * 控制通道成了再开中断通道 → 两条都在才算成功。任一环节失败就把已拿到的
 * fd 和 profile 经 Close() 一并交还。
 *
 * 先关后开与 feature 预取都不在这里：门面的 `bt::Open()` 每轮建链前先调
 * 本文件的 Close()，成功后再预取（见 bt_link.cpp）。
 *
 * @param mac 手柄的经典 BD_ADDR，必须是 ParseMac 认得的格式。
 * @return 两条通道都打开 true；false 时不留半开链路，可直接重试。
 * @note 只在 worker 主线程调用；内含 D-Bus 子进程与 connect 重试，最坏情况
 *       阻塞数秒到十几秒（ACL 轮询 5s + 每 PSM 3×(connect+500ms)）。
 */
bool Open(const std::string& mac) {
  bdaddr_t bdaddr;
  if (!ParseMac(mac, bdaddr)) {
    DS5DUMMY_LOG(ERROR, "invalid MAC address: {}", mac.c_str());
    return false;
  }

  // 按 b[5]→b[0] 打日志：bdaddr_t 是小端存放，倒着读才是人熟的 MAC 顺序。
  DS5DUMMY_LOG(INFO, "connecting to {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        bdaddr.b[5], bdaddr.b[4], bdaddr.b[3], bdaddr.b[2], bdaddr.b[1],
        bdaddr.b[0]);

  // 返回值刻意不看：ACL 没被 BlueZ 确认起来也照样裸连一把，失败会以具体
  // errno 报出来，比在这里提前判死信息更多。
  BluezEnsureAcl(mac);

  // 外层 3 轮，每轮"释放 profile → 立刻抢 connect"：L2capConnectOne 内部
  // 还有自己的 3 次 connect 重试，轮间只隔 100ms 让 profile 释放生效。
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (BluezDisconnectHid(mac)) {
      // 放开成功才记 MAC：Close() 只对真放开过的 profile 发归还——建链失败
      // 路径靠它把 profile 还回去，没放开过则省掉一发注定失败的 ConnectProfile。
      hidp_stolen_mac = mac;
    }
    // 紧接 connect：间隙一长 BlueZ input plugin 可能重新 claim PSM。
    ctrl_fd = L2capConnectOne(&bdaddr, kPsmHidControl, "control 0x11");
    if (ctrl_fd >= 0) break;
    usleep(100 * 1000);
  }
  if (ctrl_fd < 0) {
    Close();
    return false;
  }

  intr_fd = L2capConnectOne(&bdaddr, kPsmHidInterrupt, "interrupt 0x13");
  if (intr_fd < 0) {
    Close();  // 半开的控制通道 + 被放开的 profile 一起回收
    return false;
  }

  // profile 归还记录已在循环里落下（放开成功才记），Close() 据此归还。
  DS5DUMMY_LOG(INFO, "both channels open");
  // 返回值刻意不看：这条请求没有回复，唯一的失败形态是 send 本身没出去，
  // 而那已经在函数内部以 WARNING 落进日志了。两条通道是真的建好了，把一次
  // 成功的建链判成失败会让 worker 白扔掉已到手的 fd 重来一轮。
  ControlSetProtocolReportMode();

  // 裸 L2CAP 上没有别的 HID 主机跟手柄说话，手柄停在被动状态，不推这一份
  // 0x32 就不会开始上报输入。hidraw 后端由内核驱动在建节点时初始化过手柄，
  // 没有对应动作。
  SendWakeReport();

  return true;
}

void Close() {
  // 幂等：两个 fd 各自独立判关，profile 归还只处理"有"的情形。
  // feature 缓存不在这里清——它的生命周期归门面（bt_link.cpp 的 bt::Close）。
  if (ctrl_fd >= 0) {
    ::close(ctrl_fd);
    ctrl_fd = -1;
  }
  if (intr_fd >= 0) {
    ::close(intr_fd);
    intr_fd = -1;
  }
  // 先清空再调用：归还动作只对该 MAC 做一次，重复 Close() 不会重复打 D-Bus。
  if (!hidp_stolen_mac.empty()) {
    const std::string mac = hidp_stolen_mac;
    hidp_stolen_mac.clear();
    BluezRestoreHid(mac);
  }
}

/**
 * @brief 组一个 HIDP_DATA out 帧发到中断通道，一次尝试，不等待。
 *
 * 帧是 `[0xA2][报告体]`，一次 send 整体作为 SEQPACKET 数据报交出去。可写
 * 判定归调用方：写线程在 poll 里等 POLLOUT，开链末尾的唤醒直发先
 * WaitWritable——这里带 MSG_DONTWAIT，所以等不到也只是立刻交回 kAgain。
 * 超过帧缓冲容量的长度是调用方的错，直接 kFatal。
 *
 * @param data 裸报告体，data[0] 是输出 report id（不含帧头）。
 * @param len  字节数。
 * @return kOk 已整帧发出；kAgain 缓冲满（同一份字节可原样重试）；kClosed
 *         链路已不可用；kFatal fd 没开、太长、短写或没归类过的 errno。
 */
IoResult Write(const uint8_t* data, size_t len) {
  if (intr_fd < 0) return kFatal;

  // 帧缓冲按协商目标 MTU 取上限；最长的 0x36 帧 399B 宽裕。这个 672 与
  // kWantMtu 是同一个数值的两处书写：改协商目标必须连这里一起改，否则长度
  // 检查放行的帧会比实际请求的 MTU 更长，SEQPACKET 发送直接吃 EMSGSIZE。
  uint8_t frame[672];
  if (len + 1 > sizeof(frame)) {  // +1 是帧头占位
    DS5DUMMY_LOG(ERROR, "write: packet too large ({} + 1 > {})", len, sizeof(frame));
    return kFatal;
  }
  frame[0] = core::protocol::kHidpDataOut;
  memcpy(frame + 1, data, len);

  // NOSIGNAL：socket 断开时不触发 SIGPIPE；DONTWAIT：缓冲满时拿到 EAGAIN
  // 交回 kAgain，而不是陷进 blocking 发送。
  const ssize_t n =
      ::send(intr_fd, frame, len + 1, MSG_DONTWAIT | MSG_NOSIGNAL);
  if (n >= 0) {
    if (static_cast<size_t>(n) != len + 1) {
      // SEQPACKET 应当整发整收，短送说明内核没全收，没有续发这条路。
      DS5DUMMY_LOG(ERROR, "write: short send {}/{}", n, len + 1);
      return kFatal;
    }
    return kOk;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    return kAgain;  // 此刻没余量：调用方重新等可写后原样重试这一份字节
  }
  if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
    // 对端拆了通道：确定性的断链，交上层重建。
    DS5DUMMY_LOG(WARNING, "write: send() link unavailable: {} (errno={})",
          strerror(errno), errno);
    return kClosed;
  }
  DS5DUMMY_LOG(ERROR, "write: send() failed: {} (errno={})", strerror(errno), errno);
  return kFatal;
}

/**
 * @brief 在控制通道上现取一份 feature 报告；缓存一律不碰。
 *
 * 命中判断与回填都在门面的 `bt::GetFeature()`（bt_link.cpp）。控制通道上的
 * 回帧是 `[0xA3][report id][报告体]`，剥掉事务头之后的布局和调用方的 buf
 * 同形（首字节都是 report id），所以整段拷回即可。
 *
 * @param buf buf[0] 是目标 report id，成功时被覆写成报告体（首字节仍是 id）。
 * @param len buf 容量。
 * @return 写回的字节数（>= 1，含首字节 id）；链路没开、len < 2 或线上
 *         没回复时 -1。
 * @note 同步等控制通道回复（预算 2s），调用方要接受这次阻塞。
 */
int GetFeature(uint8_t* buf, size_t len) {
  if (ctrl_fd < 0) return -1;
  if (buf == nullptr || len < 2) return -1;  // 至少装下 report id + 1 字节

  // 512B：控制通道上最长的 feature 报告也远短于此，SEQPACKET 至多截到容量。
  uint8_t reply[512];
  const ssize_t n = ControlGetReport(buf[0], reply, sizeof(reply));
  if (n < 2) {
    // n<0 = 超时或出错；n==1 = 只有事务头的空回包。两种都没有报告体可交。
    if (n == 1) DS5DUMMY_LOG(WARNING, "control GET 0x{:02X}: empty reply", buf[0]);
    return -1;
  }

  // reply[1] 起就是 [report id][报告体]：按调用方容量截断，整段拷回。
  const size_t copied = std::min(static_cast<size_t>(n) - 1, len);
  memcpy(buf, reply + 1, copied);
  return static_cast<int>(copied);
}

int Fd() { return intr_fd; }

bool IsOpen() { return ctrl_fd >= 0 && intr_fd >= 0; }

/**
 * @brief 从中断通道非阻塞收一个 HIDP_DATA in 帧，剥掉帧头交出去。
 *
 * recv 用 MSG_DONTWAIT：本函数由读线程在 poll 报可读后调用，绝不能陷进阻塞
 * 等待。MSG_TRUNC 让返回值是数据报的真实长度——SEQPACKET 默认会把超出容量的
 * 那段静默丢掉并只交回缓冲区装下的部分，那样交出去的半份报告会被 bridge 当
 * 真的解析。有了真实长度，截断就是精确判定而不是猜。交出去的字节第一个就是
 * report id。控制通道不参与：它只在 feature 收发时按需同步读写。
 *
 * @param buf        调用方缓冲区，一个完整 MTU 装得下即可。
 * @param size       buf 容量，同时是截断判定线。
 * @param bytes_read 非空时先清零，成功时写入剥头后的字节数。
 * @return kOk 拿到一份；kAgain 现在没数据（EAGAIN/EWOULDBLOCK/EINTR）；
 *         kTruncated 数据报比 size 长（整帧已被内核消费，无法再取）；
 *         kClosed 对端关闭、只剩孤帧头的短帧，以及链路类 errno；
 *         kFatal fd 没开或没归类过的 errno。
 */
IoResult Read(uint8_t* buf, size_t size, size_t* bytes_read) {
  if (bytes_read != nullptr) *bytes_read = 0;
  if (intr_fd < 0) {
    errno = EBADF;
    return kFatal;
  }

  ssize_t n = ::recv(intr_fd, buf, size, MSG_DONTWAIT | MSG_TRUNC);
  if (n > 0) {
    if (static_cast<size_t>(n) > size) {
      // 整帧比缓冲长：装进 buf 的只有前 size 字节，剩下那段内核已经丢掉了。
      return kTruncated;
    }
    // 到这里数据必是 L2CAP 送来的 [0xA1][body...]；不足 2 字节只剩孤立
    // 帧头，没有任何报告内容，按链路异常处理。
    if (n < 2) {
      DS5DUMMY_LOG(WARNING, "interrupt channel received orphan HIDP header");
      return kClosed;
    }
    const size_t body_len = StripHidpDataIn(buf, static_cast<size_t>(n));
    if (bytes_read != nullptr) *bytes_read = body_len;
    return kOk;
  }

  if (n == 0) {
    // SEQPACKET 上 recv==0 = 对端有序关闭通道。
    DS5DUMMY_LOG(WARNING, "interrupt channel closed by peer");
    return kClosed;
  }

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    // EINTR 也归"现在没数据"而不是就地重试：把机会让给下一轮 poll。
    return kAgain;
  }
  // 链路类 errno：不是进程 bug，交上层按断链重建更合理，别升级成 kFatal。
  if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN ||
      errno == ESHUTDOWN || errno == ENODEV) {
    DS5DUMMY_LOG(WARNING, "interrupt channel link unavailable: {} (errno={})",
          strerror(errno), errno);
    return kClosed;
  }
  DS5DUMMY_LOG(ERROR, "interrupt channel recv error: {} (errno={})", strerror(errno),
        errno);
  return kFatal;
}

}  // namespace bt::detail
