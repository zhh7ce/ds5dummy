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

TEST(CoreDescriptorsTest, EdgeIdentityIsDualSenseEdge) {
  EXPECT_EQ(core::device::kDualSenseEdgeIdentity.vendor_id, 0x054c);
  EXPECT_EQ(core::device::kDualSenseEdgeIdentity.product_id, 0x0df2);
  EXPECT_STREQ(core::device::kDualSenseEdgeIdentity.manufacturer,
               "Sony Interactive Entertainment");
  EXPECT_STREQ(core::device::kDualSenseEdgeIdentity.product,
               "DualSense Edge Wireless Controller");
  EXPECT_EQ(sizeof(core::device::kHidReportDescriptorEdge), 453u);
}

TEST(CoreDescriptorsTest, ModelForHidIdentityAcceptsRealAndSyntheticEdges) {
  using core::device::ControllerModel;
  // 真机蓝牙 HID 身份与 USB 身份同值：Edge 的两组都判成 Edge。
  EXPECT_EQ(core::device::ModelForHidIdentity(0x054c, 0x0df2),
            ControllerModel::kEdge);
  EXPECT_EQ(core::device::ModelForHidIdentity(0x1998, 0x1103),
            ControllerModel::kEdge);
  EXPECT_EQ(core::device::ModelForHidIdentity(0x054c, 0x0ce6),
            ControllerModel::kDualSense);
  EXPECT_EQ(core::device::ModelForHidIdentity(0x1998, 0x1221),
            ControllerModel::kDualSense);
}

TEST(CoreDescriptorsTest, ProfileForKeepsIdentityAndDescriptorTogether) {
  const core::device::DeviceProfile& ds =
      core::device::ProfileFor(core::device::ControllerModel::kDualSense);
  const core::device::DeviceProfile& edge =
      core::device::ProfileFor(core::device::ControllerModel::kEdge);
  EXPECT_EQ(ds.identity.product_id, 0x0ce6);
  EXPECT_EQ(ds.report_descriptor_size, 337u);
  EXPECT_EQ(edge.identity.product_id, 0x0df2);
  EXPECT_EQ(edge.report_descriptor_size, 453u);
  // size 必须与指针实际指向的数组一致，否则 wDescriptorLength 会与应答错位。
  EXPECT_EQ(ds.report_descriptor, core::device::kHidReportDescriptor);
  EXPECT_EQ(edge.report_descriptor, core::device::kHidReportDescriptorEdge);
}

TEST(CoreDescriptorsTest, FfsBlockCarriesDs5DescriptorLength) {
  // 数组里冻结的是普通 DualSense 的长度；Edge 由装配期按偏移覆写。
  EXPECT_EQ(core::device::kFfsWDescLenOffset, 16u);
  const uint8_t lo =
      core::device::kFfsFsDescriptors[core::device::kFfsWDescLenOffset];
  const uint8_t hi =
      core::device::kFfsFsDescriptors[core::device::kFfsWDescLenOffset + 1];
  const uint32_t len = lo | (static_cast<uint32_t>(hi) << 8);
  EXPECT_EQ(len, sizeof(core::device::kHidReportDescriptor));
}
