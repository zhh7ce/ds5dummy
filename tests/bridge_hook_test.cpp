/**
 * @file
 * @brief bridge 的输入状态钩子：注册、改写、清空与麦克风帧旁路
 *
 * worker 是唯一注册方，plugin 只把函数指针交出来。这里用两个假钩子证明改写
 * 真的落在 usb 持有的那份报告上、按注册顺序跑、清空后不再被调——不需要 bt、
 * 手柄或 root，也不起任何线程。
 */

#include <cstdint>
#include <vector>

#include "bridge/bridge.h"
#include "bridge/mic_report.h"
#include "core/protocol/report.h"
#include "gtest/gtest.h"
#include "usb/usb.h"

namespace {

int g_calls = 0;

void RewriteFirstByte(uint8_t* state) {
  ++g_calls;
  state[0] = 0x5A;
}

/// 第二个钩子读第一个钩子写下的字节：只有按注册顺序跑才会看到 0x11。
void CopyPreviousByte(uint8_t* state) { state[1] = state[0]; }

class BridgeHookTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bridge::ClearStateHooks();
    g_calls = 0;
    bridge::Init("AA:BB:CC:DD:EE:FF", 2);
  }
  void TearDown() override { bridge::ClearStateHooks(); }
};

/// 一份合法的状态报告：[0x31][flags=0][63 字节状态体]，状态体首字节可指定。
std::vector<uint8_t> StateReport(uint8_t first_state_byte) {
  std::vector<uint8_t> report(2 + core::protocol::kUsbInputStateSize, 0);
  report[0] = core::protocol::kBtInputReportId;
  report[2] = first_state_byte;
  return report;
}

/// 读出 usb 当前持有的那份输入报告。
uint8_t PublishedStateByte(size_t state_index) {
  uint8_t latest[core::protocol::kUsbInputReportSize] = {};
  usb::LatestInputReport(latest, sizeof(latest));
  return latest[1 + state_index];
}

TEST_F(BridgeHookTest, NoHookLeavesTheStateUntouched) {
  const std::vector<uint8_t> report = StateReport(0x11);
  bridge::OnBtInput(report.data(), report.size());
  EXPECT_EQ(PublishedStateByte(0), 0x11);
}

TEST_F(BridgeHookTest, HookRewritesTheStatePublishedToUsb) {
  bridge::AddStateHook(&RewriteFirstByte);
  const std::vector<uint8_t> report = StateReport(0x11);
  bridge::OnBtInput(report.data(), report.size());

  // 钩子改的是发布之前的那份状态体：主机最后读到的就是 0x5A。
  EXPECT_EQ(PublishedStateByte(0), 0x5A);
  EXPECT_EQ(g_calls, 1);
}

TEST_F(BridgeHookTest, HooksRunInRegistrationOrder) {
  bridge::AddStateHook(&RewriteFirstByte);
  bridge::AddStateHook(&CopyPreviousByte);
  const std::vector<uint8_t> report = StateReport(0x11);
  bridge::OnBtInput(report.data(), report.size());

  EXPECT_EQ(PublishedStateByte(0), 0x5A);
  EXPECT_EQ(PublishedStateByte(1), 0x5A);
}

TEST_F(BridgeHookTest, NullHookIsIgnored) {
  bridge::AddStateHook(nullptr);
  const std::vector<uint8_t> report = StateReport(0x11);
  bridge::OnBtInput(report.data(), report.size());
  EXPECT_EQ(PublishedStateByte(0), 0x11);
}

TEST_F(BridgeHookTest, ClearStateHooksStopsDispatch) {
  bridge::AddStateHook(&RewriteFirstByte);
  bridge::ClearStateHooks();
  const std::vector<uint8_t> report = StateReport(0x11);
  bridge::OnBtInput(report.data(), report.size());

  EXPECT_EQ(PublishedStateByte(0), 0x11);
  EXPECT_EQ(g_calls, 0);
}

TEST_F(BridgeHookTest, MicFramesBypassHooks) {
  // flags bit1 的 0x31 是上行麦克风帧：它不发布 USB 输入报告，钩子也不该跑。
  std::vector<uint8_t> frame(bridge::detail::kMicReportSize, 0);
  frame[0] = core::protocol::kBtInputReportId;
  frame[1] = bridge::detail::kMicFlagMask;

  bridge::AddStateHook(&RewriteFirstByte);
  bridge::OnBtInput(frame.data(), frame.size());

  EXPECT_EQ(g_calls, 0);
  EXPECT_EQ(PublishedStateByte(0), 0x00);  // 还是 Init 发布的那份空闲报告
}

}  // namespace
