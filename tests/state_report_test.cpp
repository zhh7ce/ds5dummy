// bridge::detail 的 SetStateData 合并规则测试。
//
// 锁定的不是“字段长什么样”，而是固件的 allow-flag 语义：只有被主机允许的
// 字段才能进入 0x36 报告的 63 字节 SetStateData。

#include "bridge/state_report.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "core/protocol/report.h"

namespace {

namespace protocol = core::protocol;

using OutputWire = std::array<uint8_t, protocol::kUsbOutputPayloadSize>;
using ShadowWire = std::array<uint8_t, protocol::kSetStateShadowSize>;

ShadowWire Snapshot() {
  ShadowWire shadow{};
  bridge::detail::CopyOutputState(shadow.data());
  return shadow;
}

}  // namespace

TEST(StateMgrTest, InitUsesNonZeroAudioVolumes) {
  bridge::detail::InitOutputState(2);
  const ShadowWire shadow = Snapshot();
  EXPECT_EQ(shadow[4], 0x7f);
  EXPECT_EQ(shadow[5], 0x64);
}

// 前置增益写在 SetStateData byte[37] 的低 3 位（SpeakerCompPreGain），高 5 位
// 属于同一字节的其它字段，必须原样保留。
TEST(StateMgrTest, InitWritesSpeakerPreGainIntoLowThreeBits) {
  bridge::detail::InitOutputState(2);
  const ShadowWire defaults = Snapshot();
  EXPECT_EQ(defaults[37] & 0x07, 0x02);  // 固件初值

  bridge::detail::InitOutputState(7);
  const ShadowWire loudest = Snapshot();
  EXPECT_EQ(loudest[37] & 0x07, 0x07);
  EXPECT_EQ(loudest[37] & 0xf8, defaults[37] & 0xf8);
}

// 主机报告里的音频控制字段不被合并，所以用户配的前置增益不会在收到第一份
// 0x02 之后被主机值覆盖掉。
TEST(StateMgrTest, SpeakerPreGainSurvivesHostReports) {
  bridge::detail::InitOutputState(6);
  OutputWire payload{};
  // 主机把 allow 位全打开，payload 里 byte[37] 是另一套值。
  payload[1] = 0xff;
  payload[37] = 0x01;
  bridge::detail::UpdateOutputState(payload.data());
  EXPECT_EQ(Snapshot()[37] & 0x07, 0x06);
}

TEST(StateMgrTest, UpdateMergesLedOnlyWhenAllowed) {
  bridge::detail::InitOutputState(2);
  const ShadowWire before = Snapshot();

  OutputWire payload{};
  payload[1] = 0x00;  // allow_led_color = false
  payload[44] = 0x11;
  payload[45] = 0x22;
  payload[46] = 0x33;
  bridge::detail::UpdateOutputState(payload.data());
  ShadowWire after = Snapshot();
  EXPECT_EQ(after[44], before[44]);
  EXPECT_EQ(after[45], before[45]);
  EXPECT_EQ(after[46], before[46]);

  payload[1] = 0x04;  // allow_led_color = true
  bridge::detail::UpdateOutputState(payload.data());
  after = Snapshot();
  EXPECT_EQ(after[44], 0x11);
  EXPECT_EQ(after[45], 0x22);
  EXPECT_EQ(after[46], 0x33);
}

TEST(StateMgrTest, UpdateMergesPlayerIndicatorsOnlyWhenAllowed) {
  bridge::detail::InitOutputState(2);
  const ShadowWire before = Snapshot();

  OutputWire payload{};
  payload[43] = 0x3f;
  bridge::detail::UpdateOutputState(payload.data());
  ShadowWire after = Snapshot();
  EXPECT_EQ(after[43], before[43]);

  payload[1] = 0x10;  // allow_player_indicators
  bridge::detail::UpdateOutputState(payload.data());
  after = Snapshot();
  EXPECT_EQ(after[43], 0x3f);
}
