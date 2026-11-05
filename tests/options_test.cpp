// DS5Dummy — options 模块单测
//
// options 现在是"命令行两个开关 + worker 配置快照的编解码"。Manager 把生效配置
// 编成一份变长二进制快照写进 memfd，worker 的新镜像解码回来；两侧共用同一对
// 函数，所以往返必须逐字段一致，结构损坏必须在解码点被拒绝，而不是把半份配置
// 交给数据面。用户级设置走配置文件，命令行里不该再出现。

#include "daemon/options.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/device/identity.h"

namespace {

/// @brief 编码再解码一份配置，断言解码成功。
ds5::daemon::Options RoundTrip(const ds5::daemon::Options& src) {
  const std::vector<uint8_t> blob = src.Encode();
  ds5::daemon::Options out;
  std::string why;
  EXPECT_TRUE(out.Decode(blob.data(), blob.size(), why)) << why;
  return out;
}

/// @brief 改掉快照头里的 total_bytes，用于构造"长度自洽但内容不完整"的样本。
void SetTotalBytes(std::vector<uint8_t>& blob, uint32_t total) {
  std::memcpy(blob.data() + 4, &total, sizeof total);
}

}  // namespace

TEST(Options, Defaults) {
  const ds5::daemon::Options o;
  EXPECT_FALSE(o.worker);
  EXPECT_FALSE(o.show_help);
  EXPECT_TRUE(o.controller.audio);
  // auto：配置层没写 usb_model，Manager 起站时才用发现结果填上。
  EXPECT_FALSE(o.controller.usb_model.has_value());
  EXPECT_TRUE(o.controller.bt_addr.empty());
  // 设备目录是可选交接项：默认（Manager 没解析到）就是空串，worker 据此不写属性。
  EXPECT_TRUE(o.controller.host_sysfs_path.empty());
  EXPECT_EQ(o.verbosity, 0);
  EXPECT_EQ(o.controller.bt_wait_s, 60);
  // 插件配置默认是空的：配置没写 plugins 就等于每个插件用自己的默认值。
  EXPECT_TRUE(o.controller.plugins.empty());
  EXPECT_EQ(o.controller.usb_report_interval_ms, 1);
  EXPECT_TRUE(o.error.empty());
}

// Options 的数据面字段与 ControllerConfig 是同一份定义：整份赋值即可，不需要
// 改名、取反或再拼一遍。
TEST(Options, CopiesPerControllerSettingsWholesale) {
  ds5::daemon::ControllerConfig settings;
  settings.bt_addr = "AA:BB:CC:DD:EE:FF";
  settings.bt_wait_s = 12;
  settings.audio = false;
  settings.haptics_gain = 0.5f;
  settings.speaker_gain = 5;
  settings.usb_report_interval_ms = 2;
  settings.usb_model = core::device::ControllerModel::kEdge;
  settings.plugins["idle.off_after_minutes"] = "5";
  settings.host_sysfs_path = "/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0";

  ds5::daemon::Options o;
  o.controller = settings;
  EXPECT_EQ(o.controller.bt_addr, settings.bt_addr);
  EXPECT_EQ(o.controller.bt_wait_s, settings.bt_wait_s);
  EXPECT_EQ(o.controller.audio, settings.audio);
  EXPECT_FLOAT_EQ(o.controller.haptics_gain, settings.haptics_gain);
  EXPECT_EQ(o.controller.speaker_gain, settings.speaker_gain);
  EXPECT_EQ(o.controller.usb_report_interval_ms,
            settings.usb_report_interval_ms);
  EXPECT_EQ(o.controller.usb_model, settings.usb_model);
  EXPECT_EQ(o.controller.plugins, settings.plugins);
  EXPECT_EQ(o.controller.host_sysfs_path, settings.host_sysfs_path);
}

TEST(ParseArgs, RecognizesWorkerAndHelp) {
  const char* raw[] = {"ds5dummyd", "--worker", "--help"};
  char* argv[] = {const_cast<char*>(raw[0]), const_cast<char*>(raw[1]),
                  const_cast<char*>(raw[2])};
  const ds5::daemon::Options o = ds5::daemon::ParseArgs(3, argv);
  EXPECT_TRUE(o.error.empty()) << o.error;
  EXPECT_TRUE(o.worker);
  EXPECT_TRUE(o.show_help);
}

TEST(ParseArgs, StripsProgramName) {
  // 只有程序名：等价于空参数，走默认值而不是报错。
  char* bare[] = {const_cast<char*>("ds5dummyd")};
  const ds5::daemon::Options o = ds5::daemon::ParseArgs(1, bare);
  EXPECT_TRUE(o.error.empty()) << o.error;
  EXPECT_FALSE(o.worker);
  EXPECT_FALSE(o.show_help);
}

