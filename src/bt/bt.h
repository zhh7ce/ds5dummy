/**
 * @file
 * @brief bt 模块门面：向其它模块暴露"到一只 DualSense 的蓝牙链路"
 *
 * 后端在编译期二选一（见 CMake 的 BT_BACKEND）：
 * - L2CAP：裸 AF_BLUETOOTH/L2CAP，自己走 HIDP 组帧，BlueZ 只管 ACL 和配对；
 * - HIDRAW：直接借 BlueZ 内核 hidraw 节点，收发都是裸报告体。
 * 后端实现分别在 bt_l2cap.cpp、bt_hidraw.cpp；本头文件只声明上层真正需要的
 * 生命周期与收发入口，内部原语（读、取 fd、feature 缓存）在 bt_detail.h。
 *
 * 本层的实现分四处：链路生命周期、feature 缓存策略、直发口与两条线程的起停在
 * bt_link.cpp；两条线程共享的运行状态在 bt_plane.cpp；读线程本体在 bt_rx.cpp，
 * 写线程与发送队列在 bt_tx.cpp。这四份都只通过 bt_detail.h 的 §A 接缝碰后端，
 * 所以后端之间那些"先关后开、缓存清零"的约定只需写一遍。
 *
 * 边界：HIDP framing 字节（0xA1/0xA2）不出模块。本层之上看到的读结果，
 * 第一个字节是输入报告 report id；写入时第一个字节是输出报告 report id。
 * L2CAP 后端在 Read()/Write() 内部剥、补这个字节；hidraw 后端由内核代劳。
 *
 * @note 线程模型：进程级单实例，一个 worker 进程服务一只手柄。
 *       Open()/Close()/StartThreads()/StopThreads() 只由 worker 主线程调用；
 *       GetFeature() 由 usb 的 ep0 线程调用（缓存未命中时会同步等一次链路
 *       往返）；SetFeature() 同样同步写控制通道，与 GetFeature 由一把锁串行
 *       化，因此调用方线程会被一次写占用；
 *       PostOutput() 非阻塞，任何线程可调用，Handlers 的两个回调则
 *       在传输线程上执行。链路就绪后需要的 0x32 唤醒报告由 L2CAP 后端在
 *       Open() 内部自己补发，门面不提供绕过队列的直发口。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "core/debug.h"
#include "core/protocol/report.h"

namespace bt {


/**
 * @brief 建立到 @p mac 那只手柄的链路，不留上一次建链的残骸。
 *
 * 本函数在建链前先 Close() 并清掉 feature 缓存，保证不带着上一次的残骸开新
 * 链路，所以后端自己的 Open() 只做建链那部分事（见 bt_detail.h §A）；建链
 * 成功后再预取一批 feature 报告填缓存。两条后端走的是同一套流程：
 * - L2CAP：让 BlueZ 把 ACL 拉起来 → 释放 HID profile → 抢 PSM 0x11（控制）
 *   和 0x13（中断）两条通道 → 切到 report protocol → 补发一份 0x32 唤醒
 *   报告（裸 L2CAP 上没有别的 HID 主机跟手柄说话，不推这一份它不会开始
 *   上报输入）；
 * - hidraw：扫 /sys/class/hidraw，按 VID/PID（真 Sony 或本项目 DKMS 合成的
 *   那组）加可选 MAC 认出节点并打开；内核驱动已在建节点时初始化过手柄，
 *   没有唤醒这一步。
 *
 * @param mac 经典蓝牙 BD_ADDR，形如 "AA:BB:CC:DD:EE:FF"；
 *            hidraw 后端允许空串，表示匹配任意一只 DualSense。
 * @return 成功 true；失败 false，且不留半开的链路，可以直接重试。
 * @note 只在 worker 主线程调用；内部同步走 D-Bus/ioctl，可能阻塞秒级
 *        （L2CAP 建链含 ACL 轮询与 settle 等待）。
 * @see Close() —— 逆操作。
 */
bool Open(const std::string& mac);

