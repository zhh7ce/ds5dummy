/**
 * @file
 * @brief idle 插件行为的单测：武装条件、静止判据与到期那一次 power_off
 *
 * 这里绕过 plugin 门面，直接拿 plugin::idle::kDesc 的函数指针驱动：测的是这个
 * 插件自己的语义（配置取值、什么算"有人在操作"、到期后怎么重试），而装载与
 * 分派已经在 plugin_test.cpp 里单独覆盖过。
 *
 * 插件自己取单调时钟，测试不等真实时间：detail::PretendIdleFor(ms) 把"上次有人
 * 操作"拨到 ms 毫秒之前，再调 on_tick 触发判定。余量统一取 100 ms——拨表与检查
 * 之间只隔几微秒，不会跨过它。
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/protocol/input_state.h"
#include "gtest/gtest.h"
#include "plugin/idle/idle.h"
#include "plugin/plugin_api.h"

namespace {

using core::protocol::Direction;
using core::protocol::InputState;

/// power_off 被调用了几次。
int g_power_off_calls = 0;

/// power_off 的返回值：0 = 报告没交出去，1 = 已交给链路。
int g_power_off_result = 1;

/// 插件打过的日志，按 (等级, 文本) 存下，供"回落缺省时该有一条 WARNING"这类断言用。
std::vector<std::pair<int, std::string>> g_logs;

void Log(int level, const char* message) { g_logs.emplace_back(level, message); }

int PowerOff() {
  ++g_power_off_calls;
  return g_power_off_result;
}

/// 一份版本正确的宿主表：插件只核对 abi_version 这一个头部字段。
ds5Host MakeHost() {
  ds5Host host{};
  host.abi_version = kPluginAbiVersion;
  host.log = &Log;
  host.power_off = &PowerOff;
  return host;
}

/**
 * @brief 造一份状态体。
 * @param state 语义模型，摇杆等模拟量按原值编码。
 * @return 63 字节线格式。
 */
std::vector<uint8_t> Wire(const InputState& state) {
  std::vector<uint8_t> bytes(core::protocol::kUsbInputStateSize, 0);
  core::protocol::EncodeInputState(state, bytes.data());
  return bytes;
}

/// 一份"没人碰"的状态：四杆回中、扳机松开、十字键与所有按键都没按。
InputState AtRestState() {
  InputState state;
  // 真手柄静置时摇杆读的是中位而不是 0，中心带判据按这个来。
  state.left_stick_x = 128;
  state.left_stick_y = 128;
  state.right_stick_x = 128;
  state.right_stick_y = 128;
  return state;
}

/**
 * @brief 按配置文本武装这个单例插件。
 * @param host 交给插件的宿主表。
 * @param settings 形如 {{"key", "value"}, ..., {nullptr, nullptr}} 的数组，可为
 *                 nullptr。
 * @return 1 = 这一轮生效。
 */
bool Arm(ds5Host* host, const ds5Setting* settings) {
  return plugin::idle::kDesc.init(host, settings) != 0;
}

/**
 * @brief 在记下的日志里找某个等级的第一条。
 * @param level 要找的 ds5LogLevel。
 * @return 指向那条文本；一条都没有时返回 null。
 */
const char* FindLog(int level) {
  for (const auto& entry : g_logs) {
    if (entry.first == level) return entry.second.c_str();
  }
  return nullptr;
}

class IdleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_power_off_calls = 0;
    g_power_off_result = 1;
    g_logs.clear();
  }

  void TearDown() override { plugin::idle::kDesc.stop(); }

  /// 缺省期限（30 分钟）的毫秒数。
  static constexpr uint64_t kDefaultMs =
      static_cast<uint64_t>(plugin::idle::kDefaultOffMinutes) * 60ULL * 1000ULL;

  /**
   * @brief 把"上次有人操作"拨到 @p ms 之前，再跑一次心跳。
   * @param ms 静置了多久。
   */
  static void TickAfterIdle(uint64_t ms) {
    plugin::idle::detail::PretendIdleFor(ms);
    plugin::idle::kDesc.on_tick();
  }
};

