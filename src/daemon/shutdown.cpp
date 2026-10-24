/**
 * @file daemon/shutdown.cpp
 * @brief 退出信号实现
 * @details handler 只写一个标志位，这是 async-signal-safe 的唯一形式；所有
 *          实际停机动作留在主循环里做。
 * @note 层次：daemon。g_shutdown_requested 是文件级私有的进程单例。
 */

#include "daemon/shutdown.h"

#include <csignal>

namespace ds5::daemon {
namespace {

/// @brief 退出请求标志：1 = 已收到 SIGINT/SIGTERM，0 = 正常运行。
/// @details volatile std::sig_atomic_t 是 C/C++ 里唯一保证跨 handler/主程序
///          可见且不会被编译器搬动的标量类型；写成普通 bool 是 UB。
volatile std::sig_atomic_t g_shutdown_requested = 0;

/**
 * @brief SIGINT/SIGTERM 的 handler：置标志后立即返回。
 * @note 入参（信号编号）未使用：两个信号的处理完全相同。
 * @note async-signal-safe：不打日志、不加锁、不分配内存。
 */
void HandleShutdownSignal(int) { g_shutdown_requested = 1; }

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
 * @brief 读退出标志。
 * @return 已收到退出信号为 true；否则 false。
 * @note 纯读，无副作用，任意线程任意时刻可调（含轮询循环里）。
 */
bool ShutdownRequested() { return g_shutdown_requested != 0; }

}  // namespace ds5::daemon