// 用户级设置已经搬到 YAML，旧的命令行开关必须变成"不认识的选项"，而不是被
// 静默忽略——否则照着老文档敲命令的人会以为设置生效了。
TEST(ParseArgs, RejectsUnknownAndRetiredOptions) {
  for (const char* retired :
       {"--bt-addr", "--bt-wait", "--plugin-config", "--ep0", "--ep-in",
        "--ep-out", "--no-audio", "--usb-edge", "--usb-report-interval-ms",
        "--haptics-gain", "--speaker-gain", "--sysfs-path", "-v", "--ffs-path",
        "--uid", "--gid"}) {
    const char* raw[] = {"ds5dummyd", retired, "1"};
    char* argv[] = {const_cast<char*>(raw[0]), const_cast<char*>(raw[1]),
                    const_cast<char*>(raw[2])};
    const ds5::daemon::Options o = ds5::daemon::ParseArgs(3, argv);
    EXPECT_FALSE(o.error.empty()) << retired;
    EXPECT_NE(o.error.find(retired), std::string::npos) << o.error;
  }
}

// 所有字段都非默认时的往返：字符串、浮点与有序插件表都要原样回来。
TEST(Snapshot, RoundTripsAllFields) {
  ds5::daemon::Options src;
  src.controller.bt_addr = "AA:BB:CC:DD:EE:FF";
  src.verbosity = 1;
  src.controller.bt_wait_s = 42;
  src.controller.plugins["idle.off_after_minutes"] = "45";
  src.controller.plugins["idle.verbose"] = "1";
  src.controller.plugins["button-map.left_stick"] = "trigger";
  src.controller.audio = false;
  src.controller.usb_model = core::device::ControllerModel::kEdge;
  src.controller.usb_report_interval_ms = 4;
  // 0.123456789f 按原始位型走线格式：encode/decode 不是十进制往返，不能丢精度。
  src.controller.haptics_gain = 0.123456789f;
  src.controller.speaker_gain = 6;
  // 以 '/' 开头、带冒号的接口目录：证明路径不经过任何分隔符解析。
  src.controller.host_sysfs_path =
      "/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0";

  const ds5::daemon::Options got = RoundTrip(src);
  EXPECT_EQ(got.controller.bt_addr, src.controller.bt_addr);
  EXPECT_EQ(got.verbosity, src.verbosity);
  EXPECT_EQ(got.controller.bt_wait_s, src.controller.bt_wait_s);
  EXPECT_EQ(got.controller.plugins, src.controller.plugins);
  EXPECT_EQ(got.controller.audio, src.controller.audio);
  EXPECT_EQ(got.controller.usb_model, src.controller.usb_model);
  EXPECT_EQ(got.controller.usb_report_interval_ms,
            src.controller.usb_report_interval_ms);
  EXPECT_FLOAT_EQ(got.controller.haptics_gain, src.controller.haptics_gain);
  EXPECT_EQ(got.controller.speaker_gain, src.controller.speaker_gain);
  EXPECT_EQ(got.controller.host_sysfs_path, src.controller.host_sysfs_path);
}

// 另一个具体型号也要原样回来：0/1/2 三个编码值各对应一种状态。
TEST(Snapshot, RoundTripsDualSenseModel) {
  ds5::daemon::Options src;
  src.controller.usb_model = core::device::ControllerModel::kDualSense;
  const ds5::daemon::Options got = RoundTrip(src);
  ASSERT_TRUE(got.controller.usb_model.has_value());
  EXPECT_EQ(*got.controller.usb_model,
            core::device::ControllerModel::kDualSense);
}

// 全默认配置同样要往返：空字符串与空插件表是合法内容，不是"缺字段"。
TEST(Snapshot, RoundTripsDefaults) {
  const ds5::daemon::Options src;
  const ds5::daemon::Options got = RoundTrip(src);
  EXPECT_TRUE(got.controller.bt_addr.empty());
  EXPECT_TRUE(got.controller.host_sysfs_path.empty());
  EXPECT_EQ(got.controller.bt_wait_s, 60);
  EXPECT_TRUE(got.controller.plugins.empty());
  EXPECT_TRUE(got.controller.audio);
  EXPECT_FALSE(got.controller.usb_model.has_value());
  EXPECT_EQ(got.controller.usb_report_interval_ms, 1);
  EXPECT_FLOAT_EQ(got.controller.haptics_gain, 1.0f);
  EXPECT_EQ(got.controller.speaker_gain, 2);
}

