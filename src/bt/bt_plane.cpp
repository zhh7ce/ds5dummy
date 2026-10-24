/**
 * @file
 * @brief 数据平面的本体：运行标志、两个 eventfd、回调表、判死与读侧计数
 *
 * 全部状态收在 `Plane()` 那一个进程级 static 结构里，对外只有
 * `bt::plane` 里那几个函数。这里不起线程，也不 include 任何后端符号：
 * 链路 fd 每轮由两条线程自己经 `detail::Fd()` 现取，平面只管"该不该继续跑"
 * 和"跑的时候读了多少"。
 *
 * 锁：一把都没有。运行标志（以及诊断构建里的两个计数）是 atomic；两个 fd
 * 的建立与关闭都发生在没有线程在跑的时刻（`Start()` 早于起线程，`Finish()`
 * 晚于 join）；`RequestStop()` 与 `ReportDeath()` 允许两条线程并发进来，靠的
 * 是 eventfd 写本身原子、且重复通知已经被回调的幂等契约吃掉。
 *
 * @see bt_plane.h —— 这里承诺给模块内部的接口与并发契约。
 */

#include "bt/bt_plane.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>

#include "core/debug.h"
#include "loguru.hpp"

namespace {

/// 数据平面共用的全部状态。
struct Plane {
  std::atomic<bool> running{false};
#if DS5DUMMY_DEBUG
  std::atomic<uint64_t> rx_reports{0};
  std::atomic<uint64_t> rx_truncated{0};
#endif

  int stop_fd = -1;  ///< 收工通知：两条线程都 poll 它，只有 RequestStop() 写。
  int wake_fd = -1;  ///< 队列有活通知：PostOutput() 写，只有写线程读。

  /// 只在 Start() 置 running 之前写、join 之后随对象一起废弃，读侧无须加锁。
  bt::Handlers handlers;
};

/// 平面的唯一实例：一个 worker 进程只服务一只手柄，所以是函数内 static 而不是
/// 类对象；fd 不在析构里关，只由 Finish() 关（见 bt_plane.h 的调用时机）。
Plane& Get() {
  static Plane plane;
  return plane;
}

/**
 * @brief 建一个 EFD_NONBLOCK|EFD_CLOEXEC 的 eventfd。
 *
 * NONBLOCK 让踢唤醒永不阻塞；CLOEXEC 防它漏进本进程 exec 出的子进程
 * （如 BlueZ 的 D-Bus 调用）。
 *
 * @param name 失败日志里点名是哪个 fd，便于归因。
 * @return 建成的 fd；失败 -1 并打 ERROR。
 */
int CreateEventfd(const char* name) {
  const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0) {
    DS5DUMMY_LOG(ERROR, "[bt] plane: eventfd(%s) failed: {}", name, strerror(errno));
  }
  return fd;
}

/**
 * @brief 往唤醒 fd 写一个计数；写不出去追。
 *
 * NONBLOCK 的 eventfd 只在计数溢出到 U64_MAX 时返回 EAGAIN，那种情况下
 * poll 早就常醒了，丢掉这一脚不影响正确性。
 *
 * @param fd 目标唤醒 fd，负数直接当"没人可叫"忽略。
 */
void Kick(int fd) {
  if (fd < 0) return;
  // eventfd 的计数宽度是 8 字节，写不满内核不会让 poll 醒。
  const uint64_t value = 1;
  if (::write(fd, &value, sizeof(value)) < 0) {
    DS5DUMMY_DLOG(1, "[bt] plane: wakeup write failed: {}", strerror(errno));
  }
}

}  // namespace

namespace bt::plane {

bool Running() { return Get().running.load(); }

bool Start(const bt::Handlers& handlers) {
  Plane& plane = Get();
  plane.handlers = handlers;

  // 先建 fd 再置标志：置了标志却没有 stop_fd，两条线程就没有收工通知可等。
  plane.stop_fd = CreateEventfd("stop");
  if (plane.stop_fd < 0) {
    plane.handlers = bt::Handlers{};  // 失败不留半个平面：回调表跟着退回空
    return false;
  }
  plane.wake_fd = CreateEventfd("wake");
  if (plane.wake_fd < 0) {
    // stop_fd 已经建好，这里不关就漏一个 fd 给下一轮 Start()。
    ::close(plane.stop_fd);
    plane.stop_fd = -1;
    plane.handlers = bt::Handlers{};
    return false;
  }

#if DS5DUMMY_DEBUG
  // 读侧累计量也归零：写侧的在 tx::Reset() 里清，两侧合起来才是"本次平面
  // 生命周期内"的数，否则下一轮 StartThreads 会读到上一轮的死数。
  plane.rx_reports = 0;
  plane.rx_truncated = 0;
#endif
  plane.running = true;  // 最后置位：两条线程都以它作继续条件
  return true;
}

void Finish() {
  Plane& plane = Get();
  plane.running = false;
  // 走到这里两条线程都已 join，这两个 fd 不再有 poll 者，可以安全关闭。
  if (plane.stop_fd >= 0) {
    ::close(plane.stop_fd);
    plane.stop_fd = -1;
  }
  if (plane.wake_fd >= 0) {
    ::close(plane.wake_fd);
    plane.wake_fd = -1;
  }
  // 回调表跟着清空：链路已经没了，留着只会被下一轮之前的调用方误用。
  plane.handlers = bt::Handlers{};
}

void RequestStop() {
  Plane& plane = Get();
  plane.running = false;
  // 两条线程都可能睡在 poll 里（写线程队列空时是 -1 无限等），这一脚是唯一的
  // 叫醒手段；重复写无害，eventfd 只是累加计数。
  Kick(plane.stop_fd);
}

void ReportDeath(const char* why) {
  Plane& plane = Get();
  DS5DUMMY_LOG(WARNING, "[bt] link declared dead: {}", why);
  RequestStop();
  if (plane.handlers.on_disconnected) {
    plane.handlers.on_disconnected();
  }
}

void NotifyInput(const uint8_t* data, size_t len) {
  const auto& on_input = Get().handlers.on_input;
  if (on_input) on_input(data, len);
}

int StopFd() { return Get().stop_fd; }

int WakeFd() { return Get().wake_fd; }

#if DS5DUMMY_DEBUG
void CountRx() { Get().rx_reports.fetch_add(1, std::memory_order_relaxed); }

void CountTruncated() {
  Get().rx_truncated.fetch_add(1, std::memory_order_relaxed);
}

uint64_t RxReports() { return Get().rx_reports.load(); }

uint64_t RxTruncated() { return Get().rx_truncated.load(); }
#endif

}  // namespace bt::plane

#if DS5DUMMY_DEBUG
// StatsWindow() 是门面唯一的汇总点：读侧两项在 plane，写侧全部在 tx。
// 放这里而不是 bt_link.cpp，是因为它读的就是本文件的两个累计量。
namespace bt {

Stats StatsWindow() {
  Stats stats = tx::TakeWindow();
  stats.rx_reports = plane::RxReports();
  stats.truncated = plane::RxTruncated();
  return stats;
}

}  // namespace bt
#endif
