// core::device 静态设备描述测试。

#include <gtest/gtest.h>

#include <cstring>

#include "core/device/descriptors.h"
#include "core/device/identity.h"

TEST(CoreDescriptorsTest, IdentityIsDualSense) {
  EXPECT_EQ(core::device::kDualSenseIdentity.vendor_id, 0x054c);
  EXPECT_EQ(core::device::kDualSenseIdentity.product_id, 0x0ce6);
  EXPECT_STREQ(core::device::kDualSenseIdentity.manufacturer,
               "Sony Interactive Entertainment");
  EXPECT_STREQ(core::device::kDualSenseIdentity.product,
               "DualSense Wireless Controller");
}

TEST(CoreDescriptorsTest, FfsBlockHasExpectedLayout) {
  EXPECT_EQ(core::device::kFfsFsDescCount, 4u);
  EXPECT_EQ(sizeof(core::device::kFfsFsDescriptors), 32u);
  EXPECT_EQ(core::device::kFfsFsDescriptors[core::device::kFfsHidDescOffset],
            core::device::kFfsHidDescLength);
  EXPECT_EQ(
      core::device::kFfsFsDescriptors[core::device::kFfsHidDescOffset + 1],
      core::device::kDescriptorTypeHid);
}

TEST(CoreDescriptorsTest, HidReportDescriptorHasExpectedLength) {
  EXPECT_EQ(sizeof(core::device::kHidReportDescriptor), 337u);
  EXPECT_EQ(core::device::kHidReportDescriptor[0], 0x05);
  EXPECT_EQ(core::device::kHidReportDescriptor[1], 0x01);
}
