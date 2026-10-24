/**
 * @file gadget_setup.h
 * @brief configfs 装配的虚拟 USB gadget（dummy_hcd 总线上的 bring-up）
 *
 * 在 dummy_hcd 提供的虚拟 USB 总线上创建一个 DualSense 伪装 gadget：建
 * configfs 树、挂 FunctionFS、绑/解 UDC、拆机清理。属于 usb 模块的 root 侧
 * 机制层，收在 namespace usb::detail 里：生产调用方只有门面 usb.cpp 的
 * Provision/Teardown，tools/ 下的诊断程序是有意绕过门面直接驱动本文件的。
 *
 * @note 这些函数只在 root 权限下有效（写 configfs、mount）；worker 侧认领
 *       交接来的端点 fd 即可，不碰本文件任何接口。
 * @note 拆机刻意分成 GadgetUnbind 与 GadgetPurgeConfigfs 两级：Manager 需要
 *       先解绑、再关掉自己手里的端点 fd，两级之间有空档。
 * @note 本文件没有任何"记住上一次调用"的状态：gadget 目录名/ffs 实例名与
 *       FunctionFS 挂载点都由调用方每次现传，所以同一进程可以并行持有多个
 *       gadget（Manager 为每只手柄各建一站），拆机时传回同一组坐标即可。
 */

#pragma once

#include <cstdint>
#include <string>

