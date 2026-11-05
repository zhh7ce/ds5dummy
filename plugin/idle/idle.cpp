/**
 * @file
 * @brief idle 插件的实现：静止判据、无操作计时与到期时那一次 power_off
 *
 * 插件是进程级单例，状态直接放文件作用域的静态量：一份宿主能力表副本、一个期限、
 * 一个"上一次有人操作"的单调毫秒数、一个 armed 标志。on_state_in 在 bt 的读线程
 * 上写那个毫秒数，on_tick 在 worker 主循环上读它，两者之间只有这一个共享量，
 * 所以它是原子量而没有锁；其余量都在线程相位里写完再读（init 早于线程、stop
 * 晚于 join）。
 *
 * 时钟自己取（steady_clock），报告出字节走宿主给的 power_off：插件因此不认识
 * bridge、bt 或任何报告布局，一个 .so 插件能拿到的是同一组能力。
 */

#include "idle.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "plugin/plugin_api.h"
#include "plugin/plugin_util.h"

namespace plugin {
namespace idle {
namespace {

/// 本插件在配置里读的键名与它的上限；下限是 0，0 = 从不自动关机。
constexpr const char* kOffMinutesKey = "off_after_minutes";
constexpr int kOffMinutesMax = 60;

/// 摇杆中心带：上游 DS5Dongle 的 inactivity 检查用 [120, 140]，比"恰好 128"
/// 宽，容纳静置时的手柄回中误差。
constexpr int kStickCenterMin = 120;
constexpr int kStickCenterMax = 140;

/// 静止判据用到的状态体偏移，照 core/protocol/input_state.cpp 的解码表抄来：
/// 四个摇杆在 [0..3]、两个扳机在 [4]/[5]、十字键在 [7] 低 nibble、面键在 [7]
/// 高 nibble、其余按键在 [8]/[9]。插件只依赖公共 ABI 头，不 include 宿主的
/// 协议头，所以这几处布局在这里各写一份。
constexpr size_t kLeftStickX = 0;
constexpr size_t kLeftStickY = 1;
constexpr size_t kRightStickX = 2;
constexpr size_t kRightStickY = 3;
constexpr size_t kTriggerLeft = 4;
constexpr size_t kTriggerRight = 5;
constexpr size_t kButtons0 = 7;  ///< 低 nibble 十字键，高 nibble 面键。
constexpr size_t kButtons1 = 8;  ///< L1 R1 L2 R2 Create Options L3 R3。
constexpr size_t kButtons2 = 9;  ///< Home Pad Mute 未定义 左功能 右功能 左背右背。

/// 十字键未按下（hat null）的线上值；与 core 的 Direction::kNone 同值。
constexpr uint8_t kDpadNone = 8;

/// 一分钟的毫秒数：配置按分钟写，计时按毫秒比。
constexpr uint64_t kMinuteMs = 60ULL * 1000ULL;

/// 宿主能力表的副本；init 之后它的函数地址在本进程内一直有效。
ds5Host g_host{};

/// 无操作期限（毫秒），来自 off_after_minutes。
uint64_t g_limit_ms = 0;

/// 上一次有人操作时的单调毫秒数；bt 读线程写、worker 主循环读，所以是原子量。
std::atomic<uint64_t> g_last_activity_ms{0};

/// 是否已经武装；init 置位、stop 清零，运行期只读。
bool g_armed = false;

/**
 * @brief 摇杆是否留在中心带。
 * @param value 8 位摇杆原始量。
 * @return [120, 140] 内 true。
 */
bool InCenter(uint8_t value) {
  return value >= kStickCenterMin && value <= kStickCenterMax;
}

/**
 * @brief 一份状态体是否属于"无人操作"。
 * @param state 63 字节状态体。
 * @return 四个摇杆在中心带、两个扳机为 0、十字键未按、任何按键都没按时 true。
 * @note 只读前 10 个字节，不跑 DecodeInputState：这个钩子在手柄每一份报告上
 *       都会被调用（~100–120 Hz），而判据只关心那几个字节。偏移与
 *       core/protocol/input_state.cpp 的解码表一一对应，改布局要两处同步。
 * @note 纯读；触摸板与六轴刻意不参与（见 idle.h 头注释）。
 */
bool AtRest(const uint8_t* state) {
  const bool sticks_centered =
      InCenter(state[kLeftStickX]) && InCenter(state[kLeftStickY]) &&
      InCenter(state[kRightStickX]) && InCenter(state[kRightStickY]);
  const bool triggers_released =
      state[kTriggerLeft] == 0 && state[kTriggerRight] == 0;
  const bool dpad_released = (state[kButtons0] & 0x0fu) == kDpadNone;
  // [7] 的高 nibble 是方块/叉/圆/三角，[8]/[9] 是其余按键位：按下才置位，
  // 整字节为 0 即全部松开。
  const bool buttons_released = (state[kButtons0] >> 4) == 0 &&
                                state[kButtons1] == 0 && state[kButtons2] == 0;
  return sticks_centered && triggers_released && dpad_released &&
         buttons_released;
}

/**
 * @brief 读配置、记下宿主表和期限，从头开始计时。
 * @return 1 = 武装；0 = 配置关掉了它，或宿主表不是这份头编译出来的形态。
 */
int Init(ds5Host* host, const ds5Setting* settings) {
  // 版本对不上就一个字节都不读，也没法日志——这里唯一能做的就是不武装。
  if (host == nullptr || host->abi_version != kPluginAbiVersion) return 0;

  int minutes = kDefaultOffMinutes;
  const char* raw = FindSetting(settings, kOffMinutesKey);
  if (raw != nullptr && !ParseInt(raw, 0, kOffMinutesMax, minutes)) {
    host->log(DS5_LOG_WARNING,
              (std::string("idle: plugins: idle: off_after_minutes is '") +
               raw + "' but must be an integer in [0, " +
               std::to_string(kOffMinutesMax) + "] — using " +
               std::to_string(kDefaultOffMinutes))
                  .c_str());
    minutes = kDefaultOffMinutes;
  }
  if (minutes == 0) {
    // 0 是"不自动关机"：不武装，此后这个插件没有任何回调、也不读任何报告。
    host->log(DS5_LOG_INFO, "idle: auto power-off disabled");
    return 0;
  }

  g_host = *host;
  g_limit_ms = static_cast<uint64_t>(minutes) * kMinuteMs;
  g_last_activity_ms.store(NowMs());
  g_armed = true;
  host->log(DS5_LOG_INFO,
            (std::string("idle: will ask the controller to power off after ") +
             std::to_string(minutes) + " min without user input")
                .c_str());
  return 1;
}

/// 解除武装；之后 on_tick / on_state_in 都直接返回。
void Stop() { g_armed = false; }

/**
 * @brief 到期检查：先推起点，再发一次关机报告。
 * @note 起点重置在发送之前：power_off 失败（链路已经断了）也只等下一个完整
 *       周期再试，不会每轮主循环都往控制通道上撞一次。
 */
void OnTick() {
  if (!g_armed) return;
  const uint64_t now = NowMs();
  // 单调时钟保证 now ≥ last，差值不需要处理下溢；比较的是时间戳差，不是
  // 累计的节拍数，所以主循环某一轮睡得久一点不会让静置时长算错。
  if (now - g_last_activity_ms.load() < g_limit_ms) return;
  g_last_activity_ms.store(now);
  if (g_host.power_off() == 0) {
    g_host.log(DS5_LOG_ERROR,
               "idle: asked the controller to power off but the report was not "
               "sent; trying again after the next idle period");
  }
}

/// 观察一份状态体：有人操作就把计时起点推到此刻。
void OnStateIn(uint8_t* state) {
  if (!g_armed) return;
  // 静止报告不刷新时间：计时量的是"多长时间没人动"，不是"多久没收到报告"。
  if (!AtRest(state)) g_last_activity_ms.store(NowMs());
}

}  // namespace

#ifdef DS5DUMMY_TESTING

namespace detail {

void PretendIdleFor(uint64_t ms) { g_last_activity_ms.store(NowMs() - ms); }

}  // namespace detail

#endif  // DS5DUMMY_TESTING

/// 装载器的识别标记：这个节存在且内容匹配，宿主才会把它当成插件去 dlopen。
__attribute__((used, section(".ds5_plugin")))
const char ds5_plugin_marker[] = "DS5 Plugin";

/// 装载点按 kPluginEntryName 这个 C 名字 dlsym；idle 单测经 kDesc 用同一份对象。
extern "C" const ds5PluginDesc ds5_plugin_desc{
    kPluginAbiVersion, "idle", "1", &Init, &Stop, &OnTick, &OnStateIn};

const ds5PluginDesc& kDesc = ds5_plugin_desc;

}  // namespace idle
}  // namespace plugin
