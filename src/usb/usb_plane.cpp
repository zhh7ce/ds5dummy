/**
 * @file usb_plane.cpp
 * @brief 数据面共享状态的唯一实例
 *
 * 只有 `Get()` 那一个进程级 static；全部字段的含义与并发契约见
 * usb_plane.h，循环本体不在这里（usb_inout.cpp、usb_ep0.cpp）。
 */

#include "usb/usb_plane.h"

#include <cstring>

namespace usb::plane {

DataPlane& Get() {
  static DataPlane plane;
  return plane;
}

void DataPlane::PublishInput(const uint8_t* report, size_t len) {
  const size_t copied =
      len < input_report.size() ? len : input_report.size();
  std::memcpy(input_report.data(), report, copied);
  // 短于线尺寸时清尾巴：保持的是一份完整状态，而不是新数据配上一轮的残尾。
  std::memset(input_report.data() + copied, 0, input_report.size() - copied);
  // real-time 档的 IN 线程靠这一位区分"新样本"和"已写过的一份"；重发档不看它。
  pending_ready = true;
}

const std::array<uint8_t, core::protocol::kUsbInputReportSize>&
DataPlane::LatestInput() const {
  return input_report;
}

}  // namespace usb::plane
