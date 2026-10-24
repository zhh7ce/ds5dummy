/**
 * @file
 * @brief 写线程与它拥有的发送队列：POLLOUT 事件驱动，一份没发完不取下一份
 *
 * 队列是背压的终点：`PostOutput()` 在任何生产方线程上非阻塞入队，seq 与尾部
 * CRC 就在入队的锁里一次盖好——所以队列里躺着的字节和线上发出去的字节逐字节
 * 相同，出问题好归因。满 8 份丢刚来的那份，宁丢最新也不让 backlog 无限增长
 * （丢最旧会在已分配的 seq 上留空洞，手柄因此跳号丢音频）。
 *
 * 线程只做两件事：从队首取一份，然后等这条链路的 POLLOUT 把它发出去。
 * 两个设计约束决定了这里的形状：
 * - POLLOUT 只在手上有活的的时候才进 poll set。L2CAP 的 sndbuf 有空位时它恒
 *   真，hidraw 更是"always writable"（内核 `hidraw_poll()` 连 EPOLLOUT 都是
 *   无条件置上的），常驻 poll set 会把这条线程变成 100% 空转。
 * - 队列空时真的睡（poll 超时 -1）：链路的死活归读线程的静默看门狗判，写侧
 *   不需要自己的节拍，也就不再每秒空醒上百次。
 *
 * 判死只有一条路：`detail::Write` 或 poll 给出确定性错误，或写侧 fd 报
 * HUP/ERR/NVAL。等不到可写不是判死：这份的 seq/CRC 已经盖过，原地等下一次
 * POLLOUT 重发同一份字节即可，没有"退回队首、交回外层节奏"这回事。
 *
 * @see bt_plane.h —— 收工 fd 与 wake fd；bt_detail.h §A —— 写原语（不等待）。
 */

#include "bt/bt.h"

#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

#include "bt/bt_detail.h"
#include "bt/bt_plane.h"
#include "core/debug.h"
#include "core/protocol/crc32.h"
#include "core/protocol/report.h"
#include "loguru.hpp"

namespace bt {
namespace {

/// 队列里的一份报告体：seq 和尾部 CRC 已在 `PostOutput()` 盖好，
/// 本线程只负责整份写出去，不再碰内容。
struct SendItem {
  std::array<uint8_t, kMaxOutputBody> body = {};  ///< 定长槽，实际长度看 len
  size_t len = 0;
};

#if DS5DUMMY_DEBUG
/// 本线程算 Write 耗时用的单调时钟，不受系统时钟跳变影响。
using Clock = std::chrono::steady_clock;
#endif

// ---------------------------------------------------------------------------
// 写线程的私有状态：一把锁管队列与 seq；诊断构建下再加窗口量与累计量
// ---------------------------------------------------------------------------

/// 本线程的句柄。只由 bt::StartThreads()/StopThreads() 经 tx:: 接口碰。
std::thread thread;

std::mutex queue_mutex;
std::deque<SendItem> queue;         ///< 待发的报告体，FIFO
uint8_t report_seq = 0;             ///< 下一份入队报告要盖的 4-bit seq

#if DS5DUMMY_DEBUG
size_t queue_high_water = 0;        ///< 窗口量：本窗口内队列到过的最深
uint64_t write_us_total = 0;        ///< 窗口量：链路 Write 的总耗时（微秒）
uint64_t write_us_max = 0;          ///< 窗口量：单次 Write 的最长耗时（微秒）
uint64_t write_count = 0;           ///< 窗口量：链路 Write 的调用次数

// 累计量：写线程或 PostOutput() 写、任意线程可读，atomic 即够，不碰锁。
std::atomic<uint64_t> tx_31{0};     ///< 真正写进链路的 0x31（普通输出）份数
std::atomic<uint64_t> tx_36{0};     ///< 真正写进链路的 0x36（音频）份数
std::atomic<uint64_t> dropped{0};   ///< 队列满被丢弃的份数
#endif

// ---------------------------------------------------------------------------
// 队列的取
// ---------------------------------------------------------------------------

/**
 * @brief 取队首那份；锁内只搬运，链路写发生在锁外，不阻塞 `PostOutput()` 入队。
 *
 * @param out 取出时整份搬进这里（含定长槽），成功时有效。
 * @return 取到 true；队列空 false。
 */
bool TryPop(SendItem& out) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queue.empty()) return false;
  out = std::move(queue.front());
  queue.pop_front();
  return true;
}

// ---------------------------------------------------------------------------
// 唤醒 fd 与诊断计数
// ---------------------------------------------------------------------------

/**
 * @brief 把 wake fd 的累加计数读空。
 *
 * 不读干净的话下一次 poll 会立刻假醒，写线程就在"有活"的错觉里空转。
 * eventfd 的一次读就取走整个累计计数，EAGAIN 表示已经被读空。
 */
void DrainWakeFd() {
  const int fd = plane::WakeFd();
  if (fd < 0) return;
  uint64_t credits = 0;
  (void)::read(fd, &credits, sizeof(credits));
}

