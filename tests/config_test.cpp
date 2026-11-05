// DS5Dummy — 启动配置单测（daemon/config.cpp）
//
// 覆盖 YAML schema 的严格解析（缺省值、未知键、类型错误、跨字段约束、
// log_level 复用 ParseVerbosity）、LoadUserConfig/LoadSystemConfig 的加固读取，
// 以及发布的用户模板与内置默认层的一致性（家目录没有配置时生效的就是后者）。
// 全部用例不需要 root 与手柄：临时目录建在测试进程自己的 uid 下，属主校验
// 天然成立；"属主不符"用把期望属主传错（getuid()+1）的方式覆盖，同样不需要
// chown。需要以 root 跑的用例显式 skip。

#include "daemon/config.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

namespace {

using ds5::daemon::BtTargetMode;
using ds5::daemon::ControllerConfig;
using ds5::daemon::IdentityMode;
using ds5::daemon::LoadResult;
using ds5::daemon::SystemConfig;
using ds5::daemon::UserConfig;

// ---------------------------------------------------------------------------
// ParseSystemConfig
// ---------------------------------------------------------------------------

TEST(SystemConfigParse, EmptyAndCommentOnlyYieldDefaults) {
  for (const char* text : {"", "# 只有注释\n", "\n\n", "identity:\n"}) {
    SystemConfig cfg;
    std::string why;
    ASSERT_TRUE(ds5::daemon::ParseSystemConfig(text, cfg, why))
        << text << ": " << why;
    EXPECT_EQ(cfg.ffs_path, "/dev/ffs-ds5");
    EXPECT_EQ(cfg.discovery_interval_s, 3);
    EXPECT_EQ(cfg.log_level, -1);
    EXPECT_EQ(cfg.identity_mode, IdentityMode::kAuto);
    EXPECT_TRUE(cfg.allowed_users.empty());
  }
}

TEST(SystemConfigParse, FullAutoConfigRoundTrips) {
  const std::string text =
      "ffs_path: /dev/ffs-other\n"
      "discovery_interval_s: 5\n"
      "log_level: INFO\n"
      "identity:\n"
      "  mode: auto\n";
  SystemConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseSystemConfig(text, cfg, why)) << why;
  EXPECT_EQ(cfg.ffs_path, "/dev/ffs-other");
  EXPECT_EQ(cfg.discovery_interval_s, 5);
  EXPECT_EQ(cfg.log_level, 0);  // INFO
  EXPECT_EQ(cfg.identity_mode, IdentityMode::kAuto);
  EXPECT_TRUE(cfg.allowed_users.empty());
}

TEST(SystemConfigParse, FullListConfigRoundTrips) {
  const std::string text =
      "identity:\n"
      "  mode: list\n"
      "  users:\n"
      "    - uid: 1000\n"
      "      gid: 998\n"
      "    - uid: 1001\n"
      "      gid: 1001\n";
  SystemConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseSystemConfig(text, cfg, why)) << why;
  EXPECT_EQ(cfg.identity_mode, IdentityMode::kList);
  ASSERT_EQ(cfg.allowed_users.size(), 2u);
  EXPECT_EQ(cfg.allowed_users[0].uid, 1000u);
  EXPECT_EQ(cfg.allowed_users[0].gid, 998u);
  EXPECT_EQ(cfg.allowed_users[1].uid, 1001u);
  EXPECT_EQ(cfg.allowed_users[1].gid, 1001u);
}

TEST(SystemConfigParse, RejectsUnknownKey) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig("ffs-path: /dev/ffs-ds5\n", cfg,
                                              why));
  EXPECT_NE(why.find("ffs-path"), std::string::npos) << why;
}

TEST(SystemConfigParse, RejectsUnknownNestedKey) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig(
      "identity:\n  mode: list\n  users:\n    - uid: 1000\n      gid: 998\n"
      "      home: /home/x\n",
      cfg, why));
  EXPECT_NE(why.find("home"), std::string::npos) << why;
}

TEST(SystemConfigParse, RejectsRootScalar) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig("just a string\n", cfg, why));
  EXPECT_FALSE(why.empty());
}

TEST(SystemConfigParse, ListWithoutUsersIsRejected) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(
      ds5::daemon::ParseSystemConfig("identity:\n  mode: list\n", cfg, why));
  EXPECT_NE(why.find("users"), std::string::npos) << why;
}

TEST(SystemConfigParse, AutoWithUsersIsRejected) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig(
      "identity:\n  mode: auto\n  users:\n    - uid: 1000\n      gid: 998\n",
      cfg, why));
  EXPECT_FALSE(why.empty());
}

TEST(SystemConfigParse, NonPositiveIdsAreRejected) {
  for (const char* text :
       {"identity:\n  mode: list\n  users:\n    - uid: 0\n      gid: 998\n",
        "identity:\n  mode: list\n  users:\n    - uid: 1000\n      gid: 0\n",
        "identity:\n  mode: list\n  users:\n    - uid: -1\n      gid: 998\n",
        "identity:\n  mode: list\n  users: [0]\n"}) {
    SystemConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseSystemConfig(text, cfg, why)) << text;
    EXPECT_FALSE(why.empty());
  }
}

// gid 省略时取 uid：单用户桌面上主组与 uid 同号是常态。
TEST(SystemConfigParse, GidDefaultsToUid) {
  SystemConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseSystemConfig(
      "identity:\n  mode: list\n  users:\n    - uid: 1000\n", cfg, why))
      << why;
  ASSERT_EQ(cfg.allowed_users.size(), 1u);
  EXPECT_EQ(cfg.allowed_users[0].uid, 1000u);
  EXPECT_EQ(cfg.allowed_users[0].gid, 1000u);
}

