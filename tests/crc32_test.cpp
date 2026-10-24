// CRC-32 (DualSense 自定义 seed) 单测。
//
// 基准来自独立实现的反射 CRC-32 (poly 0xEDB88320)：
//   - seed=0 时 "123456789" 应得标准 CRC-32/ISO-HDLC 校验值 0xCBF43926，
//     这一条独立证明查表与主循环，不是拿被测代码当基准。
//   - 空数据 crc == seed，证明两个 seed 接线正确。
//   - 若干 golden + fill_* 小端布局，证明尾 4 字节写入位置/字节序。

#include "core/protocol/crc32.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

TEST(Crc32, MatchesStandardCheckValue) {
  const uint8_t msg[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(core::protocol::Crc32Seeded(msg, sizeof(msg), 0), 0xCBF43926u);
}

TEST(Crc32, EmptyDataReturnsSeed) {
  EXPECT_EQ(core::protocol::Crc32(nullptr, 0), 0xEADA2D49u);
  EXPECT_EQ(core::protocol::Crc32Feature(nullptr, 0), 0x2060EFC3u);
}

TEST(Crc32, GoldenVectors) {
  const std::array<uint8_t, 4> v1{0x31, 0x10, 0x90, 0x3f};
  EXPECT_EQ(core::protocol::Crc32(v1.data(), v1.size()), 0x5F53A078u);

  std::array<uint8_t, 20> v2{};
  for (size_t i = 0; i < v2.size(); ++i) v2[i] = static_cast<uint8_t>(i);
  EXPECT_EQ(core::protocol::Crc32(v2.data(), v2.size()), 0x34B318A4u);
  EXPECT_EQ(core::protocol::Crc32Feature(v2.data(), v2.size()), 0xB1177C5Cu);
}

TEST(Crc32, FillOutputChecksumWritesLittleEndian) {
  uint8_t buf[24]{};
  for (size_t i = 0; i < 20; ++i) buf[i] = static_cast<uint8_t>(i);
  core::protocol::FillOutputReportChecksum(buf, sizeof(buf));
  // crc32(buf, 20) == 0x34B318A4，尾 4 字节应为其小端表示。
  EXPECT_EQ(buf[20], 0xA4);
  EXPECT_EQ(buf[21], 0x18);
  EXPECT_EQ(buf[22], 0xB3);
  EXPECT_EQ(buf[23], 0x34);
  EXPECT_EQ(core::protocol::Crc32(buf, 20), 0x34B318A4u);
}

TEST(Crc32, FillFeatureChecksumWritesLittleEndian) {
  uint8_t buf[24]{};
  for (size_t i = 0; i < 20; ++i) buf[i] = static_cast<uint8_t>(i);
  core::protocol::FillFeatureReportChecksum(buf, sizeof(buf));
  // crc32_feature(buf, 20) == 0xB1177C5C 的小端表示。
  EXPECT_EQ(buf[20], 0x5C);
  EXPECT_EQ(buf[21], 0x7C);
  EXPECT_EQ(buf[22], 0x17);
  EXPECT_EQ(buf[23], 0xB1);
}