#if DS5DUMMY_DEBUG
/**
 * @brief 把一次链路 Write 的耗时记进窗口量。
 *
 * @param elapsed_us 这一次调用从发起到返回的微秒数。
 */
void RecordWrite(uint64_t elapsed_us) {
  std::lock_guard<std::mutex> lock(queue_mutex);
  write_us_total += elapsed_us;
  write_count++;
  if (elapsed_us > write_us_max) write_us_max = elapsed_us;
}

/**
 * @brief 记一份真正写进链路的报告，按 report id 分流。
 *
 * @param report_id 那份报告的 data[0]；0x36 之外都归 0x31（当前只有这两种
 *                  输出报告入队）。
 */
void CountReport(uint8_t report_id) {
  if (report_id == core::protocol::kBtAudioReportId) {
    tx_36.fetch_add(1, std::memory_order_relaxed);
  } else {
    tx_31.fetch_add(1, std::memory_order_relaxed);
  }
}
#endif

// ---------------------------------------------------------------------------
// 线程本体
// ---------------------------------------------------------------------------

/**
 * @brief 睡到有活或收工。
 *
 * 队列空时唯一的等待点：不看链路 fd 的 POLLOUT（理由见文件头），真的睡到
 * `PostOutput()` 踢 wake fd。收工不用在这里判断——本函数返回后外层循环会看
 * `plane::Running()`。
 */
void SleepForWork() {
  struct pollfd fds[2] = {
      {plane::WakeFd(), POLLIN, 0},
      {plane::StopFd(), POLLIN, 0},
  };
  const int result = ::poll(fds, 2, -1);
  if (result < 0 && errno != EINTR) {
    // 自己的 eventfd 都 poll 不动，这条线程再也不会接到活：判死退出，
    // 别留一个永不干活的写侧。
    DS5DUMMY_LOG(WARNING, "[bt] tx poll error: {}", strerror(errno));
    plane::ReportDeath("tx poll error");
    return;
  }
  if (result > 0 && (fds[0].revents & POLLIN)) DrainWakeFd();
}

/**
 * @brief 把手上这份报告发到链路，直到发出或收工。
 *
 * 两种结局：整份发出；收工——`poll` 看见 `stop_fd`，或运行标志已落定。收工时
 * 清单不必退回队列：`StopThreads()` 在 join 之后会清空它。写不进去的 `kAgain`
 * 不换手，原地接着等下一次 POLLOUT：这份的 seq/CRC 已经盖过，重发同一份字节
 * 是唯一无损的选择。
 *
 * @param item 要发的那份，内容在调用期间不变。
 */
void SendReport(const SendItem& item) {
  for (;;) {
    // 只等两件事：收工，和这条链路可写。唤醒 fd 不在这里——手上这份没出去
    // 之前，后面排的都排在它后面，叫醒写线程改变不了取哪一份。
    struct pollfd fds[2] = {
        {plane::StopFd(), POLLIN, 0},
        {detail::Fd(), POLLOUT, 0},
    };
    const int result = ::poll(fds, 2, -1);

    if (result < 0) {
      if (errno == EINTR) continue;  // 信号打断不算异常，重新等
      DS5DUMMY_LOG(WARNING, "[bt] tx poll error: {}", strerror(errno));
      plane::ReportDeath("tx poll error");
      return;
    }

    // 收工的两种表达：标志已经落定，或有人踢了 stop_fd——后者是前者的成因，
    // 但睡在 poll 里的这一轮只能靠 revents 看见。
    if (!plane::Running() || (fds[0].revents & POLLIN)) return;

    // 写侧 fd 报 HUP/ERR/NVAL：链路没了。hidraw 的 EPOLLERR 只在设备消失时
    // 置上，L2CAP 的在通道挂断时置上，都不是误报。
    if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
      DS5DUMMY_LOG(WARNING, "[bt] write channel poll revents=0x{:x}",
            static_cast<unsigned>(fds[1].revents));
      plane::ReportDeath("write channel hangup");
      return;
    }
    if (!(fds[1].revents & POLLOUT)) continue;  // 链路 fd 没可写事件：接着等

#if DS5DUMMY_DEBUG
    const auto start = Clock::now();
#endif
    const auto write_result = detail::Write(item.body.data(), item.len);
#if DS5DUMMY_DEBUG
    RecordWrite(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                              start)
            .count()));
#endif

    if (write_result == detail::kOk) {
#if DS5DUMMY_DEBUG
      CountReport(item.body[0]);
#endif
      return;
    }
    if (write_result == detail::kAgain) {
      // poll 说可写而写不进是竞态窗口（sndbuf 刚被别的写占掉）：接着等下一次
      // POLLOUT，不 sleep——睡才是把背压变成延迟。
      continue;
    }
    DS5DUMMY_LOG(WARNING,
          "[bt] send failed on 0x{:02X} report — link is down (result={})",
          item.body[0], static_cast<int>(write_result));
    plane::ReportDeath("write failed");
    return;
  }
}