// 字符串是"长度 + 原始字节"，不是 C 字符串：内嵌 NUL 与空值都必须原样保留。
TEST(Snapshot, PreservesEmbeddedNulAndEmptyValues) {
  ds5::daemon::Options src;
  src.controller.bt_addr = std::string("AA\0BB", 5);
  src.controller.plugins["idle.key"] = "";
  src.controller.plugins["idle.blob"] = std::string("x\0y", 3);

  const ds5::daemon::Options got = RoundTrip(src);
  EXPECT_EQ(got.controller.bt_addr, src.controller.bt_addr);
  EXPECT_EQ(got.controller.bt_addr.size(), 5u);
  EXPECT_EQ(got.controller.plugins, src.controller.plugins);
}

// 解码只写配置字段：调用方在读取快照前已经用 ParseArgs 填好的进程内标志不能被
// 清掉，否则 worker 会丢掉自己的角色。
TEST(Snapshot, LeavesRuntimeFlagsUntouched) {
  ds5::daemon::Options out;
  out.worker = true;
  out.show_help = true;
  out.error = "keep me";
  const std::vector<uint8_t> blob = ds5::daemon::Options{}.Encode();
  std::string why;
  ASSERT_TRUE(out.Decode(blob.data(), blob.size(), why)) << why;
  EXPECT_TRUE(out.worker);
  EXPECT_TRUE(out.show_help);
  EXPECT_EQ(out.error, "keep me");
}

TEST(Snapshot, RejectsBadMagic) {
  std::vector<uint8_t> blob = ds5::daemon::Options{}.Encode();
  blob[0] ^= 0xFF;
  ds5::daemon::Options out;
  std::string why;
  EXPECT_FALSE(out.Decode(blob.data(), blob.size(), why));
  EXPECT_NE(why.find("magic"), std::string::npos) << why;
}

// usb_model 只编码 0/1/2 三个值；其余字节说明快照损坏，不能当成"未知型号"。
// 型号字节在头里的偏移：magic 4 + total 4 + 四个 i32 16 + f32 4 + audio 1。
TEST(Snapshot, RejectsBadUsbModelCode) {
  std::vector<uint8_t> blob = ds5::daemon::Options{}.Encode();
  blob[29] = 3;
  ds5::daemon::Options out;
  std::string why;
  EXPECT_FALSE(out.Decode(blob.data(), blob.size(), why));
  EXPECT_NE(why.find("usb model"), std::string::npos) << why;
}

TEST(Snapshot, RejectsTruncatedHeader) {
  const std::vector<uint8_t> blob = ds5::daemon::Options{}.Encode();
  ds5::daemon::Options out;
  std::string why;
  EXPECT_FALSE(out.Decode(nullptr, 0, why));
  EXPECT_FALSE(out.Decode(blob.data(), 10, why));
  EXPECT_FALSE(out.Decode(blob.data(), 31, why));
}

TEST(Snapshot, RejectsLengthMismatch) {
  const std::vector<uint8_t> blob = ds5::daemon::Options{}.Encode();
  ds5::daemon::Options out;
  std::string why;

  // 少一个字节：头里的 total 仍是原长。
  EXPECT_FALSE(out.Decode(blob.data(), blob.size() - 1, why));
  EXPECT_NE(why.find("length"), std::string::npos) << why;

  // 多一个字节但不改 total：同样按长度不符拒绝。
  std::vector<uint8_t> longer = blob;
  longer.push_back(0);
  why.clear();
  EXPECT_FALSE(out.Decode(longer.data(), longer.size(), why));
  EXPECT_NE(why.find("length"), std::string::npos) << why;
}

// 长度自洽但内部字符串越界：不能把半个字符串读进配置。
TEST(Snapshot, RejectsStringLongerThanSnapshot) {
  ds5::daemon::Options src;
  src.controller.bt_addr = "AA:BB";
  std::vector<uint8_t> blob = src.Encode();
  // 头 32 字节之后是 bt_addr 的长度字段（magic 4 + total 4 + 五个标量 20 +
  // 两个 u8 + 2 字节保留）。把它改成远超剩余字节的值。
  const uint32_t huge = 0xFFFFFFFFu;
  std::memcpy(blob.data() + 32, &huge, sizeof huge);
  ds5::daemon::Options out;
  std::string why;
  EXPECT_FALSE(out.Decode(blob.data(), blob.size(), why));
  EXPECT_NE(why.find("truncated"), std::string::npos) << why;
}

// 长度自洽但末尾多了不属于任何字段的字节：格式错误，不是"多读一点也无所谓"。
TEST(Snapshot, RejectsTrailingBytes) {
  std::vector<uint8_t> blob = ds5::daemon::Options{}.Encode();
  blob.push_back(0x7F);
  SetTotalBytes(blob, static_cast<uint32_t>(blob.size()));
  ds5::daemon::Options out;
  std::string why;
  EXPECT_FALSE(out.Decode(blob.data(), blob.size(), why));
  EXPECT_NE(why.find("trailing"), std::string::npos) << why;
}