/**
 * @brief 关闭链路并清掉 feature 缓存；未打开时调用无害（幂等）。
 *
 * 缓存必须跟着链路作废：旧手柄的报告字节对新链路没有任何意义。
 * L2CAP 后端额外把 HID profile 还给 BlueZ，只对 Open() 成功记下的那个
 * MAC 做一次，避免 BlueZ 重新认领一只并没有连着手柄的旧设备。
 *
 * @note 只在 worker 主线程调用，且发生在其它线程 join 之后。
 */
void Close();

/**
 * @brief 取一份 feature 报告；buf[0] 进、出都是 report id。
 *
 * 先查进程内 feature 缓存，没命中才去链路现取（L2CAP 走控制通道的
 * GET_REPORT，hidraw 走 HIDIOCGFEATURE ioctl），取到写回缓存——同一份
 * 报告在线上只跑一次。现取先进模块自己的整份缓冲再按 @p len 交给你，所以
 * 缓存里存的永远是完整一份，不会因为某次调用方给的缓冲区偏小而留下截短
 * 的条目。
 *
 * @param buf buf[0] 是目标 report id；成功时被覆写成完整报告（首字节仍是 id）。
 * @param len buf 的容量，决定这次交回多少字节；向链路索要的长度不受它限制。
 * @return 写回的字节数（>= 1）；链路没开、参数不合法（len < 2）或线上
 *         拿不到回复时 -1。
 * @note 缓存未命中时会在调用线程上同步等回复：L2CAP 的整段预算是 2s，
 *       hidraw 的 ioctl 由内核决定何时返回。调用方（ep0 线程）要接受这个阻塞。
 * @see bt::Open() —— 建链成功后批量预取常用的几份，走的是同一份缓存。
 */
int GetFeature(uint8_t* buf, size_t len);

/**
 * @brief 往手柄写一份 feature 报告（控制通道 SET_REPORT），不等应答。
 *
 * 与 PostOutput 的区别是走的通道和帧格式：这里进控制通道、用 HIDP
 * SET_REPORT 的 `[0x53]` 事务头，不是中断通道上的普通输出报告，也不经过
 * 发送队列。@p data 必须已经是完整线上形态（首字节 report id + 尾部 CRC），
 * 本层不补帧、不算 CRC、不改任何字节。
 *
 * @param data 完整报告，data[0] 是 report id。
 * @param len  @p data 的字节数，必须 >= 2。
 * @return 已整份发出 true；链路没开、参数不合法或写失败 false。
 * @note 同步写控制通道，可能短暂阻塞调用线程。与 ep0 线程上现取的
 *       GetFeature 由模块内一把互斥锁串行化，两份请求不会在控制通道上交错。
 *       手柄不回应答，返回 true 只代表"已交给链路"。
 * @see bridge::PowerOff() —— 用它把冻结的 0x08 报告发给手柄，让它自己关机。
 */
bool SetFeature(const uint8_t* data, size_t len);

// ---------------------------------------------------------------------------
// 传输线程与输出队列
// ---------------------------------------------------------------------------

/// 发送队列里一份报告体的长度上限：取最长的真实输出报告，0x36 音频报告
/// （398 字节），其余报告（0x31 的 78 字节）天然装得下。
constexpr size_t kMaxOutputBody = core::protocol::kBtAudioReportSize;
/// 长度下限：report id（1）+ seq 槽（1）+ 尾部 CRC（4）。短于此，入队点
/// 盖 seq 和算 CRC 的两步会互相踩到对方的字节。
constexpr size_t kMinOutputBody = 6;
/// 队列深度上限。满了丢刚来的那一份（不排队、不覆盖已在队的）：这里是
/// 背压的终点，宁丢最新也不让链路 backlog 无限增长。
constexpr size_t kQueueMax = 8;

/**
 * @brief 传输线程的回调集合，由 worker 组装后交给 StartThreads()。
 *
 * @note on_input 在读线程上执行；on_disconnected 可能在读或写任一线程上、
 *       甚至两侧各一次。实现方要自己保证所调用内容的线程安全。
 */
struct Handlers {
  /// 一份输入报告的去处：裸 DualSense 报告体，data[0] 是 report id，
  /// 不含 HIDP framing；指针只在回调期间有效。
  std::function<void(const uint8_t* data, size_t len)> on_input;
  /// 链路读不下去的通知：读侧或写侧报了确定性错误，或超过 5 秒没有任何
  /// 输入。只保证"判死了会通知"，不保证只通知一次，实现必须幂等。
  std::function<void()> on_disconnected;
};

