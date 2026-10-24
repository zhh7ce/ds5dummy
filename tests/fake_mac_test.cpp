// DS5Dummy — 伪装 MAC（fake_mac.cpp）的单测
//
// 都是纯字节逻辑，不碰 fd、不碰链路，无手柄即可全覆盖。测试点：
//   - FillFakeMac：逆序 MAC 且末位已翻、capacity 截断、nullptr/0 返回 0。
//   - FlipReplyMac：仅 report[0]==0x09 且 len>=7 才翻位，别的原样返回。
//   - 两条来源叠起来线上都只翻一次（BothPathsFlipOnce）。
//
// 两处够不到的：
//   - "非 0x09 不兜底" 的判据在 OnClassRequest 的分支条件里，要 usb ep0 的
//     桩才够得到（TODO.md §4 还欠的 test_bridge）。
//   - 0x32 唤醒报告是 L2CAP 后端里的一份固定字节数组，建链末尾组好即发，
//     无手柄时没有可断言的中间产物。

#include "bridge/fake_mac.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

TEST(BridgeFillFakeMac, WritesReversedMacWithLastOctetFlipped) {
  bridge::detail::SetFakeMac("AA:BB:CC:DD:EE:FF");
  std::array<uint8_t, 20> buf{};
  const int n = bridge::detail::FillFakeMac(buf.data(), buf.size());
  EXPECT_EQ(n, 19);  // capacity 20 → min(cap,19)=19
  // 逆序 FF EE DD CC BB AA，且末位（正常序）翻过一位：0xFF → 0xFE。
  EXPECT_EQ(buf[0], 0xFE);
  EXPECT_EQ(buf[1], 0xEE);
  EXPECT_EQ(buf[2], 0xDD);
  EXPECT_EQ(buf[3], 0xCC);
  EXPECT_EQ(buf[4], 0xBB);
  EXPECT_EQ(buf[5], 0xAA);
  for (size_t i = 6; i < 19; ++i) EXPECT_EQ(buf[i], 0x00) << i;
}

TEST(BridgeFillFakeMac, RespectsSmallCapacity) {
  bridge::detail::SetFakeMac("11:22:33:44:55:66");
  std::array<uint8_t, 3> buf{};
  EXPECT_EQ(bridge::detail::FillFakeMac(buf.data(), buf.size()), 3);
  EXPECT_EQ(buf[0], 0x67);  // 0x66 翻末位
  EXPECT_EQ(buf[1], 0x55);
  EXPECT_EQ(buf[2], 0x44);
}

TEST(BridgeFillFakeMac, NullOrEmptyReturnsZero) {
  std::array<uint8_t, 8> buf{};
  EXPECT_EQ(bridge::detail::FillFakeMac(nullptr, 8), 0);
  EXPECT_EQ(bridge::detail::FillFakeMac(buf.data(), 0), 0);
}

TEST(BridgeSetFakeMac, InvalidParseKeepsPrevious) {
  bridge::detail::SetFakeMac("01:02:03:04:05:06");
  bridge::detail::SetFakeMac("not-a-mac");  // 解析失败应原样保留
  std::array<uint8_t, 20> buf{};
  bridge::detail::FillFakeMac(buf.data(), buf.size());
  EXPECT_EQ(buf[0], 0x07);  // 仍是上一次有效 MAC 的末位，翻过一位
  EXPECT_EQ(buf[1], 0x05);
  EXPECT_EQ(buf[5], 0x01);
}

TEST(BridgeFlipReplyMac, FlipsLastOctetForReport09) {
  std::array<uint8_t, 7> report{0x09, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  bridge::detail::FlipReplyMac(report.data(), report.size());
  EXPECT_EQ(report[1], 0xAB);  // 0xAA ^ 0x01
  // 只动 report[1]，其余不变。
  EXPECT_EQ(report[0], 0x09);
  EXPECT_EQ(report[2], 0xBB);
  EXPECT_EQ(report[6], 0xFF);
}

TEST(BridgeFlipReplyMac, LeavesOtherReportsUntouched) {
  std::array<uint8_t, 7> report{0x20, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  bridge::detail::FlipReplyMac(report.data(), report.size());
  EXPECT_EQ(report[1], 0xAA);  // id 不是 0x09 → 不翻位
}

TEST(BridgeFlipReplyMac, ShortOrEmptyGuarded) {
  std::array<uint8_t, 7> report{0x09, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  bridge::detail::FlipReplyMac(report.data(), 6);  // len<7 → 原样
  EXPECT_EQ(report[1], 0xAA);
  bridge::detail::FlipReplyMac(nullptr, 0);  // nullptr 安全早退，不崩
}

// 合成路径自己翻，真回复路径由 FlipReplyMac 翻：两条必须交同一份字节，
// 谁都不许被翻第二次（翻两次等没翻，主机又会撞回 -EEXIST）。
TEST(BridgeFakeMac, BothPathsFlipOnce) {
  bridge::detail::SetFakeMac("AA:BB:CC:DD:EE:FF");
  std::array<uint8_t, 20> filled{};
  ASSERT_EQ(bridge::detail::FillFakeMac(filled.data(), filled.size()), 19);

  std::array<uint8_t, 20> reply{0x09, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
  bridge::detail::FlipReplyMac(reply.data(), reply.size());
  for (size_t i = 0; i < 6; ++i) EXPECT_EQ(reply[i + 1], filled[i]) << i;
}