// 裸整数是 {uid, gid: uid} 的简写；混写两种形态也必须能解析。
TEST(SystemConfigParse, BareUidEntriesMeanGidEqualsUid) {
  SystemConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseSystemConfig(
      "identity:\n  mode: list\n  users: [1000, 1001]\n", cfg, why))
      << why;
  ASSERT_EQ(cfg.allowed_users.size(), 2u);
  EXPECT_EQ(cfg.allowed_users[0].uid, 1000u);
  EXPECT_EQ(cfg.allowed_users[0].gid, 1000u);
  EXPECT_EQ(cfg.allowed_users[1].uid, 1001u);
  EXPECT_EQ(cfg.allowed_users[1].gid, 1001u);
}

// uid 是唯一必填项：没有它就没有降权目标，gid 也无法推导。
TEST(SystemConfigParse, UserEntryNeedsUid) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig(
      "identity:\n  mode: list\n  users:\n    - gid: 1000\n", cfg, why));
  EXPECT_NE(why.find("uid"), std::string::npos) << why;
}

TEST(SystemConfigParse, InvalidModeIsRejected) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig(
      "identity:\n  mode: manual\n", cfg, why));
  EXPECT_NE(why.find("manual"), std::string::npos) << why;
}

TEST(SystemConfigParse, WrongTypesAreRejected) {
  for (const char* text :
       {"ffs_path: [a]\n", "log_level: [INFO]\n",
        "log_level: true\n", "identity: 5\n",
        "identity:\n  users: nope\n",
        "identity:\n  mode: list\n  users: {}\n",
        "identity:\n  mode: list\n  users:\n    - uid: abc\n      gid: 998\n",
        "identity:\n  mode: list\n  users:\n    - uid: 1000\n      gid: [1]\n"}) {
    SystemConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseSystemConfig(text, cfg, why)) << text;
    EXPECT_FALSE(why.empty()) << text;
  }
}

TEST(SystemConfigParse, LogLevelNamesAndIntegers) {
  struct Case {
    const char* text;
    int want;
  };
  // 名字表与整数语义必须与 -v 完全一致（loguru 枚举值）。
  const Case cases[] = {
      {"log_level: OFF\n", -9},     {"log_level: FATAL\n", -3},
      {"log_level: ERROR\n", -2},   {"log_level: WARNING\n", -1},
      {"log_level: INFO\n", 0},     {"log_level: DEBUG\n", 1},
      {"log_level: VERBOSE\n", 2},  {"log_level: TRACE\n", 2},
      {"log_level: 3\n", 3},        {"log_level: -5\n", -5},
  };
  for (const Case& c : cases) {
    SystemConfig cfg;
    std::string why;
    ASSERT_TRUE(ds5::daemon::ParseSystemConfig(c.text, cfg, why))
        << c.text << ": " << why;
    EXPECT_EQ(cfg.log_level, c.want) << c.text;
  }
}

TEST(SystemConfigParse, LogLevelOutOfIntRangeIsRejected) {
  // ParseVerbosity 的整数分支是 long → int 的无检查强转；4294967296 会回绕成 0，
  // 配置层必须在委托前拦下来，而不是静默接受一个错的级别。
  for (const char* text :
       {"log_level: 4294967296\n", "log_level: -2147483649\n",
        "log_level: 99999999999999999999999\n"}) {
    SystemConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseSystemConfig(text, cfg, why)) << text;
    EXPECT_NE(why.find("out of range"), std::string::npos) << why;
  }
}

TEST(SystemConfigParse, DiscoveryIntervalAcceptsItsBounds) {
  struct Case {
    const char* text;
    int want;
  };
  // 上限 2147483 = INT_MAX/1000：Manager 把秒乘成毫秒节拍，这个值之后乘积会回绕。
  const Case cases[] = {
      {"discovery_interval_s: 1\n", 1},
      {"discovery_interval_s: 3\n", 3},
      {"discovery_interval_s: 2147483\n", 2147483},
  };
  for (const Case& c : cases) {
    SystemConfig cfg;
    std::string why;
    ASSERT_TRUE(ds5::daemon::ParseSystemConfig(c.text, cfg, why))
        << c.text << ": " << why;
    EXPECT_EQ(cfg.discovery_interval_s, c.want) << c.text;
  }
}

TEST(SystemConfigParse, DiscoveryIntervalRejectsOutOfRangeAndBadTypes) {
  // 0 = 监督循环空转；2147484 是上限的下一个整数；1.5 不是整数节拍。
  for (const char* text :
       {"discovery_interval_s: 0\n", "discovery_interval_s: -1\n",
        "discovery_interval_s: 2147484\n",
        "discovery_interval_s: 99999999999999999999\n",
        "discovery_interval_s: 1.5\n", "discovery_interval_s: [3]\n",
        "discovery_interval_s: soon\n"}) {
    SystemConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseSystemConfig(text, cfg, why)) << text;
    EXPECT_FALSE(why.empty()) << text;
  }
}

TEST(SystemConfigParse, RejectsUnknownKeyDirectlyUnderIdentity) {
  SystemConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig(
      "identity:\n  role: auto\n", cfg, why));
  EXPECT_NE(why.find("role"), std::string::npos) << why;
}

TEST(SystemConfigParse, FailureLeavesOutUntouched) {
  SystemConfig cfg;
  cfg.ffs_path = "/dev/prior";
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseSystemConfig("bogus: 1\n", cfg, why));
  EXPECT_EQ(cfg.ffs_path, "/dev/prior");
}

// ---------------------------------------------------------------------------
// ParseUserConfig
// ---------------------------------------------------------------------------

