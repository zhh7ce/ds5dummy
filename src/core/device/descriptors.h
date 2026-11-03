/**
 * @file descriptors.h
 * @brief 虚拟 DualSense / DualSense Edge 的 USB / HID / FunctionFS 静态描述符字节
 *
 * 本文件是设备对主机呈现的全部 USB 描述符的唯一事实源，分两块：HID report
 * descriptor（普通 337 字节、Edge 453 字节，主机经标准 GET_DESCRIPTOR 索取，
 * 决定内核 hid 驱动如何解析输入/输出/feature 报告）与 FunctionFS 全速描述符块
 * （32 字节，写进 ep0 的 fs_descs，声明 interface + HID class + 两个中断端点）。
 * 设备身份（VID/PID/字符串）见 identity.h，两份描述符块与身份如何配对见下面的
 * `DeviceProfile`。
 *
 * 结构约定：意图常量（计数、偏移、类型码）先声明，字节数组居中，文件末尾
 * 用 static_assert 反向验证数组里的偏移与常量一致——改数组而忘了改常量时，
 * 编译期即失败，而不是主机 enumeration 出错。
 *
 * @note 数组内逐行的 `// Usage Page ...` 一类注释是 HID 规范逐字节对照，
 *       保持与字节同行，不套 Doxygen 标签。
 * @note 本文件符号全部位于 namespace core::device，且均为 constexpr。
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/device/identity.h"

namespace core::device {
// ---------------------------------------------------------------------------
// 常量 — 描述符类型码、FFS 块内的计数与偏移
//
// 先声明这些意图值，字节数组在下方，文件末尾的 static_assert 反向验证偏移
// ---------------------------------------------------------------------------

// ch9.h 没有这两个 descriptor type 的定义（HID class 层私有），故自持。
/// HID class descriptor 的类型码。
static constexpr uint8_t kDescriptorTypeHid = 0x21;

/// HID report descriptor 的类型码。
static constexpr uint8_t kDescriptorTypeReport = 0x22;

/// 全速块内含的描述符个数，作为 FunctionFS header 的 fs_count 上报给内核。
static constexpr uint32_t kFfsFsDescCount = 4;

// HID class descriptor 紧跟 interface descriptor 之后；主机用标准
// GET_DESCRIPTOR 索取它和 report descriptor，FunctionFS 不代答而是转发
// 到用户态，因此应答内容由 ep0 代码按这里的偏移/长度切片。
/// HID class descriptor 在 kFfsFsDescriptors 内的起始字节偏移。
static constexpr uint32_t kFfsHidDescOffset = 9;
/// HID class descriptor 的字节长度，也是主机索取它时的应答上限。
static constexpr uint32_t kFfsHidDescLength = 9;

// wDescriptorLength 是 HID class descriptor 的第 8、9 字节（小端），紧接在
// bLength/bDescriptorType/bcdHID/bCountryCode/bNumDescriptors/bDescriptorType
// 之后。两个型号的描述符长度不同（DS5 的 337 与 Edge 的 453），所以这个字段
// 和两个 bInterval 一样在装配期按型号打补丁——数组里冻结的是 DS5 的值。
/// HID class descriptor 里 wDescriptorLength 低字节的偏移。
static constexpr uint32_t kFfsWDescLenOffset = kFfsHidDescOffset + 7;

// 装配期被改写的字节只有两处：两个中断端点的 bInterval，和 HID class
// descriptor 里的 wDescriptorLength。前者就是配置里的 usb_report_interval_ms
// （全速总线一帧 = 1 ms，毫秒数直接落进这个字节，real-time 档写最快的 1），
// 后者由型号决定——DS5 与 Edge 的 report descriptor 长度不同。描述符数组本身
// 保持 constexpr 不动，FfsDevice 在写 ep0 前拷贝一份再打补丁。
/// EP1 IN 的 bInterval 在 kFfsFsDescriptors 内的字节偏移。
static constexpr uint32_t kFfsEpInIntervalOffset = 24;
/// EP2 OUT 的 bInterval 在 kFfsFsDescriptors 内的字节偏移。
static constexpr uint32_t kFfsEpOutIntervalOffset = 31;

/// 两个中断端点的 wMaxPacketSize：64，全速中断端点的上限。
/// @details 读端点的缓冲按这个尺寸开而不是按报告尺寸：主机侧的短包只要比排队
///          请求的缓冲大，udc（dummy_hcd）就把那份请求判成 -EOVERFLOW，而
///          0x02 输出报告只有 48 字节，主机在 48 与 maxpacket 之间多发都会踩中。
///          缓冲按 maxpacket 开之后同样的包只是一次普通短读；多出来的字节原样
///          交给策略层，它只取前面的有效 payload。
static constexpr uint32_t kUsbEpMaxPacketSize = 64;

// ---------------------------------------------------------------------------
// HID Report Descriptor — 337 字节
// ---------------------------------------------------------------------------

/**
 * @brief 设备的 HID report descriptor 原始字节，共 337 字节。
 *
 * 声明 report ID 0x01 的输入报告（摇杆/十字键/按键/触摸板/传感器，63 字节
 * payload）、0x02 的输出报告（47 字节）以及 0x05..0xF9 共 30 余个 feature
 * 报告——报告 ID 与字段布局由此处字节直接决定，core/protocol 的编解码只是
 * 它的镜像。主机拿到什么就按什么解析，任何一个字节改动都是协议变更。
 *
 * @note wDescriptorLength 由 sizeof 推导进下方 HID class descriptor，
 *       长度漂移会在末尾 static_assert 处编译失败。
 */
