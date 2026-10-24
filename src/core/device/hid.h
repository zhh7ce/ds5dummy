/**
 * @file hid.h
 * @brief USB HID 1.11 类规范在 ep0 上的线格式取值
 *
 * 主机经 endpoint 0 发 HID class request 时，bRequest 用这里的
 * request code，wValue 高字节用这里的 report type 编号。两者都取自
 * HID 1.11 §7.2/§7.3，是 USB 标准值而非 DualSense 私有格式，故放在
 * core::device（设备/总线层）而不是 core::protocol。
 *
 * @note 勿与 core::protocol::kHidpGetReport 混淆：那个是蓝牙 HIDP 控制
 *       事务头的 payload 字节，数值和含义都与此处不同。
 * @note linux/usb/ch9.h 只定义 descriptor type，不定义此处任何一个值，
 *       因此由本仓库自持。
 */

#pragma once

#include <cstdint>

namespace core::device {

/// HID class request：GET_REPORT，主机向设备索取一份报告。
inline constexpr uint8_t kHidGetReport = 0x01;
/// HID class request：GET_IDLE，查询空闲期。
inline constexpr uint8_t kHidGetIdle = 0x02;
/// HID class request：GET_PROTOCOL，查询 boot/report 协议模式。
inline constexpr uint8_t kHidGetProtocol = 0x03;
/// HID class request：SET_REPORT，主机向设备送达一份报告（含 feature）。
inline constexpr uint8_t kHidSetReport = 0x09;
/// HID class request：SET_IDLE，设置空闲期。
inline constexpr uint8_t kHidSetIdle = 0x0A;
/// HID class request：SET_PROTOCOL，切换 boot/report 协议模式。
inline constexpr uint8_t kHidSetProtocol = 0x0B;

/// wValue 高字节的 report type 编号：input。
inline constexpr uint8_t kHidReportTypeInput = 1;
/// wValue 高字节的 report type 编号：output。
inline constexpr uint8_t kHidReportTypeOutput = 2;
/// wValue 高字节的 report type 编号：feature。
inline constexpr uint8_t kHidReportTypeFeature = 3;

}  // namespace core::device
