// DS5Dummy — 0x36 音频报告构造器单测（audio_report.cpp）
//
// detail::BuildAudioReport 是纯字节布局：给定触觉包(64B)、喇叭包(200B)、
// SetStateData(63B)、packet_counter、buffer_length，产出固定 398B 的报告体。
// 两段数据是分开的两个指针——audio 交出来的就是这个形状，布局不该要求它们相邻。
// 刻意留空的 out[1](seq) 和尾部 CRC 由传输层在入队点盖，这里验证构造函数
// 只把它们清零、不擅自填写。
//
// 段长来自 core（audio 与 bridge 共用）。被测的是 bridge 侧的布局，所以不
// include audio。

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "bridge/audio_report.h"
#include "core/protocol/report.h"

namespace {

// 每字节等于自己的下标模 256：任何错位、漏写、串道都会直接暴露在断言里。
// 触觉与喇叭各自从 0 起计，两段落在报告的不同区间，混淆会立刻显形。
std::array<uint8_t, core::protocol::kHapticsSize> make_haptics() {
  std::array<uint8_t, core::protocol::kHapticsSize> h{};
  for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<uint8_t>(i);
  return h;
}

std::array<uint8_t, core::protocol::kSpeakerSize> make_speaker() {
  std::array<uint8_t, core::protocol::kSpeakerSize> s{};
  for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<uint8_t>(i % 256);
  return s;
}

std::array<uint8_t, core::protocol::kSetStateShadowSize> make_state() {
  std::array<uint8_t, core::protocol::kSetStateShadowSize> s{};
  for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<uint8_t>(i);
  return s;
}

}  // namespace

TEST(ReportAudio, ConstantsMatchProtocol) {
  EXPECT_EQ(core::protocol::kHapticsSize, 64u);
  EXPECT_EQ(core::protocol::kSpeakerSize, 200u);
  EXPECT_EQ(core::protocol::kHapticsSize + core::protocol::kSpeakerSize, 264u);
  EXPECT_EQ(core::protocol::kSetStateShadowSize, 63u);
  EXPECT_EQ(core::protocol::kBtAudioReportSize, 398u);
  EXPECT_EQ(core::protocol::kBtAudioReportId, 0x36);
}

TEST(ReportAudio, HeaderAndBufferLengthBytes) {
  const auto haptics = make_haptics();
  const auto speaker = make_speaker();
  const auto state_data = make_state();
  std::array<uint8_t, core::protocol::kBtAudioReportSize> out{};

  bridge::detail::BuildAudioReport(out.data(), 0x5A, haptics.data(),
                                         speaker.data(), state_data.data());

  EXPECT_EQ(out[0], core::protocol::kBtAudioReportId);
  EXPECT_EQ(out[1], 0x00);  // seq：留给 bridge，这里必须为清零值
  EXPECT_EQ(out[2], 0x91);  // 未知子报告（固件写死）
  EXPECT_EQ(out[3], 0x07);
  EXPECT_EQ(out[4], 0xFE);
  for (size_t i = 5; i <= 9; ++i)
    EXPECT_EQ(out[i], bridge::detail::kAudioBufferLength) << i;
  EXPECT_EQ(out[10], 0x5A);  // packet_counter 原样落位
}

TEST(ReportAudio, CustomBufferLengthOnlyChangesFiveBytes) {
  const auto haptics = make_haptics();
  const auto speaker = make_speaker();
  const auto state_data = make_state();
  std::array<uint8_t, core::protocol::kBtAudioReportSize> out{};

  bridge::detail::BuildAudioReport(out.data(), 0x00, haptics.data(),
                                         speaker.data(), state_data.data(), 32);
  for (size_t i = 5; i <= 9; ++i) EXPECT_EQ(out[i], 32) << i;
  // 只有 byte[9] 真正 gating 缓冲，其余四份是固件遗留——值都相同。
}

TEST(ReportAudio, SetStateDataPlacedAtOffset13) {
  const auto haptics = make_haptics();
  const auto speaker = make_speaker();
  const auto state_data = make_state();
  std::array<uint8_t, core::protocol::kBtAudioReportSize> out{};

  bridge::detail::BuildAudioReport(out.data(), 0x00, haptics.data(),
                                         speaker.data(), state_data.data());

  EXPECT_EQ(out[11], 0x90);
  EXPECT_EQ(out[12], core::protocol::kSetStateShadowSize);
  for (size_t i = 0; i < core::protocol::kSetStateShadowSize; ++i) {
    EXPECT_EQ(out[13 + i], state_data[i]) << "state offset " << i;
  }
}

TEST(ReportAudio, HapticsAndSpeakerSegmentsPlaced) {
  const auto haptics = make_haptics();
  const auto speaker = make_speaker();
  const auto state_data = make_state();
  std::array<uint8_t, core::protocol::kBtAudioReportSize> out{};

  bridge::detail::BuildAudioReport(out.data(), 0x00, haptics.data(),
                                         speaker.data(), state_data.data());

  // 触觉：64B 在 78..141。
  EXPECT_EQ(out[76], 0x92);
  EXPECT_EQ(out[77], core::protocol::kHapticsSize);
  for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
    EXPECT_EQ(out[78 + i], haptics[i]) << "haptics " << i;
  }

  // 喇叭：200B 在 144..343，独立指针，不与触觉相邻。
  EXPECT_EQ(out[142], bridge::detail::kSpeakerSubReport);
  EXPECT_EQ(out[143], core::protocol::kSpeakerSize);
  for (size_t i = 0; i < core::protocol::kSpeakerSize; ++i) {
    EXPECT_EQ(out[144 + i], speaker[i]) << "speaker " << i;
  }
}

TEST(ReportAudio, MicFlagSetsOnlyBit0OfByte4) {
  const auto haptics = make_haptics();
  const auto speaker = make_speaker();
  const auto state_data = make_state();
  std::array<uint8_t, core::protocol::kBtAudioReportSize> out{};

  // 显式 false 必须与默认参数一致：byte[4] 是配置子报告的数据字节，只有
  // bit0 随麦克风开关变化。
  bridge::detail::BuildAudioReport(out.data(), 0x00, haptics.data(),
                                   speaker.data(), state_data.data(),
                                   bridge::detail::kAudioBufferLength, false);
  EXPECT_EQ(out[4], 0xFE);

  bridge::detail::BuildAudioReport(out.data(), 0x00, haptics.data(),
                                   speaker.data(), state_data.data(),
                                   bridge::detail::kAudioBufferLength, true);
  EXPECT_EQ(out[4], 0xFF);
}

TEST(ReportAudio, TailAfterSpeakerIsZeroFilled) {
  const auto haptics = make_haptics();
  const auto speaker = make_speaker();
  const auto state_data = make_state();
  // 预置脏数据，确认 memset 把 344..397 段清零。
  std::array<uint8_t, core::protocol::kBtAudioReportSize> out{};
  out.fill(0xFF);

  bridge::detail::BuildAudioReport(out.data(), 0x00, haptics.data(),
                                         speaker.data(), state_data.data());

  for (size_t i = core::protocol::kBtAudioReportSize - 54; i < out.size();
       ++i) {
    EXPECT_EQ(out[i], 0x00) << "tail " << i;
  }
}