TEST(UserConfigParse, EmptyDocumentDefaultsToAuto) {
  // 空文件等于"启用 + 发现哪只手柄就服务哪只"，符合插上即用的默认体验。
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig("", cfg, why)) << why;
  EXPECT_TRUE(cfg.enabled);
  EXPECT_EQ(cfg.bt_mode, BtTargetMode::kAuto);
  EXPECT_TRUE(cfg.bt_addr.empty());
  EXPECT_EQ(cfg.defaults.bt_wait_s, 60);
  EXPECT_TRUE(cfg.defaults.audio);
  EXPECT_FLOAT_EQ(cfg.defaults.haptics_gain, 1.0f);
  EXPECT_EQ(cfg.defaults.speaker_gain, 2);  // 固件初值，不是"中立"的 0
  EXPECT_EQ(cfg.defaults.usb_report_interval_ms, 1);
  EXPECT_FALSE(cfg.defaults.usb_model.has_value());  // auto：跟随发现结果
  // 默认插件层是空的：装载由插件目录决定，这一节只给同名插件传键。
  EXPECT_TRUE(cfg.defaults.plugins.empty());
  EXPECT_TRUE(cfg.controllers.empty());
  EXPECT_FALSE(cfg.log_level.has_value());
}

TEST(UserConfigParse, ShippedTemplateMatchesBuiltinDefaults) {
  // 家目录没有配置时 Manager 用的就是默认构造的 UserConfig（LoadUserConfig 在
  // kAbsent 不动出参），而用户照抄的是 pkg/user-config.example.yaml。这两份必须
  // 是同一个答案：模板里每个未注释掉的取值都必须等于内置默认，否则"没配置文件"
  // 和"从模板复制一份"会走出两种行为——而模板正是用来宣告默认值的。
  std::ifstream in(DS5DUMMY_USER_TEMPLATE);
  ASSERT_TRUE(in.is_open()) << DS5DUMMY_USER_TEMPLATE;
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  UserConfig parsed;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, parsed, why)) << why;

  const UserConfig builtin;  // 内置默认层，即"没有文件"时的生效配置
  EXPECT_EQ(parsed.enabled, builtin.enabled);
  EXPECT_EQ(parsed.bt_mode, builtin.bt_mode);
  EXPECT_EQ(parsed.bt_addr, builtin.bt_addr);
  EXPECT_EQ(parsed.log_level, builtin.log_level);
  EXPECT_TRUE(parsed.controllers.empty()) << "模板不该带 controllers 条目";
  EXPECT_EQ(parsed.defaults.bt_wait_s, builtin.defaults.bt_wait_s);
  EXPECT_EQ(parsed.defaults.audio, builtin.defaults.audio);
  EXPECT_FLOAT_EQ(parsed.defaults.haptics_gain,
                  builtin.defaults.haptics_gain);
  EXPECT_EQ(parsed.defaults.speaker_gain, builtin.defaults.speaker_gain);
  EXPECT_EQ(parsed.defaults.usb_report_interval_ms,
            builtin.defaults.usb_report_interval_ms);
  EXPECT_EQ(parsed.defaults.usb_model, builtin.defaults.usb_model);
  EXPECT_EQ(parsed.defaults.plugins, builtin.defaults.plugins);
}

TEST(UserConfigParse, DisabledWithoutAddressIsFine) {
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig("enabled: false\n", cfg, why))
      << why;
  EXPECT_FALSE(cfg.enabled);
  EXPECT_EQ(cfg.bt_mode, BtTargetMode::kAuto);
  EXPECT_TRUE(cfg.bt_addr.empty());
}

TEST(UserConfigParse, SingleMacIsCanonicalised) {
  // 地址大小写由配置层统一成大写：它是 controllers 的键与手柄身份的比对口径。
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(
      "bt_addr: \"aa:bb:cc:dd:ee:ff\"\n", cfg, why))
      << why;
  EXPECT_EQ(cfg.bt_mode, BtTargetMode::kSingle);
  EXPECT_EQ(cfg.bt_addr, "AA:BB:CC:DD:EE:FF");
}

TEST(UserConfigParse, ListModeRequiresControllers) {
  UserConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseUserConfig("bt_addr: list\n", cfg, why));
  EXPECT_NE(why.find("controllers"), std::string::npos) << why;
}

TEST(UserConfigParse, ListModeUsesControllerKeysAsAllowlist) {
  const std::string text =
      "bt_addr: list\n"
      "controllers:\n"
      "  \"aa:bb:cc:dd:ee:01\": {}\n"
      "  \"AA:BB:CC:DD:EE:02\":\n"
      "    audio: false\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  EXPECT_EQ(cfg.bt_mode, BtTargetMode::kList);
  ASSERT_EQ(cfg.controllers.size(), 2u);
  EXPECT_TRUE(cfg.controllers.count("AA:BB:CC:DD:EE:01") == 1);
  EXPECT_TRUE(cfg.controllers["AA:BB:CC:DD:EE:01"].audio);
  EXPECT_FALSE(cfg.controllers["AA:BB:CC:DD:EE:02"].audio);
}

TEST(UserConfigParse, SingleModeRejectsOtherControllerEntries) {
  // 只服务一个 MAC 时，别的条目永远不会生效；报错而不是静默忽略。
  UserConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseUserConfig(
      "bt_addr: \"AA:BB:CC:DD:EE:01\"\n"
      "controllers:\n"
      "  \"AA:BB:CC:DD:EE:02\": {}\n",
      cfg, why));
  EXPECT_NE(why.find("AA:BB:CC:DD:EE:02"), std::string::npos) << why;
}

TEST(UserConfigParse, SingleModeAcceptsOwnControllerEntry) {
  const std::string text =
      "bt_addr: \"aa:bb:cc:dd:ee:01\"\n"
      "controllers:\n"
      "  \"AA:BB:CC:DD:EE:01\":\n"
      "    haptics_gain: 0.5\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  ASSERT_EQ(cfg.controllers.size(), 1u);
  EXPECT_FLOAT_EQ(cfg.controllers["AA:BB:CC:DD:EE:01"].haptics_gain, 0.5f);
}

