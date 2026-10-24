/**
 * @file
 * @brief bt 的 hidraw 后端：把 BlueZ 内核驱动当作链路，只实现 §A 接缝
 *
 * 本后端不碰 L2CAP，也不自己组 HIDP 帧：ACL、HID session、0xA1/0xA2 帧头
 * 都由内核 hidp/hidraw 建立和维护。它只做三件事——在 /sys/class/hidraw
 * 里认出那只手柄、读写裸报告体、用 ioctl 现取 feature 报告。
 *
 * 因此 Read()/Write() 一个字节都不加减：进来的报告体和出去的报告体首字节
 * 都是 report id。"让手柄开始推输入报告"这件事内核驱动建节点时已经替我们
 * 做过，所以建链末尾不需要 L2CAP 那样的 0x32 唤醒报告。
 *
 * 建链先关后开、feature 缓存、预取都不在这里——见 bt_link.cpp。
 * 本文件的 Write() 一步都不等：发不出就交回 kAgain。
 *
 * 节点的写路径不看 O_NONBLOCK：写之前的可写判定由调用方用 poll 代劳，
 * 理由写在 Write() 上。
 * @see bt_detail.h —— 本文件要实现的那 7 个符号。
 */

#include "bt/bt_detail.h"
#include "core/debug.h"
#include "core/device/identity.h"

#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "loguru.hpp"

/// std::filesystem 的短名，本文件只用它遍历 /sys/class/hidraw。
namespace fs = std::filesystem;

namespace {

/// 已打开的 hidraw 节点，-1 表示链路没开。Fd()/IsOpen()/Read()/Write() 都
/// 认这一个 fd；只由本文件的 Open() 赋值、Close() 复位，无并发写者。
int hidraw_fd = -1;

/// 上一次打"没找到手柄"WARNING 的时刻。建链阶段 worker 每 500ms 重试一次
/// Open()，这个闸门把该日志限流成最多 5 秒一条。
std::chrono::steady_clock::time_point last_missing_log{};

/**
 * @brief 扫 /sys/class/hidraw，打开第一个被认定为 DualSense 的节点。
 *
 * 认定条件（从各节点的 device/uevent 读取）：
 * - HID_ID 的总线字段是 0x0005（Bluetooth HID 总线），排除 USB 直连的那只；
 * - VID/PID 匹配真 Sony 054C:0CE6 或 DKMS 合成的 1998:1221；
 * - want_mac 非空且节点带 HID_UNIQ 时，MAC 必须大小写不敏感地相等；
 *   UNIQ 为空的节点放行——只有多设备时才需要靠它区分。
 *
 * @param want_mac 目标手柄的 MAC，空串表示任意一只。
 * @return 打开成功的 fd（O_RDWR|O_NONBLOCK，读侧的 EAGAIN 靠这个标志）；
 *         没有匹配项时 -1。
 * @note 匹配即返回：一次调用只开一个节点，后面的候选即便更像也不再看。
 */
int FindDualSenseHidraw(const std::string& want_mac) {
  for (const auto& entry : fs::directory_iterator("/sys/class/hidraw")) {
    fs::path uevent = entry.path() / "device" / "uevent";
    // "re"：只读 + 关本进程泄漏出去的 fd，扫描中途失败不留尾巴。
    FILE* f = fopen(uevent.c_str(), "re");
    if (!f) continue;

    uint32_t hid_bus = 0, vid = 0, pid = 0;
    std::string uniq;
    // 逐行读的单行缓冲：uevent 的键值行远短于 512，真超长会被 fgets 切成两段、
    // 前缀匹配落在段首之外而读不到，最坏结果是这一只手柄认不出来。
    char line[512];
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "HID_ID=", 7) == 0) {
        // uevent 里是 "HID_ID=总线:VID:PID" 的十六进制三段。
        unsigned a, b, c;
        if (sscanf(line, "HID_ID=%x:%x:%x", &a, &b, &c) == 3) {
          hid_bus = a;
          vid = b;
          pid = c;
        }
      }
      if (strncmp(line, "HID_UNIQ=", 9) == 0) {
        uniq = line + 9;
        while (!uniq.empty() && (uniq.back() == '\n' || uniq.back() == '\r'))
          uniq.pop_back();  // 去掉行尾换行，比较时才不夹带 \n
      }
    }
    fclose(f);

    // 总线必须是 Bluetooth HID；VID/PID 两对候选命中其一即可：真机那一对，
    // 或本项目 DKMS 补丁给真手柄合成的 1998:1221。
    if (hid_bus != 0x0005 ||
        !((vid == core::device::kSonyVid && pid == core::device::kDs5Pid) ||
          (vid == core::device::kDs5DummyVid &&
           pid == core::device::kDs5DummyPid)))
      continue;

    // MAC 过滤只对手柄真报了 UNIQ 的节点生效。
    if (!want_mac.empty() && !uniq.empty()) {
      if (strcasecmp(uniq.c_str(), want_mac.c_str()) != 0) continue;
    }

    std::string dev = "/dev/" + entry.path().filename().string();
    DS5DUMMY_LOG(INFO, "[bt] hidraw: found DualSense at {} (UNIQ={})", dev.c_str(),
          uniq.empty() ? "(unknown)" : uniq.c_str());

    // O_NONBLOCK 是给读侧的：hidraw_read() 只在节点带这个标志时才交回
    // EAGAIN，否则队列空时它睡在 read_mutex 上——读线程的"读到空为止"就变成
    // "睡到下一份报告为止"，收工通知和静默看门狗都走不到。写侧不受影响：
    // hidraw_write() 根本不看文件标志。
    int fd = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      DS5DUMMY_LOG(ERROR, "[bt] hidraw: open {} failed: {}", dev.c_str(),
            strerror(errno));
    }
    // 无论开成功与否都返回：认定条件是"是这只手柄"，节点打不开属于更高
    // 一层的问题（权限/竞态），交给调用方判，不再继续扫别的候选。
    return fd;
  }
  return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// §A 后端接缝：本文件唯一对外负责的东西（bt_detail.h 里那 7 个符号）
