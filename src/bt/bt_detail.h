/**
 * @file
 * @brief bt 模块内部契约：§A 后端接缝、§B feature 缓存、§C 共用小件
 *
 * 本头文件只在 `src/bt/` 内部使用（模块自己的单测是唯一外部读者）。按内容分三段：
 *
 * - **§A 后端接缝**：一个后端要提供的全部 8 个原语。链接期只编进 `bt_l2cap.cpp`
 *   或 `bt_hidraw.cpp` 之一（见 CMake 的 BT_BACKEND），所以这一组符号就是后端
 *   与共享层（`bt_link.cpp`、`bt_plane.cpp`、`bt_rx.cpp`、`bt_tx.cpp`）之间
 *   唯一的接缝。上层只见 `bt.h`，看不到这里。
 * - **§B feature 缓存**：只有共享层可用——**后端不得读写它**。缓存何时冷、
 *   命中与现取如何折成一个返回值，全部单源在 `bt_link.cpp`。
 * - **§C 共用小件**：等待可写与剥 HIDP 帧头等辅助，后端和共享层都可能用。
 *
 * @warning 其它模块只用 bt.h，不要 include 本文件。
 * @see bt.h —— 上层唯一入口。
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "bt/bt.h"

namespace bt::detail {

// ---------------------------------------------------------------------------
// §A 后端接缝：每个后端各自实现一份
// ---------------------------------------------------------------------------

/**
 * @brief 一次链路 I/O 的统一结果。
 *
 * 后端把各自的 errno 折叠到这五个值上，具体映射表写在每个后端的
 * Read()/Write() 里。跨后端共同的判据：
 * - kAgain 只表示"暂时不行"，同一份字节原样重试是安全的；
 * - kTruncated 只在读侧出现：报告体比缓冲区长，缓冲区里那半截不可用，
 *   整份丢掉（后端已经把数据报从链路消费掉了，不会有第二次机会）；
 * - kClosed 表示这条链路已经不可用，重试无意义，只能重建；
 * - kFatal 表示链路以外的问题：fd 没开、缓冲区不合法、短读写，
 *   以及后端没有归类过的 errno。
 *
 * @warning 上层只对 kAgain 重试。把 kClosed 当 kAgain 反复打，是对一条
 *          已死链路的无效循环。
 * @see bt::PostOutput() —— 输出报告的队列消费按这套判据断链；后端的
 *      Read()/Write() 是这些值的来源。
 */
enum IoResult {
  kOk,         ///< 整份报告已完整收发
  kAgain,      ///< 暂不可读/不可写；同一份字节原样重试是安全的
  kTruncated,  ///< 读到的报告比缓冲区长，整份丢弃（仅读侧）
  kClosed,     ///< 链路不可用，交给上层重建
  kFatal,      ///< 链路之外的问题，errno 保留现场
};

/**
 * @brief 建链；成功即认为链路可用，本函数不 Close、不碰缓存、不预取报告。
 *
 * - L2CAP：让 BlueZ 把 ACL 拉起来 → 释放 HID profile → 抢 PSM 0x11（控制）
 *   和 0x13（中断）→ 切到 report protocol → 补发一份 0x32 唤醒报告（裸
 *   L2CAP 上没有别的 HID 主机跟手柄说话，不推这一份它不会开始上报）；
 * - hidraw：扫 /sys/class/hidraw，按 VID/PID（真 Sony 或本项目 DKMS 合成的
 *   那组）加可选 MAC 认出节点并打开；内核驱动已在建节点时初始化过手柄，
 *   没有唤醒这一步。
 *
 * 先关后开与预取由门面 `bt::Open()` 负责，见 bt_link.cpp。
 *
 * @param mac 经典蓝牙 BD_ADDR；hidraw 后端允许空串表示匹配任意一只 DualSense。
 * @return 成功 true；失败 false 且不留半开的链路。
 * @see bt::Open() —— 调用方看到的那一层。
 */
bool Open(const std::string& mac);

/**
 * @brief 关掉本后端的 fd；幂等，不碰 feature 缓存（缓存随 `bt::Close()` 作废）。
 */
void Close();

/**
 * @brief 这条链路当前是否可用。
 *
 * L2CAP 要求控制、中断两条通道都在才算开；hidraw 只看那个节点 fd。
 */
bool IsOpen();

/**
 * @brief 收、发共用的那个链路 fd；链路没开时 -1。
 *
 * L2CAP 是中断通道（输入报告都从它来，控制通道只在取 feature 时按需同步收发），
 * hidraw 是 /dev/hidrawN。
 */
int Fd();

/**
 * @brief 非阻塞地取一份输入报告体（已剥 HIDP framing，首字节即 report id）。
 *
 * 由读线程在 poll 报可读后调用。一次调用最多消费一个数据报/一次 read，
 * 绝不搬半个报告——报告比缓冲区长时不交半份，直接交回 kTruncated。
 *
 * "非阻塞"是对后端的硬要求：读线程把一次醒来读空为止（循环到 kAgain），
 * 后端若在无数据时睡在系统调用里，静默看门狗和收工通知都走不到。L2CAP 靠
 * MSG_DONTWAIT，hidraw 靠节点以 O_NONBLOCK 打开。
 *
 * @param buf        调用方缓冲区，容量必须装得下一个完整报告。
 * @param size       buf 容量，也是截断判定线。
 * @param bytes_read 非空时先清零，成功时写入本次实际读到的字节数。
 * @return kOk 拿到一份；kAgain 现在没数据；kTruncated 报告比 size 长，这一份
 *         已作废；kClosed 链路不可用；kFatal fd 没开、缓冲区为 0 或后端没归
 *         类过的 errno（errno 保留现场）。
 */