TEST(UserConfigParse, GlobalLayersUnderControllerEntries) {
  // 内置默认 ← global ← controllers[mac]：没写的键逐层继承，写了的覆盖。
  const std::string text =
      "bt_addr: auto\n"
      "global:\n"
      "  audio: false\n"
      "  haptics_gain: 1.5\n"
      "  usb_report_interval_ms: 2\n"
      "controllers:\n"
      "  \"AA:BB:CC:DD:EE:01\":\n"
      "    haptics_gain: 0.5\n"
      "    speaker_gain: 5\n"
      "  \"AA:BB:CC:DD:EE:02\": {}\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;

  // global 层本身是"内置默认 + global 覆盖"。
  EXPECT_FALSE(cfg.defaults.audio);
  EXPECT_FLOAT_EQ(cfg.defaults.haptics_gain, 1.5f);
  EXPECT_EQ(cfg.defaults.usb_report_interval_ms, 2);
  EXPECT_EQ(cfg.defaults.speaker_gain, 2);  // 没写，保持内置默认

  // 条目 1 只覆盖两个键，其余继承 global。
  const ds5::daemon::ControllerConfig& first =
      cfg.controllers["AA:BB:CC:DD:EE:01"];
  EXPECT_FALSE(first.audio);
  EXPECT_FLOAT_EQ(first.haptics_gain, 0.5f);
  EXPECT_EQ(first.speaker_gain, 5);
  EXPECT_EQ(first.usb_report_interval_ms, 2);

  // 空条目（或只写键不写值）等于完全继承 global。
  const ds5::daemon::ControllerConfig& second =
      cfg.controllers["AA:BB:CC:DD:EE:02"];
  EXPECT_FALSE(second.audio);
  EXPECT_FLOAT_EQ(second.haptics_gain, 1.5f);
  EXPECT_EQ(second.speaker_gain, 2);
  EXPECT_EQ(second.usb_report_interval_ms, 2);
}

TEST(UserConfigParse, NullGlobalAndNullControllerEntryMeanNoOverrides) {
  const std::string text =
      "bt_addr: auto\n"
      "global:\n"
      "controllers:\n"
      "  \"AA:BB:CC:DD:EE:01\":\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  EXPECT_FLOAT_EQ(cfg.controllers["AA:BB:CC:DD:EE:01"].haptics_gain, 1.0f);
  EXPECT_EQ(cfg.controllers["AA:BB:CC:DD:EE:01"].bt_wait_s, 60);
}

TEST(UserConfigParse, RejectsDuplicateControllersAfterCanonicalisation) {
  // YAML 允许大小写不同的两个键；归一后它们是同一只手柄，后者会静默覆盖前者。
  UserConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseUserConfig(
      "bt_addr: auto\n"
      "controllers:\n"
      "  \"AA:BB:CC:DD:EE:01\": {}\n"
      "  \"aa:bb:cc:dd:ee:01\": {}\n",
      cfg, why));
  EXPECT_NE(why.find("duplicate"), std::string::npos) << why;
}

TEST(UserConfigParse, RejectsInvalidControllerKey) {
  UserConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseUserConfig(
      "bt_addr: auto\ncontrollers:\n  \"AA:BB:CC:DD:EE\": {}\n", cfg, why));
  EXPECT_NE(why.find("not a MAC"), std::string::npos) << why;
}

TEST(UserConfigParse, FullConfigRoundTrips) {
  const std::string text =
      "enabled: true\n"
      "bt_addr: \"AA:BB:CC:DD:EE:FF\"\n"
      "log_level: DEBUG\n"
      "global:\n"
      "  bt_wait_s: 0\n"
      "  audio: false\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  EXPECT_TRUE(cfg.enabled);
  EXPECT_EQ(cfg.bt_mode, BtTargetMode::kSingle);
  EXPECT_EQ(cfg.bt_addr, "AA:BB:CC:DD:EE:FF");
  EXPECT_EQ(cfg.defaults.bt_wait_s, 0);
  EXPECT_FALSE(cfg.defaults.audio);
  ASSERT_TRUE(cfg.log_level.has_value());
  EXPECT_EQ(*cfg.log_level, 1);  // DEBUG
}

TEST(UserConfigParse, ExplicitWarningOverridesSystemDefault) {
  // WARNING 的数值就是 -1：用户必须能显式选它，而不是被当成"未配置"。
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig("log_level: WARNING\n", cfg, why))
      << why;
  ASSERT_TRUE(cfg.log_level.has_value());
  EXPECT_EQ(*cfg.log_level, -1);
}

TEST(UserConfigParse, AdvancedSettingsRoundTrip) {
  const std::string text =
      "bt_addr: \"AA:BB:CC:DD:EE:FF\"\n"
      "global:\n"
      "  haptics_gain: 1.5\n"
      "  speaker_gain: 5\n"
      "  usb_report_interval_ms: 2\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  EXPECT_FLOAT_EQ(cfg.defaults.haptics_gain, 1.5f);
  EXPECT_EQ(cfg.defaults.speaker_gain, 5);
  EXPECT_EQ(cfg.defaults.usb_report_interval_ms, 2);
}

// 配置层对 plugins: 只做形状检查，把取值当原样文本交出去：键的含义住在插件
// 自己那里（见 idle 插件），所以这里连"30 该不该是数字"都不判。
TEST(UserConfigParse, PluginsSectionIsHandedOverVerbatim) {
  const std::string text =
      "plugins:\n"
      "  idle:\n"
      "    off_after_minutes: 45\n"
      "    mode: keep-alive\n"
      "  unmapped:\n"
      "    cross: circle\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  ASSERT_EQ(cfg.defaults.plugins.size(), 3u);
  EXPECT_EQ(cfg.defaults.plugins.at("idle.off_after_minutes"), "45");
  EXPECT_EQ(cfg.defaults.plugins.at("idle.mode"), "keep-alive");
  // 不认识的插件与键照样收：这一层不认识插件名单。
  EXPECT_EQ(cfg.defaults.plugins.at("unmapped.cross"), "circle");
  EXPECT_TRUE(cfg.controllers.empty());
}

