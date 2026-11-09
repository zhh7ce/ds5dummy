/**
 * @file
 * @brief 配置文本 → 规则表：插件配置那一节的全部语法
 *
 * settings 不认识 63 字节的布局（那是 ids），也不做判定（那是 pipeline）。它
 * 唯一的生产物是 rules::Set。
 */

#pragma once

#include <string>

#include "plugin/plugin_api.h"
#include "rules.h"

namespace buttons {
namespace settings {

/**
 * @brief 读 init 交来的配置数组，把能认的条目变成规则。
 * @param settings 本插件的配置项（键是段间带点的键路径，如 remap.circle），以
 *                 {nullptr, nullptr} 结尾；可以为 null。
 * @param out 先清空再填；只收合法的条目。
 * @param host 只用来记日志：每一条被拒的取值都在这里有对应的一行 WARNING。
 * @return out 里至少有一条规则时 true。false 只有一种含义：整节没有可解析的内容
 *         （C-04），调用方据此决定武装还是不武装。
 * @details 单条不合法**不**返回失败：那一条不生效、其余照常（G-06）。不认识的
 *          组名与不认识的键一律忽略（C-05），只有取值本身读不通时才记 WARNING。
 * @note 分派靠"遍历数组 + 比组前缀"，不靠精确查键：摊平后交给插件的键本身就带
 *       点，而组内条目数不定。数组不保证按组连续，所以也不假设相邻。
 */
bool Load(const ds5Setting* settings, rules::Set& out, const ds5Host& host);

/**
 * @brief 一份规则表的组成：每组各有多少条生效。
 * @param rules 已经解析好的规则表。
 * @return 形如 "2 remap(s), 1 axis rewrite(s), 0 turbo(s), 0 shortcut(s),
 *         0 macro(s)" 的单行文本。
 * @details 轴改写不进任何 vector：只写一句 `invert_rstick_x` 的用户在这行里看不到
 *          东西，就会以为那句话没被读进去，所以轴单独计数，判据是
 *          AxisRewrite::IsIdentity。
 * @note 计数口径与 Set::Empty 一致：五项都是 0 时这张表什么也不会做。武装摘要由
 *       插件本体与配置校验工具共用这一份，两处不会各写一版。
 */
std::string Summary(const rules::Set& rules);

}  // namespace settings
}  // namespace buttons
