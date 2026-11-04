// DS5Dummy — 关机报告（power_report.h）的单测
//
// 这份 0x08 feature 报告是整包冻结的：没有运行时盖章，所以这里逐字节钉住
// 布局与 CRC 覆盖范围。CRC 用仓库自己的 Crc32Feature（reflect poly
// 0xEDB88320，seed 0x2060EFC3）复算，覆盖到报告总长减 4 字节为止。

#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "bridge/power_report.h"
#include "core/protocol/crc32.h"
#include "core/protocol/report.h"

namespace {

using Report = std::array<uint8_t, bridge::detail::kPowerOffReportSize>;

Report build_report() {
  Report out{};
  std::memcpy(out.data(), bridge::detail::kPowerOffReport, out.size());
  return out;
}

}  // namespace

TEST(PowerReport, IsThe0x08BluetoothControlReport) {
  const Report report = build_report();
  EXPECT_EQ(report.size(), 48u);
  EXPECT_EQ(report[0], core::protocol::kFeatureReportIdBluetooth);
  EXPECT_EQ(report[0], 0x08);
  EXPECT_EQ(report[1], 0x02);  // 关；0x01 是开
}

TEST(PowerReport, PayloadIsZeroPaddedBeforeTheChecksum) {
  const Report report = build_report();
  for (size_t i = 2; i < report.size() - 4; ++i) {
    EXPECT_EQ(report[i], 0x00) << "offset " << i;
  }
}

// Report Count 47 不含 report id，线上 48 字节；CRC 覆盖报告总长减尾部 4 字节
// （即 44 字节），起点是 report id。这一段同时钉住 seed 与长度——按 43 字节
// 覆盖会得到另一个值。
TEST(PowerReport, ChecksumCoversReportIdAndPayload) {
  const Report report = build_report();
  EXPECT_EQ(core::protocol::Crc32Feature(report.data(), report.size() - 4),
            0x23A2EFE0u);
}

TEST(PowerReport, ChecksumTailIsLittleEndian) {
  const Report report = build_report();
  EXPECT_EQ(report[44], 0xE0);
  EXPECT_EQ(report[45], 0xEF);
  EXPECT_EQ(report[46], 0xA2);
  EXPECT_EQ(report[47], 0x23);
}