// 三个深度逐键合并：根级是插件默认层，global 覆盖写了的键，手柄条目再在其上
// 覆盖，每层没写的键与没写的插件都原样留下。
TEST(UserConfigParse, PluginsMergeLeafWiseAcrossTheThreeLayers) {
  const std::string text =
      "plugins:\n"
      "  idle:\n"
      "    off_after_minutes: 30\n"
      "  other:\n"
      "    key: root\n"
      "global:\n"
      "  plugins:\n"
      "    idle:\n"
      "      off_after_minutes: 15\n"
      "controllers:\n"
      "  \"AA:BB:CC:DD:EE:01\":\n"
      "    plugins:\n"
      "      idle:\n"
      "        off_after_minutes: 0\n"
      "  \"AA:BB:CC:DD:EE:02\": {}\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;

  EXPECT_EQ(cfg.defaults.plugins.at("idle.off_after_minutes"), "15");
  // 根级的另一个插件没被 global 写过，整个键保留。
  EXPECT_EQ(cfg.defaults.plugins.at("other.key"), "root");

  const ControllerConfig& pad1 = cfg.controllers.at("AA:BB:CC:DD:EE:01");
  EXPECT_EQ(pad1.plugins.at("idle.off_after_minutes"), "0");
  EXPECT_EQ(pad1.plugins.at("other.key"), "root");

  // 只写 MAC 不写键的条目继承 global 层的全部插件配置。
  const ControllerConfig& pad2 = cfg.controllers.at("AA:BB:CC:DD:EE:02");
  EXPECT_EQ(pad2.plugins, cfg.defaults.plugins);
}

// 插件名以下不是一层固定形状：往下想嵌几层就嵌几层，段数与段名一样都交给插件
// 解释，配置层只把它们拼成点分路径。
TEST(UserConfigParse, PluginSettingsNestToArbitraryDepth) {
  const std::string text =
      "plugins:\n"
      "  buttons:\n"
      "    remap:\n"
      "      circle: cross\n"
      "      dpad:\n"
      "        up: dpad_up\n"
      "    shortcut:\n"
      "      1: create+options\n"
      "    axis:\n"
      "      group: {}\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  ASSERT_EQ(cfg.defaults.plugins.size(), 3u);
  EXPECT_EQ(cfg.defaults.plugins.at("buttons.remap.circle"), "cross");
  EXPECT_EQ(cfg.defaults.plugins.at("buttons.remap.dpad.up"), "dpad_up");
  // 段名与 YAML 的键同形：数字开头的键就是它自己，不做类型转换。
  EXPECT_EQ(cfg.defaults.plugins.at("buttons.shortcut.1"), "create+options");
  // 写了组名却留空 = 这一组没写：零条键，也不算错误。
  EXPECT_EQ(cfg.defaults.plugins.count("buttons.axis.group"), 0u);
}

// 逐层合并摊平之后进行，所以覆盖的单位是整条点分路径，不是某一层。
TEST(UserConfigParse, NestedPluginsMergePerFlattenedKey) {
  const std::string text =
      "plugins:\n"
      "  buttons:\n"
      "    remap:\n"
      "      circle: cross\n"
      "      dpad:\n"
      "        up: dpad_up\n"
      "global:\n"
      "  plugins:\n"
      "    buttons:\n"
      "      remap:\n"
      "        circle: square\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  EXPECT_EQ(cfg.defaults.plugins.at("buttons.remap.circle"), "square");
  // 覆盖 circle 不会带走整个 remap 组：同组没被上层写过的键留着。
  EXPECT_EQ(cfg.defaults.plugins.at("buttons.remap.dpad.up"), "dpad_up");
}

// 深度上限是一条真实约束，不是形状：叶子的键路径最多 kMaxPluginDepth 段，
// 再套一层组就要拒，并且报出越界那一组自己的路径。
TEST(UserConfigParse, PluginNestingDepthIsBounded) {
  // groups 层组，叶子在最下面：叶子的键路径因此有 groups+1 段。缩进每层多两格。
  const auto yaml_with_groups = [](int groups) {
    std::string text = "plugins:\n  buttons:\n";
    for (int i = 0; i < groups; ++i) {
      text += std::string(static_cast<size_t>(4 + 2 * i), ' ') + "l" +
              std::to_string(i) + ":\n";
    }
    text += std::string(static_cast<size_t>(4 + 2 * groups), ' ') + "leaf: v\n";
    return text;
  };
  const auto path_with_groups = [](int groups) {
    std::string path = "buttons";
    for (int i = 0; i < groups; ++i) path += ".l" + std::to_string(i);
    return path;
  };

  const int max_path = ds5::daemon::kMaxPluginDepth;
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(yaml_with_groups(max_path - 1),
                                           cfg, why))
      << why;
  EXPECT_EQ(cfg.defaults.plugins.at(path_with_groups(max_path - 1) + ".leaf"),
            "v");

  cfg = UserConfig{};
  EXPECT_FALSE(
      ds5::daemon::ParseUserConfig(yaml_with_groups(max_path), cfg, why));
  EXPECT_NE(why.find("nests deeper"), std::string::npos) << why;
  // 报的是越界那一组（带 YAML 里的 `plugins.` 前缀），不是它的父级。
  EXPECT_NE(why.find(std::string("plugins.") + path_with_groups(max_path)),
            std::string::npos)
      << why;
}

TEST(UserConfigParse, EmptyPluginsSectionIsNoOverride) {
  UserConfig cfg;
  std::string why;
  // `plugins:` 只写键不写值等于本层不覆盖，与 identity:/global: 同一种宽容。
  ASSERT_TRUE(ds5::daemon::ParseUserConfig("plugins:\nglobal:\n  plugins:\n",
                                           cfg, why))
      << why;
  EXPECT_TRUE(cfg.defaults.plugins.empty());
}

