// DS5Dummy — usb 数据面与站点坐标的单测
//
// 覆盖两类不碰 fd 的纯逻辑：PostInputReport / LatestInputReport /
// InputWrites（纯进程内状态——UsbDataPlane 里 input_mutex 保护的
// latest_input 槽、pending_ready 标志与 input_writes 原子计数，不需要真实
// FunctionFS），以及站点坐标派生（ResolveGadgetId）和主机侧 sysfs 反查
// （detail::FindHostDeviceDir 与 FindHostInterfaceDir，用临时目录搭出的假
// sysfs 树）。ep0_* /
// StartThreads / Provision / Adopt 全部依赖端点 fd，归硬件验证清单，不在
// 这里调用。
//
// 交接单位是整份报告：本层收到的第一个字节就是主机看到的那个字节，报告
// 布局的正确性由 usb_report_test.cpp 针对 bridge::detail 那一侧断言。

#include "usb/usb.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include "core/protocol/report.h"
#include "usb/gadget_setup.h"

namespace {

// 缓存槽容量：与数据面的覆盖式槽位同为 USB 输入报告的线尺寸。
constexpr size_t kSlot = core::protocol::kUsbInputReportSize;

// 发布→读取的往返：整份报告按字节原样进槽、原样出来，搬运不做任何加工。
TEST(UsbInputCache, PostThenLatestRoundTrips) {
  std::array<uint8_t, kSlot> report{};
  for (size_t i = 0; i < report.size(); ++i) {
    report[i] = static_cast<uint8_t>(i);
  }

  usb::PostInputReport(report.data(), report.size());

  std::array<uint8_t, kSlot> out{};
  out.fill(0xFF);
  usb::LatestInputReport(out.data(), out.size());
  EXPECT_EQ(report, out);
}

// 超长发布只截断不报错：槽位是定长缓冲区，截断是它的既定行为。
TEST(UsbInputCache, PostTruncatesToSlotSize) {
  // 超过一槽的发布只留前 kSlot 字节，多出来的部分被丢弃。
  std::array<uint8_t, kSlot + 16> oversized{};
  oversized.fill(0xAB);
  for (size_t i = 0; i < kSlot; ++i) {
    oversized[i] = static_cast<uint8_t>(0x10 + i);
  }

  usb::PostInputReport(oversized.data(), oversized.size());

  std::array<uint8_t, kSlot> out{};
  usb::LatestInputReport(out.data(), out.size());
  // 槽里就是来报告的前 kSlot 字节，0xAB 的尾巴没有渗进来。
  for (size_t i = 0; i < kSlot; ++i) {
    EXPECT_EQ(out[i], static_cast<uint8_t>(0x10 + i)) << "at " << i;
  }
}

// 作废式发布（null 指针、零长度）不能碰槽位：主机读到的还是上一份完整报告，
// 而不是半帧或被清零的空帧。两种无效入参各测一次。
TEST(UsbInputCache, EmptyPublishKeepsPreviousReport) {
  std::array<uint8_t, kSlot> report{};
  report.fill(0x5C);  // 非零哨兵：槽位若被清零或截断，这里立刻看得出来
  usb::PostInputReport(report.data(), report.size());

  // 两次无效入参各一次：有长度无内容（null 指针）、有内容零长度。
  usb::PostInputReport(nullptr, kSlot);
  usb::PostInputReport(report.data(), 0);

  std::array<uint8_t, kSlot> out{};
  out.fill(0x00);
  usb::LatestInputReport(out.data(), out.size());
  EXPECT_EQ(out, report);
}

// 读取侧的 cap 是硬边界：只覆写前 cap 字节，越界部分一个都不许碰。
TEST(UsbInputCache, LatestHonorsSmallerCap) {
  std::array<uint8_t, kSlot> report{};
  report.fill(0x5C);
  usb::PostInputReport(report.data(), report.size());

  std::array<uint8_t, kSlot> out{};
  out.fill(0x00);
  // 只要前 8 字节，其余保持调用方原有的 0x00 不被覆写。
  usb::LatestInputReport(out.data(), 8);

  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(out[i], 0x5C) << "copied at " << i;
  }
  for (size_t i = 8; i < kSlot; ++i) {
    EXPECT_EQ(out[i], 0x00) << "untouched at " << i;
  }
}

