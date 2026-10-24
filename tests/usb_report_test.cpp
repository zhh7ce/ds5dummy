// DS5Dummy — USB 0x01 输入报告布局（bridge/usb_report.cpp）的单测
//
// 断言到字节偏移：[0]=0x01，[1..63] 逐字节照搬状态体，总长即
// kUsbInputReportSize。这份布局原来藏在 usb::PostInputReport 的 report_id
// 参数里，收进 bridge 之后由本文件锁住；usb 侧只测搬运（见 usb_test.cpp）。

#include "bridge/usb_report.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "core/protocol/report.h"

namespace {

// 布局契约：ID 落在 [0]，状态体从 [1] 起逐字节照搬，不偏移、不重排。
TEST(UsbReportLayout, PlacesIdByteThenStateBodyByteForByte) {
  std::array<uint8_t, core::protocol::kUsbInputStateSize> state{};
  // 从 0x20 起的递增序列：每字节互不相同且都不为 0，位移错一格或漏写一节
  // 都会撞见不相等的邻居，全零槽混进来也一眼可见。
  for (size_t i = 0; i < state.size(); ++i) {
    state[i] = static_cast<uint8_t>(0x20 + i);
  }

  std::array<uint8_t, core::protocol::kUsbInputReportSize> report{};
  report.fill(0xFF);  // 哨兵：布局若漏写某个字节，这里就会留下来
  bridge::detail::BuildUsbInputReport(report.data(), state.data());

  EXPECT_EQ(report[0], core::protocol::kUsbInputReportId);
  for (size_t i = 0; i < state.size(); ++i) {
    EXPECT_EQ(report[i + 1], state[i]) << "at state byte " << i;
  }
}

// 空闲报告（bridge::Init 占槽用）的形态：除 ID 外全零且铺满整帧——主机在
// 第一批 BT 输入到达之前也能读到结构合法的一帧。
TEST(UsbReportLayout, IdleReportIsIdPlusZeroState) {
  std::array<uint8_t, core::protocol::kUsbInputStateSize> zeros{};
  std::array<uint8_t, core::protocol::kUsbInputReportSize> report{};
  report.fill(0xA5);  // 哨兵：布局若漏写某个字节，这里会把它留在原地
  bridge::detail::BuildUsbInputReport(report.data(), zeros.data());

  EXPECT_EQ(report[0], core::protocol::kUsbInputReportId);
  for (size_t i = 1; i < report.size(); ++i) {
    EXPECT_EQ(report[i], 0x00) << "at " << i;
  }
}

}  // namespace
