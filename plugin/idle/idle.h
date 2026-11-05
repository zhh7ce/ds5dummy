/**
 * @file
 * @brief 随包的 idle 插件——多久没人碰手柄就请它自己关机
 *
 * 它是第一个按 plugin_api.h 的契约写出来的插件，也是那份契约的样例：进程级单例，
 * 只用 on_tick/on_state_in 两个钩子；动作只用宿主给的 power_off，不引用宿主的
 * 任何符号。源码放在仓库根的 plugin/ 下（不在 src/ 里），只 include 公共 ABI 头，
 * 因此不依赖宿主程序就能单独编译成 idle.so。
 *
 * 判据（与上游 DS5Dongle 的 inactivity 检查同一口径）：四个摇杆各自留在中心带
 * [120, 140]、两个扳机的模拟量为 0、十字键是 hat null、其余按键全未按。触摸板
 * 接触与六轴不参与判定：手放在触摸板上、或手柄被挪动都不算"用户在操作"，
 * 静置时的传感器噪声也不该让计时永远不触发。
 *
 * 配置只读一个键 off_after_minutes，取值 [0, 60]，缺省 kDefaultOffMinutes；
 * 0 = 从不自动关机（init 直接返回 0，插件这一轮不生效）。
 * 写了却不认识的键忽略，取值不合法时记 WARNING 并回到缺省。
 *
 * @note 本模块只被 CMake 的 ds5dummy_idle 目标与 idle 单测编译，主程序不
 *       include 它——装进插件目录后由宿主自动装载，配置里只写可选的其他键。
 * @see plugin/plugin_api.h —— 它遵守的那份契约。
 */

#pragma once

#include "plugin/plugin_api.h"

namespace plugin {
namespace idle {

/// 这个插件的完整描述符；idle.so 的导出名 ds5_plugin_desc 指向同一份对象。
extern const ds5PluginDesc& kDesc;

/// off_after_minutes 的缺省值（分钟）：配置没写这个键时就用它。
inline constexpr int kDefaultOffMinutes = 30;

#ifdef DS5DUMMY_TESTING

namespace detail {

/**
 * @brief 仅测试用：把"上次有人操作"拨到 @p ms 毫秒之前。
 * @param ms 要回拨的毫秒数。
 * @details 插件自己取时钟，测试没法注入假时间；这个函数直接改那份时间戳，让
 *          到期判定在毫秒内就能跑完，而不是真等 30 分钟。只在定义了
 *          DS5DUMMY_TESTING 的构建（测试目标）里存在。
 */
void PretendIdleFor(uint64_t ms);

}  // namespace detail

#endif  // DS5DUMMY_TESTING

}  // namespace idle
}  // namespace plugin