// 名字必须是 plugin::IsToken 认的 token：argv 用 '.' 分隔插件名与键、用 '='
// 分隔键与值，名字里出现这两个字符就没法无损传下去。形状错误在这里是启动前的
// 配置错误，而取值合不合插件的心意不在这里判。
TEST(UserConfigParse, RejectsMalformedPluginsSections) {
  for (const char* text :
       {"plugins: 30\n",
        // 插件名与键都要过 token：非法字符、空名、带分隔符的名字。
        "plugins:\n  two words:\n    key: v\n",
        "plugins:\n  \"\":\n    key: v\n",
        "plugins:\n  idle:\n    bad.key: v\n",
        "plugins:\n  idle:\n    \"\": v\n",
        // 嵌套不改变规则：更深一层的段名同样不能带分隔符，也不能只写键名。
        "plugins:\n  buttons:\n    remap:\n      bad.key: v\n",
        "plugins:\n  buttons:\n    remap:\n",
        // 设置段必须是映射，取值必须是非空标量。
        "plugins:\n  idle: 30\n",
        "plugins:\n  idle:\n    off_after_minutes: [30]\n",
        "plugins:\n  idle:\n    off_after_minutes:\n",
        // 三个深度走同一套检查：手柄级条目里的错误要报出是哪个 MAC。
        "bt_addr: list\ncontrollers:\n  \"AA:BB:CC:DD:EE:FF\":\n"
        "  plugins:\n    idle:\n      off after minutes: 90\n"}) {
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << text;
    // 每条错误文本都带 "plugin"：要么是 'plugins' 段的形状，要么是插件名/键名。
    EXPECT_NE(why.find("plugin"), std::string::npos) << why;
  }
}

// 边界的两种写法都必须收：整数 2 与 0 是用户最可能手写的形态，不能因为
// YAML 标量不是 "2.0" 就被判成非 number。
TEST(UserConfigParse, GainBoundsAreInclusive) {
  const std::string text =
      "global:\n"
      "  haptics_gain: 0\n"
      "  speaker_gain: 7\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  EXPECT_FLOAT_EQ(cfg.defaults.haptics_gain, 0.0f);
  EXPECT_EQ(cfg.defaults.speaker_gain, 7);
}

TEST(UserConfigParse, RejectsGainsOutsideTheirRange) {
  for (const char* text :
       {"global:\n  haptics_gain: -0.1\n",
        "global:\n  haptics_gain: 2.1\n",
        "global:\n  haptics_gain: .nan\n",
        "global:\n  haptics_gain: .inf\n",
        "global:\n  haptics_gain: loud\n",
        "global:\n  speaker_gain: -1\n",
        "global:\n  speaker_gain: 8\n",
        "global:\n  speaker_gain: 2.5\n",
        // 手柄级条目与 global 走同一套键校验。
        "controllers:\n  \"AA:BB:CC:DD:EE:FF\":\n    haptics_gain: 3\n"}) {
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << text;
    EXPECT_FALSE(why.empty()) << text;
  }
}

// 间隔不是连续量：1/2/4 ms 是能原样落进 bInterval 的三档，0 是独立的
// real-time 档，表外的值（3 ms、8 ms）既落不到档位也没有定义，一律报错。
TEST(UserConfigParse, RejectsUnsupportedReportInterval) {
  for (const char* text :
       {"global:\n  usb_report_interval_ms: 3\n",
        "global:\n  usb_report_interval_ms: 8\n",
        "global:\n  usb_report_interval_ms: 2.5\n",
        "controllers:\n  \"AA:BB:CC:DD:EE:FF\":\n    usb_report_interval_ms: 5\n"}) {
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << text;
    EXPECT_NE(why.find("usb_report_interval_ms"), std::string::npos) << why;
  }
}

// 0 是 real-time 档，不是"零毫秒"：IN 线程只在收到新蓝牙样本时发一份。
TEST(UserConfigParse, ZeroReportIntervalSelectsRealTime) {
  const std::string text = "global:\n  usb_report_interval_ms: 0\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  EXPECT_EQ(cfg.defaults.usb_report_interval_ms, 0);
}

// usb_model：auto 是不覆盖（std::nullopt），另两档钉死一个型号；横幅回显
// UsbModelName 必须与这三种写法一致。
TEST(UserConfigParse, UsbModelAcceptsAutoDualSenseAndEdge) {
  using core::device::ControllerModel;
  struct Case {
    const char* text;
    std::optional<ControllerModel> want;
  };
  const Case cases[] = {
      {"auto", std::nullopt},
      {"dualsense", ControllerModel::kDualSense},
      {"edge", ControllerModel::kEdge},
  };
  for (const Case& c : cases) {
    const std::string text = std::string("global:\n  usb_model: ") + c.text;
    UserConfig cfg;
    std::string why;
    ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why))
        << text << ": " << why;
    EXPECT_EQ(cfg.defaults.usb_model, c.want) << c.text;
    EXPECT_STREQ(ds5::daemon::UsbModelName(cfg.defaults.usb_model), c.text);
  }
}

TEST(UserConfigParse, UsbModelLayersPerController) {
  using core::device::ControllerModel;
  const std::string text =
      "global:\n"
      "  usb_model: edge\n"
      "controllers:\n"
      "  \"AA:BB:CC:DD:EE:01\":\n"
      "    usb_model: dualsense\n"
      "  \"AA:BB:CC:DD:EE:02\": {}\n";
  UserConfig cfg;
  std::string why;
  ASSERT_TRUE(ds5::daemon::ParseUserConfig(text, cfg, why)) << why;
  ASSERT_TRUE(cfg.defaults.usb_model.has_value());
  EXPECT_EQ(*cfg.defaults.usb_model, ControllerModel::kEdge);
  ASSERT_TRUE(cfg.controllers["AA:BB:CC:DD:EE:01"].usb_model.has_value());
  EXPECT_EQ(*cfg.controllers["AA:BB:CC:DD:EE:01"].usb_model,
            ControllerModel::kDualSense);
  // 空条目完全继承 global。
  ASSERT_TRUE(cfg.controllers["AA:BB:CC:DD:EE:02"].usb_model.has_value());
  EXPECT_EQ(*cfg.controllers["AA:BB:CC:DD:EE:02"].usb_model,
            ControllerModel::kEdge);
}