static constexpr uint8_t kHidReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x35,        //   Usage (Rz)
    0x09, 0x33,        //   Usage (Rx)
    0x09, 0x34,        //   Usage (Ry)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x06,        //   Report Count (6)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20,        //   Usage (0x20)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null State)
    0x65, 0x00,        //   Unit (None)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0F,        //   Usage Maximum (0x0F)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0F,        //   Report Count (15)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x21,        //   Usage (0x21)
    0x95, 0x0D,        //   Report Count (13)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x22,        //   Usage (0x22)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x34,        //   Report Count (52)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x23,        //   Usage (0x23)
    0x95, 0x2F,        //   Report Count (47)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x85, 0x05,        //   Report ID (5)
    0x09, 0x33,        //   Usage (0x33)
    0x95, 0x28,        //   Report Count (40)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x08,        //   Report ID (8)
    0x09, 0x34,        //   Usage (0x34)
    0x95, 0x2F,        //   Report Count (47)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x09,        //   Report ID (9)
    0x09, 0x24,        //   Usage (0x24)
    0x95, 0x13,        //   Report Count (19)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x0A,        //   Report ID (10)
    0x09, 0x25,        //   Usage (0x25)
    0x95, 0x1A,        //   Report Count (26)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x0B,        //   Report ID (11)
    0x09, 0x41,        //   Usage (0x41)
    0x95, 0x29,        //   Report Count (41)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x0C,        //   Report ID (12)
    0x09, 0x42,        //   Usage (0x42)
    0x95, 0x29,        //   Report Count (41)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x20,        //   Report ID (32)
    0x09, 0x26,        //   Usage (0x26)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x21,        //   Report ID (33)
    0x09, 0x27,        //   Usage (0x27)
    0x95, 0x04,        //   Report Count (4)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x22,        //   Report ID (34)
    0x09, 0x40,        //   Usage (0x40)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x80,        //   Report ID (128)
    0x09, 0x28,        //   Usage (0x28)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x81,        //   Report ID (129)
    0x09, 0x29,        //   Usage (0x29)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x82,        //   Report ID (130)
    0x09, 0x2A,        //   Usage (0x2A)
    0x95, 0x09,        //   Report Count (9)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x83,        //   Report ID (131)
    0x09, 0x2B,        //   Usage (0x2B)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x84,        //   Report ID (132)
    0x09, 0x2C,        //   Usage (0x2C)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x85,        //   Report ID (133)
    0x09, 0x2D,        //   Usage (0x2D)
    0x95, 0x02,        //   Report Count (2)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xA0,        //   Report ID (160)
    0x09, 0x2E,        //   Usage (0x2E)
    0x95, 0x01,        //   Report Count (1)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xE0,        //   Report ID (224)
    0x09, 0x2F,        //   Usage (0x2F)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF0,        //   Report ID (240)
    0x09, 0x30,        //   Usage (0x30)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF1,        //   Report ID (241)
    0x09, 0x31,        //   Usage (0x31)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF2,        //   Report ID (242)
    0x09, 0x32,        //   Usage (0x32)
    0x95, 0x0F,        //   Report Count (15)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF4,        //   Report ID (244)
    0x09, 0x35,        //   Usage (0x35)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF5,        //   Report ID (245)
    0x09, 0x36,        //   Usage (0x36)
    0x95, 0x03,        //   Report Count (3)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF6,        //   Report ID (246)
    0x09, 0x37,        //   Usage (Vendor 0x37)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF7,        //   Report ID (247)
    0x09, 0x38,        //   Usage (Vendor 0x38)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF8,        //   Report ID (248)
    0x09, 0x39,        //   Usage (Vendor 0x39)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF9,        //   Report ID (249)
    0x09, 0x3A,        //   Usage (Vendor 0x3A)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xFA,        //   Report ID (250)
    0x09, 0x3B,        //   Usage (Vendor 0x3B)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xFB,        //   Report ID (251)
    0x09, 0x3C,        //   Usage (Vendor 0x3C)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0xC0,              // End Collection
};