// ---------------------------------------------------------------------------

namespace bt::detail {

bool Open(const std::string& mac) {
  // "先关后开"与预取都在门面里（bt_link.cpp），这里只负责认出节点并打开。
  hidraw_fd = FindDualSenseHidraw(mac);
  if (hidraw_fd < 0) {
    // WARNING 限流：初始值全零的 last_missing_log 保证第一次失败必打。
    const auto now = std::chrono::steady_clock::now();
    if (last_missing_log.time_since_epoch().count() == 0 ||
        now - last_missing_log >= std::chrono::seconds(5)) {
      DS5DUMMY_LOG(WARNING,
            "[bt] hidraw: no Bluetooth DualSense found (target={}); "
            "is it paired and connected?",
            mac.empty() ? "any" : mac.c_str());
      last_missing_log = now;
    }
    return false;
  }
  last_missing_log = {};  // 找到即复位闸门：下一轮"从有到无"要立刻报出来

  DS5DUMMY_LOG(INFO, "[bt] hidraw: connected (fd={})", hidraw_fd);
  return true;
}

void Close() {
  if (hidraw_fd >= 0) {
    ::close(hidraw_fd);
    hidraw_fd = -1;
  }
}

/**
 * @brief 把一个完整裸报告体交给 hidraw 节点，一次尝试，不等待。
 *
 * 这里刻意不 poll 也不 MSG_DONTWAIT：hidraw_write() 不看 O_NONBLOCK，写会
 * 一直等到内核把报告交给设备，**调用方必须先确认 POLLOUT**（传输线程在
 * poll 里等可写）。BlueZ 的 hidraw
 * 在输入队列满时给出的 errno 并不是 EAGAIN，非阻塞写会冒出一个看起来像
 * "设备没了"的错误码——把可写判定交给 poll、这里只做一次整报文写，就绕开了
 * 那个坑。
 *
 * @param data 裸报告体，data[0] 是输出 report id，不含任何 framing。
 * @param len  字节数。
 * @return kOk 已整份发出；kAgain 内核此刻没收（可原样重试同一份字节）；
 *         kClosed 节点已不可用；kFatal 短写、fd 没开或没归类过的 errno。
 */
IoResult Write(const uint8_t* data, size_t len) {
  if (hidraw_fd < 0) return kFatal;

  const ssize_t n = ::write(hidraw_fd, data, len);
  if (n >= 0) {
    if (static_cast<size_t>(n) != len) {
      // hidraw 写是整报文语义：短写就是内核没全收，没有"续写"这条路。
      DS5DUMMY_LOG(ERROR, "[bt] hidraw: short write {}/{}", n, len);
      return kFatal;
    }
    return kOk;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    // 理论上 POLLOUT 之后读不到这三个；真出现就是 poll 与 write 之间的竞态
    // 窗口，交回 kAgain 让调用方重新等可写、原样重试同一份字节。
    return kAgain;
  }
  if (errno == ENODEV || errno == ENOTCONN || errno == ESHUTDOWN) {
    // "节点还摸得到、链路已经没了"的三种表现，一律按断链交上层重建。
    DS5DUMMY_LOG(WARNING, "[bt] hidraw: fd unavailable: {} (errno={})",
          strerror(errno), errno);
    return kClosed;
  }
  DS5DUMMY_LOG(ERROR, "[bt] hidraw: write 0x{:02X} failed: {} (errno={})", data[0],
        strerror(errno), errno);
  return kFatal;
}

/**
 * @brief 用 HIDIOCGFEATURE 现取一份 feature 报告；缓存一律不碰。
 *
 * 命中判断与回填都在门面的 `bt::GetFeature()`（bt_link.cpp），所以这里每次
 * 调用都真的上线跑一趟。
 *
 * @param buf buf[0] 是目标 report id，成功时被内核写回的报告覆写。
 * @param len buf 容量，同时也是向内核索要的字节数上限。
 * @return 实际字节数（>= 1，含首字节 id）；链路没开、len < 2（id 之外至少
 *         得有一个数据字节）、ioctl 失败或内核返回 0 字节时 -1。
 */
int GetFeature(uint8_t* buf, size_t len) {
  if (hidraw_fd < 0) return -1;
  if (buf == nullptr || len < 2) return -1;

  // 命令号 HIDIOCGFEATURE(len) 把缓冲区大小编码进去了：传 buf 实际容量。
  const int got = ::ioctl(hidraw_fd, HIDIOCGFEATURE(len), buf);
  if (got < 0) {
    DS5DUMMY_LOG(ERROR, "[bt] hidraw: HIDIOCGFEATURE(0x{:02X}) failed: {}", buf[0],
          strerror(errno));
    return -1;
  }
  if (got == 0) {
    // 内核回了个空报告：没有可交出去的字节，按现取失败处理（缓存保持冷）。
    DS5DUMMY_LOG(WARNING, "[bt] hidraw: HIDIOCGFEATURE(0x{:02X}) returned 0 bytes",
          buf[0]);
    return -1;
  }
  return got;
}

int Fd() { return hidraw_fd; }

bool IsOpen() { return hidraw_fd >= 0; }

/**
 * @brief 从 hidraw 节点读一份裸输入报告体。
 *
 * 内核已经剥掉 HIDP 帧头，读到的第一个字节就是 report id。节点以 O_NONBLOCK
 * 打开（见 Open()），所以队列空时 read 交回 EAGAIN 而不是睡在内核里——读线程
 * 要的是"一次醒来读到空为止"，能睡就等于没有收工与看门狗。
 *
 * 截断是这里唯一没法精确知道的事：`hidraw_read()` 把报告按 `min(count,
 * report->size)` 交出来，长了的部分被静默丢掉，接口上没有任何标志。所以判定
 * 只能是"装满容量即可疑"——容量 128（`bt_rx.cpp` 的 `kMaxInputBody`）比已知最长
 * 的输入报告（0x31，78 字节，含首字节 report id，见 `kBtReportSize`）大一截，
 * 真装满只可能是设备送来了没见过的长报告。
 *
 * @param buf        调用方缓冲区。
 * @param size       buf 容量，必须 >= 1，同时是截断判定线。
 * @param bytes_read 非空时先清零，成功时写入本次读到的字节数。
 * @return kOk 拿到一份；kAgain 现在没数据；kTruncated 读满容量，这一份按可疑
 *         处理已作废；kClosed 读到 0 字节，或 ENODEV/ENOTCONN/ESHUTDOWN 这种
 *         "节点还在、链路已没"；kFatal fd 没开、size 为 0 或没归类过的 errno。
 */
IoResult Read(uint8_t* buf, size_t size, size_t* bytes_read) {
  if (bytes_read != nullptr) *bytes_read = 0;
  // 手动置 errno：这两个分支不是系统调用失败出来的，置出来让 kFatal 的
  // "errno 保留现场"约定对调用方始终成立。
  if (hidraw_fd < 0) {
    errno = EBADF;
    return kFatal;
  }
  if (size < 1) {
    errno = EINVAL;
    return kFatal;
  }

  for (;;) {
    ssize_t n = ::read(hidraw_fd, buf, size);
    if (n > 0) {
      if (static_cast<size_t>(n) == size) {
        // 读满容量：内核多半是把这份报告切断了交出来。半份报告交给 bridge
        // 按字段解析比丢一份更糟，所以这里丢掉、交回 kTruncated。
        return kTruncated;
      }
      if (bytes_read != nullptr) *bytes_read = static_cast<size_t>(n);
      return kOk;
    }
    if (n == 0) {
      // hidraw 不该 EOF；读到 0 就是设备消失的内核表达，按断链处理。
      DS5DUMMY_LOG(WARNING, "[bt] hidraw: connection closed by peer");
      return kClosed;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return kAgain;
    if (errno == ENODEV || errno == ENOTCONN || errno == ESHUTDOWN) {
      DS5DUMMY_LOG(WARNING, "[bt] hidraw: read link unavailable: {} (errno={})",
            strerror(errno), errno);
      return kClosed;
    }
    DS5DUMMY_LOG(ERROR, "[bt] hidraw: read error: {} (errno={})", strerror(errno),
          errno);
    return kFatal;
  }
}

}  // namespace bt::detail