TEST(UserConfigParse, RejectsUnknownUsbModel) {
  for (const char* text :
       {"global:\n  usb_model: duel\n",
        "global:\n  usb_model: Edge\n",   // 取值区分大小写，与 auto/list 一致
        "global:\n  usb_model: 1\n",
        "global:\n  usb_model: [edge]\n",
        "controllers:\n  \"AA:BB:CC:DD:EE:FF\":\n    usb_model: ps5\n"}) {
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << text;
    EXPECT_NE(why.find("usb_model"), std::string::npos) << why;
  }
}

TEST(UserConfigParse, RejectsInvalidMac) {
  for (const char* addr :
       {"AA:BB:CC:DD:EE", "GG:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FF:00",
        "AA-BB-CC-DD-EE-FF", "AA:BB:CC:DD:EE:F", "AA:BB:CC:DD:EE:FF ",
        "AA:BB:CC:DD:EE:FG"}) {
    const std::string text = std::string("bt_addr: \"") + addr + "\"\n";
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << addr;
    EXPECT_FALSE(why.empty()) << addr;
  }
}

TEST(UserConfigParse, RejectsRootOnlyKeys) {
  for (const char* text :
       {"ffs_path: /dev/ffs-ds5\n", "uid: 1000\n", "gid: 998\n",
        // 手柄级键不再是根键：写错层级会被明确拒绝。
        "bt_wait_s: 10\n", "audio: false\n"}) {
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << text;
    EXPECT_NE(why.find("unknown key"), std::string::npos) << why;
  }
}

TEST(UserConfigParse, RejectsUnknownKeysInNestedBlocks) {
  for (const char* text :
       {"global:\n  jack: 3.5\n",
        "controllers:\n  \"AA:BB:CC:DD:EE:FF\":\n    jack: 3.5\n"}) {
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << text;
    EXPECT_NE(why.find("unknown key"), std::string::npos) << why;
  }
}

TEST(UserConfigParse, RejectsWrongTypes) {
  for (const char* text :
       {"enabled: banana\n", "bt_addr: [AA]\n", "bt_addr: 42\n",
        "global: [audio]\n",
        "global:\n  bt_wait_s: 12.5\n",
        "global:\n  bt_wait_s: 12abc\n",
        "global:\n  audio: [true]\n",
        "log_level: [DEBUG]\n",
        "controllers: [\"AA:BB:CC:DD:EE:FF\"]\n"}) {
    UserConfig cfg;
    std::string why;
    EXPECT_FALSE(ds5::daemon::ParseUserConfig(text, cfg, why)) << text;
    EXPECT_FALSE(why.empty()) << text;
  }
}

TEST(UserConfigParse, RejectsNegativeWait) {
  UserConfig cfg;
  std::string why;
  EXPECT_FALSE(ds5::daemon::ParseUserConfig(
      "global:\n  bt_wait_s: -1\n", cfg, why));
  EXPECT_NE(why.find("bt_wait_s"), std::string::npos) << why;
}

// ---------------------------------------------------------------------------
// LoadSystemConfig / LoadUserConfig —— 文件系统加固（临时目录，无特权）
// ---------------------------------------------------------------------------

/**
 * 每个用例一个 mkdtemp 临时根（0700），TearDown 整树删除。
 * WriteFile 自动建父目录，便于拼出 <root>/.config/ds5dummy/config.yaml。
 */
class TempConfigTree : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/ds5dummy-config-test-XXXXXX";
    const char* made = ::mkdtemp(tmpl);
    ASSERT_NE(made, nullptr);
    root_ = made;
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  /// 临时根下的绝对路径；只拼路径，不创建任何东西。
  std::string Path(const std::string& rel) const { return root_ + "/" + rel; }

  /// 写文件（自动建父目录）并设置精确权限；失败返回空串。
  std::string WriteFile(const std::string& rel, const std::string& text,
                        mode_t mode = 0600) {
    const std::string path = Path(rel);
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    if (ec) return {};
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) return {};
    if (::chmod(path.c_str(), mode) != 0) return {};
    return path;
  }

  std::string root_;
};

/// 生成恰好 total 字节的合法用户配置：一行正文，再用一行注释把长度补足。
/// total 小于正文 + 2 时只返回正文（边界用例传的都是一 KiB 级的值）。
std::string PaddedUserConfig(std::size_t total) {
  const std::string body = "enabled: false\n";
  if (total < body.size() + 2) return body;
  std::string text = body;
  text += '#';
  text.append(total - body.size() - 2, 'x');
  text += '\n';
  return text;
}

TEST_F(TempConfigTree, UserConfigMissingFileIsAbsent) {
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kAbsent);
  EXPECT_TRUE(why.empty());
}

TEST_F(TempConfigTree, UserConfigMissingAppDirIsAbsent) {
  std::error_code ec;
  std::filesystem::create_directories(Path(".config"), ec);
  ASSERT_FALSE(ec);
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kAbsent);
}

TEST_F(TempConfigTree, UserConfigValidFileLoads) {
  ASSERT_FALSE(WriteFile(".config/ds5dummy/config.yaml",
                         "enabled: true\nbt_addr: \"AA:BB:CC:DD:EE:FF\"\n")
                   .empty());
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kOk)
      << why;
  EXPECT_EQ(cfg.bt_addr, "AA:BB:CC:DD:EE:FF");
}

TEST_F(TempConfigTree, UserConfigGroupWritableIsRejected) {
  ASSERT_FALSE(WriteFile(".config/ds5dummy/config.yaml",
                         "enabled: false\n", 0666)
                   .empty());
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kError);
  EXPECT_NE(why.find("writable"), std::string::npos) << why;
}

TEST_F(TempConfigTree, UserConfigSymlinkIsRejected) {
  ASSERT_FALSE(WriteFile(".config/ds5dummy/real.yaml", "enabled: false\n")
                   .empty());
  ASSERT_EQ(::symlink("real.yaml", Path(".config/ds5dummy/config.yaml").c_str()),
            0);
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kError);
  EXPECT_FALSE(why.empty());
}

