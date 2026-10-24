// core::protocol::OutputState 显式编解码测试。
//
// 主机 USB 0x02 报告只传 SetStateData 前 47 字节；完整 63 字节里有 16 字节
// reserved padding。测试同时锁定 47 字节线格式与 63 字节整份 round-trip。

#include "core/protocol/output_state.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

namespace protocol = core::protocol;

using OutputWire = std::array<uint8_t, protocol::kUsbOutputPayloadSize>;
using ShadowWire = std::array<uint8_t, protocol::kSetStateShadowSize>;

OutputWire MakeKnownWire() {
  OutputWire wire{};
  wire[0] = 0x0d;  // rumble emulation + allow right/left trigger FFB
  wire[1] = 0x14;  // allow led color + allow player indicators
  wire[2] = 0x11;
  wire[3] = 0x22;
  wire[4] = 0x7f;
  wire[5] = 0x64;
  wire[6] = 0xff;
  wire[7] = 0x59;  // mic select=1, echo=0, noise=1, output=1, input=1
  wire[8] = 2;     // breathing mute light
  wire[9] = 0x81;  // touch power save + haptic mute
  for (size_t i = 0; i < 11; ++i) wire[10 + i] = static_cast<uint8_t>(i + 1);
  for (size_t i = 0; i < 11; ++i) wire[21 + i] = static_cast<uint8_t>(0x20 + i);
  wire[32] = 0x78;
  wire[33] = 0x56;
  wire[34] = 0x34;
  wire[35] = 0x12;
  wire[36] = 0xaf;
  wire[37] = 0x1d;
  wire[38] = 0x0f;
  wire[39] = 0x81;
  wire[40] = 0x5a;
  wire[41] = 2;  // fade out
  wire[42] = 1;  // mid brightness
  wire[43] = 0x3f;
  wire[44] = 0x11;
  wire[45] = 0x22;
  wire[46] = 0x33;
  return wire;
}

}  // namespace

TEST(OutputStateTest, DecodesKnownBytes) {
  const OutputWire wire = MakeKnownWire();
  const protocol::OutputState state =
      protocol::DecodeOutputState(wire.data());

  EXPECT_TRUE(state.enable_rumble_emulation);
  EXPECT_FALSE(state.use_rumble_not_haptics);
  EXPECT_TRUE(state.allow_right_trigger_ffb);
  EXPECT_TRUE(state.allow_left_trigger_ffb);
  EXPECT_TRUE(state.allow_led_color);
  EXPECT_TRUE(state.allow_player_indicators);
  EXPECT_EQ(state.rumble_emulation_right, 0x11);
  EXPECT_EQ(state.rumble_emulation_left, 0x22);
  EXPECT_EQ(state.volume_headphones, 0x7f);
  EXPECT_EQ(state.volume_speaker, 0x64);
  EXPECT_EQ(state.volume_mic, 0xff);
  EXPECT_EQ(state.mic_select, 1u);
  EXPECT_FALSE(state.echo_cancel_enable);
  EXPECT_TRUE(state.noise_cancel_enable);
  EXPECT_EQ(state.output_path_select, 1u);
  EXPECT_EQ(state.input_path_select, 1u);
  EXPECT_EQ(state.mute_light_mode, protocol::MuteLightState::kBreathing);
  EXPECT_TRUE(state.touch_power_save);
  EXPECT_TRUE(state.haptic_mute);
  EXPECT_EQ(state.right_trigger_ffb[0], 1u);
  EXPECT_EQ(state.left_trigger_ffb[10], 0x2au);
  EXPECT_EQ(state.host_timestamp, 0x12345678u);
  EXPECT_EQ(state.rumble_motor_power_reduction, 0xfu);
  EXPECT_EQ(state.trigger_motor_power_reduction, 0xau);
  EXPECT_EQ(state.speaker_comp_pre_gain, 0x5u);
  EXPECT_TRUE(state.beamforming_enable);
  EXPECT_TRUE(state.allow_light_brightness_change);
  EXPECT_TRUE(state.allow_color_light_fade_animation);
  EXPECT_TRUE(state.enable_improved_rumble_emulation);
  EXPECT_TRUE(state.use_rumble_not_haptics_2);
  EXPECT_TRUE(state.haptic_low_pass_filter);
  EXPECT_EQ(state.unknown_39, 0x40u);
  EXPECT_EQ(state.unknown_byte_40, 0x5a);
  EXPECT_EQ(state.light_fade_animation, protocol::LightFadeAnimationMode::kOut);
  EXPECT_EQ(state.light_brightness, protocol::LightBrightnessLevel::kMid);
  EXPECT_TRUE(state.player_light_1);
  EXPECT_TRUE(state.player_light_5);
  EXPECT_TRUE(state.player_light_fade);
  EXPECT_EQ(state.unknown_player_lights, 0u);
  EXPECT_EQ(state.led_red, 0x11);
  EXPECT_EQ(state.led_green, 0x22);
  EXPECT_EQ(state.led_blue, 0x33);
}

TEST(OutputStateTest, EncodesPayloadRoundTrip) {
  const OutputWire wire = MakeKnownWire();
  const protocol::OutputState state =
      protocol::DecodeOutputState(wire.data());

  ShadowWire shadow{};
  protocol::EncodeOutputState(state, shadow.data());

  for (size_t i = 0; i < wire.size(); ++i) {
    EXPECT_EQ(shadow[i], wire[i]) << "byte " << i;
  }
  for (size_t i = protocol::kUsbOutputPayloadSize; i < shadow.size(); ++i) {
    EXPECT_EQ(shadow[i], 0u) << "reserved byte " << i;
  }
}

TEST(OutputStateTest, PreservesReservedTail) {
  protocol::OutputState state{};
  for (size_t i = 0; i < state.reserved.size(); ++i) {
    state.reserved[i] = static_cast<uint8_t>(0xf0 + i);
  }

  ShadowWire shadow{};
  protocol::EncodeOutputState(state, shadow.data());

  for (size_t i = 0; i < state.reserved.size(); ++i) {
    EXPECT_EQ(shadow[protocol::kUsbOutputPayloadSize + i], state.reserved[i]);
  }
}