/// report descriptor 的字节数契约：HID class descriptor 的 wDescriptorLength
/// 由 sizeof 推导，主机侧按此长度索取并校验，337 变了整条解析链都要复核。
static_assert(sizeof(kHidReportDescriptor) == 337,
              "HID report descriptor must be 337 bytes");

// ---------------------------------------------------------------------------
// HID Report Descriptor — DualSense Edge，453 字节
// ---------------------------------------------------------------------------

/**
 * @brief Edge 的 HID report descriptor 原始字节，共 453 字节。
 *
 * 与普通那份的差别只有两处（其余字节逐行相同，出处是上游 DS5Dongle 的
 * `desc_hid_report_dse`，它与我们的 337 字节那份同源）：
 *
 * - 0x02 输出报告的字段数是 63 而不是 47。主机侧仍只写 48 字节
 *   （`dualsense_init_output_report()` 按自己的结构体定长，不按描述符），
 *   所以 bridge 的 OUT 路径不需要跟着改；多出来的 16 字节正是
 *   `core::protocol::kSetStateShadowSize` 里那段主机从不发送的 reserved
 *   padding。
 * - 在 0xF5 与 0xF6 之间多出 19 个 feature 报告：`0x60..0x65`、`0x68` 与
 *   `0x70..0x7B`，即 Edge 的 profile / 按键映射块。它们能否读写取决于 bridge
 *   是否把主机的 SET_REPORT(FEATURE) 转到手柄（见实施计划的任务 2）。
 *
 * @note 与 `kDualSenseEdgeIdentity` 成对使用，配对点在下面的 `DeviceProfile`；
 *       长度变了要在下面补一条 HID class descriptor 的 static_assert 复核。
 */
