/**
 * @file daemon/discovery.h
 * @brief 在 sysfs 里找出已经建立 HID 会话的 DualSense，供 Manager 分发给 worker
 * @details Manager 需要知道"系统里现在有哪些手柄、各自的 MAC 是什么"，才能
 *          给每只起一个 worker。两个后端都先由 BlueZ 建起 HID 会话（hidraw 后端
 *          一直用它，L2CAP 后端接管 PSM 之后内核会话会消失），而在那之前
 *          /sys/class/hidraw 下已经有节点，节点里带总线号、VID/PID 与 BD_ADDR
 *          ——这就是本模块唯一的输入。它回答的是"现在该为谁起站"，不是"链路是否
 *          一直可用"。
 * @details 不负责：判断手柄是否可用（节点打不开、链路断开由 worker 自己发现）、
 *          按配置过滤（Manager 拿结果与 UserConfig 比对）、去重服务（Manager
 *          用"已服务的 MAC 集合"判断）。
 * @note 层次：daemon。线程模型：纯函数，无全局状态；Manager 每轮轮询时调用。
 * @see daemon/config.h —— MAC 的合法形式与大写归一。
 */

#pragma once

#include <string>
#include <vector>

namespace ds5::daemon {

/// sysfs 里 hidraw 类的挂载点。测试传入一个临时目录来跑同一套解析。
inline constexpr const char* kHidrawSysfsBase = "/sys/class/hidraw";

/**
 * @brief 一次发现的结果：一只手柄的身份与它的节点名。
 * @note hidraw 节点名只用于日志；worker 自己按 MAC 重新找节点，不做 fd 交接。
 */
struct DiscoveredController {
  /// 规范大写 BD_ADDR（如 "AA:BB:CC:DD:EE:FF"）。
  std::string mac;
  /// hidraw 节点名（如 "hidraw3"），不带目录。
  std::string node;
};

/**
 * @brief 扫描 hidraw 类目录，返回每只有 HID 会话的 DualSense 系手柄。
 *
 * 认定条件（逐节点读 device/uevent）：
 * - HID_ID 的总线字段是 0x0005（Bluetooth HID 总线），排除 USB 直连的手柄；
 * - VID/PID 命中 core::device::IsDs5HidIdentity：真机 DualSense（054C:0CE6）
 *   或 Edge（054C:0DF2），以及 DKMS 补丁合成的 1998:1221 / 1998:1103；
 * - HID_UNIQ 是合法的 XX:XX:XX:XX:XX:XX 形式——缺了它就拿不到 MAC，无从分发。
 *
 * @param sysfs_base hidraw 类目录（生产用 kHidrawSysfsBase，测试用临时目录）。
 * @return 按 MAC 升序排列的结果；同一 MAC 的多个节点只保留节点名最小的那个。
 *         目录不存在或一只都没匹配时返回空列表（正常状态，不报错）。
 * @note 有磁盘 I/O，只在 Manager 的轮询线程调用。节点可能在被读的同时消失，
 *       任何单节点的读取失败都只跳过该节点。
 */
std::vector<DiscoveredController> ScanControllers(const std::string& sysfs_base);

}  // namespace ds5::daemon
