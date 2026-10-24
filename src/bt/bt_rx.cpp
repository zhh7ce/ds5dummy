/**
 * @file
 * @brief 读线程：POLLIN 驱动，一次醒来把链路上的报告读到空为止
 *
 * 它只做读：poll(链路 fd, 收工 fd) → 可读就把当前排着的报告一份接一份取走，
 * 交给 `plane::NotifyInput` → 读空了回 poll。写侧完全不在这里，发送队列的
 * 背压、阻塞和重试都与这条线程无关——这是把读写拆成两条线程要买的东西：
 * 一份报告进 bridge 的延迟不再取决于前面积压了多少份要发。
 *
 * 判死有三条，任一成立就 `plane::ReportDeath` 后退出：
 * 1. `detail::Read` 交回既不是 kOk 也不是 kAgain/kTruncated 的结果；
 * 2. 链路 fd 报 POLLHUP/POLLERR/POLLNVAL；
 * 3. 连续 5 秒没有任何输入（满 1 秒先打一条 WARNING）。
 * 第三条兜的是"fd 还在、链路已经不送数据"：手柄连上后持续推输入报告，
 * 静默就是死了。这条看门狗只活在读线程，写线程因此可以真的睡等。
 *
 * @note 缓冲容量 `kMaxInputBody` 比最长的已知输入报告（0x31 的 78 字节）大一
 *       截，装不下的那份后端会交回 kTruncated：宁可丢这一份，也不把半份
 *       报告交给 bridge 去按字段解析。
 * @see bt_plane.h —— 运行标志、收工 fd 与回调表；bt_detail.h §A —— 读原语。
 */

#include "bt/bt.h"

#include <poll.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <thread>

#include "bt/bt_detail.h"
#include "bt/bt_plane.h"
#include "core/debug.h"
#include "core/protocol/report.h"
#include "loguru.hpp"

