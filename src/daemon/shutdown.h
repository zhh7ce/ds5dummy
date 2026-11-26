/**
 * @file daemon/shutdown.h
 * @brief 进程退出信号、控制 socket 请求与只读退出标志
 * @details SIGINT/SIGTERM handler 和控制 socket 的 `abort` 只做一件事：把进程级
 *          状态从"运行"改成"正常退出"或"异常退出"。真正的停机由轮询这个状态的
 *          主循环完成，它再去调用各模块的 Stop 接口——数据面线程不直接读状态，
 *          它们只是在被 EINTR 打断的阻塞调用里自然醒来。
 * @details 正常退出返回 0（Ctrl-C / SIGTERM）；异常退出返回 -1（控制 socket 的
 *          `abort`），systemd 的 `Restart=on-failure` 会把它当成重启请求。两者
 *          共用同一条收摊流程，只有退出码不同。
 * @details 不负责：任何清理动作本身（各模块自己的 Stop/Close），也不负责
 *          信号安全的日志输出。
 * @note 层次：daemon。安装一次、全局生效；状态是进程级单例，写侧先到先得，
 *       读侧无锁。
 */

#pragma once

namespace ds5::daemon {

/**
 * @brief 安装 SIGINT/SIGTERM handler。
 * @note 必须从主线程调用一次；重复调用会覆盖同一份配置，无副作用差异。
 * @warning 不带 SA_RESTART：阻塞的系统调用必须被 EINTR 唤醒，否则 Ctrl-C 要
 *          等到下一次 IO 才生效。
 * @see ShutdownRequested, ShutdownExitCode。
 */
void InstallSignalHandlers();

/**
 * @brief 是否已经收到退出请求（信号或控制 socket 的 `abort`）。
 * @return 收到后为 true，之后不会再变回 false。
 * @note 信号安全：读的是 lock-free atomic，可在任意线程轮询。
 * @see InstallSignalHandlers, RequestAbnormalExit。
 */
bool ShutdownRequested();

/**
 * @brief 请求正常退出：与 SIGINT/SIGTERM 等价，退出码为 0。
 * @note 信号安全；若已有退出请求则什么都不做（先到先得）。
 */
void RequestShutdown();

/**
 * @brief 请求异常退出：收摊流程不变，但 Manager 最终以 -1 返回。
 * @note 供控制 socket 的 `abort` 命令调用；信号安全，先到先得。
 * @see ShutdownExitCode。
 */
void RequestAbnormalExit();

/**
 * @brief 本次退出的进程返回码。
 * @return 未请求退出或正常退出为 0；已请求异常退出为 -1。
 * @note 在收摊结束后读，主循环用它作为 RunManager 的返回值。
 */
int ShutdownExitCode();

#ifdef DS5DUMMY_TESTING
/// @brief 单测用：回到"还没有退出请求"，让每条用例从同一状态开始。
void ResetForTesting();
#endif

}  // namespace ds5::daemon
