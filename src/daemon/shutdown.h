/**
 * @file daemon/shutdown.h
 * @brief 进程退出信号与只读退出标志
 * @details SIGINT/SIGTERM 的 handler 只做一件事：把一个 volatile
 *          std::sig_atomic_t 置 1。真正的停机由轮询这个标志的主循环完成，它
 *          再去调用各模块的 Stop 接口——数据面线程不直接读标志，它们只是被
 *          EINTR 打断的阻塞调用里自然醒来。
 * @details 不负责：任何清理动作本身（各模块自己的 Stop/Close），也不负责
 *          信号安全的日志输出。
 * @note 层次：daemon。安装一次、全局生效；标志是进程级单例，读侧无锁。
 */

#pragma once

namespace ds5::daemon {

/**
 * @brief 安装 SIGINT/SIGTERM handler。
 * @note 必须从主线程调用一次；重复调用会覆盖同一份配置，无副作用差异。
 * @warning 不带 SA_RESTART：阻塞的系统调用必须被 EINTR 唤醒，否则 Ctrl-C 要
 *          等到下一次 IO 才生效。
 * @see ShutdownRequested。
 */
void InstallSignalHandlers();

/**
 * @brief 是否已经收到退出请求。
 * @return 收到 SIGINT 或 SIGTERM 后为 true，之后不会再变回 false。
 * @note 信号安全：读的是 volatile std::sig_atomic_t，可在任意线程轮询。
 * @see InstallSignalHandlers。
 */
bool ShutdownRequested();

}  // namespace ds5::daemon