namespace bt {
namespace {

/// poll 的 tick：不为吞吐，只为两件事——看见 running 落定、给静默看门狗一个
/// 检查点。输入本身靠 POLLIN 事件驱动，拆线程后不再需要 10ms 那种紧 tick。
constexpr int kRxTickMs = 100;

/// 一次一份输入报告体的容量。当前最长的输入报告是 0x31 的 78 字节，留足
/// 余量给报告 id 之外的扩展；比它长就交回 kTruncated（见文件头注记）。
constexpr size_t kMaxInputBody = 128;
static_assert(kMaxInputBody >= core::protocol::kBtReportSize,
              "rx buffer must hold the longest known input report");

/// 静默多久提醒一次、多久判死。相对最近一次成功读的时刻。
constexpr auto kSilenceWarn = std::chrono::seconds(1);
constexpr auto kSilenceDeath = std::chrono::seconds(5);

/// 本线程的单调时钟：所有超时与静默判定都相对 steady_clock 的读数算，
/// 不受系统时钟跳变影响。
using Clock = std::chrono::steady_clock;

/// 本线程的句柄。只由 bt::StartThreads()/StopThreads() 经 rx:: 接口碰。
std::thread thread;

/**
 * @brief 把链路当前可读的报告一份接一份取走，直到读空。
 *
 * 循环到 kAgain 为止是拆线程后才敢做的事：一次 poll 醒来把内核队列读空，
 * 每份报告都当场交给 bridge，不再"一个 tick 取一份"。
 *
 * @param buffer        复用的读缓冲，容量即截断判定线。
 * @param last_rx       最近一次成功读的时刻，本函数负责更新。
 * @param warned_silent 静默 WARNING 的武装标志，读到新报告后重新武装。
 * @return true 链路还能读（含读空与截断）；false 读侧已判死，调用方退出。
 */
bool DrainInputs(std::array<uint8_t, kMaxInputBody>& buffer,
                 Clock::time_point& last_rx, bool& warned_silent) {
  for (;;) {
    size_t read_size = 0;
    const auto result = detail::Read(buffer.data(), buffer.size(), &read_size);
    if (result == detail::kOk) {
      last_rx = Clock::now();
      warned_silent = false;
#if DS5DUMMY_DEBUG
      plane::CountRx();
#endif
      plane::NotifyInput(buffer.data(), read_size);
      continue;
    }
    if (result == detail::kAgain) {
      // poll 说可读而读空是竞态窗口，不是断链：回 poll 等下一次真可读。
      return true;
    }
    if (result == detail::kTruncated) {
      // 报告比缓冲长：这一份不完整，丢掉（诊断构建下再计一笔），链路照常可用。
      // 回 poll 而不是就地续读，是为了让这条 WARNING 的频率跟着 POLLIN 走，
      // 不会刷屏。
#if DS5DUMMY_DEBUG
      plane::CountTruncated();
#endif
      DS5DUMMY_LOG(WARNING, "[bt] input report truncated at {} B — dropped",
            buffer.size());
      return true;
    }
    DS5DUMMY_LOG(WARNING, "[bt] interrupt channel lost (result={})",
          static_cast<int>(result));
    plane::ReportDeath("read failed");
    return false;
  }
}

/**
 * @brief 静默看门狗：满 1 秒提醒一次，满 5 秒判死。
 *
 * @param last_rx         最近一次成功读的时刻。
 * @param warned_silent   WARNING 只打一次，读到新输入后重新武装。
 * @return true 还可以等；false 已判死，调用方退出。
 */
bool CheckSilence(Clock::time_point last_rx, bool& warned_silent) {
  const auto silent_for = Clock::now() - last_rx;
  if (silent_for >= kSilenceWarn && !warned_silent) {
    DS5DUMMY_LOG(WARNING, "[bt] interrupt channel silent for 1s");
    warned_silent = true;
  }
  if (silent_for >= kSilenceDeath) {
    DS5DUMMY_LOG(WARNING,
          "[bt] interrupt channel silent for 5s; link considered lost");
    plane::ReportDeath("interrupt channel silent for 5s");
    return false;
  }
  return true;
}

/**
 * @brief 读线程主循环。
 *
 * 每轮：poll 两个 fd → 链路可读就把排着的报告读空 → 链路 fd 报
 * HUP/ERR/NVAL 判死 → 静默检查。收工不做判死：`plane::Running()` 变 false
 * 时正常退出，不回调 on_disconnected。
 */
void RxLoop() {
  std::array<uint8_t, kMaxInputBody> buffer{};
  bool warned_silent = false;
  auto last_rx = Clock::now();

  while (plane::Running()) {
    // fd 每轮现取：后端链路换 fd（重建）后这条线程不用重启。
    struct pollfd fds[2] = {
        {detail::Fd(), POLLIN, 0},
        {plane::StopFd(), POLLIN, 0},
    };
    const int result = ::poll(fds, 2, kRxTickMs);

    if (result < 0) {
      if (errno == EINTR) continue;  // 信号打断不算异常，重新 poll
      DS5DUMMY_LOG(WARNING, "[bt] rx poll error: {}", strerror(errno));
      plane::ReportDeath("rx poll error");
      return;
    }

    if (result > 0 && (fds[0].revents & POLLIN)) {
      if (!DrainInputs(buffer, last_rx, warned_silent)) return;
    }

    // 挂断/错误/失效都算链路没了——POLLNVAL 意味着 fd 已经被后端关掉。
    if (result > 0 && (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))) {
      DS5DUMMY_LOG(WARNING, "[bt] interrupt channel poll revents=0x{:x}",
            static_cast<unsigned>(fds[0].revents));
      plane::ReportDeath("interrupt channel hangup");
      return;
    }

    if (!CheckSilence(last_rx, warned_silent)) return;
  }
}

}  // namespace

namespace rx {

bool Start() {
  // 上一轮线程若因判死自己退出过，running 已是 false 但句柄还 joinable，
  // 先回收——否则这里会拒绝一次本可以成功的启动。
  if (thread.joinable()) thread.join();
  try {
    thread = std::thread(RxLoop);
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[bt] cannot start rx thread: {}", error.what());
    return false;
  }
  return true;
}

void Join() {
  if (thread.joinable()) thread.join();
}

}  // namespace rx

}  // namespace bt
