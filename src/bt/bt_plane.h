/**
 * @file
 * @brief 两条传输线程共享的运行状态，加上两条线程各自的入口
 *
 * 只在 `src/bt/` 内部使用：`bt_link.cpp` 用这里的入口把线程拉起来，
 * `bt_rx.cpp`、`bt_tx.cpp` 是这两条线程的本体。上层模块只看 `bt.h`。
 *
 * 分成三段是因为职责不同：
 * - **`bt::plane`**：只有一条传输线程时代不存在的东西——两个方向共用的
 *   运行标志、两个 eventfd、回调表、判死入口和读侧计数（仅
 *   `DS5DUMMY_DEBUG` 构建）。谁都不拥有谁，所以收在这个共享命名空间里，
 *   由 `bt_link.cpp` 建、由它销。
 * - **`bt::rx`**：读线程的起停。链路 fd 的 POLLIN 只跟它有关。
 * - **`bt::tx`**：写线程的起停，外加它拥有的发送队列与写侧计数。
 *
 * @note 并发契约：`Running()` 是 atomic 读，任意线程可调；`RequestStop()`
 *       与 `ReportDeath()` 允许两条线程并发调用，不加锁也不去重——判死之后
 *       进程就要退出，重复通知由回调自己幂等消化（见 `bt::Handlers`）。
 * @see bt.h —— 上层看到的门面；bt_detail.h —— 后端接缝。
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "bt/bt.h"
#include "core/debug.h"

namespace bt::plane {

/**
 * @brief 数据平面是否在跑（线程该不该继续工作）。
 *
 * 读的是 atomic，任意线程任何时候调用都安全。两条线程的主循环都用它作
 * 继续条件。
 */
bool Running();

/**
 * @brief 建起数据平面：两个 eventfd、存下回调、置运行标志。
 *
 * 顺序是刻意的：运行标志最后置位，所以 eventfd 建失败时不会有线程以为自己
 * 该跑。失败时已建的 fd 就地回收，不留半个平面。
 *
 * @param handlers 按值存进单实例，生命周期跟着数据平面走。
 * @return 两个 eventfd 都建成 true；任一 `eventfd()` 失败 false（已回收）。
 * @note 只由 `bt::StartThreads()` 在 worker 主线程调用，且必须早于
 *       `rx::Start()`/`tx::Start()`——两条线程都要 poll `StopFd()`。
 * @see Finish() —— 逆操作。
 */
bool Start(const bt::Handlers& handlers);

/**
 * @brief 关掉两个 eventfd 并把运行标志落回 false；幂等。
 *
 * @warning 只在两条线程都已经 join 之后调用——它们还在 poll 这两个 fd。
 */
void Finish();

/**
 * @brief 请求收工：置 `running=false` 并踢一脚 `StopFd()`。
 *
 * 幂等、可并发、不阻塞（eventfd 是 NONBLOCK 的，计数写满也无害）。两条
 * 线程都可能在 poll 里睡着，这一脚是唯一能把它们同时叫回来的东西。
 *
 * @see ReportDeath() —— 判死路径，包含本函数。
 */
void RequestStop();

/**
 * @brief 判死：收工 + 通知 `on_disconnected`。
 *
 * 不加锁、不去重：读写两侧各自发现链路没了都会走到这里，可能各调一次
 * 回调。这是门面契约而不是疏忽——判死的后果是整个进程退出，谁先发现都
 * 一样，去重反而要让一侧等另一侧。
 *
 * @param why 判死判据的短句，进日志用；调用方负责先把自己的数值细节
 *            （result、revents）打成一条 WARNING。
 */
void ReportDeath(const char* why);

/**
 * @brief 把一份输入报告交给 `handlers.on_input`；回调没装时静默丢弃。
 *
 * @param data 裸报告体，data[0] 是 report id；只在调用线程（读线程）期间有效。
 * @param len  @p data 的字节数。
 */
void NotifyInput(const uint8_t* data, size_t len);

/// 收工用的 eventfd；`Start()` 之前、`Finish()` 之后是 -1。
int StopFd();
/// 发送队列的"有活了"通知 fd；写侧 `PostOutput()` 踢，写线程读空。
int WakeFd();

#if DS5DUMMY_DEBUG
/// 记一次成功的读侧投递（进 `bt::Stats::rx_reports`，累计不清）。
void CountRx();
/// 记一次报告体被缓冲区容量截断的读（进 `bt::Stats::truncated`，累计不清）。
void CountTruncated();
/// 读走的累计值，不清零。
uint64_t RxReports();
/// 读走的累计值，不清零。
uint64_t RxTruncated();
#endif

}  // namespace bt::plane

namespace bt::rx {

/**
 * @brief 起读线程。
 *
 * @return 线程起来 true；`std::thread` 创建失败 false。
 * @note 已经在跑的线程不在这里的拒绝范围：句柄 joinable 时先 join 再起新的，
 *       靠这一点回收"自己因判死退出"留下的死句柄。重复启动由
 *       `bt::StartThreads()` 的 `plane::Running()` 闸门挡在外面，它不会把
 *       一条活线程 join 成死等。
 * @see Join() —— 逆操作；`bt::StartThreads()` 是唯一的调用方。
 */
bool Start();

/**
 * @brief join 读线程并丢掉句柄；没起过或已经自己退出都无害。
 *
 * 无条件可调用：线程可能因为判死自己退出过，句柄仍 joinable，必须回收。
 */
void Join();

}  // namespace bt::rx

namespace bt::tx {

/**
 * @brief 起写线程；语义同 `rx::Start()`（含 joinable 先回收那一条）。
 *
 * @return 线程起来 true；`std::thread` 创建失败 false。
 */
bool Start();

/**
 * @brief join 写线程并丢掉句柄；语义同 `rx::Join()`。
 */
void Join();

/**
 * @brief 把写侧的单实例状态归零：队列、seq，以及诊断计数（仅
 * `DS5DUMMY_DEBUG` 构建）。
 *
 * @note 只在线程没跑时调用（`bt::StartThreads()` 在建 fd 之前）。
 */
void Reset();

/**
 * @brief 清空队列，保留全部计数。
 *
 * @warning 只在写线程 join 之后调用：此时才没有并发消费者会碰这些槽。
 */
void ClearQueue();

#if DS5DUMMY_DEBUG
/**
 * @brief 读走写侧计数并清掉窗口量；读侧两项由 `bt::StatsWindow()` 补齐。
 *
 * @return 填好 `tx_31`/`tx_36`/`dropped` 与全部窗口量的 `bt::Stats`。
 */
bt::Stats TakeWindow();
#endif

}  // namespace bt::tx
