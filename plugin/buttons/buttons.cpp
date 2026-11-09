/**
 * @file
 * @brief buttons 插件的装配层：描述符、静态存储和四个槽的函数体
 *
 * 这一层只做接线：配置交给 settings 解析成规则表，收到的状态体交给 ids 解码，
 * 判定交给 sense，写回交给 pipeline。字节布局在 ids，语法在 settings，时序判据
 * 在 sense，表算法在 pipeline；本模块自己不含任何一种，换掉任何一层都不动它。
 *
 * 线程相位决定了这里不需要任何锁：g_rules 与 g_state 只在 init 里写（worker 主
 * 线程、两条传输线程起来之前），之后只在 bt 的读线程上被读写，主循环一个字节都
 * 不碰。唯一的跨线程量是 g_power_off_pending：识别在输入钩子上（K-07 要求读帧），
 * 执行必须在心跳上（K-10 不许在读线程上同步碰控制通道），于是一个布尔位就是这两
 * 条线程之间全部的信息。
 */

#include <atomic>
#include <cstdint>
#include <string>

#include "ids.h"
#include "pipeline.h"
#include "plugin/plugin_api.h"
#include "plugin/plugin_util.h"
#include "rules.h"
#include "sense.h"
#include "settings.h"

namespace buttons {
namespace {

/// 配置解析出来的规则表：init 写满，运行期每帧只读。
rules::Set g_rules;

/// 跨帧的判定状态（触发闸门、连点相位、宏脉冲、方向迟滞）：只在 bt 读线程上动。
sense::State g_state;

/// 宿主能力表的副本：init 交来的那一份只在这次调用期间有效。
ds5Host g_host{};

/// 已经武装；init 置位、stop 清零，两个钩子都在自己开头看它。
bool g_armed = false;

/// 这一帧命中了关机动作，等心跳去执行（K-10）。
std::atomic<bool> g_power_off_pending{false};

/**
 * @brief 读配置、填规则表、按规则定容判定状态、武装。
 * @return 1 = 生效；0 = 这一轮不生效（ABI 版本不是这份头编译出来的、或整节没有
 *         可解析的规则），宿主自己打那一行 "not armed"。
 */
int Init(ds5Host* host, const ds5Setting* settings) {
  // 版本对不上就一个字节都不读，也没法日志——这里唯一能做的就是不武装。
  if (host == nullptr || host->abi_version != kPluginAbiVersion) return 0;
  if (!settings::Load(settings, g_rules, *host)) return 0;

  g_host = *host;
  g_power_off_pending.store(false);
  sense::Reset(g_state, g_rules);
  g_armed = true;
  host->log(DS5_LOG_INFO,
            ("buttons: plugins: buttons: armed with " +
             settings::Summary(g_rules))
                .c_str());
  return 1;
}

/// 解除武装；之后两个钩子都直接返回。规则与判定状态留给下一次 init 整份覆盖。
void Stop() {
  g_armed = false;
  // 攒着的那次关机请求不能活过这一轮：Stop 之后没有任何东西会再执行它，而重新
  // Start 时它会把一次早已过去的按下变成一条 0x08 报告。
  g_power_off_pending.store(false);
}

/**
 * @brief 每帧的入口：解码成快照、判定、把结果作用回那份缓冲。
 * @param state 手柄这一帧报的 63 字节状态体，可写。
 */
void OnStateIn(uint8_t* state) {
  if (!g_armed) return;
  const ids::Frame frame = ids::DecodeFrame(state);
  const sense::FrameDecision decision =
      sense::Evaluate(g_rules, frame, plugin::NowMs(), g_state);
  pipeline::Apply(g_rules, decision, frame, state);
  if (decision.power_off) {
    g_power_off_pending.store(true, std::memory_order_relaxed);
  }
}

/**
 * @brief 心跳：把上一帧识别出的关机动作执行掉。
 * @details 取走即清空，失败也不放回去（K-11）：想再要一次动作得重新按那组键，
 *          所以链路坏了也不会每 100 ms 撞一次控制通道。
 */
void OnTick() {
  if (!g_armed) return;
  if (!g_power_off_pending.exchange(false, std::memory_order_relaxed)) return;
  if (g_host.power_off() == 0) {
    g_host.log(DS5_LOG_WARNING,
               "buttons: plugins: buttons: a shortcut asked the controller to "
               "power off but the report was not sent; pressing the same keys "
               "again is what re-arms it");
  }
}

}  // namespace

/// 装载器的识别标记：这个节存在且内容匹配，宿主才会把它当成插件去 dlopen。
__attribute__((used, section(".ds5_plugin")))
const char ds5_plugin_marker[] = "DS5 Plugin";

/// 装载点按 kPluginEntryName 这个 C 名字 dlsym 到这里。
extern "C" const ds5PluginDesc ds5_plugin_desc{
    kPluginAbiVersion, "buttons", "1", &Init, &Stop, &OnTick, &OnStateIn};

}  // namespace buttons