TEST(UsbInputCache, InputWritesIsZeroWithoutWriterThread) {
  // 计数只由 IN 写线程递增；这里从未 StartThreads，读到初源即 0。
  EXPECT_EQ(usb::InputWrites(), 0u);
}

TEST(UsbAdopt, RejectsUnsetEndpointFds) {
  // 认领的门面校验走 usb::detail::FfsDevice::Adopt：fd<0 在 fcntl 之前短路判负，
  // 不涉及真实端点，因此可在无硬件环境验证。
  EXPECT_FALSE(usb::Adopt(usb::Endpoints{-1, -1, -1}));
  EXPECT_FALSE(usb::Adopt(usb::Endpoints{5, -1, 6}));
  EXPECT_FALSE(usb::Adopt(usb::Endpoints{5, 6, -1}));
}

TEST(UsbPathSafety, AcceptsPlainDeviceAndConfigPaths) {
  EXPECT_TRUE(usb::IsSafeGadgetPath("/dev/ffs-ds5"));
  EXPECT_TRUE(usb::IsSafeGadgetPath("/sys/kernel/config/usb_gadget/ds5dummy"));
}

TEST(UsbPathSafety, RejectsShellTraversalAndEmpty) {
  EXPECT_FALSE(usb::IsSafeGadgetPath(""));
  EXPECT_FALSE(usb::IsSafeGadgetPath("/dev/ffs;rm"));
  EXPECT_FALSE(usb::IsSafeGadgetPath("/dev/ffs$(id)"));
  EXPECT_FALSE(usb::IsSafeGadgetPath("/dev/ffs|tee"));
  EXPECT_FALSE(usb::IsSafeGadgetPath("/dev/ffs dir"));
  EXPECT_FALSE(usb::IsSafeGadgetPath("/dev/../etc/passwd"));
}

// ---- 站点坐标（实例名 / 挂载点 / 序列号）派生 ----

// MAC 是本站全部坐标的唯一来源：实例名、挂载点、序列号三处必须同源，
// 否则拆站时坐标对不上，会留下拆不掉的 configfs 残留。
TEST(UsbGadgetId, DerivesInstanceMountAndSerial) {
  const usb::GadgetId id =
      usb::ResolveGadgetId("AA:BB:CC:DD:EE:FF", "/dev/ffs-ds5");
  EXPECT_EQ(id.instance, "ds5dummy-aabbccddeeff");
  EXPECT_EQ(id.ffs_path, "/dev/ffs-ds5.aabbccddeeff");
  EXPECT_EQ(id.serial, "aabbccddeeff");
}

// ':'、'-' 与大小写只是同一枚 MAC 的不同写法：三处派生结果必须逐字相同，
// 否则同一只手柄两次上线会被当成两只，各占一个 dummy_udc。
TEST(UsbGadgetId, NotationDoesNotChangeCoordinates) {
  const usb::GadgetId colon =
      usb::ResolveGadgetId("aa:bb:cc:dd:ee:ff", "/dev/ffs-ds5");
  const usb::GadgetId dash =
      usb::ResolveGadgetId("aa-bb-cc-dd-ee-ff", "/dev/ffs-ds5");
  const usb::GadgetId bare =
      usb::ResolveGadgetId("AABBCCDDEEFF", "/dev/ffs-ds5");
  EXPECT_EQ(colon.instance, dash.instance);
  EXPECT_EQ(colon.instance, bare.instance);
  EXPECT_EQ(colon.ffs_path, bare.ffs_path);
  EXPECT_EQ(colon.serial, bare.serial);
}

