/**
 * @file
 * @brief bridge 内部：USB 0x01 输入报告的字节布局（report ID + 63 字节状态体）
 *
 * USB 侧的一份输入报告就是 `[0x01][63 字节输入状态体]` 共 64 字节。这 63 字节
 * 与 BT 0x31 报告 [2..64] 里那份同源同序，两条链路的差别只在前面这个 ID 字节
 * 和 BT 侧独有的尾部 CRC——USB 链路没有 CRC（见 core/protocol/report.h 的链路
 * 差异说明），所以本函数摆完 64 字节即为线上终态。
 *
 * 归属理由：跨 bridge 边界的交接单位是"一条链路上的整份报告"，出站布局一律
 * 在 bridge 组装，usb 只把整份字节交给端点，不再认识 report ID。
 */

#pragma once

#include <cstdint>

namespace bridge {
namespace detail {

/**
 * @brief 把 63 字节输入状态体拼成整份 64 字节 USB 0x01 输入报告。
 *
 * @param out   目标缓冲区，容量为 core::protocol::kUsbInputReportSize（64）。
 * @param state core::protocol::kUsbInputStateSize（63）字节输入状态体，取自
 *              BT 0x31 报告的 [2..64]，或全零（空闲报告）。
 * @note 纯填充：不发送、不读模块状态，跑在调用方线程上。与
 *       BuildAudioReport() 分列两条链路各自的出站布局，两者都不碰帧头。
 * @see bridge::OnBtInput() —— 运行时调用方：校验过 BT 报告后取其状态体。
 * @see bridge::Init() —— 发布第一份空闲报告，作为 usb 持有的那份状态。
 */
void BuildUsbInputReport(uint8_t* out, const uint8_t* state);

}  // namespace detail
}  // namespace bridge