// ---------------------------------------------------------------------------
// 描述符形态与武装条件
// ---------------------------------------------------------------------------

TEST_F(IdleTest, DescriptorIsASingletonWithoutSelfOrClock) {
  // 插件是进程级单例：描述符只带版本、名字与四个无 self 的槽。
  EXPECT_EQ(plugin::idle::kDesc.abi_version, kPluginAbiVersion);
  EXPECT_STREQ(plugin::idle::kDesc.name, "idle");
  EXPECT_NE(plugin::idle::kDesc.on_tick, nullptr);
  EXPECT_NE(plugin::idle::kDesc.on_state_in, nullptr);
}

TEST_F(IdleTest, ArmsWhenTheConfigSaysNothing) {
  ds5Host host = MakeHost();
  EXPECT_TRUE(Arm(&host, nullptr));
}

TEST_F(IdleTest, DoesNotArmWhenMinutesIsZero) {
  const ds5Setting settings[] = {{"off_after_minutes", "0"},
                                 {nullptr, nullptr}};
  ds5Host host = MakeHost();
  // 0 = 从不自动关机：不武装，并留下一条能看懂的 INFO。
  EXPECT_FALSE(Arm(&host, settings));
  ASSERT_EQ(g_logs.size(), 1u);
  EXPECT_EQ(g_logs[0].first, DS5_LOG_INFO);
}

TEST_F(IdleTest, ArmsWithDefaultWhenTheValueIsUnusable) {
  // 越界（61）、负数、非数字：都记 WARNING 并回到缺省，而不是关掉这个插件。
  for (const char* raw : {"off_after_minutes=61", "off_after_minutes=-5",
                          "off_after_minutes=abc", "off_after_minutes=",
                          "off_after_minutes=0x1e"}) {
    g_logs.clear();
    const ds5Setting settings[] = {{"off_after_minutes", raw},
                                   {nullptr, nullptr}};
    ds5Host host = MakeHost();
    EXPECT_TRUE(Arm(&host, settings)) << raw;
    // 一条 WARNING 说明回落，随后照常武装并记一条 INFO。
    ASSERT_EQ(g_logs.size(), 2u) << raw;
    EXPECT_EQ(g_logs[0].first, DS5_LOG_WARNING) << raw;
    plugin::idle::kDesc.stop();
  }
}

TEST_F(IdleTest, UnrelatedKeysDoNotAffectIt) {
  // 前缀撞上（off_after_minutes_extra）与别的插件的键都要被跳过，取值走缺省。
  const ds5Setting settings[] = {{"off_after_minutes_extra", "5"},
                                 {"verbose", "1"},
                                 {nullptr, nullptr}};
  ds5Host host = MakeHost();
  EXPECT_TRUE(Arm(&host, settings));
  // 缺省 30 分钟：差 1 秒到期时不该有任何动作。
  TickAfterIdle(kDefaultMs - 1000);
  EXPECT_EQ(g_power_off_calls, 0);
}

TEST_F(IdleTest, RejectsAHostTableItDidNotCompileAgainst) {
  // 版本对不上就一个字节都不读：既不武装也不留日志。
  g_logs.clear();
  ds5Host host = MakeHost();
  host.abi_version ^= 1u;
  EXPECT_FALSE(Arm(&host, nullptr));
  EXPECT_TRUE(g_logs.empty());

  // 空表同理，而且连"读版本号"这一步都不能做。
  EXPECT_FALSE(Arm(nullptr, nullptr));
  EXPECT_TRUE(g_logs.empty());
}

// ---------------------------------------------------------------------------
// 到期与静止判据
// ---------------------------------------------------------------------------

TEST_F(IdleTest, PowersOffExactlyAfterTheIdleWindow) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  TickAfterIdle(kDefaultMs - 100);
  EXPECT_EQ(g_power_off_calls, 0);

  TickAfterIdle(kDefaultMs + 100);
  EXPECT_EQ(g_power_off_calls, 1);
}

