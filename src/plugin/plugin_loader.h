/**
 * @file
 * @brief plugin 模块内部：装载机制——目录扫描、标记识别、.so 打开、扁平配置转数组
 *
 * 四样纯机制，没有策略：谁该被装载、装载进来算不算成功、日志怎么说，都在
 * plugin.cpp 的装载路径里；这里只回答"这个目录里有哪些候选 .so"、"这个文件是不是
 * 我们的插件"、"这个路径能不能给它一个描述符指针"、"这份 map 怎么变成 C 数组"。
 *
 * 因此本文件不碰实例表、不调用任何插件的钩子，也不核对版本号（那是
 * plugin.cpp 装载路径的事）。
 *
 * @note 只在 plugin 模块内部使用，不进门面。
 * @see plugin/plugin.cpp —— 唯一调用方。
 */

#pragma once

#include <string>
#include <vector>

#include "plugin/plugin.h"

namespace plugin {
namespace detail {

/**
 * @brief 列出一个目录里的候选插件文件。
 * @param dir 要扫描的目录；不存在、不是目录或打不开时返回空表。
 * @return 目录里以 `.so` 结尾的普通文件（跟随符号链接）的完整路径，按文件名排序。
 * @details 排序只为了结果不取决于文件系统的返回顺序：同一套目录内容，装载顺序
 *          与日志顺序每次都一样。非 `.so` 文件、子目录与指向目录的链接一律略过。
 */
std::vector<std::string> ListPlugins(const std::string& dir);

/**
 * @brief 判断一个 .so 是不是按本头写的插件（不执行它的任何代码）。
 * @param path .so 的路径。
 * @param why 不是插件时写入原因（不是 ELF、没有标记节、标记值不对）；是插件时
 *            不修改。
 * @return ELF 节表里有 `.ds5_plugin` 节且内容逐字节等于 kPluginMarkerMagic 时
 *         true（字符串连结尾 NUL 一起比）。
 * @details 只看文件字节，不 dlopen：目录里放错的东西不会被跑起来，静态构造器也
 *          不会执行。只认 64 位小端 ELF（本项目的构建目标），其余形态按"不是
 *          我们的插件"处理，原因写进 @p why。
 */
bool HasMarker(const std::string& path, std::string& why);

/**
 * @brief 打开一个 .so 并取出它的描述符。
 *
 * 用 `RTLD_NOW | RTLD_LOCAL`：符号在装载这一刻全部解析完，所以一个直接引用宿主
 * C++ 符号（比如想够 bridge::PowerOff）的插件会在 dlopen 时就失败，而不是等到
 * 某次回调里踩空；LOCAL 让这个 .so 也看不见别的插件的符号，插件之间只能靠
 * 宿主给的 ds5Host 对话。找到描述符之后**不** dlclose：实例表里的指针要一直
 * 有效，而一个可能自己起过线程的代码段，卸载比留着危险。
 *
 * @param path 目录扫描给出的绝对或相对路径。
 * @param why 失败时写入 dlerror() 的文本或本函数自己给的原因；成功时不修改。
 * @return 描述符指针（版本号由调用方核对）；打不开、没有那个导出符号或符号为
 *         空时 nullptr。
 */
const ds5PluginDesc* Open(const std::string& path, std::string& why);

/**
 * @brief 把某个插件的配置转成 ds5PluginDesc::init 要的那个数组。
 * @param settings 全部插件的扁平配置（键是 `插件名.键路径`，按扁平键有序）。
 * @param name 当前要装载的插件名。
 * @return 指向 @p settings 的 `{键, 值}` 数组，键已剥掉 `名字.` 前缀，末尾一项
 *         是 `{nullptr, nullptr}`。指针指向 @p settings 里的文本，所以 @p settings
 *         必须在 init 调用期间一直有效（插件契约也只允许它用到 init 返回）。
 * @details 只取键带 `name.` 前缀的条目，并按前缀长度一次剥掉：插件的 C 边界看到
 *          的是剩下的整串键路径（**包括里面那些点**，如 `remap.circle`），不知道
 *          宿主用哪种容器摊平它们、也不知道配置文件里分了几层。插件名段里不含
 *          '.'（plugin::IsToken），所以按这一条前缀筛不会串到别的插件；配置文件
 *          里同组的条目剥完前缀后仍共享同一个段前缀，@ref Settings 的有序遍历
 *          把它们相邻放着——但相邻不是契约，插件要按组取键时应当自己扫整串数组。
 */
std::vector<ds5Setting> BuildSettings(const Settings& settings,
                                      const std::string& name);

}  // namespace detail
}  // namespace plugin
