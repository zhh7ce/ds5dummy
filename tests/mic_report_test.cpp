// DS5Dummy — 麦克风开关报告（mic_report.h）与上行帧路由的单测
//
// 分两层：
//   - detail::kMicStatusOff / kMicStatusOn 是两份冻结模板：142 字节，只有
//     [4] 的 bit0 不同；
//   - bridge::OnMicOpen / OnBtInput 的 mic 分支不做真实发送：前者只往 bt 的
//     发送队列入队（用 StatsWindow 的窗口高水位观察），后者只把包体交给
//     MicSink。全程不调 bt::StartThreads()，不需要链路与手柄。
//
// bridge 的开关状态是进程级全局，用例之间靠 TearDown 的 OnMicOpen(false)
// 复位；队列是进程级状态，每个用例前后用 bt::StopThreads() 清空。

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "bridge/bridge.h"
#include "bridge/mic_report.h"
#include "bt/bt.h"
#include "core/protocol/audio_format.h"
#include "core/protocol/report.h"
#include "usb/usb.h"

namespace {

using StatusReport = std::array<uint8_t, bridge::detail::kMicStatusReportSize>;

StatusReport build_status(bool enabled) {
  StatusReport out{};
  const uint8_t* src =
      enabled ? bridge::detail::kMicStatusOn : bridge::detail::kMicStatusOff;
  std::memcpy(out.data(), src, out.size());
  return out;
}

// 一份完整的带麦克风负载的 0x31 报告：[0]=id、[1]=flags(bit1)、[2]=跳过字节、
// [3..] = 71 字节 Opus 包体。包体每个字节带下标签名，便于发现错位。
std::vector<uint8_t> make_mic_report() {
  std::vector<uint8_t> report(bridge::detail::kMicReportSize, 0);
  report[0] = core::protocol::kBtInputReportId;
  report[1] = bridge::detail::kMicFlagMask;
  for (size_t i = 0; i < core::protocol::kMicOpusSize; ++i) {
    report[bridge::detail::kMicPayloadOffset + i] = static_cast<uint8_t>(0xA0 + i);
  }
  return report;
}

// 一份常规的状态体报告：65 字节，[2..64] 是 63 字节状态体。
std::vector<uint8_t> make_state_report() {
  std::vector<uint8_t> report(65, 0);
  report[0] = core::protocol::kBtInputReportId;
  for (size_t i = 0; i < core::protocol::kUsbInputStateSize; ++i) {
    report[2 + i] = static_cast<uint8_t>(i);
  }
  return report;
}

class BridgeMic : public ::testing::Test {
 protected:
  void SetUp() override {
    // 先清可能残留的开关状态（上一个用例若开着，会入队一份关麦报告），
    // 再清队列，保证本用例从"队列空、开关关"开始。
    bt::StopThreads();
    bridge::OnMicOpen(false);
    bt::StopThreads();
    bridge::SetMicSink(nullptr);
    bt::StatsWindow();  // 读走即清：窗口高水位从 0 起算
  }

  void TearDown() override {
    bridge::OnMicOpen(false);  // 复位全局开关，避免影响后续用例
    bridge::SetMicSink(nullptr);
    bt::StopThreads();
  }

