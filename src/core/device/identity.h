/**
 * @file identity.h
 * @brief 设备身份常量：虚拟 USB 设备的静态身份，以及识别真蓝牙手柄的 VID/PID 名单
 *
 * VID/PID、厂商/产品字符串在编译期固定为真机的 Sony DualSense
 * 身份；主机 enumeration 时由 usb 模块原样呈报。身份目前是编译期常量而非
 * 运行时配置——将来需要伪装成其它设备时，再从本文件抽成可注入的配置对象。
 *
 * 序列号不在这里：它是**每站一个**的值，由手柄 MAC 派生（见
 * `usb::ResolveGadgetId`）。多站共用同一个序列号会让主机把它们当成同一台
 * 设备反复出现，所以本文件只放真正全站相同的字段。
 */

#pragma once

#include <cstdint>

namespace core::device {

// 设备身份里的 VID/PID 数值：
// - 真机两条（DualSense / Edge）用于发现与 hidraw 后端认出蓝牙手柄；
// - 虚拟 USB 设备只呈现普通 DualSense（kDualSenseIdentity），不区分型号；
// - 合成两条只用于本项目 DKMS 补丁给真手柄造出的 hidraw 节点。
/// Sony 的 USB VID。
inline constexpr uint16_t kSonyVid = 0x054C;
/// DualSense 的 USB PID（蓝牙 HID 身份同为 0x0CE6）。
inline constexpr uint16_t kDs5Pid = 0x0CE6;
/// DualSense Edge 的 USB PID（蓝牙 HID 身份同为 0x0DF2）。
inline constexpr uint16_t kDs5EdgePid = 0x0DF2;
/// DKMS 补丁给真蓝牙手柄合成的 VID，避免与 in-tree hid_playstation 抢驱动。
inline constexpr uint16_t kDs5DummyVid = 0x1998;
/// 与 kDs5DummyVid 成对的合成 PID：DualSense。
inline constexpr uint16_t kDs5DummyPid = 0x1221;
/// 与 kDs5DummyVid 成对的合成 PID：DualSense Edge。
inline constexpr uint16_t kDs5DummyEdgePid = 0x1103;

/**
 * @brief 是否为本项目服务的手柄 HID 身份：真机或 DKMS 合成，DualSense 或 Edge。
 * @param vid HID 身份里的 VID。
 * @param pid HID 身份里的 PID。
 * @return 命中 true。
 * @note 只判 VID/PID 组合，不判总线；调用方仍需自己过滤蓝牙总线（0x0005）。
 *       参数用 uint32_t 是为了直接接 sysfs 解析结果：先截成 16 位再比较，
 *       会让越界的数字恰好截成真机 VID 而误判。
 */
constexpr bool IsDs5HidIdentity(uint32_t vid, uint32_t pid) {
  const bool sony = vid == kSonyVid && (pid == kDs5Pid || pid == kDs5EdgePid);
  const bool synthesized = vid == kDs5DummyVid &&
                           (pid == kDs5DummyPid || pid == kDs5DummyEdgePid);
  return sony || synthesized;
}

/// USB 厂商字符串（String Descriptor），与真机 iManufacturer 一致。
inline constexpr char kManufacturer[] = "Sony Interactive Entertainment";
/// USB 产品字符串，与真机 iProduct 一致。
inline constexpr char kProduct[] = "DualSense Wireless Controller";

/**
 * @brief 一台虚拟 USB 设备的静态身份：VID/PID 加两个字符串描述符内容。
 *
 * 全成员均为 POD/指向 constexpr 字符串的指针，可 constexpr 聚合初始化，
 * 因此既可以常量形式存放（kDualSenseIdentity），也可按值传给 gadget
 * 描述符组装代码，无生命周期约束。序列号刻意不在这里——它按站派生。
 */
struct DeviceIdentity {
  uint16_t vendor_id;        ///< USB VID，主机侧驱动匹配的第一要素。
  uint16_t product_id;       ///< USB PID，与 VID 成对决定 hid 驱动绑定。
  const char* manufacturer;  ///< 厂商字符串，UTF-8 字面量，entry 指向静态存储。
  const char* product;       ///< 产品字符串，同上。
};

/**
 * @brief 本项目对外呈现的唯一设备身份：复刻真机 DualSense。
 * @note 0x054c:0x0ce6 是 Sony DualSense (USB) 的在册 VID/PID；改这组数字会
 *       改变主机侧驱动匹配行为，不是可调的外观参数。
 */
inline constexpr DeviceIdentity kDualSenseIdentity = {kSonyVid, kDs5Pid,
                                                      kManufacturer, kProduct};

}  // namespace core::device
