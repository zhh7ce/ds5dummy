/**
 * @file
 * @brief `bridge::detail::BuildUsbInputReport` 实现：USB 0x01 输入报告的布局
 */

#include "bridge/usb_report.h"

#include <cstddef>
#include <cstring>

#include "core/protocol/report.h"

namespace bridge {
namespace detail {

// 报告 = 1 字节 ID + 状态体：下面 memcpy 的长度算式以这个关系为前提。
static_assert(core::protocol::kUsbInputReportSize ==
                  core::protocol::kUsbInputStateSize + 1,
              "USB 输入报告的布局是 [ID][状态体]，两个长度常量必须相差 1 字节");

void BuildUsbInputReport(uint8_t* out, const uint8_t* state) {
  out[0] = core::protocol::kUsbInputReportId;
  std::memcpy(out + 1, state, core::protocol::kUsbInputStateSize);
}

}  // namespace detail
}  // namespace bridge
