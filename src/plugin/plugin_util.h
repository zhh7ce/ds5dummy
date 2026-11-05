/**
 * @file
 * @brief 插件作者可直接复用的几件小工具：查配置键、解析整数、取单调时钟
 *
 * 这些函数不属于 ABI：全部是头文件里的 inline 函数，插件编译时各自带一份，
 * 装载期不需要宿主导出任何符号（宿主本来也不导出符号，RTLD_NOW 下真去引用
 * 宿主符号的 .so 会被当场拒绝）。因此改这里的实现不会让已有的 .so 失效，
 * 只有 plugin_api.h 里的结构体布局变了才需要动版本号。
 *
 * 随包插件与第三方插件共用：只想拿 ABI 的插件只 include plugin_api.h；
 * 想用这些工具就连本头一起编进去，不需要别的依赖。
 *
 * @see plugin/plugin_api.h —— ds5Setting 的定义与 init 的契约。
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>

#include "plugin/plugin_api.h"

namespace plugin {

/**
 * @brief 在 init 收到的配置数组里找一个键。
 * @param settings 以 {nullptr, nullptr} 结尾的配置数组；可以为 null（等于没有任何键）。
 * @param key 要找的键路径，与配置文件里那一节的写法一致：一层就写
 *            `off_after_minutes`，分了组就写 `remap.circle`（段间是 '.'，数组里
 *            的键已经不含插件名前缀）。
 * @return 命中时指向值字符串（宿主持有的字符串，只在 init 期间有效）；
 *         没找到 null。
 * @note 精确匹配，没有前缀语义：`off_after_minutes_extra` 不会命中
 *       `off_after_minutes`，`remap.circle_x` 也不会命中 `remap.circle`。要按组
 *       取一批键（组内条目数不定）就直接遍历这个数组自己比前缀，别依赖数组顺序：
 *       同组条目相邻是当前实现的性质，不是契约。
 */
inline const char* FindSetting(const ds5Setting* settings, const char* key) {
  if (settings == nullptr) return nullptr;
  for (const ds5Setting* entry = settings; entry->key != nullptr; ++entry) {
    if (std::strcmp(entry->key, key) == 0) return entry->value;
  }
  return nullptr;
}

/**
 * @brief 把配置里的标量文本读成 [min_value, max_value] 内的非负整数。
 * @param text 配置文本；null 或空串都算不合法。
 * @param min_value 允许的下限（含）。
 * @param max_value 允许的上限（含）。
 * @param out 成功时写入数值；失败时不改动。
 * @return 成功 true；空串、含非数字字符或越界 false。
 * @details 逐字符判数字而不是直接 strtol：strtol 认前导空白、'+'、'-' 与
 *          "0x" 前缀，这些形态出现在插件配置里没有第二种含义，接进来只会
 *          把"配置写错"变成"看不出来的取值"。上限在循环里判，文本再长也
 *          只会在这里停，不会先回绕再比较。
 * @note 只接受非负十进制（`0`、`30`、`030`）；不接受正负号。
 */
inline bool ParseInt(const char* text, int min_value, int max_value, int& out) {
  if (text == nullptr || *text == '\0') return false;
  long long value = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + (*p - '0');
    if (value > max_value) return false;
  }
  if (value < min_value) return false;
  out = static_cast<int>(value);
  return true;
}

/**
 * @brief 单调时钟的毫秒读数。
 * @return 自时钟起点的毫秒数；只用于本进程内比较大小，不跨进程解释。
 * @note 用 steady_clock（Linux 上是 CLOCK_MONOTONIC）：挂起时间不计入，
 *       换成 CLOCK_BOOTTIME 或 system_clock 会把挂起时长也算进计时。
 * @note 宿主不提供时间：要计时的插件从这里现取，精度因此不取决于 on_tick
 *       被调用的节拍。
 */
inline uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace plugin