IoResult Read(uint8_t* buf, size_t size, size_t* bytes_read);

/**
 * @brief 发一份输出报告体，**本函数内部不等待**：此刻发不出就交回 kAgain。
 *
 * 等不等由调用方决定：写线程靠 poll(POLLOUT) 事件驱动，L2CAP 开链末尾的
 * 唤醒直发先 WaitWritable。L2CAP 在这里补 0xA2 帧头，hidraw 原样交给
 * 内核；两条路径都是"整份发出或明确失败"，不交半份。
 *
 * @param data 裸报告体，data[0] 是输出 report id，不含 HIDP framing。
 * @param len  @p data 的字节数。
 * @return kOk 已发出；kAgain 此刻没余量（同一份字节可原样重试）；
 *         kClosed/kFatal 按断链处理。
 */
IoResult Write(const uint8_t* data, size_t len);

/**
 * @brief 上线现取一份 feature 报告，**不查也不写缓存**。
 *
 * L2CAP 走控制通道的 GET_REPORT（等 0xA3 回包），hidraw 走 HIDIOCGFEATURE。
 * 缓存的命中判断与回填都在 `bt::GetFeature()`。
 *
 * @param buf buf[0] 是目标 report id；成功时被覆写成完整报告（首字节仍是 id）。
 * @param len buf 容量，同时是向链路索要的字节数上限。
 * @return 写回的字节数（>= 1，含首字节 id）；链路没开、参数不合法（len < 2）
 *         或线上没回复时 -1。
 * @note 会在调用线程上同步等回复：L2CAP 的整段预算是 2s，hidraw 的 ioctl
 *       由内核决定何时返回。
 */
int GetFeature(uint8_t* buf, size_t len);

/**
 * @brief 上线 SET 一份 feature 报告，**不等应答**。
 *
 * L2CAP 在控制通道上发 `[0x53][report id + 报告体]`；hidraw 用
 * HIDIOCSFEATURE(len) 把整份报告交给内核。两条路径的 @p data 都已经是完整
 * 线上形态：首字节 report id，尾部 4 字节 CRC——后端不补帧、不算 CRC。
 *
 * 手柄对 SET_REPORT 不回应答，所以"发出去"就是全部语义；迟到的 HIDP
 * HANDSHAKE（若有）由下一次 GET_REPORT 当作无关控制包丢弃。
 *
 * @param data 完整报告，data[0] 是 report id。
 * @param len  @p data 的字节数，必须 >= 2。
 * @return 已整份发出 true；链路没开、参数不合法、短写或后端报错 false。
 * @note 会在调用线程上同步写控制通道；与并发的 GetFeature 由门面串行化。
 */
bool SetFeature(const uint8_t* data, size_t len);

// ---------------------------------------------------------------------------
// §B feature 缓存：只有共享层（bt_link.cpp）与模块单测可用，后端不得触碰
// ---------------------------------------------------------------------------

/**
 * @brief 查 feature 缓存。
 *
 * @param buf buf[0] 是目标 report id；命中时被覆写成缓存里的那份。
 * @param len buf 容量；缓存条目比它长时按容量截断。
 * @return 写回的字节数（>= 1）；没命中、buf 为空或 len 为 0 时 -1。
 */
int CacheLookup(uint8_t* buf, size_t len);

/**
 * @brief 把一份 feature 报告存进缓存，同一个 id 直接覆盖旧条目。
 *
 * @param report_id 缓存键。
 * @param data      完整报告字节，data[0] 应当就是 report_id（调用方约定，
 *                  本函数不校验）。
 * @param len       @p data 的字节数；为空或 0 时什么都不存。
 */
void CacheStore(uint8_t report_id, const uint8_t* data, size_t len);

/// 清掉整个缓存。旧手柄的报告对新链路没有意义，所以 `bt::Open()` 预取前与
/// `bt::Close()` 关链后各清一次。
void CacheClear();

// ---------------------------------------------------------------------------
// §C 后端与共享层共用的常量与小件
// ---------------------------------------------------------------------------

/**
 * @brief 从 HIDP_DATA in 帧里原地剥掉 1 字节帧头。
 *
 * 不校验 buf[0] 是否为 0xA1，直接左移一格——完整帧的判据由调用方（后端
 * Read()）负责，走到这里的数据已经被确认为 `[0xA1][body...]`。
 *
 * @param buf 入参 `[0xA1][body...]`；返回时变为 `[body...]`，原地搬移。
 * @param n   buf 上当前的字节数。
 * @return 剥头后的长度 n-1；n < 1 时返回 0（空输入没有可剥的东西）。
 */
size_t StripHidpDataIn(uint8_t* buf, size_t n);

/**
 * @brief 等 @p fd 可写，带总预算。
 *
 * deadline_ms > 0 时它是整次等待的总预算：循环按已消耗的时间递减剩余，
 * 不会每轮重新等满；deadline_ms <= 0 时原样交给 poll()，0 表示不等、
 * 负值表示一直等。
 *
 * @param fd          要等可写的文件描述符。
 * @param deadline_ms 总预算（毫秒）；<= 0 直接透传给 poll()。
 * @return kOk 可以发；kAgain 预算内没等到可写；kClosed 等到 POLLHUP/POLLNVAL；
 *         kFatal poll() 自身出错（EINTR 除外，内部重试）或等到 POLLERR。
 * @note 纯等待，不写任何数据；调用方拿到 kOk 后自己立刻写，两次调用之间
 *       的可写状态不作保证。
 */
IoResult WaitWritable(int fd, int deadline_ms);

}  // namespace bt::detail