/**
 * @brief 拉起读、写两条传输线程。
 *
 * 顺序是定死的：写侧状态归零 → 建两个 eventfd 并置运行标志 → 起线程。运行
 * 标志必须在两条线程起来之前落定，它们的主循环都以它作继续条件；线程起不来
 * 时整个平面就地收回，不留"一条在跑、一条没跑"的半成品。
 *
 * @param handlers 按值拷贝进单实例，生命周期跟着线程走。
 * @return 两条线程都起来 true；已经在跑、eventfd 或 std::thread 创建失败
 *         false（失败时不留已起的线程）。
 * @warning 前置条件：bt::Open() 已成功、detail::Fd() 有效——poll 的链路 fd
 *          是每轮循环现取的，链路没开时读线程只会白等到静默超时判死。
 * @see StopThreads() —— 逆操作。
 */
bool StartThreads(const Handlers& handlers);

/**
 * @brief 停两条线程并回收资源；幂等，没起来时调用无害。
 *
 * 顺序：置 running=false 并踢收工 fd → join 读线程 → join 写线程 → 关两个
 * eventfd → 清空队列。踢 fd 是必要的：写线程在队列空时是无超时睡等，只看得到
 * 这一脚；读线程最迟一个 100ms tick 后也会自己发现 running 已经落定。
 * join 无条件：线程可能已经因判死自己退出过，句柄仍 joinable。
 * 诊断计数器（仅 `DS5DUMMY_DEBUG` 构建）不清零：退出前最后一次
 * StatsWindow() 仍能看到整段运行的统计。
 *
 * @warning 只能从传输线程之外的线程调用——在线程自己里面调就是 join 自己。
 */
void StopThreads();

/**
 * @brief 把一份输出报告体排进发送队列（常规输出报告的唯一正路）。
 *
 * 在队列锁内完成三件事：给 body[1] 的高 4 位盖 seq（4 位回卷）、按报告体
 * 尾部 4 字节写输出 CRC、踢一下 wake fd 把睡等的写线程叫回来。整步非阻塞，
 * 可以在任何线程上调用。
 *
 * 队列满时丢的就是这一份并返回 false：没入队就不占 seq 号，线上不会因此
 * 出现 seq 空洞。
 *
 * @param data 裸报告体，data[0] 是输出 report id，本函数不动它。
 * @param len  必须在 [kMinOutputBody, kMaxOutputBody] 区间内。
 * @return 入队 true；指针为空、长度不合法或队列已满 false。
 */
bool PostOutput(const uint8_t* data, size_t len);

#if DS5DUMMY_DEBUG
/**
 * @brief 传输侧计数的读取快照；只在诊断构建里存在。
 *
 * 累计量随进程单调增长；窗口量代表两次 StatsWindow() 之间那一段的健康度
 * （worker 每 5 秒打一行日志消费它）。
 */
struct Stats {
  uint64_t rx_reports = 0;      ///< 读侧拿到一份报告的累计次数
  uint64_t truncated = 0;       ///< 读侧因缓冲装不下而丢弃的累计份数
  uint64_t tx_31 = 0;           ///< 真正写进链路的 0x31（普通输出）份数
  uint64_t tx_36 = 0;           ///< 真正写进链路的 0x36（音频）份数
  uint64_t dropped = 0;         ///< 队列满被丢弃的份数（累计）
  size_t queue_high_water = 0;  ///< 本窗口内队列到过的最深
  uint64_t write_us_total = 0;  ///< 本窗口内链路 Write 的总耗时（微秒）
  uint64_t write_us_max = 0;    ///< 本窗口内单次 Write 的最长耗时（微秒）
  uint64_t write_count = 0;     ///< 本窗口内链路 Write 的调用次数
};

/**
 * @brief 读走窗口计数并清零；累计计数原样保留。
 *
 * @warning 这不是只读访问：同一个窗口里第二次调用拿到的窗口量是 0。
 */
Stats StatsWindow();
#endif

}  // namespace bt