TEST_F(IdleTest, HonoursAConfiguredWindow) {
  const ds5Setting settings[] = {{"off_after_minutes", "5"},
                                 {nullptr, nullptr}};
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, settings));

  constexpr uint64_t kWindowMs = 5ULL * 60ULL * 1000ULL;
  TickAfterIdle(kWindowMs - 100);
  EXPECT_EQ(g_power_off_calls, 0);
  TickAfterIdle(kWindowMs + 100);
  EXPECT_EQ(g_power_off_calls, 1);
}

TEST_F(IdleTest, RestingReportsDoNotResetTheTimer) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  // 可写：on_state_in 交出去的是宿主那份状态体的本体，插件改完就直接发给主机。
  std::vector<uint8_t> rest = Wire(AtRestState());
  // 手柄每秒推十几份静止报告：计时量的是"多久没人动"，不是"多久没收到报告"。
  plugin::idle::detail::PretendIdleFor(kDefaultMs);
  for (int i = 0; i < 50; ++i) plugin::idle::kDesc.on_state_in(rest.data());
  plugin::idle::kDesc.on_tick();
  EXPECT_EQ(g_power_off_calls, 1);
}

TEST_F(IdleTest, AnInputMovesTheIdleWindow) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  InputState pressed = AtRestState();
  pressed.button_cross = true;
  std::vector<uint8_t> bytes = Wire(pressed);
  // 已经静置满一个周期，但此刻有人按了一下：起点被推到现在。
  plugin::idle::detail::PretendIdleFor(kDefaultMs);
  plugin::idle::kDesc.on_state_in(bytes.data());
  plugin::idle::kDesc.on_tick();
  EXPECT_EQ(g_power_off_calls, 0);

  // 从那一次操作起再满一个完整周期才发。
  TickAfterIdle(kDefaultMs);
  EXPECT_EQ(g_power_off_calls, 1);
}

TEST_F(IdleTest, EveryControlThatCountsAsInputResetsTheTimer) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  // 每个"该算操作"的量单独试一次：只改它就足以推翻静止判定。
  struct Case {
    const char* what;
    void (*bend)(InputState&);
  };
  const Case cases[] = {
      {"square", [](InputState& s) { s.button_square = true; }},
      {"l1", [](InputState& s) { s.button_l1 = true; }},
      {"l2 bit", [](InputState& s) { s.button_l2 = true; }},
      {"create", [](InputState& s) { s.button_create = true; }},
      {"home", [](InputState& s) { s.button_home = true; }},
      {"pad press", [](InputState& s) { s.button_pad = true; }},
      {"mute", [](InputState& s) { s.button_mute = true; }},
      {"left function", [](InputState& s) { s.button_left_function = true; }},
      {"right paddle", [](InputState& s) { s.button_right_paddle = true; }},
      {"unknown button", [](InputState& s) { s.unknown_button = true; }},
      {"dpad west", [](InputState& s) { s.dpad = Direction::kWest; }},
      {"left stick y", [](InputState& s) { s.left_stick_y = 60; }},
      {"right stick x", [](InputState& s) { s.right_stick_x = 200; }},
      {"trigger left", [](InputState& s) { s.trigger_left = 17; }},
  };
  for (const Case& c : cases) {
    InputState state = AtRestState();
    c.bend(state);
    std::vector<uint8_t> bytes = Wire(state);
    // 到点前 1 ms 有操作：起点被推走，紧接着的到点那一轮不该发。
    plugin::idle::detail::PretendIdleFor(kDefaultMs - 1);
    plugin::idle::kDesc.on_state_in(bytes.data());
    plugin::idle::kDesc.on_tick();
    EXPECT_EQ(g_power_off_calls, 0) << c.what;
    // 从那一次操作起再满一个完整周期才发，证明推走起点的正是这个字段。
    TickAfterIdle(kDefaultMs);
    EXPECT_EQ(g_power_off_calls, 1) << c.what;
    g_power_off_calls = 0;
  }
}