  // 装一个记录调用的 sink；captured 只在用例内有效。
  void capture_mic(std::vector<uint8_t>* captured, int* calls) {
    bridge::SetMicSink([captured, calls](const uint8_t* packet, size_t len) {
      ++*calls;
      captured->assign(packet, packet + len);
    });
  }
};

// 关麦模板：除 [0..4] 外全零，[4] 是 0b10。
TEST(MicReport, OffTemplateBytes) {
  const StatusReport report = build_status(false);
  EXPECT_EQ(report[0], 0x32);
  EXPECT_EQ(report[1], 0x00);  // seq 槽：传输层入队时盖
  EXPECT_EQ(report[2], 0x91);  // 子报告头，与唤醒报告的 0x90 不同
  EXPECT_EQ(report[3], 0x01);  // 子报告数据长度：只有一个参数字节
  EXPECT_EQ(report[4], 0x02);  // 关麦 = bit0 清
  for (size_t i = 5; i < report.size(); ++i) {
    EXPECT_EQ(report[i], 0x00) << "offset " << i;
  }
}

// 开麦与关麦只差 [4] 的 bit0：其余字节逐位相同。
TEST(MicReport, EnableOnlyFlipsBit0) {
  const StatusReport off = build_status(false);
  const StatusReport on = build_status(true);
  EXPECT_EQ(on[4], 0x03);
  for (size_t i = 0; i < off.size(); ++i) {
    if (i == 4) continue;
    EXPECT_EQ(on[i], off[i]) << "offset " << i;
  }
}

// 状态真的翻转才入队：同值的重复调用被去重，链路不会被状态回调刷。
TEST_F(BridgeMic, OnMicOpenPostOnlyOnChange) {
  bridge::OnMicOpen(true);
  EXPECT_EQ(bt::StatsWindow().queue_high_water, 1u);
  bridge::OnMicOpen(true);  // 同值：不发（窗口量读走即清，这里应保持 0）
  EXPECT_EQ(bt::StatsWindow().queue_high_water, 0u);
  // 翻转：再发一份。测试里没有写线程排空队列，第一份还在队里，所以本窗口
  // 的队列深度是 2 而不是 1。
  bridge::OnMicOpen(false);
  EXPECT_EQ(bt::StatsWindow().queue_high_water, 2u);
}

// 麦克风开着时，带 mic 标志的 0x31 把 71 字节包体原样交给 sink。
TEST_F(BridgeMic, MicFrameForwardedWhenOpen) {
  std::vector<uint8_t> captured;
  int calls = 0;
  capture_mic(&captured, &calls);
  bridge::OnMicOpen(true);

  const std::vector<uint8_t> report = make_mic_report();
  bridge::OnBtInput(report.data(), report.size());

  ASSERT_EQ(calls, 1);
  ASSERT_EQ(captured.size(), core::protocol::kMicOpusSize);
  for (size_t i = 0; i < captured.size(); ++i) {
    EXPECT_EQ(captured[i], report[bridge::detail::kMicPayloadOffset + i])
        << "payload " << i;
  }
}

// 开关关着（主机没在录音）时没人消费这帧：整份丢弃。
TEST_F(BridgeMic, MicFrameDroppedWhenClosed) {
  std::vector<uint8_t> captured;
  int calls = 0;
  capture_mic(&captured, &calls);

  const std::vector<uint8_t> report = make_mic_report();
  bridge::OnBtInput(report.data(), report.size());
  EXPECT_EQ(calls, 0);
}

// 残缺帧（不足偏移 + 一帧 Opus）不交给 sink：宁丢也不把半帧喂给解码器。
TEST_F(BridgeMic, ShortMicFrameDropped) {
  std::vector<uint8_t> captured;
  int calls = 0;
  capture_mic(&captured, &calls);
  bridge::OnMicOpen(true);

  const std::vector<uint8_t> report = make_mic_report();
  bridge::OnBtInput(report.data(), bridge::detail::kMicReportSize - 1);
  EXPECT_EQ(calls, 0);
}

// 没有装 sink 时，麦克风开着的帧也只是被丢掉，不会崩。
TEST_F(BridgeMic, MicFrameWithoutSinkIsDropped) {
  bridge::OnMicOpen(true);
  const std::vector<uint8_t> report = make_mic_report();
  bridge::OnBtInput(report.data(), report.size());
  // 没有可观察的副作用：不崩即通过（本用例的价值就是回归空 sink 路径）。
}

// 麦克风开着不影响常规状态报告：它照样换成 USB 0x01 报告发布出去。
TEST_F(BridgeMic, StateReportStillPublishedWhileMicOpen) {
  bridge::OnMicOpen(true);

  const std::vector<uint8_t> report = make_state_report();
  bridge::OnBtInput(report.data(), report.size());

  std::array<uint8_t, core::protocol::kUsbInputReportSize> latest{};
  size_t len = latest.size();
  usb::LatestInputReport(latest.data(), len);
  EXPECT_EQ(latest[0], core::protocol::kUsbInputReportId);
  for (size_t i = 0; i < core::protocol::kUsbInputStateSize; ++i) {
    EXPECT_EQ(latest[1 + i], report[2 + i]) << "state " << i;
  }
}

// mic 帧不是状态报告：它不能顶掉 USB 槽位里最近的一份输入状态。
TEST_F(BridgeMic, MicFrameDoesNotTouchUsbSlot) {
  // 先发一份可识别状态，再发一帧麦克风。
  const std::vector<uint8_t> state = make_state_report();
  bridge::OnBtInput(state.data(), state.size());
  bridge::OnMicOpen(true);
  const std::vector<uint8_t> mic = make_mic_report();
  bridge::OnBtInput(mic.data(), mic.size());

  std::array<uint8_t, core::protocol::kUsbInputReportSize> latest{};
  size_t len = latest.size();
  usb::LatestInputReport(latest.data(), len);
  EXPECT_EQ(latest[0], core::protocol::kUsbInputReportId);
  for (size_t i = 0; i < core::protocol::kUsbInputStateSize; ++i) {
    EXPECT_EQ(latest[1 + i], state[2 + i]) << "state " << i;
  }
}

}  // namespace
