// core::protocol::InputState 显式编解码测试。
//
// 协议线格式是 63 字节，不允许依赖 packed bitfield。测试分两类：
//   1. 已知字节 → 字段语义；
//   2. 任意字节 round-trip，确保未知位与保留位不会在编解码中丢失。

#include "core/protocol/input_state.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

namespace protocol = core::protocol;

using InputWire = std::array<uint8_t, protocol::kUsbInputStateSize>;

InputWire MakeKnownWire() {
  InputWire wire{};
  wire[0] = 10;
  wire[1] = 20;
  wire[2] = 30;
  wire[3] = 40;
  wire[4] = 50;
  wire[5] = 60;
  wire[6] = 7;
  wire[7] = 0x83;  // DPad=3, Triangle=1
  wire[8] = 0x85;  // L1, L2, R3
  wire[9] = 0x21;  // Home, RightFunction
  wire[10] = 0x5a;
  wire[11] = 0x78;
  wire[12] = 0x56;
  wire[13] = 0x34;
  wire[14] = 0x12;
  wire[15] = 0xfe;
  wire[16] = 0xff;  // angular_velocity_x = -2
  wire[32] = 0x10;
  wire[33] = 0x32;
  wire[34] = 0x54;
  wire[35] = 0x76;  // touch finger 0 = 0x76543210
  wire[40] = 0x99;
  wire[41] = 0x5a;  // right stop=0xA, right status=0x5
  wire[42] = 0x3c;  // left stop=0xC, left status=0x3
  wire[52] = 0xa3;  // power percent=3, power=AbnormalVoltage
  wire[53] = 0x05;  // headphones + mic muted
  wire[54] = 0x06;  // external mic=0, haptic lpf=1, unknown=1
  for (size_t i = 0; i < 8; ++i) wire[55 + i] = static_cast<uint8_t>(0xa0 + i);
  return wire;
}

}  // namespace

TEST(InputStateTest, DecodesKnownBytes) {
  const InputWire wire = MakeKnownWire();
  const protocol::InputState state =
      protocol::DecodeInputState(wire.data());

  EXPECT_EQ(state.left_stick_x, 10);
  EXPECT_EQ(state.left_stick_y, 20);
  EXPECT_EQ(state.right_stick_x, 30);
  EXPECT_EQ(state.right_stick_y, 40);
  EXPECT_EQ(state.trigger_left, 50);
  EXPECT_EQ(state.trigger_right, 60);
  EXPECT_EQ(state.sequence_number, 7);
  EXPECT_EQ(state.dpad, protocol::Direction::kSouthEast);
  EXPECT_TRUE(state.button_triangle);
  EXPECT_FALSE(state.button_square);
  EXPECT_TRUE(state.button_l1);
  EXPECT_FALSE(state.button_r1);
  EXPECT_TRUE(state.button_l2);
  EXPECT_TRUE(state.button_r3);
  EXPECT_TRUE(state.button_home);
  EXPECT_TRUE(state.button_right_function);
  EXPECT_EQ(state.unknown_2, 0x5a);
  EXPECT_EQ(state.unknown_counter, 0x12345678u);
  EXPECT_EQ(state.angular_velocity_x, -2);

  EXPECT_EQ(state.touch.fingers[0].index, 0x10u);
  EXPECT_FALSE(state.touch.fingers[0].not_touching);
  EXPECT_EQ(state.touch.fingers[0].x, 0x432u);
  EXPECT_EQ(state.touch.fingers[0].y, 0x765u);
  EXPECT_EQ(state.touch.timestamp, 0x99u);

  EXPECT_EQ(state.trigger_right_stop_location, 0xau);
  EXPECT_EQ(state.trigger_right_status, 0x5u);
  EXPECT_EQ(state.trigger_left_stop_location, 0xcu);
  EXPECT_EQ(state.trigger_left_status, 0x3u);

  EXPECT_EQ(state.power_percent, 3u);
  EXPECT_EQ(state.power, protocol::PowerState::kAbnormalVoltage);
  EXPECT_TRUE(state.plugged_headphones);
  EXPECT_FALSE(state.plugged_mic);
  EXPECT_TRUE(state.mic_muted);
  EXPECT_FALSE(state.plugged_external_mic);
  EXPECT_TRUE(state.haptic_low_pass_filter);
  EXPECT_EQ(state.unknown_54, 1u);
  EXPECT_EQ(state.aes_cmac[0], 0xa0u);
  EXPECT_EQ(state.aes_cmac[7], 0xa7u);
}

TEST(InputStateTest, RoundTripsEveryByte) {
  InputWire wire{};
  for (size_t i = 0; i < wire.size(); ++i) {
    wire[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xffu);
  }

  const protocol::InputState state =
      protocol::DecodeInputState(wire.data());
  InputWire encoded{};
  protocol::EncodeInputState(state, encoded.data());

  EXPECT_EQ(encoded, wire);
}
