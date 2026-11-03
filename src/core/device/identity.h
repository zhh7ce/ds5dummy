/**
 * @file identity.h
 * @brief 设备身份常量：虚拟 USB 设备可呈现的两种静态身份，以及识别真蓝牙手柄的 VID/PID 名单
 *
 * VID/PID、厂商/产品字符串在编译期固定为真机的 Sony DualSense 身份；主机
 * enumeration 时由 usb 模块按本站选定的型号原样呈报。型号由发现阶段的 HID
 * VID/PID 判定（见 `ModelForHidIdentity`），身份与描述符的配对在
 * `core/device/descriptors.h` 的 `DeviceProfile` 里给出——**身份不是运行时自由
 * 配置**：可取的只有 `kDualSenseProfile` 与 `kDualSenseEdgeProfile` 两份，
 * 每一份的 PID、产品字符串与 HID report 描述符是一个整体，不能混搭。
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
// - 虚拟 USB 设备呈现哪一条由本站型号决定（见 kDualSenseProfile /
//   kDualSenseEdgeProfile）；
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

/**
 * @brief 本站要呈现的手柄型号：决定虚拟 USB 设备的身份与 HID report 描述符。
 * @details 只有这两个取值，对应两份编译期常量 `DeviceProfile`；型号不是自由
 *          配置项，它跟着真手柄走（见 `ModelForHidIdentity`）。
 * @see core/device/descriptors.h 的 ProfileFor —— 由型号取那一整份 profile。
 */
enum class ControllerModel {
  kDualSense,  ///< 普通 DualSense：054c:0ce6 + 337 字节 report 描述符。
  kEdge,       ///< DualSense Edge：054c:0df2 + 453 字节 report 描述符。
};

/**
 * @brief 由 HID 的 VID/PID 判定手柄型号。
 * @param vid sysfs `HID_ID` 的 VID 段。
 * @param pid sysfs `HID_ID` 的 PID 段。
 * @return Edge 的两组身份之一命中时 `kEdge`，否则 `kDualSense`。
 * @warning 入参必须已经过 `IsDs5HidIdentity`：本函数只区分型号，不把"不认识
 *          的 PID"报成错误——判定与筛检分两处，是因为筛检发生在发现阶段，
 *          而发现阶段之后所有人都只关心"是哪一型"。
 * @note 真机 `054c:0df2` 与 DKMS 合成的 `1998:1103` 是同一只 Edge 的两种写法：
 *       装 in-tree `hid_playstation` 的那条打包路径（l2cap 包）在 sysfs 里读到
 *       前者，装本项目 DKMS 补丁的那条（hidraw 包）读到后者——补丁会把蓝牙手柄
 *       的 `hdev->product` 换成合成 PID。两个名字都必须认，否则同一只 Edge 在
 *       一个包上呈现成 Edge、在另一个包上呈现成普通手柄。
 */
constexpr ControllerModel ModelForHidIdentity(uint32_t vid, uint32_t pid) {
  const bool edge = (vid == kSonyVid && pid == kDs5EdgePid) ||
                    (vid == kDs5DummyVid && pid == kDs5DummyEdgePid);
  return edge ? ControllerModel::kEdge : ControllerModel::kDualSense;
}

/// USB 厂商字符串（String Descriptor），与真机 iManufacturer 一致。
inline constexpr char kManufacturer[] = "Sony Interactive Entertainment";
/// USB 产品字符串，与真机 iProduct 一致。
inline constexpr char kProduct[] = "DualSense Wireless Controller";
/// Edge 的产品字符串，与上游 DS5Dongle 呈现 Edge 时用的字面量相同
/// （`usb_descriptors.cpp` 的 `tud_descriptor_string_cb`）。
inline constexpr char kProductEdge[] = "DualSense Edge Wireless Controller";

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
 * @brief 普通 DualSense 的设备身份：复刻真机有线模式。
 * @note 0x054c:0x0ce6 是 Sony DualSense (USB) 的在册 VID/PID；改这组数字会
 *       改变主机侧驱动匹配行为，不是可调的外观参数。
 */
inline constexpr DeviceIdentity kDualSenseIdentity = {kSonyVid, kDs5Pid,
                                                      kManufacturer, kProduct};

/**
 * @brief DualSense Edge 的设备身份。
 * @note 0x054c:0x0df2 是在册的 Edge USB PID；主机侧的 hid-playstation 按它把
 *       设备认成 Edge，并因此走 `use_vibration_v2` 那条震动路径（in-tree 与
 *       本项目 DKMS 补丁都是这个条件）。PID 与 `kProductEdge`、453 字节的
 *       report 描述符必须成对使用，配对点在 `core/device/descriptors.h` 的
 *       `kDualSenseEdgeProfile`。
 */
inline constexpr DeviceIdentity kDualSenseEdgeIdentity = {
    kSonyVid, kDs5EdgePid, kManufacturer, kProductEdge};

}  // namespace core::device