// 序列号逐站唯一是硬要求：主机把 iSerialNumber 当设备实例标识，两站共用一个
// 值会让主机把它们看成同一台设备反复插拔。
TEST(UsbGadgetId, DifferentMacsGiveDifferentSerials) {
  const usb::GadgetId a =
      usb::ResolveGadgetId("AA:BB:CC:DD:EE:FF", "/dev/ffs-ds5");
  const usb::GadgetId b =
      usb::ResolveGadgetId("AA:BB:CC:DD:EE:01", "/dev/ffs-ds5");
  EXPECT_NE(a.serial, b.serial);
  EXPECT_NE(a.instance, b.instance);
  EXPECT_NE(a.ffs_path, b.ffs_path);
}

// 非 12 个十六进制数字的 MAC 一律拒绝，且三个字段同时为空——调用方只看
// instance 是否为空就能判定"这枚 MAC 不能建站"。
TEST(UsbGadgetId, RejectsMalformedMac) {
  for (const char* bad :
       {"", "aa:bb:cc:dd:ee", "zz:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ee:ff:00"}) {
    const usb::GadgetId id = usb::ResolveGadgetId(bad, "/dev/ffs-ds5");
    EXPECT_TRUE(id.instance.empty()) << "mac: " << bad;
    EXPECT_TRUE(id.ffs_path.empty()) << "mac: " << bad;
    EXPECT_TRUE(id.serial.empty()) << "mac: " << bad;
  }
}

// ---- 主机侧 sysfs 反查 ----

/**
 * @brief 在临时目录里搭出 dummy_hcd 的 sysfs 形状，供设备目录与接口目录两个
 *        反查函数（FindHostDeviceDir / FindHostInterfaceDir）走查。
 *
 * 真实形状是两个同索引的兄弟平台设备：`<sys>/devices/platform/dummy_udc.0/udc/
 * dummy_udc.0`（UDC 自己）与 `<sys>/devices/platform/dummy_hcd.0/usb9/9-1`
 * （主机侧 root hub 及其孩子）。root hub 不在 UDC 目录的上溯路径里，所以这棵
 * 假树必须把两者摆成兄弟——把它们摆成父子会让测试通过而真机失败。
 * 这里把 `<sys>` 换成临时目录根，总线号保持和实测一致，其余属性文件由用例按需
 * 创建。测试不碰真 /sys，也不需要 root。
 */
class FakeHostSysfs : public ::testing::Test {
 protected:
  /// 同一进程内区分多个用例的目录序号；单测二进制内串行执行，够用。
  static inline int case_seq = 0;

  /// 建本用例专属的假 sysfs 根，并先摆好 UDC 目录（被测函数的起点）。
  void SetUp() override {
    // pid 进目录名：ctest 会把每个用例当独立进程并发跑，只用序号会让两个
    // 进程在同一时刻撞进同一个目录。
    root_ = std::filesystem::temp_directory_path() /
            ("ds5dummy_sysfs_" + std::to_string(::getpid()) + "_" +
             std::to_string(++case_seq));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(udc_dir());
  }

  /// 删掉本用例搭的临时树；忽略删除失败（临时目录，下一次开机自然清）。
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  /// 假 sysfs 里 `/sys/devices/platform` 的那一层，两个平台设备都挂在它下面。
  std::filesystem::path platform_dir() const {
    return root_ / "devices" / "platform";
  }

  /// 主机控制器目录：被测函数按 UDC 名换算出的兄弟目录。
  std::filesystem::path hcd_dir() const {
    return platform_dir() / "dummy_hcd.0";
  }

  /// 传给被测函数的 UDC 目录（真机上是 /sys/class/udc 的 canonical 结果）。
  std::filesystem::path udc_dir() const {
    return platform_dir() / "dummy_udc.0" / "udc" / "dummy_udc.0";
  }