/**
 * @brief 写线程主循环：取一份、发一份，队列空就睡等。
 */
void TxLoop() {
  while (plane::Running()) {
    // 槽位在栈上，每轮新建：TryPop 整份搬进来，发完即弃，线程间不共享缓冲。
    SendItem item;
    if (!TryPop(item)) {
      SleepForWork();
      continue;
    }
    // SendReport 只在收工时返回；结束循环交给上面的条件。
    SendReport(item);
  }
}

}  // namespace

namespace tx {

bool Start() {
  // 同 rx::Start()：自己判死退出过的线程句柄还 joinable，先回收再起新的。
  if (thread.joinable()) thread.join();
  try {
    thread = std::thread(TxLoop);
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[bt] cannot start tx thread: {}", error.what());
    return false;
  }
  return true;
}

void Join() {
  if (thread.joinable()) thread.join();
}

void Reset() {
  // 调用点在 bt::StartThreads() 里、两条线程起来之前，此刻并无竞争；仍然走
  // queue_mutex，是为了"这些字段一律在这把锁里改"这条规则没有例外——同一批
  // 字段在没有线程时也会被 PostOutput() 写（模块单测就是这么测队列的）。
  std::lock_guard<std::mutex> lock(queue_mutex);
  queue.clear();
  report_seq = 0;
#if DS5DUMMY_DEBUG
  queue_high_water = 0;
  write_us_total = 0;
  write_us_max = 0;
  write_count = 0;
  // 累计量也清零：窗口量与累计量必须来自同一段运行，否则下一轮的比值失真。
  tx_31 = 0;
  tx_36 = 0;
  dropped = 0;
#endif
}

void ClearQueue() {
  std::lock_guard<std::mutex> lock(queue_mutex);
  queue.clear();
}

#if DS5DUMMY_DEBUG
Stats TakeWindow() {
  Stats stats;
  {
    // 窗口量读走即清（见 bt.h 的警告）：清与读必须在同一把锁里，否则丢样本。
    std::lock_guard<std::mutex> lock(queue_mutex);
    stats.queue_high_water = queue_high_water;
    stats.write_us_total = write_us_total;
    stats.write_us_max = write_us_max;
    stats.write_count = write_count;
    queue_high_water = 0;
    write_us_total = 0;
    write_us_max = 0;
    write_count = 0;
  }
  // 累计量是 atomic，锁外读：拿到的是近似快照，不参与清零。
  stats.tx_31 = tx_31.load();
  stats.tx_36 = tx_36.load();
  stats.dropped = dropped.load();
  return stats;
}
#endif

}  // namespace tx

bool PostOutput(const uint8_t* data, size_t len) {
  // 长度先卡在 [kMinOutputBody, kMaxOutputBody]：短了 seq/CRC 两步互踩，
  // 长了定长槽装不下。
  if (data == nullptr || len < kMinOutputBody || len > kMaxOutputBody) {
    DS5DUMMY_LOG(ERROR, "[bt] PostOutput rejected: len={}", len);
    return false;
  }

  std::lock_guard<std::mutex> lock(queue_mutex);
  // 满则丢最新：入队方（audio 编码线程）不能在这里被阻塞；丢弃量只在诊断
  // 构建里记进 dropped。
  if (queue.size() >= kQueueMax) {
#if DS5DUMMY_DEBUG
    dropped.fetch_add(1, std::memory_order_relaxed);
#endif
    return false;
  }

  // 拷进自有槽而不是存调用方指针：调用方返回后栈缓冲就失效了。
  SendItem item;
  std::memcpy(item.body.data(), data, len);
  item.len = len;
  // byte[1] 整字节覆写：高 4 位 = seq（0..15 回卷），低 4 位被清成 0。调用方
  // 在这一个字节里写什么都不会被带上链路，所以两个生产方都把它留成全零。
  item.body[1] = (report_seq & 0x0F) << 4;
  // 加与与都在 int 里算（整型提升），转回 uint8_t 才存得进计数器。
  report_seq = static_cast<uint8_t>((report_seq + 1) & 0x0F);
  // 尾部 4 字节小端 CRC 在这里算：seq 盖完才算，CRC 才盖得住含 seq 的字节。
  core::protocol::FillOutputReportChecksum(item.body.data(), len);
  queue.push_back(std::move(item));
#if DS5DUMMY_DEBUG
  if (queue.size() > queue_high_water) {
    queue_high_water = queue.size();
  }
#endif

  // 唤醒失败不影响正确性：这一份已经在队里，下一次入队或读空后的取队首都会
  // 把它带上——踢 fd 只是把写线程从睡等里叫回来。
  const int wake_fd = plane::WakeFd();
  if (wake_fd >= 0) {
    const uint64_t one = 1;
    if (::write(wake_fd, &one, sizeof(one)) < 0) {
      DS5DUMMY_DLOG(1, "[bt] send wakeup write failed: {}", strerror(errno));
    }
  }
  return true;
}

}  // namespace bt