// 每个 loguru 级别名都要落到约定数值；TRACE 是 VERBOSE 的别名。
TEST(Verbosity, MapsAllLevelNames) {
  struct Case {
    const char* name;
    int want;
  };
  const Case cases[] = {
      {"OFF", -9}, {"FATAL", -3}, {"ERROR", -2},  {"WARNING", -1},
      {"INFO", 0}, {"DEBUG", 1},  {"VERBOSE", 2}, {"TRACE", 2},
  };
  for (const Case& c : cases) {
    int out = 12345;
    EXPECT_TRUE(ds5::daemon::internal::ParseVerbosity(c.name, out))
        << c.name;
    EXPECT_EQ(out, c.want) << c.name;
  }
}

// 每个可配置级别名的往返：名字 → 数值 → 名字 → 同一数值；TRACE 是 VERBOSE 别名。
TEST(Verbosity, EveryNamedLevelRoundTripsThroughVerbosityName) {
  struct Case {
    const char* parsed;     // 配置文件里可以写出的名字
    const char* canonical;  // VerbosityName 回写的规范名
  };
  const Case cases[] = {
      {"OFF", "OFF"},         {"FATAL", "FATAL"}, {"ERROR", "ERROR"},
      {"WARNING", "WARNING"}, {"INFO", "INFO"},   {"DEBUG", "DEBUG"},
      {"VERBOSE", "VERBOSE"}, {"TRACE", "VERBOSE"},
  };
  for (const Case& c : cases) {
    int level = 12345;
    ASSERT_TRUE(ds5::daemon::internal::ParseVerbosity(c.parsed, level))
        << c.parsed;
    const char* name = ds5::daemon::internal::VerbosityName(level);
    ASSERT_NE(name, nullptr) << c.parsed;
    EXPECT_STREQ(name, c.canonical) << c.parsed;
    // 回写出的名字必须解析回同一个数值，否则横幅显示的级别与实际生效不符。
    int again = 12345;
    EXPECT_TRUE(ds5::daemon::internal::ParseVerbosity(name, again)) << name;
    EXPECT_EQ(again, level) << c.parsed;
  }
}

// 表外的整数没有名字：调用方据此打印数字，也不会发生 int8_t 截断后的别名。
TEST(Verbosity, UnnamedIntegersHaveNoName) {
  EXPECT_EQ(ds5::daemon::internal::VerbosityName(5), nullptr);
  EXPECT_EQ(ds5::daemon::internal::VerbosityName(255), nullptr);
  EXPECT_EQ(ds5::daemon::internal::VerbosityName(-4), nullptr);
}

TEST(Verbosity, AcceptsSignedIntegers) {
  int out = 0;
  EXPECT_TRUE(ds5::daemon::internal::ParseVerbosity("5", out));
  EXPECT_EQ(out, 5);
  EXPECT_TRUE(ds5::daemon::internal::ParseVerbosity("-3", out));
  EXPECT_EQ(out, -3);
}

TEST(Verbosity, RejectsEmptySignOnlyAndNonNumeric) {
  int out = 0;
  EXPECT_FALSE(ds5::daemon::internal::ParseVerbosity(
      "", out));  // 空串
  EXPECT_FALSE(
      ds5::daemon::internal::ParseVerbosity("-", out));  // 只有符号
  EXPECT_FALSE(ds5::daemon::internal::ParseVerbosity("LOUD", out));
  EXPECT_FALSE(ds5::daemon::internal::ParseVerbosity("12x", out));
}

// PrintUsage 把用法首行的程序名换成实际 argv[0]；nullptr 回落 "ds5dummy"。
TEST(Usage, PrintUsageReplacesProgName) {
  testing::internal::CaptureStdout();
  ds5::daemon::PrintUsage("./ds5dummyd");
  const std::string out = testing::internal::GetCapturedStdout();
  EXPECT_NE(out.find("Usage: ./ds5dummyd"), std::string::npos) << out;

  testing::internal::CaptureStdout();
  ds5::daemon::PrintUsage(nullptr);
  const std::string fallback = testing::internal::GetCapturedStdout();
  EXPECT_NE(fallback.find("Usage: ds5dummy"), std::string::npos) << fallback;
}

// 用法文本对用户只说配置文件：两份路径都要出现，内部角色开关与旧参数都不列。
TEST(Usage, PointsAtBothConfigFiles) {
  const std::string usage = ds5::daemon::internal::UsageText();
  EXPECT_NE(usage.find("/etc/ds5dummy/config.yaml"), std::string::npos)
      << usage;
  EXPECT_NE(usage.find(".config/ds5dummy/config.yaml"), std::string::npos)
      << usage;
  EXPECT_EQ(usage.find("--worker"), std::string::npos) << usage;
  EXPECT_EQ(usage.find("--bt-addr"), std::string::npos) << usage;
  EXPECT_EQ(usage.find("--ffs-path"), std::string::npos) << usage;
}