static constexpr uint8_t kHidReportDescriptorEdge[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x35,        //   Usage (Rz)
    0x09, 0x33,        //   Usage (Rx)
    0x09, 0x34,        //   Usage (Ry)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x06,        //   Report Count (6)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x20,        //   Usage (0x20)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null State)
    0x65, 0x00,        //   Unit (None)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0F,        //   Usage Maximum (0x0F)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0F,        //   Report Count (15)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x21,        //   Usage (0x21)
    0x95, 0x0D,        //   Report Count (13)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
    0x09, 0x22,        //   Usage (0x22)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x34,        //   Report Count (52)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x23,        //   Usage (0x23)
    0x95, 0x3F,        //   Report Count (63)
    0x91, 0x02,        //   Output (Data,Var,Abs)
    0x85, 0x05,        //   Report ID (5)
    0x09, 0x33,        //   Usage (Rx)
    0x95, 0x28,        //   Report Count (40)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x08,        //   Report ID (8)
    0x09, 0x34,        //   Usage (Ry)
    0x95, 0x2F,        //   Report Count (47)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x09,        //   Report ID (9)
    0x09, 0x24,        //   Usage (0x24)
    0x95, 0x13,        //   Report Count (19)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x0A,        //   Report ID (10)
    0x09, 0x25,        //   Usage (0x25)
    0x95, 0x1A,        //   Report Count (26)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x0B,        //   Report ID (11)
    0x09, 0x41,        //   Usage (0x41)
    0x95, 0x29,        //   Report Count (41)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x0C,        //   Report ID (12)
    0x09, 0x42,        //   Usage (0x42)
    0x95, 0x29,        //   Report Count (41)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x20,        //   Report ID (32)
    0x09, 0x26,        //   Usage (0x26)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x21,        //   Report ID (33)
    0x09, 0x27,        //   Usage (0x27)
    0x95, 0x04,        //   Report Count (4)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x22,        //   Report ID (34)
    0x09, 0x40,        //   Usage (0x40)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x80,        //   Report ID (128)
    0x09, 0x28,        //   Usage (0x28)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x81,        //   Report ID (129)
    0x09, 0x29,        //   Usage (0x29)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x82,        //   Report ID (130)
    0x09, 0x2A,        //   Usage (0x2A)
    0x95, 0x09,        //   Report Count (9)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x83,        //   Report ID (131)
    0x09, 0x2B,        //   Usage (0x2B)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x84,        //   Report ID (132)
    0x09, 0x2C,        //   Usage (0x2C)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x85,        //   Report ID (133)
    0x09, 0x2D,        //   Usage (0x2D)
    0x95, 0x02,        //   Report Count (2)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xA0,        //   Report ID (160)
    0x09, 0x2E,        //   Usage (0x2E)
    0x95, 0x01,        //   Report Count (1)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xE0,        //   Report ID (224)
    0x09, 0x2F,        //   Usage (0x2F)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF0,        //   Report ID (240)
    0x09, 0x30,        //   Usage (X)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF1,        //   Report ID (241)
    0x09, 0x31,        //   Usage (Y)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF2,        //   Report ID (242)
    0x09, 0x32,        //   Usage (Z)
    0x95, 0x34,        //   Report Count (52)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF4,        //   Report ID (244)
    0x09, 0x35,        //   Usage (Rz)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF5,        //   Report ID (245)
    0x09, 0x36,        //   Usage (0x36)
    0x95, 0x03,        //   Report Count (3)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x60,        //   Report ID (96)
    0x09, 0x41,        //   Usage (0x41)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x61,        //   Report ID (97)
    0x09, 0x42,        //   Usage (0x42)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x62,        //   Report ID (98)
    0x09, 0x43,        //   Usage (0x43)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x63,        //   Report ID (99)
    0x09, 0x44,        //   Usage (0x44)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x64,        //   Report ID (100)
    0x09, 0x45,        //   Usage (0x45)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x65,        //   Report ID (101)
    0x09, 0x46,        //   Usage (0x46)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x68,        //   Report ID (104)
    0x09, 0x47,        //   Usage (0x47)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x70,        //   Report ID (112)
    0x09, 0x48,        //   Usage (0x48)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x71,        //   Report ID (113)
    0x09, 0x49,        //   Usage (0x49)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x72,        //   Report ID (114)
    0x09, 0x4A,        //   Usage (0x4A)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x73,        //   Report ID (115)
    0x09, 0x4B,        //   Usage (0x4B)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x74,        //   Report ID (116)
    0x09, 0x4C,        //   Usage (0x4C)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x75,        //   Report ID (117)
    0x09, 0x4D,        //   Usage (0x4D)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x76,        //   Report ID (118)
    0x09, 0x4E,        //   Usage (0x4E)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x77,        //   Report ID (119)
    0x09, 0x4F,        //   Usage (0x4F)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x78,        //   Report ID (120)
    0x09, 0x50,        //   Usage (0x50)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x79,        //   Report ID (121)
    0x09, 0x51,        //   Usage (0x51)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x7A,        //   Report ID (122)
    0x09, 0x52,        //   Usage (0x52)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0x7B,        //   Report ID (123)
    0x09, 0x53,        //   Usage (0x53)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF6,        //   Report ID (246)
    0x09, 0x37,        //   Usage (Vendor 0x37)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF7,        //   Report ID (247)
    0x09, 0x38,        //   Usage (Vendor 0x38)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF8,        //   Report ID (248)
    0x09, 0x39,        //   Usage (Hat switch)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xF9,        //   Report ID (249)
    0x09, 0x3A,        //   Usage (Vendor 0x3A)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xFA,        //   Report ID (250)
    0x09, 0x3B,        //   Usage (Vendor 0x3B)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0x85, 0xFB,        //   Report ID (251)
    0x09, 0x3C,        //   Usage (Vendor 0x3C)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0xC0,              // End Collection
};

static_assert(sizeof(kHidReportDescriptorEdge) == 453,
              "Edge HID report descriptor must be 453 bytes");

