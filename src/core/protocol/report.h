/**
 * @file report.h
 * @brief DualSense 报告 ID 与线格式尺寸常量（跨模块共享）
 *
 * 本文件只放跨模块共享的协议标识与尺寸：USB/BT 两条链路各自的报告 ID、
 * 报告体字节数、feature 报告清单、HIDP 帧头字节。字段的位级语义不放这里
 * ——输入侧见 input_state.h，输出侧见 output_state.h，0x36 的音频分段
 * 见 audio_format.h。
 *
 * @note USB 与 BT 的链路层不同但报告体相同：同一份 63 字节输入状态既嵌在
 *       USB 0x01 报告里，也嵌在 BT 0x31 报告的 [3..65]；BT 链路的所有报告
 *       体都自带尾部 4 字节 CRC（见 crc32.h），USB 链路没有 CRC。
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace core::protocol {

// USB HID report IDs.
/// USB 输入报告的 report ID（设备→主机，经 EP1 IN）。
inline constexpr uint8_t kUsbInputReportId = 0x01;
/// USB 输出报告的 report ID（主机→设备，经 EP2 OUT）。
inline constexpr uint8_t kUsbOutputReportId = 0x02;

// BT HIDP report IDs.
/// BT 常规输入/输出报告的 report ID；收发复用同一个编号，方向由链路区分。
inline constexpr uint8_t kBtInputReportId = 0x31;
/// BT 常规输出报告的 report ID（与输入同值，见上）。
inline constexpr uint8_t kBtOutputReportId = 0x31;
/// BT 音频/振动复合报告的 report ID，携带 0x9x 子报告。
inline constexpr uint8_t kBtAudioReportId = 0x36;
/// BT 唤醒报告的 report ID，仅休眠唤醒路径发送。
inline constexpr uint8_t kBtWakeReportId = 0x32;

// USB wire sizes.
/// USB 输入报告去掉 report ID 后的状态体长度（即 InputState 线格式）。
inline constexpr size_t kUsbInputStateSize = 63;
/// USB 输入报告在 EP1 IN 上的总字节数：report ID + 状态体。
inline constexpr size_t kUsbInputReportSize = kUsbInputStateSize + 1;
/// USB 0x02 输出报告主机实际发送的 payload 长度（不含 report ID）。
inline constexpr size_t kUsbOutputPayloadSize = 47;
/// USB 输出报告在 EP2 OUT 上的总字节数：report ID + payload。
inline constexpr size_t kUsbOutputReportSize = kUsbOutputPayloadSize + 1;
/// SetStateData 影子缓冲长度：47 字节有效 payload + 16 字节主机从不发送的
/// reserved padding，0x36 报告需要完整 63 字节。
inline constexpr size_t kSetStateShadowSize = 63;

// BT wire sizes. Both 0x31 and 0x36 bodies carry their own 4-byte CRC.
/// BT 0x31 报告体长度（含内嵌 63 字节输入状态与尾部 CRC）。
inline constexpr size_t kBtReportSize = 78;
/// BT 0x36 音频报告体长度（含内嵌 264 字节音频段与尾部 CRC）。
inline constexpr size_t kBtAudioReportSize = 398;
/// BT 0x32 唤醒报告总长：[id][3B 头][63B 状态][填充][CRC32]。
inline constexpr size_t kBtWakeReportSize = 142;

// DualSense feature reports. 只有 0x09（反序 BD_ADDR）和 0x20（出厂日期块）
// 的字节会被本项目解释；0x22 与 0x05 连接时预取后按不透明载荷缓存。
/// feature 报告 0x09：设备 MAC，payload 是反序 BD_ADDR。
inline constexpr uint8_t kFeatureReportIdMac = 0x09;
/// feature 报告 0x20：出厂构建日期块。
inline constexpr uint8_t kFeatureReportIdFactory = 0x20;
/// 连接后预取的 feature 报告清单；顺序即预取顺序（bt 侧逐个 GET）。
inline constexpr uint8_t kFeatureReportIds[] = {
    kFeatureReportIdMac, kFeatureReportIdFactory, 0x22, 0x05};

// HIDP framing bytes on the raw L2CAP interrupt/control channels. 裸 L2CAP
// 后端直接用这里的值组/拆帧，hidraw 后端由内核 hidp 驱动代为处理。
/// HIDP DATA 事务头（低 2 位选报告类型）：input，设备→主机方向。
inline constexpr uint8_t kHidpDataIn = 0xA1;
/// HIDP DATA 事务头：output，主机→设备方向。
inline constexpr uint8_t kHidpDataOut = 0xA2;
/// HIDP CONTROL 事务：GET_REPORT（feature 类型）。
inline constexpr uint8_t kHidpGetReport = 0x43;
/// HIDP feature 报告数据帧头，用于辨认控制通道上的 GET_REPORT 应答。
inline constexpr uint8_t kHidpFeatureReply = 0xA3;

}  // namespace core::protocol
