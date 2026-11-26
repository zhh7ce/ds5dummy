/**
 * @file daemon/shutdown.cpp
 * @brief 退出状态实现：一个 lock-free atomic，信号 handler 与 socket 命令共用
 *
 * 原来的 volatile sig_atomic_t 只能表达"要不要退"，现在还要表达"以什么码退"。
 * 用一个 lock-free 的 std::atomic<int> 同时承载两件事：0 运行、1 正常退出、
 * 2 异常退出。C++ 标准允许信号 handler 访问 lock-free atomic，所以 handler
 * 仍然只做一次 CAS，不打日志、不加锁、不分配内存。
 *
 * 写入先到先得：先收到信号就以 0 退，先收到 socket 的 abort 就以 -1 退。
 * 所有实际停机动作仍留在主循环里做。
 */

#include "daemon/shutdown.h"

#include <atomic>
#include <csignal>

namespace ds5::daemon {
namespace {

constexpr int kRunning = 0;   ///< 还没有退出请求。
constexpr int kNormal = 1;    ///< 信号或 RequestShutdown：退出码 0。
constexpr int kAbnormal = 2;  ///< 控制 socket 的 abort：退出码 -1。

static_assert(std::atomic<int>::is_always_lock_free,
              "the shutdown state must be lock-free for signal-handler use");

std::atomic<int> g_shutdown_state{kRunning};

/**
 * @brief 把状态从"运行"改成请求的原因；已有请求时保持原样。
 * @note async-signal-safe：一次 lock-free CAS，无日志、无锁、无分配。
 */
void RequestExit(int reason) {
  int expected = kRunning;
  g_shutdown_state.compare_exchange_strong(expected, reason,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed);
}

/**
 * @brief SIGINT/SIGTERM 的 handler：请求正常退出后立即返回。
 * @note 入参（信号编号）未使用：两个信号的处理完全相同。
 */
void HandleShutdownSignal(int) { RequestExit(kNormal); }

}  // namespace

/**
 * @brief 把 HandleShutdownSignal 装到 SIGINT 和 SIGTERM 上。
 * @note sigaction 的返回值未检查：这两个标准信号的安装只在 EINVAL（非法信号
 *       编号）下失败，而编号是常量，不构成实际失败面。
 * @see ShutdownRequested。
 */
void InstallSignalHandlers() {
  struct sigaction sa{};  // 零初始化：sa_mask 空、sa_restorer 空，只留 handler
  sa.sa_handler = HandleShutdownSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // 不带 SA_RESTART：阻塞的系统调用必须被 EINTR 唤醒。
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
}

/**
 * @brief 读退出状态。
 * @return 已收到任一退出请求为 true；否则 false。
 * @note 纯读，无副作用，任意线程任意时刻可调（含轮询循环里）。
 */
bool ShutdownRequested() {
  return g_shutdown_state.load(std::memory_order_relaxed) != kRunning;
}

void RequestShutdown() { RequestExit(kNormal); }

void RequestAbnormalExit() { RequestExit(kAbnormal); }

int ShutdownExitCode() {
  return g_shutdown_state.load(std::memory_order_relaxed) == kAbnormal ? -1 : 0;
}

#ifdef DS5DUMMY_TESTING
void ResetForTesting() { g_shutdown_state.store(kRunning, std::memory_order_relaxed); }
#endif

}  // namespace ds5::daemon
