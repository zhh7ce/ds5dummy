// DS5Dummy — options 模块单测
//
// options 现在是 worker 交接编解码：Manager 把生效配置写成 argv，worker 解析
// 回来，两侧必须严格互逆，否则 Manager 传给子进程的参数会和它自己解析到的不是
// 同一件事。用户级设置走配置文件，命令行里不该再出现。

#include "daemon/options.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

TEST(Options, Defaults) {
  ds5::daemon::Options o = ds5::daemon::internal::ParseArgv({});
  EXPECT_TRUE(o.error.empty()) << o.error;
  EXPECT_FALSE(o.worker);
  EXPECT_FALSE(o.show_help);
  EXPECT_FALSE(o.no_audio);
  EXPECT_TRUE(o.bt_addr.empty());
  // 设备目录是可选交接项：默认（Manager 没解析到）就是空串，worker 据此不写属性。
  EXPECT_TRUE(o.host_sysfs_path.empty());
  EXPECT_EQ(o.verbosity, 0);
  EXPECT_EQ(o.bt_wait_s, 60);
  EXPECT_EQ(o.ep0_fd, -1);
  EXPECT_EQ(o.ep_in_fd, -1);
  EXPECT_EQ(o.ep_out_fd, -1);
}

TEST(Options, ParsesAllInternalFlags) {
  ds5::daemon::Options o = ds5::daemon::internal::ParseArgv(
      {"--worker", "--ep0", "3", "--ep-in", "4", "--ep-out", "5", "--bt-addr",
       "AA:BB:CC:DD:EE:FF", "--bt-wait", "5", "-v", "DEBUG", "--no-audio"});
  EXPECT_TRUE(o.error.empty()) << o.error;
  EXPECT_TRUE(o.worker);
  EXPECT_EQ(o.ep0_fd, 3);
  EXPECT_EQ(o.ep_in_fd, 4);
  EXPECT_EQ(o.ep_out_fd, 5);
  EXPECT_EQ(o.bt_addr, "AA:BB:CC:DD:EE:FF");
  EXPECT_EQ(o.bt_wait_s, 5);
  EXPECT_EQ(o.verbosity, 1);  // -v DEBUG 解析成 loguru 的 1
  EXPECT_TRUE(o.no_audio);
}

TEST(Options, ParsesAdvancedFlags) {
  ds5::daemon::Options o = ds5::daemon::internal::ParseArgv(
      {"--haptics-gain", "1.5", "--speaker-gain", "5"});
  EXPECT_TRUE(o.error.empty()) << o.error;
  EXPECT_FLOAT_EQ(o.haptics_gain, 1.5f);
  EXPECT_EQ(o.speaker_gain, 5);

  // 两端都在闭区间内：0 = 静音，2 = 两倍。
  EXPECT_FLOAT_EQ(
      ds5::daemon::internal::ParseArgv({"--haptics-gain", "0"}).haptics_gain,
      0.0f);
  EXPECT_FLOAT_EQ(
      ds5::daemon::internal::ParseArgv({"--haptics-gain", "2"}).haptics_gain,
      2.0f);
}

TEST(Options, RejectsAdvancedFlagsOutsideTheirRange) {
  for (const char* value : {"-0.1", "2.5", "nan", "inf", "loud", ""}) {
    ds5::daemon::Options o = ds5::daemon::internal::ParseArgv(
        {"--haptics-gain", value});
    EXPECT_FALSE(o.error.empty()) << value;
  }
  for (const char* value : {"-1", "8", "2.0", "loud"}) {
    ds5::daemon::Options o = ds5::daemon::internal::ParseArgv(
        {"--speaker-gain", value});
    EXPECT_FALSE(o.error.empty()) << value;
  }
}

TEST(Options, WorkerNeedsAllThreeEndpointFds) {
  EXPECT_FALSE(
      ds5::daemon::internal::ParseArgv({"--worker"}).error.empty());

  ds5::daemon::Options missing_out =
      ds5::daemon::internal::ParseArgv(
          {"--worker", "--ep0", "3", "--ep-in", "4"});
  EXPECT_FALSE(missing_out.error.empty());

  ds5::daemon::Options missing_in =
      ds5::daemon::internal::ParseArgv(
          {"--worker", "--ep0", "3", "--ep-out", "5"});
  EXPECT_FALSE(missing_in.error.empty());

  // 不带 --worker 时三个 fd 号都不是必需项：Manager 侧解析的就是这种形态。
  EXPECT_TRUE(
      ds5::daemon::internal::ParseArgv({"--bt-wait", "7"}).error.empty());
}