TEST_F(IdleTest, StickDeflectionCountsAsInput) {
  ds5Host host = MakeHost();

  // 中心带 [120, 140]：带内不算操作，带外算。
  const struct { uint8_t value; bool active; } cases[] = {
      {119, true}, {120, false}, {128, false}, {140, false}, {141, true},
      {0, true},   {255, true},
  };
  for (const auto& c : cases) {
    ASSERT_TRUE(Arm(&host, nullptr));

    InputState state = AtRestState();
    state.left_stick_x = c.value;
    std::vector<uint8_t> bytes = Wire(state);
    plugin::idle::detail::PretendIdleFor(kDefaultMs);
    plugin::idle::kDesc.on_state_in(bytes.data());
    plugin::idle::kDesc.on_tick();
    // 算操作 → 刚推过起点，不发；不算 → 正好到点，发一次。
    EXPECT_EQ(g_power_off_calls, c.active ? 0 : 1)
        << "value=" << static_cast<int>(c.value);
    g_power_off_calls = 0;
    plugin::idle::kDesc.stop();
  }
}

TEST_F(IdleTest, TriggerDeflectionCountsAsInput) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  InputState state = AtRestState();
  state.trigger_right = 30;
  std::vector<uint8_t> bytes = Wire(state);
  plugin::idle::detail::PretendIdleFor(kDefaultMs);
  plugin::idle::kDesc.on_state_in(bytes.data());
  plugin::idle::kDesc.on_tick();
  EXPECT_EQ(g_power_off_calls, 0);
}

TEST_F(IdleTest, TouchAndMotionAreNotInput) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  // 手搭在触摸板上、或手柄被挪动（六轴有读数）都不算"用户在操作"：静置时
  // 传感器的噪声也不该让计时永远不触发。
  InputState touch = AtRestState();
  touch.touch.fingers[0].not_touching = false;
  touch.touch.fingers[0].x = 1200;
  std::vector<uint8_t> touch_bytes = Wire(touch);

  InputState motion = AtRestState();
  motion.accelerometer_x = 512;
  motion.angular_velocity_z = -333;
  motion.sensor_timestamp = 424242;
  std::vector<uint8_t> motion_bytes = Wire(motion);

  plugin::idle::detail::PretendIdleFor(kDefaultMs);
  plugin::idle::kDesc.on_state_in(touch_bytes.data());
  plugin::idle::kDesc.on_state_in(motion_bytes.data());
  plugin::idle::kDesc.on_tick();
  EXPECT_EQ(g_power_off_calls, 1);
}

TEST_F(IdleTest, StateInDoesNotRewriteTheReport) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  const std::vector<uint8_t> rest = Wire(AtRestState());
  std::vector<uint8_t> bytes = rest;
  plugin::idle::kDesc.on_state_in(bytes.data());
  // 它只观察：转发给 USB 的必须是手柄原样那份字节（seq 与传感器时间戳都不能动）。
  EXPECT_TRUE(std::memcmp(bytes.data(), rest.data(), rest.size()) == 0);
}

TEST_F(IdleTest, PowerOffFailureWaitsAFullPeriod) {
  g_power_off_result = 0;  // 链路没开：报告没交出去
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));

  TickAfterIdle(kDefaultMs);
  EXPECT_EQ(g_power_off_calls, 1);
  // 武装时那条 INFO 之外，必须还能看到一条说"没发出去"的 ERROR。
  EXPECT_NE(FindLog(DS5_LOG_ERROR), nullptr);

  // 失败已经把起点推回去了：紧接着的几轮不许再撞控制通道。
  TickAfterIdle(kDefaultMs - 1000);
  EXPECT_EQ(g_power_off_calls, 1);

  TickAfterIdle(kDefaultMs + 100);
  EXPECT_EQ(g_power_off_calls, 2);
}

TEST_F(IdleTest, StopDisarmsTheTick) {
  ds5Host host = MakeHost();
  ASSERT_TRUE(Arm(&host, nullptr));
  plugin::idle::kDesc.stop();

  TickAfterIdle(kDefaultMs);
  EXPECT_EQ(g_power_off_calls, 0);
}

}  // namespace