namespace usb {
namespace detail {

/**
 * @brief configfs 里 gadget 目录的父目录，内核固定的挂载点。
 *
 * 绕过门面直接写属性的诊断工具（tools/mini_gadget）按
 * `kConfigfsBase / kGadgetInstance / <属性>` 拼路径，基址不在第二处重复。
 */
inline constexpr const char* kConfigfsBase = "/sys/kernel/config/usb_gadget";

/**
 * @brief 诊断与脚本使用的 gadget 实例名。
 *
 * 生产路径上的实例名由 Manager 按手柄 MAC 派生（`ds5dummy-<12 位十六进制>`），
 * 因为一台 Manager 可能要同时持有多个 gadget，configfs 目录名不能重复；
 * `tools/mini_gadget` 与 `scripts/` 只建一站，仍用这个固定的名字。
 */
inline constexpr const char* kGadgetInstance = "ds5dummy";

/**
 * @brief 建 configfs gadget 树并把 FunctionFS 挂到 ffs_path。
 *
 * 流程：白名单校验 → mount configfs（已挂载只读时 remount 成可写）→
 * 清残留 → 建 gadget/字符串/function/config 各级目录并写属性 → mount
 * functionfs → 符号链接把 function 挂进 configuration。调用后 gadget 尚未
 * attach 到 USB 核。
 *
 * @param ffs_path FunctionFS 挂载点目录，会直接进 mkdir/mount，须先过门面的
 *                 IsSafeGadgetPath；本函数按白名单再挡一遍。
 * @param instance configfs 里的 gadget 目录名，同时是 ffs function 的实例名
 *                 （functions/ffs.<instance>）与 FunctionFS 的挂载源。调用方
 *                 保证它在所有活跃 gadget 之间唯一；字符集须能过
 *                 IsSafeGadgetPath。
 * @param serial 写进 `strings/0x409/serialnumber` 的 iSerialNumber 内容。主机
 *               侧把它当作设备的实例标识，所以多站必须各用不同值（生产路径由
 *               ResolveGadgetId 按 MAC 派生）；空串会让内核在 bind 时拒绝缺
 *               序列号的部分配置，调用方不该传。
 * @return 成功 true；中途失败时已创建的部分留在现场，由下一次 Setup 的
 *         残留清理或 GadgetTeardown 负责。
 * @see GadgetBind —— 描述符就位后才允许的下一步。
 */
bool GadgetSetup(const std::string& ffs_path, const std::string& instance,
                 const std::string& serial);

/**
 * @brief 把 gadget attach 到一个空闲的 dummy_hcd UDC（写 UDC 属性）。
 *
 * 必须在 FunctionFS 描述符写入之后调用：function 报告"未就绪"时内核以
 * ENODEV 拒绝 bind；失败时会读回 ffs.<instance>/ready 并把线索指向内核日志。
 *
 * @param instance GadgetSetup 用过的实例名。
 * @return 绑定成功 true；没有空闲 dummy_udc 时也返回 false（先于写属性）。
 * @details "空闲"按 configfs 现状判定：已被别的 gadget 绑走的 UDC 不再分配，
 *          所以 N 只手柄各占一个 dummy_udc.N。这要求 dummy_hcd 的 num_dummy
 *          不小于同时服务的手柄数。
 */
bool GadgetBind(const std::string& instance);

/**
 * @brief 从一个 UDC 的 sysfs 符号链出发，找到它所在主机控制器下已枚举的 USB
 *        设备目录（单次尝试，不等待）。
 *
 * 路径形状是 dummy_hcd 固定的：`/sys/class/udc/dummy_udc.N` 符号链指向
 * `/sys/devices/platform/dummy_udc.N/udc/dummy_udc.N`，上溯两级得到的是 UDC
 * 自己的平台设备；root hub 挂在**同索引的兄弟平台设备** `dummy_hcd.N` 下面
 * （模块加载时同一个 i 各注册一个），它是那层里唯一的 `usbM` 子目录，我们那台
 * 虚拟设备是 root hub 的孩子。VID/PID 在 root hub 的直接孩子里匹配——root hub
 * 自身的身份是内核 HCD 的，不会撞。
 *
 * @param udc_sysfs_path `/sys/class/udc/<控制器名>` 的**已规范化**绝对路径。
 *        必须是 canonical 结果：符号链本身不能用于上溯父目录之外的扫描。
 * @param vendor_id 本站呈报给主机的 USB VID。
 * @param product_id 本站呈报给主机的 USB PID。
 * @return 命中的设备目录绝对路径；尚未枚举出来、目录不可读或身份不符时返回
 *         空串（空串是"再试一次"的信号，不是错误）。
 * @note 只读 sysfs，无副作用；调用方负责轮询与超时预算。
 * @see FindHostDevicePath —— 把 configfs 里的 UDC 属性换算成本函数的入参。
 */
std::string FindHostDeviceDir(const std::string& udc_sysfs_path,
                              uint16_t vendor_id, uint16_t product_id);

/**
 * @brief 在已枚举出来的设备目录里取一个 USB 接口子目录（绝对路径）。
 *
 * 设备目录（`…/usb9/9-1`）自己就是那个 `usb_device`，而沿父链回溯的读者是从它的
 * **父级**开始找的，所以把设备目录交出去只会命中 root hub。接口目录（`9-1:1.0`）
 * 往上一段就是本站设备，是这棵树上最浅的、父链正确的目录，
 * 也是本项目能给出的与真声卡 `…/9-1/9-1:1.3/sound/card1` 最同形的东西。
 *
 * @param device_dir FindHostDeviceDir 的绝对结果，形如
 *        `/sys/devices/platform/dummy_hcd.0/usb9/9-1`。
 * @return 字典序最小的接口目录绝对路径；没有接口目录（枚举还没走完）或入参
 *         非法时返回空串，空串对调用方是"稍后再试"。
 * @note 只读 sysfs，无副作用，单次尝试不等待。本站描述符只声明一个接口（HID），
 *       所以正常就只有一个 `9-1:1.0`；多接口时取哪一个算出的父设备都相同——选
 *       最小只为让结果跨重启稳定，不承载身份。
 * @see FindHostDevicePath —— 唯一的调用方；它把结果换成相对 /sys 的形态再交出。
 */
std::string FindHostInterfaceDir(const std::string& device_dir);

/**
 * @brief 反查本站在主机侧被枚举出来的 USB **接口**目录（单次尝试，不等待）。
 *
 * 三步：读回 configfs 的 `UDC` 属性（GadgetBind 写进去的控制器名，本站独有），
 * 规范化 `/sys/class/udc/<名>` 后交给 FindHostDeviceDir，再降到它下面的接口目录
 * （FindHostInterfaceDir）。之所以不遍历整个 `/sys/bus/usb/devices`：同一身份
 * （054c:0ce6）的设备可能不止一台在册，而 UDC 名字只属于这一站。
 *
 * @param instance 已 bind 的 gadget 实例名。
 * @param vendor_id 本站呈报给主机的 USB VID。
 * @param product_id 本站呈报给主机的 USB PID。
 * @return 接口目录路径，**去掉 `/sys` 前缀**（形如
 *         `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`）——开头带 `/`、
 *         相对 `/sys`，与 PipeWire 真设备上 `device.sysfs.path` 的写法同形，
 *         可直接透传；没枚举出来、身份不符或接口未出现返回空串。
 * @note 只读 sysfs 与 configfs，无副作用；bind 之后设备目录与接口目录都是内核
 *       异步建出来的，调用方需要带预算地重试。
 * @see FindHostDeviceDir、FindHostInterfaceDir。
 */
std::string FindHostDevicePath(const std::string& instance, uint16_t vendor_id,
                               uint16_t product_id);

/**
 * @brief 完整拆机：先 GadgetUnbind 再 GadgetPurgeConfigfs。
 * @param ffs_path 本轮运行用过的 FunctionFS 挂载点，detach 它。
 * @param instance 本轮的 gadget/ffs 实例名。
 */
void GadgetTeardown(const std::string& ffs_path, const std::string& instance);

/**
 * @brief 条件式解绑：只在确实还绑着 UDC 时写 UDC="\n"。
 *
 * f_fs 在最后一个端点 fd 关闭时会自行解绑，此时属性已是空串；"本来就没
 * 绑"与写时撞上 ENODEV 都按成功处理，避免把幂等操作报成故障。
 *
 * @param instance 本轮的 gadget 实例名。
 * @return 当前无绑定或解绑成功 true；其余 false。
 */
bool GadgetUnbind(const std::string& instance);

/**
 * @brief detach FunctionFS 挂载并按 rmdir 次序拆除 configfs 各级目录。
 *
 * 必须在 GadgetUnbind 之后运行：configfs 拒绝删除仍绑定的 gadget；
 * configfs group 只能 rmdir，其属性文件不可 unlink，递归删除会以 EPERM 失败。
 *
 * @param ffs_path 待 detach 的 FunctionFS 挂载点。
 * @param instance 本轮的 gadget/ffs 实例名。
 */
void GadgetPurgeConfigfs(const std::string& ffs_path,
                         const std::string& instance);

/**
 * @brief 在 /sys/class/udc 下查找名为 dummy_udc.N 的虚拟 UDC。
 * @return 首个 UDC 名（如 "dummy_udc.0"）；目录不存在或没有匹配项时返回空串。
 * @note 只回答"dummy_hcd 有没有起来"，不关心它是否已被占用；分配由
 *       GadgetBind 自己挑空闲的那个。
 */
std::string FindDummyUdc();

}  // namespace detail
}  // namespace usb