TEST(Options, RejectsNonNumericOrNegativeFd) {
  EXPECT_FALSE(ds5::daemon::internal::ParseArgv(
                   {"--worker", "--ep0", "not-a-number", "--ep-in", "4",
                    "--ep-out", "5"})
                   .error.empty());
  EXPECT_FALSE(ds5::daemon::internal::ParseArgv(
                   {"--worker", "--ep0", "-1", "--ep-in", "4", "--ep-out", "5"})
                   .error.empty());
}

TEST(Options, RejectsUnknownOptionAndMissingValue) {
  ds5::daemon::Options unknown =
      ds5::daemon::internal::ParseArgv({"--nope"});
  EXPECT_FALSE(unknown.error.empty());
  EXPECT_NE(unknown.error.find("--nope"), std::string::npos) << unknown.error;

  EXPECT_FALSE(
      ds5::daemon::internal::ParseArgv({"--bt-addr"}).error.empty());
  EXPECT_FALSE(ds5::daemon::internal::ParseArgv({"--bt-wait", "abc"})
                   .error.empty());
}

// 用户级设置已经搬到 YAML，旧的命令行开关必须变成"不认识的选项"，而不是被
// 静默忽略——否则照着老文档敲命令的人会以为设置生效了。
TEST(Options, RejectsRetiredUserFacingFlags) {
  for (const char* retired : {"--ffs-path", "--uid", "--gid"}) {
    ds5::daemon::Options o =
        ds5::daemon::internal::ParseArgv({retired, "1"});
    EXPECT_FALSE(o.error.empty()) << retired;
    EXPECT_NE(o.error.find(retired), std::string::npos) << o.error;
  }
}

TEST(Options, RejectsUnknownVerbosityInsteadOfLettingLoguruAbort) {
  EXPECT_FALSE(
      ds5::daemon::internal::ParseArgv({"-v", "LOUD"}).error.empty());
  EXPECT_EQ(ds5::daemon::internal::ParseArgv({"-v", "OFF"}).verbosity,
            -9);
  EXPECT_EQ(ds5::daemon::internal::ParseArgv({"-v", "2"}).verbosity, 2);
}

TEST(Options, HelpIsNotAnError) {
  ds5::daemon::Options o =
      ds5::daemon::internal::ParseArgv({"--help"});
  EXPECT_TRUE(o.error.empty()) << o.error;
  EXPECT_TRUE(o.show_help);
}

// ParseArgs 是 main 的真入口：必须剥掉 argv[0] 再交给 ParseArgv。
TEST(Options, ParseArgsStripsProgramName) {
  const char* raw[] = {"ds5dummy", "--bt-wait", "7", "--no-audio"};
  char* argv[] = {const_cast<char*>(raw[0]), const_cast<char*>(raw[1]),
                  const_cast<char*>(raw[2]), const_cast<char*>(raw[3])};
  ds5::daemon::Options o = ds5::daemon::ParseArgs(4, argv);
  EXPECT_TRUE(o.error.empty()) << o.error;
  EXPECT_EQ(o.bt_wait_s, 7);
  EXPECT_TRUE(o.no_audio);

  // 只有程序名：等价于空参数，走默认值而不是报错。
  char* bare[] = {const_cast<char*>("ds5dummy")};
  ds5::daemon::Options d = ds5::daemon::ParseArgs(1, bare);
  EXPECT_TRUE(d.error.empty()) << d.error;
  EXPECT_EQ(d.bt_wait_s, 60);
  EXPECT_FALSE(d.worker);
}

// ToArgv 产出 execv 直接吃的 NULL 结尾数组。
TEST(Options, ToArgvIsNullTerminated) {
  const std::vector<std::string> av{"ds5dummy", "--worker", "--no-audio"};
  std::vector<char*> cargv = ds5::daemon::ToArgv(av);
  ASSERT_EQ(cargv.size(), av.size() + 1);
  EXPECT_STREQ(cargv[0], "ds5dummy");
  EXPECT_STREQ(cargv[1], "--worker");
  EXPECT_STREQ(cargv[2], "--no-audio");
  EXPECT_EQ(cargv.back(), nullptr);
}

