/**
 * @file
 * @brief 测试夹具：一份真的会被 dlopen 的 .so 插件
 *
 * 它存在的唯一理由是把装载链路的最后一环也测到：目录扫描 + 标记识别 +
 * dlopen + dlsym(kPluginEntryName) + 版本核对 + init + 分派。这些环节里任何一环
 * 写错（标记节漏了、导出名拼错、描述符没按 C 布局、宿主忘了 RTLD_NOW），单测就红。
 *
 * 它只做一个动作：把交给它的每个输入状态体的首字节写成 0x5A。测试因此不需要
 * 读它的内部状态（.so 里的变量本来也读不到），看自己那份 buffer 就知道钩子有没有
 * 真的跑过。
 *
 * 同一份源码还会以不同的 DS5_PLUGIN_TEST_ABI_VERSION 再编一份（见
 * tests/CMakeLists.txt），用来证明宿主会拒绝版本号不符的 .so。
 *
 * @note 与主程序零链接关系：只 include plugin/plugin_api.h，所有能力都从宿主
 *       给的 ds5Host 拿。它连 main 都没有，也不调用任何宿主符号——正是
 *       RTLD_NOW 要当场拒绝的那种写法它才写得出来。
 */

#include "plugin/plugin_api.h"

#ifdef DS5_PLUGIN_TEST_ABI_VERSION
/// 测试编译期覆盖：导出宿主不认的版本号。
constexpr uint32_t kProbeAbiVersion = DS5_PLUGIN_TEST_ABI_VERSION;
#else
constexpr uint32_t kProbeAbiVersion = kPluginAbiVersion;
#endif

namespace {

ds5Host g_host{};
int g_ticks = 0;

int ProbeInit(ds5Host* host, const ds5Setting* /*settings*/) {
  if (host == nullptr || host->abi_version != kProbeAbiVersion) return 0;
  g_host = *host;
  g_host.log(DS5_LOG_INFO, "probe: initialised");
  return 1;
}

void ProbeStop() { g_ticks = 0; }

void ProbeTick() { ++g_ticks; }

void ProbeStateIn(uint8_t* state) { state[0] = 0x5A; }

}  // namespace

#ifndef DS5DUMMY_TEST_NO_MARKER
/// 识别标记：没有它，装载器不会 dlopen 这个文件。
__attribute__((used, section(".ds5_plugin")))
const char ds5_plugin_marker[] = "DS5 Plugin";
#endif

/// 导出名必须是 kPluginEntryName 那个字符串；这里是它的字面量写法。
extern "C" const ds5PluginDesc ds5_plugin_desc{
    kProbeAbiVersion, "probe", "1", &ProbeInit, &ProbeStop, &ProbeTick,
    &ProbeStateIn};