// ---------------------------------------------------------------------------
// FunctionFS 全速描述符块（32 字节）
//
// FunctionFS 只接受 function 自身的描述符；configuration descriptor 放进来
// 会被内核以 EINVAL 拒绝——composite 层从 configfs 属性自己构造它。
// 布局：Interface 0 HID (9) + HID class (9) + EP IN (7) + EP OUT (7) = 32
// ---------------------------------------------------------------------------

/**
 * @brief 写进 FunctionFS ep0 的全速描述符块，32 字节。
 *
 * 依次是 Interface 0（HID class, 无 boot 子类, 2 端点）、HID class
 * descriptor（bcdHID 1.11，wDescriptorLength 按下面两个数组里 DS5 的长度
 * 冻结）、EP1 IN 与 EP2 OUT 两个中断端点描述符。只声明 HID 一个 interface：
 * 音频不走 USB，由 pipe 桥接进蓝牙侧的 0x36 报告。
 *
 * @note 端点地址 0x81/0x02（EP1 IN / EP2 OUT）与 usb 模块的读写 fd 对应，
 *       wMaxPacketSize=64 是全速中断端点的上限。两处 bInterval 的默认值 1
 *       表示 1 ms（1000 Hz），是最高档；装配期原样写进配置的毫秒数
 *       （1/2/4，real-time 档取 1），数组本身不改。
 * @see DeviceProfile 决定装配期把 wDescriptorLength 覆写成哪个长度。
 */
static constexpr uint8_t kFfsFsDescriptors[] = {
    // --- Interface 0: HID (9 bytes) ---
    9,     // bLength
    0x04,  // bDescriptorType (INTERFACE)
    0,     // bInterfaceNumber
    0,     // bAlternateSetting
    2,     // bNumEndpoints (EP1 IN + EP2 OUT)
    0x03,  // bInterfaceClass (HID)
    0x00,  // bInterfaceSubClass (No Boot)
    0x00,  // bInterfaceProtocol
    0,     // iInterface

    // --- HID Class Descriptor (9 bytes) ---
    9,     // bLength
    0x21,  // bDescriptorType (HID)
    0x11,
    0x01,  // bcdHID 1.11
    0x00,  // bCountryCode
    1,     // bNumDescriptors
    0x22,  // bDescriptorType (Report)
    sizeof(kHidReportDescriptor) & 0xFF,
    (sizeof(kHidReportDescriptor) >> 8) & 0xFF,  // wDescriptorLength = 337

    // --- Endpoint 1 IN: Interrupt (7 bytes) ---
    7,     // bLength
    0x05,  // bDescriptorType (ENDPOINT)
    0x81,  // bEndpointAddress (EP1 IN)
    0x03,  // bmAttributes (Interrupt)
    0x40,
    0x00,  // wMaxPacketSize (64)
    1,     // bInterval (默认 1ms → 1000Hz；装配期可覆写)

    // --- Endpoint 2 OUT: Interrupt (7 bytes) ---
    7,     // bLength
    0x05,  // bDescriptorType (ENDPOINT)
    0x02,  // bEndpointAddress (EP2 OUT)
    0x03,  // bmAttributes (Interrupt)
    0x40,
    0x00,  // wMaxPacketSize (64)
    1,     // bInterval (默认 1ms；装配期可覆写)
};

/// 全速块的总字节契约：interface(9) + HID class(9) + IN(7) + OUT(7) = 32。
static_assert(sizeof(kFfsFsDescriptors) == 32,
              "FFS full-speed block is interface + HID + IN + OUT = 32 bytes");

/// kFfsHidDescOffset/kFfsHidDescLength 必须对上数组里 HID class descriptor
/// 的实际位置：bLength 落在偏移 9 且等于 9、bDescriptorType 为 0x21。
static_assert(kFfsFsDescriptors[kFfsHidDescOffset] == kFfsHidDescLength &&
                  kFfsFsDescriptors[kFfsHidDescOffset + 1] == 0x21,
              "HID class descriptor must sit at offset 9");

/// 两个 bInterval 偏移必须分别落在 EP1 IN / EP2 OUT 端点描述符里：
/// 偏移 -6 是 bLength(7)、-5 是 bDescriptorType(0x05)、-4 是端点地址。
static_assert(kFfsFsDescriptors[kFfsEpInIntervalOffset - 6] == 7 &&
                  kFfsFsDescriptors[kFfsEpInIntervalOffset - 5] == 0x05 &&
                  kFfsFsDescriptors[kFfsEpInIntervalOffset - 4] == 0x81,
              "EP1 IN bInterval offset must sit in its endpoint descriptor");