  /// 建一个设备目录并写它的 idVendor/idProduct；hub 与设备用同一种文件形状。
  void MakeUsbDir(const std::filesystem::path& dir, uint16_t vendor,
                  uint16_t product) {
    std::filesystem::create_directories(dir);
    // sysfs 以 %04x 输出，即小写四位、无 0x 前缀、结尾一个换行。
    std::ofstream(dir / "idVendor") << std::format("{:04x}\n", vendor);
    std::ofstream(dir / "idProduct") << std::format("{:04x}\n", product);
  }

  std::filesystem::path root_;
};

TEST_F(FakeHostSysfs, FindsChildMatchingVendorAndProduct) {
  MakeUsbDir(hcd_dir() / "usb9", 0x1D6B, 0x0003);  // root hub 自己是内核 HCD 的身份
  const std::filesystem::path pad = hcd_dir() / "usb9" / "9-1";
  MakeUsbDir(pad, 0x054C, 0x0CE6);

  EXPECT_EQ(usb::detail::FindHostDeviceDir(udc_dir().string(), 0x054C, 0x0CE6),
            pad.string());
}

// root hub 自身的 idVendor/idProduct 不参与匹配：它挂在 hub 那一层，不是孩子。
// 这一条挡住的是"把控制器当成手柄"的错误。
TEST_F(FakeHostSysfs, IgnoresTheRootHubItself) {
  MakeUsbDir(hcd_dir() / "usb9", 0x054C, 0x0CE6);
  EXPECT_TRUE(
      usb::detail::FindHostDeviceDir(udc_dir().string(), 0x054C, 0x0CE6)
          .empty());
}

// 身份不符（另一台同厂不同型号的设备）必须返回空，调用方据此继续等待或放弃。
TEST_F(FakeHostSysfs, RejectsDifferentProduct) {
  MakeUsbDir(hcd_dir() / "usb9", 0x1D6B, 0x0003);
  MakeUsbDir(hcd_dir() / "usb9" / "9-1", 0x054C, 0x0DF2);
  EXPECT_TRUE(
      usb::detail::FindHostDeviceDir(udc_dir().string(), 0x054C, 0x0CE6).empty());
}

// 枚举还没完成时配对出的控制器目录下没有 usbM 孩子（本用例连控制器目录都没
// 建）：这是正常时序而不是错误。
TEST_F(FakeHostSysfs, EmptyBeforeEnumeration) {
  EXPECT_TRUE(
      usb::detail::FindHostDeviceDir(udc_dir().string(), 0x054C, 0x0CE6).empty());
}

// UDC 目录名之外的非法字符（含 ".."）在遍历之前就被白名单挡下。
TEST_F(FakeHostSysfs, RejectsUnsafePath) {
  EXPECT_TRUE(usb::detail::FindHostDeviceDir("/sys/../etc", 0x054C, 0x0CE6)
                  .empty());
}

// 配对靠同索引，不靠"随便哪个 dummy_hcd"：dummy_udc.3 只能走 dummy_hcd.3，
// 索引 0 那台的孩子哪怕身份完全相符也不算命中。
TEST_F(FakeHostSysfs, UsesTheMatchingControllerIndex) {
  const std::filesystem::path udc3 =
      platform_dir() / "dummy_udc.3" / "udc" / "dummy_udc.3";
  std::filesystem::create_directories(udc3);
  MakeUsbDir(hcd_dir() / "usb9" / "9-1", 0x054C, 0x0CE6);
  EXPECT_TRUE(
      usb::detail::FindHostDeviceDir(udc3.string(), 0x054C, 0x0CE6).empty());

  const std::filesystem::path pad3 =
      platform_dir() / "dummy_hcd.3" / "usb12" / "12-1";
  MakeUsbDir(pad3, 0x054C, 0x0CE6);
  EXPECT_EQ(usb::detail::FindHostDeviceDir(udc3.string(), 0x054C, 0x0CE6),
            pad3.string());
}

