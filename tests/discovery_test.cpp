// DS5Dummy — sysfs 发现单测（daemon/discovery.cpp）
//
// 用临时目录伪造 /sys/class/hidraw 的节点结构（hidrawN/device/uevent），
// 覆盖三种认定条件（蓝牙总线、两型号 × 真机/合成共四对 VID/PID、合法 HID_UNIQ）
// 与去重/排序。全部用例不需要硬件：真实 sysfs 只读且内容随机器变化，不适合做断言。

#include "daemon/discovery.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

// 每个用例一个独立目录：进程号 + 自增序号，避免并行跑 ctest 时互相清目录。
int next_dir = 0;

class DiscoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = fs::temp_directory_path() /
            ("ds5dummy-discovery-" + std::to_string(::getpid()) + "-" +
             std::to_string(next_dir++));
    fs::create_directories(base_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(base_, ec);
  }

  /**
   * @brief 建一个 hidraw 节点目录并写入它的 device/uevent。
   * @param name 节点名（如 "hidraw3"）。
   * @param uevent uevent 文件全文，调用方决定包含哪些行。
   */
  void AddNode(const std::string& name, const std::string& uevent) {
    const fs::path dir = base_ / name / "device";
    fs::create_directories(dir);
    std::ofstream(dir / "uevent") << uevent;
  }

  fs::path base_;
};

/// 蓝牙总线上、DKMS 合成 VID/PID 的节点 uevent；MAC 用 aa:bb:cc:dd:ee:ff。
std::string FakeUevent() {
  return "DRIVER=hid-generic\n"
         "HID_ID=0005:00001998:00001221\n"
         "HID_NAME=Sony Interactive Entertainment DualSense Wireless Controller\n"
         "HID_UNIQ=aa:bb:cc:dd:ee:ff\n";
}

/// 蓝牙总线上、真机 VID/PID 的节点 uevent；MAC 用 11:22:33:44:55:66。
std::string SonyUevent() {
  return "DRIVER=playstation\n"
         "HID_ID=0005:0000054C:00000CE6\n"
         "HID_UNIQ=11:22:33:44:55:66\n";
}

TEST_F(DiscoveryTest, FindsBothVidPidPairsAndSortsByMac) {
  // 目录遍历顺序不可依赖，这里故意让 MAC 顺序与创建顺序相反。
  AddNode("hidraw0", FakeUevent());
  AddNode("hidraw1", SonyUevent());

  const std::vector<ds5::daemon::DiscoveredController> found =
      ds5::daemon::ScanControllers(base_.string());

  ASSERT_EQ(found.size(), 2u);
  // 结果按 MAC 升序，且 MAC 统一成大写。
  EXPECT_EQ(found[0].mac, "11:22:33:44:55:66");
  EXPECT_EQ(found[0].node, "hidraw1");
  EXPECT_EQ(found[1].mac, "AA:BB:CC:DD:EE:FF");
  EXPECT_EQ(found[1].node, "hidraw0");
}

TEST_F(DiscoveryTest, FindsDualSenseEdgeIdentities) {
  // DualSense Edge：真机 054C:0DF2 与 DKMS 合成的 1998:1103 都要认。
  AddNode("hidraw0",
          "HID_ID=0005:0000054C:00000DF2\n"
          "HID_UNIQ=11:22:33:44:55:66\n");
  AddNode("hidraw1",
          "HID_ID=0005:00001998:00001103\n"
          "HID_UNIQ=aa:bb:cc:dd:ee:ff\n");

  const std::vector<ds5::daemon::DiscoveredController> found =
      ds5::daemon::ScanControllers(base_.string());

  ASSERT_EQ(found.size(), 2u);
  EXPECT_EQ(found[0].node, "hidraw0");
  EXPECT_EQ(found[1].node, "hidraw1");
}

TEST_F(DiscoveryTest, SkipsUsbWiredControllers) {
  // 总线 0x0003 = USB；同一对 VID/PID 的直连手柄不归本程序桥接。
  AddNode("hidraw0",
          "HID_ID=0003:0000054C:00000CE6\n"
          "HID_UNIQ=11:22:33:44:55:66\n");

  EXPECT_TRUE(ds5::daemon::ScanControllers(base_.string()).empty());
}

TEST_F(DiscoveryTest, SkipsUnknownVendorOrProduct) {
  // 蓝牙总线，但 VID/PID 既不是真机也不是合成值。
  AddNode("hidraw0", "HID_ID=0005:0000045E:0000028E\n"
                     "HID_UNIQ=11:22:33:44:55:66\n");
  // 合成 VID 配错 PID：两段必须成对命中。
  AddNode("hidraw1", "HID_ID=0005:00001998:00000001\n"
                     "HID_UNIQ=22:33:44:55:66:77\n");

  EXPECT_TRUE(ds5::daemon::ScanControllers(base_.string()).empty());
}

TEST_F(DiscoveryTest, SkipsNodesWithoutUsableMac) {
  // 没有 HID_UNIQ：拿不到 MAC，无从分发。
  AddNode("hidraw0", "HID_ID=0005:0000054C:00000CE6\n");
  // UNIQ 不是 MAC 形式。
  AddNode("hidraw1", "HID_ID=0005:0000054C:00000CE6\n"
                     "HID_UNIQ=not-a-mac\n");
  // 没有 uevent 文件的节点（扫描中途消失的形态）。
  fs::create_directories(base_ / "hidraw2");

  EXPECT_TRUE(ds5::daemon::ScanControllers(base_.string()).empty());
}

TEST_F(DiscoveryTest, StripsTrailingCarriageReturn) {
  AddNode("hidraw0", "HID_ID=0005:0000054C:00000CE6\r\n"
                     "HID_UNIQ=11:22:33:44:55:66\r\n");

  const std::vector<ds5::daemon::DiscoveredController> found =
      ds5::daemon::ScanControllers(base_.string());
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].mac, "11:22:33:44:55:66");
}

TEST_F(DiscoveryTest, SameMacKeepsLexicographicallySmallestNode) {
  // 同一只手柄出现两个节点时只报一次，并固定选节点名最小的那个，
  // 让每轮扫描对同一份 sysfs 给出同一个答案。
  AddNode("hidraw2", FakeUevent());
  AddNode("hidraw10", FakeUevent());

  const std::vector<ds5::daemon::DiscoveredController> found =
      ds5::daemon::ScanControllers(base_.string());
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].mac, "AA:BB:CC:DD:EE:FF");
  EXPECT_EQ(found[0].node, "hidraw10");  // 字典序 "hidraw10" < "hidraw2"
}

TEST_F(DiscoveryTest, MissingBaseDirectoryYieldsNothing) {
  // hidraw 类目录不存在（没有加载任何 HID 设备）是正常状态，不报错。
  const fs::path missing = base_ / "nope";
  EXPECT_TRUE(ds5::daemon::ScanControllers(missing.string()).empty());
}

}  // namespace