TEST_F(TempConfigTree, UserConfigSymlinkedIntermediateComponentIsRejected) {
  // .config 指向另一个目录，里面的配置本身完全合法：逐级 O_NOFOLLOW 必须
  // 在中间这一级就失败，而不是跟过去把别处的文件当作用户的配置。
  ASSERT_FALSE(WriteFile("real-config/ds5dummy/config.yaml",
                         "enabled: false\n")
                   .empty());
  ASSERT_EQ(::symlink("real-config", Path(".config").c_str()), 0);
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kError);
  EXPECT_FALSE(why.empty());
}

TEST_F(TempConfigTree, UserConfigOwnerMismatchIsRejected) {
  // 文件属于测试进程自己；把期望属主传成另一个 uid，等价于"用户配置被别人的
  // 文件顶替"。以 root 跑测试时属主检查会把 root 当合法属主，这个用例没有意义。
  if (::getuid() == 0) GTEST_SKIP() << "requires an unprivileged test user";
  ASSERT_FALSE(WriteFile(".config/ds5dummy/config.yaml", "enabled: false\n")
                   .empty());
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid() + 1, root_, cfg, why),
            LoadResult::kError);
  EXPECT_NE(why.find("owned"), std::string::npos) << why;
}

TEST_F(TempConfigTree, UserConfigSizeBoundary) {
  ASSERT_EQ(PaddedUserConfig(65536).size(), 65536u);
  ASSERT_FALSE(
      WriteFile(".config/ds5dummy/config.yaml", PaddedUserConfig(65536))
          .empty());
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kOk)
      << why;
  EXPECT_FALSE(cfg.enabled);

  ASSERT_FALSE(
      WriteFile(".config/ds5dummy/config.yaml", PaddedUserConfig(65537))
          .empty());
  UserConfig too_big;
  std::string why_big;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, too_big, why_big),
            LoadResult::kError);
  EXPECT_NE(why_big.find("64 KiB"), std::string::npos) << why_big;
}

TEST_F(TempConfigTree, UserConfigOversizedIsRejected) {
  ASSERT_FALSE(WriteFile(".config/ds5dummy/config.yaml", std::string(65537, '#'))
                   .empty());
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kError);
  EXPECT_NE(why.find("64 KiB"), std::string::npos) << why;
}

TEST_F(TempConfigTree, UserConfigNonRegularFileIsRejected) {
  std::error_code ec;
  std::filesystem::create_directories(Path(".config/ds5dummy/config.yaml"),
                                      ec);
  ASSERT_FALSE(ec);
  UserConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadUserConfig(::getuid(), root_, cfg, why),
            LoadResult::kError);
  EXPECT_FALSE(why.empty());
}

TEST_F(TempConfigTree, SystemConfigMissingFileIsAbsent) {
  SystemConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadSystemConfig(Path("no-such.yaml"), cfg, why),
            LoadResult::kAbsent);
  EXPECT_TRUE(why.empty());
}

TEST_F(TempConfigTree, SystemConfigDirectoryIsRejected) {
  std::error_code ec;
  std::filesystem::create_directories(Path("sysdir"), ec);
  ASSERT_FALSE(ec);
  SystemConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadSystemConfig(Path("sysdir"), cfg, why),
            LoadResult::kError);
  EXPECT_FALSE(why.empty());
}

TEST_F(TempConfigTree, SystemConfigValidFileLoads) {
  ASSERT_FALSE(
      WriteFile("system.yaml", "ffs_path: /dev/ffs-test\n", 0644).empty());
  SystemConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadSystemConfig(Path("system.yaml"), cfg, why),
            LoadResult::kOk)
      << why;
  EXPECT_EQ(cfg.ffs_path, "/dev/ffs-test");
}

TEST_F(TempConfigTree, SystemConfigOversizedIsRejected) {
  ASSERT_FALSE(WriteFile("system.yaml", std::string(65537, '#'), 0644).empty());
  SystemConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadSystemConfig(Path("system.yaml"), cfg, why),
            LoadResult::kError);
}

TEST_F(TempConfigTree, SystemConfigExistingGidIsAccepted) {
  // 测试进程的主组一定在组库里；uid 只要求 > 0，不查 passwd。
  const std::string text =
      "identity:\n  mode: list\n  users:\n    - uid: 1000\n      gid: " +
      std::to_string(::getgid()) + "\n";
  ASSERT_FALSE(WriteFile("system.yaml", text, 0644).empty());
  SystemConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadSystemConfig(Path("system.yaml"), cfg, why),
            LoadResult::kOk)
      << why;
  ASSERT_EQ(cfg.allowed_users.size(), 1u);
  EXPECT_EQ(cfg.allowed_users[0].gid, ::getgid());
}

TEST_F(TempConfigTree, SystemConfigMissingGidIsRejected) {
  // gid_t 的最大值（4294967295）不会出现在组库里：/etc/group 的组号由发行版
  // 分配，远小于这个值；这里用它代表"组库里查不到的 gid"。
  const gid_t missing = std::numeric_limits<gid_t>::max();
  const std::string text =
      "identity:\n  mode: list\n  users:\n    - uid: 1000\n      gid: " +
      std::to_string(missing) + "\n";
  ASSERT_FALSE(WriteFile("system.yaml", text, 0644).empty());
  SystemConfig cfg;
  std::string why;
  EXPECT_EQ(ds5::daemon::LoadSystemConfig(Path("system.yaml"), cfg, why),
            LoadResult::kError);
  // 报错必须能定位到具体条目与 gid，而不是笼统的"配置错误"。
  EXPECT_NE(why.find("users[0]"), std::string::npos) << why;
  EXPECT_NE(why.find(std::to_string(missing)), std::string::npos) << why;
}

}  // namespace