// 换算不出配对控制器的 UDC 名（真实控制器如 dwc2）返回空而不是猜：宁可不
// 填 device.sysfs.path，也不能把别的控制器的孩子认成本站。
TEST_F(FakeHostSysfs, RejectsUnpairedUdcName) {
  const std::filesystem::path udc =
      platform_dir() / "dwc2.1" / "udc" / "dwc2.1";
  std::filesystem::create_directories(udc);
  MakeUsbDir(hcd_dir() / "usb9" / "9-1", 0x054C, 0x0CE6);
  EXPECT_TRUE(
      usb::detail::FindHostDeviceDir(udc.string(), 0x054C, 0x0CE6).empty());
}

// ---- 设备目录之下的接口目录 ----

// 真机形状：设备目录 9-1 下面是接口目录，同级还有平台设备的通用条目。本站描述符
// 只声明一个接口（`9-1:1.0`），这里多摆一个 `9-1:1.1` 钉住"取字典序最小"——父设备
// 是同一个，选谁都不改变算出的容器，规则要的只是结果跨重启稳定。
TEST_F(FakeHostSysfs, PicksTheSmallestInterfaceDir) {
  const std::filesystem::path pad = hcd_dir() / "usb9" / "9-1";
  MakeUsbDir(pad, 0x054C, 0x0CE6);
  std::filesystem::create_directories(pad / "9-1:1.1");
  std::filesystem::create_directories(pad / "9-1:1.0");
  std::filesystem::create_directories(pad / "driver");     // 通用条目，无冒号
  std::filesystem::create_directories(pad / "subsystem");  // 同上
  std::ofstream(pad / "bDeviceProtocol") << "00\n";  // 属性文件也在这同一层

  EXPECT_EQ(usb::detail::FindHostInterfaceDir(pad.string()),
            (pad / "9-1:1.0").string());
}

// 判据是"设备名 + 冒号"这一整段前缀，不是"名字里带冒号"：同层的 9-10:1.0 是
// 另一个设备口的接口名，前缀是本站设备名的超集；靠"见到冒号就算"会把别人的
// 接口交出去，父链一回溯就成了另一台设备。
TEST_F(FakeHostSysfs, RequiresColonRightAfterTheDeviceName) {
  const std::filesystem::path pad = hcd_dir() / "usb9" / "9-1";
  MakeUsbDir(pad, 0x054C, 0x0CE6);
  std::filesystem::create_directories(pad / "9-10:1.0");
  std::filesystem::create_directories(pad / "9-1x:1.0");

  EXPECT_TRUE(usb::detail::FindHostInterfaceDir(pad.string()).empty());
}

// 设备目录已经建出来、接口目录一个都还没有（内核枚举走到一半）：空串，与设备
// 目录那一层的反查共用同一套"稍后再试"语义。
TEST_F(FakeHostSysfs, EmptyWhileInterfacesAreMissing) {
  const std::filesystem::path pad = hcd_dir() / "usb9" / "9-1";
  MakeUsbDir(pad, 0x054C, 0x0CE6);
  std::filesystem::create_directories(pad / "power");

  EXPECT_TRUE(usb::detail::FindHostInterfaceDir(pad.string()).empty());
}

// 连设备目录都不在：directory_iterator 的失败走 error_code 出参，不向上抛，
// 调用方拿到的仍然是空串。
TEST_F(FakeHostSysfs, EmptyWhenTheDeviceDirIsGone) {
  EXPECT_TRUE(
      usb::detail::FindHostInterfaceDir((hcd_dir() / "usb9" / "9-1").string())
          .empty());
}

// 含 ".." 的入参在遍历之前就被白名单挡下：这棵假树建在 /tmp 下，真去遍历会让
// 结果取决于机器上恰好有什么。
TEST_F(FakeHostSysfs, RejectsUnsafeInterfacePath) {
  EXPECT_TRUE(
      usb::detail::FindHostInterfaceDir((root_ / ".." / "..").string()).empty());
}

}  // namespace
