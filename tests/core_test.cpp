// DS5Dummy — 测试骨架冒烟用例
//
// 唯一目的：证明 tests/ 的 include 路径、googletest 与 CTest 接线都通了，
// 后续每个任务的单测挂进这个目录。

#include <gtest/gtest.h>

#include "core/protocol/audio_format.h"

TEST(Smoke, TypesHeaderUsable) {
  EXPECT_EQ(core::protocol::kUsbInputStateSize, 63u);
}
