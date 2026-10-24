// DS5Dummy — bt 后端契约单测
//
// bt.h 的绝大部分接口（Open/GetFeature/PostOutput/StartThreads…）都要真
// 手柄 + 已建立的 ACL 链路，无法在无硬件环境测——那些归 tools/spike_* 与集成
// 验收。这里只锁一个不碰硬件的契约点：
//   - detail::IsOpen()：连接前两个后端都应是 false（fd 初值为 -1）。
//
// 同一个测试二进制按 BT_BACKEND 编到不同后端。

#include <gtest/gtest.h>

#include "bt/bt.h"
#include "bt/bt_detail.h"

// 后端接缝的初值契约：没建过链时 IsOpen() 必须为 false，门面据此拒绝收发。
TEST(BtBackend, NotOpenBeforeConnect) {
  // 本测试进程从不调 bt::Open，后端全局 fd 保持初值 -1。
  EXPECT_FALSE(bt::detail::IsOpen());
}
