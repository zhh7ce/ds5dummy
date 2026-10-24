// bt 模块 feature 缓存语义单测
//
// cache 现在是 bt 进程级单例（内部实现细节），通过 bt::detail::Cache* 内部 API
// 操作。每个用例起先清空，避免前一个用例的数据残留。

#include "bt/bt_detail.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

class BtCacheTest : public ::testing::Test {
 protected:
  void SetUp() override { bt::detail::CacheClear(); }
};

TEST_F(BtCacheTest, MissReturnsNegativeOne) {
  std::array<uint8_t, 8> buf{0x09};
  EXPECT_EQ(bt::detail::CacheLookup(buf.data(), buf.size()), -1);
}

TEST_F(BtCacheTest, StoresAndLooksUpCompleteReport) {
  const std::array<uint8_t, 5> report{0x09, 0x11, 0x22, 0x33, 0x44};
  bt::detail::CacheStore(0x09, report.data(), report.size());

  std::array<uint8_t, 8> out{0x09};
  EXPECT_EQ(bt::detail::CacheLookup(out.data(), out.size()), 5);
  EXPECT_EQ(out[0], 0x09);
  EXPECT_EQ(out[1], 0x11);
  EXPECT_EQ(out[4], 0x44);
}

TEST_F(BtCacheTest, LookupTruncatesToCallerCapacity) {
  const std::array<uint8_t, 6> report{0x20, 1, 2, 3, 4, 5};
  bt::detail::CacheStore(0x20, report.data(), report.size());

  std::array<uint8_t, 3> out{0x20};
  EXPECT_EQ(bt::detail::CacheLookup(out.data(), out.size()), 3);
  EXPECT_EQ(out[0], 0x20);
  EXPECT_EQ(out[1], 1);
  EXPECT_EQ(out[2], 2);
}

TEST_F(BtCacheTest, StoreOverwritesSameReportId) {
  const std::array<uint8_t, 3> first{0x09, 1, 2};
  const std::array<uint8_t, 3> second{0x09, 7, 8};
  bt::detail::CacheStore(0x09, first.data(), first.size());
  bt::detail::CacheStore(0x09, second.data(), second.size());

  std::array<uint8_t, 3> out{0x09};
  EXPECT_EQ(bt::detail::CacheLookup(out.data(), out.size()), 3);
  EXPECT_EQ(out[1], 7);
  EXPECT_EQ(out[2], 8);
}

TEST_F(BtCacheTest, ClearRemovesAllEntries) {
  const std::array<uint8_t, 2> report{0x09, 1};
  bt::detail::CacheStore(0x09, report.data(), report.size());
  bt::detail::CacheClear();

  std::array<uint8_t, 2> out{0x09};
  EXPECT_EQ(bt::detail::CacheLookup(out.data(), out.size()), -1);
}