static_assert(kFfsFsDescriptors[kFfsEpOutIntervalOffset - 6] == 7 &&
                  kFfsFsDescriptors[kFfsEpOutIntervalOffset - 5] == 0x05 &&
                  kFfsFsDescriptors[kFfsEpOutIntervalOffset - 4] == 0x02,
              "EP2 OUT bInterval offset must sit in its endpoint descriptor");

/// wMaxPacketSize 是端点描述符里紧挨 bInterval 之前的两个小端字节；缓冲按
/// kUsbEpMaxPacketSize 开，所以这两处必须和常量对得上，否则读端点会被内核
/// 判成 EOVERFLOW（改描述符却漏改常量就是这条断言要拦的错）。
static_assert(kFfsFsDescriptors[kFfsEpInIntervalOffset - 2] ==
                  (kUsbEpMaxPacketSize & 0xFF) &&
                  kFfsFsDescriptors[kFfsEpInIntervalOffset - 1] ==
                      ((kUsbEpMaxPacketSize >> 8) & 0xFF),
                  "EP1 IN wMaxPacketSize must match kUsbEpMaxPacketSize");
static_assert(kFfsFsDescriptors[kFfsEpOutIntervalOffset - 2] ==
                  (kUsbEpMaxPacketSize & 0xFF) &&
                  kFfsFsDescriptors[kFfsEpOutIntervalOffset - 1] ==
                      ((kUsbEpMaxPacketSize >> 8) & 0xFF),
                  "EP2 OUT wMaxPacketSize must match kUsbEpMaxPacketSize");

/// wDescriptorLength 必须落在 HID class descriptor 内的第 8、9 字节：前一格是
/// bDescriptorType(Report)=0x22，两字节小端值等于 DS5 数组的长度（数组里冻结的
/// 就是 DS5 那份；Edge 靠装配期覆写）。
static_assert(kFfsFsDescriptors[kFfsWDescLenOffset - 1] == kDescriptorTypeReport &&
                  (kFfsFsDescriptors[kFfsWDescLenOffset] |
                   (kFfsFsDescriptors[kFfsWDescLenOffset + 1] << 8)) ==
                      sizeof(kHidReportDescriptor),
                  "wDescriptorLength offset must point at the little-endian 337");

// ---------------------------------------------------------------------------
// 型号档案：VID/PID、产品字符串与 report descriptor 必须同族
// ---------------------------------------------------------------------------

/**
 * @brief 一套虚拟 USB 身份的完整档案：identity 加它的 report descriptor。
 *
 * 一个型号的可调外观不是几个独立参数：PID、产品字符串、report descriptor 三者
 * 必须成组切换，任何一处与其余不一致都会让主机侧的解析错位——内核
 * hid-playstation 按 VID/PID 选解析器，而字段布局由 report descriptor 决定。
 * 组合关系冻在这里，装配代码只认这一个结构。
 *
 * @note 序列号不在档案里：它是每站一个、由手柄 MAC 派生的值（见 usb::ResolveGadgetId）。
 */
struct DeviceProfile {
  DeviceIdentity identity;             ///< VID/PID 与两个字符串描述符内容。
  const uint8_t* report_descriptor;    ///< 该型号的 report descriptor 字节。
  uint16_t report_descriptor_size;     ///< 字节数，同时是 wDescriptorLength 的取值。
};

/// DS5 有线模式的档案：054c:0ce6 + 337 字节描述符，也是未识别成 Edge 时的默认。
inline constexpr DeviceProfile kDualSenseProfile = {
    kDualSenseIdentity, kHidReportDescriptor,
    static_cast<uint16_t>(sizeof(kHidReportDescriptor))};

/// DualSense Edge 的档案：054c:0df2 + 453 字节描述符。
inline constexpr DeviceProfile kDualSenseEdgeProfile = {
    kDualSenseEdgeIdentity, kHidReportDescriptorEdge,
    static_cast<uint16_t>(sizeof(kHidReportDescriptorEdge))};

/**
 * @brief 按型号取档案。
 * @param model 发现阶段判出的控制器型号。
 * @return 对应档案的常量引用。
 */
constexpr const DeviceProfile &ProfileFor(ControllerModel model) {
  return model == ControllerModel::kEdge ? kDualSenseEdgeProfile
                                        : kDualSenseProfile;
}

}  // namespace core::device