// 所有字段都非默认时的往返：BuildWorkerArgv 产出的 argv 必须能被 ParseArgv
// 原样读回，这是"两侧看到同一份生效配置"的唯一保证。
TEST(Options, WorkerArgvRoundTripsWithOptionalFields) {
  ds5::daemon::Options src;
  src.bt_addr = "AA:BB:CC:DD:EE:FF";
  src.verbosity = 1;
  src.bt_wait_s = 42;
  src.no_audio = true;
  // 0.123456789f 是 std::to_string 往返会失真的值：固定 6 位小数写回后不是
  // 同一个 float，这里用它钉住 format_gain 的最短表示契约。
  src.haptics_gain = 0.123456789f;
  src.speaker_gain = 6;
  // 建站时解析到的主机侧 USB 接口目录，相对 /sys；值以 '/' 开头，正好验证它作为
  // 独立参数不会被解析器当成选项名，而名字里的冒号（`9-1:1.0`）验证它不被当成分隔符。
  src.host_sysfs_path = "/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0";
  // Manager 在建好实例后把交接目标号写进这三个字段（handoff 固定 3/4/5）。
  src.ep0_fd = 3;
  src.ep_in_fd = 4;
  src.ep_out_fd = 5;

  std::vector<std::string> av =
      ds5::daemon::BuildWorkerArgv(src, "/usr/bin/ds5dummy");
  ASSERT_FALSE(av.empty());
  EXPECT_EQ(av[0], "/usr/bin/ds5dummy");

  std::vector<std::string> tail(av.begin() + 1, av.end());
  ds5::daemon::Options got =
      ds5::daemon::internal::ParseArgv(tail);
  EXPECT_TRUE(got.error.empty()) << got.error;
  EXPECT_TRUE(got.worker);
  EXPECT_EQ(got.ep0_fd, 3);
  EXPECT_EQ(got.ep_in_fd, 4);
  EXPECT_EQ(got.ep_out_fd, 5);
  EXPECT_EQ(got.bt_addr, src.bt_addr);
  EXPECT_EQ(got.verbosity, src.verbosity);
  EXPECT_EQ(got.bt_wait_s, src.bt_wait_s);
  EXPECT_EQ(got.no_audio, src.no_audio);
  EXPECT_FLOAT_EQ(got.haptics_gain, src.haptics_gain);
  EXPECT_EQ(got.speaker_gain, src.speaker_gain);
  EXPECT_EQ(got.host_sysfs_path, src.host_sysfs_path);
}

// 可选字段全为默认时同样要往返：缺省不写进 argv，读回后仍是默认值。
TEST(Options, WorkerArgvRoundTripsDefaults) {
  ds5::daemon::Options src;
  src.ep0_fd = 3;
  src.ep_in_fd = 4;
  src.ep_out_fd = 5;

  const std::vector<std::string> av =
      ds5::daemon::BuildWorkerArgv(src, "ds5dummy");
  const std::vector<std::string> tail(av.begin() + 1, av.end());
  // 可选段一个都不该出现：空 bt_addr 若被写成 "--bt-addr" 会成缺值错误。
  for (const std::string& arg : tail) {
    EXPECT_NE(arg, "--bt-addr") << "空 bt_addr 不应出现在 argv 里";
    EXPECT_NE(arg, "--no-audio") << "默认的 no_audio 不应出现在 argv 里";
    EXPECT_NE(arg, "--haptics-gain") << "默认增益 1.0 不应出现在 argv 里";
    EXPECT_NE(arg, "--speaker-gain") << "默认增益 2 不应出现在 argv 里";
    EXPECT_NE(arg, "--sysfs-path") << "没解析到设备目录时不应写出该参数";
  }

  ds5::daemon::Options got = ds5::daemon::internal::ParseArgv(tail);
  EXPECT_TRUE(got.error.empty()) << got.error;
  EXPECT_TRUE(got.worker);
  EXPECT_TRUE(got.bt_addr.empty());
  EXPECT_TRUE(got.host_sysfs_path.empty());
  EXPECT_EQ(got.bt_wait_s, 60);
  EXPECT_FALSE(got.no_audio);
  EXPECT_FLOAT_EQ(got.haptics_gain, 1.0f);
  EXPECT_EQ(got.speaker_gain, 2);
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

// 用法文本对用户只说配置文件：两份路径都要出现，内部交接参数不再列出来。
TEST(Usage, PointsAtBothConfigFiles) {
  const std::string usage = ds5::daemon::internal::UsageText();
  EXPECT_NE(usage.find("/etc/ds5dummy/config.yaml"), std::string::npos)
      << usage;
  EXPECT_NE(usage.find(".config/ds5dummy/config.yaml"), std::string::npos)
      << usage;
  EXPECT_EQ(usage.find("--bt-addr"), std::string::npos) << usage;
  EXPECT_EQ(usage.find("--ffs-path"), std::string::npos) << usage;
}
