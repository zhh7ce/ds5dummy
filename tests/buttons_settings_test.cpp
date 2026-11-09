/**
 * @file
 * @brief buttons 插件配置语法的单测：五个功能组的分派（C-01..C-05）与 G-06 的单条回落
 *
 * 测的是"一份配置文本变成几条规则、哪一行留下了日志"，不涉及 63 字节。规则怎么
 * 作用到帧上是 buttons_pipeline_test.cpp 与 buttons_sense_test.cpp 的事。
 *
 * 无 root、无手柄。
 */

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "plugin/buttons/ids.h"
#include "plugin/buttons/rules.h"
#include "plugin/buttons/settings.h"
#include "plugin/plugin_api.h"

namespace {

using buttons::ids::Axis;
using buttons::ids::BitOf;
using buttons::ids::Source;
using buttons::rules::Set;
using buttons::rules::Timing;

/// 每个等级收到的日志行，下标是 ds5LogLevel。
std::vector<std::string> g_lines[4];

/// 记录一行：等级 + 原文，用例按等级数条数、按内容查回显。
void Log(int level, const char* message) {
  if (level >= 0 && level <= DS5_LOG_DEBUG) g_lines[level].emplace_back(message);
}

/// 本切片没有动作，关机通道只用来把宿主表填满：被调用就是缺陷。
int PowerOff() {
  ADD_FAILURE() << "settings must never touch the link";
  return 0;
}

/**
 * @brief 一份只用来收日志的宿主表。
 * @return 版本号与函数指针都填好的 ds5Host。
 */
ds5Host MakeHost() {
  ds5Host host{};
  host.abi_version = kPluginAbiVersion;
  host.log = &Log;
  host.power_off = &PowerOff;
  return host;
}

/**
 * @brief 一份 init 会收到的配置数组：键与值的存储和指向它们的数组同生命周期。
 * @details 生产里这些字符串由宿主的快照持有、只在 init 期间有效，这里用同一个
 *          对象把两件事一起管住，避免指向已经析构的临时字符串。
 */
class FakeSettings {
 public:
  FakeSettings(std::initializer_list<std::pair<std::string, std::string>> items) {
    for (const auto& item : items) {
      keys_.push_back(item.first);
      values_.push_back(item.second);
    }
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      entries_.push_back(ds5Setting{keys_[i].c_str(), values_[i].c_str()});
    }
    entries_.push_back(ds5Setting{nullptr, nullptr});
  }

  /// 交给 Load 的那个数组首地址。
  const ds5Setting* data() const { return entries_.data(); }

 private:
  std::vector<std::string> keys_;
  std::vector<std::string> values_;
  std::vector<ds5Setting> entries_;
};

/**
 * @brief 在规则表里按源找一条映射。
 * @param set 解析出来的规则表。
 * @param src 要找的源。
 * @return 命中的条目；没有时 null。
 */
const buttons::rules::Remap* Find(const Set& set, Source src) {
  for (const buttons::rules::Remap& rule : set.remaps) {
    if (rule.src == src) return &rule;
  }
  return nullptr;
}

class ButtonsSettings : public ::testing::Test {
 protected:
  void SetUp() override {
    for (std::vector<std::string>& lines : g_lines) lines.clear();
  }

  /**
   * @brief 跑一遍 Load。
   * @param settings 配置数组。
   * @param out 解析结果。
   * @return Load 的返回值（是否至少有一条规则）。
   */
  bool Parse(const FakeSettings& settings, Set& out) {
    const ds5Host host = MakeHost();
    return buttons::settings::Load(settings.data(), out, host);
  }

  /// 某个等级收到的行数。
  size_t Lines(ds5LogLevel level) const { return g_lines[level].size(); }
};

// ---------------------------------------------------------------------------
// 正例：一行配置 = 一条规则
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, RemapGroupBecomesRules) {
  const FakeSettings settings{
      {"remap.circle", "cross"},   {"remap.square", "triangle"},
      {"remap.l2", "r2"},          {"remap.mute", "disable"},
      {"remap.cross", "l3+keep"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.remaps.size(), 5u);
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);

  EXPECT_EQ(Find(set, Source::kCircle)->targets, BitOf(Source::kCross));
  EXPECT_EQ(Find(set, Source::kCircle)->keep_source, false);
  EXPECT_EQ(Find(set, Source::kSquare)->targets, BitOf(Source::kTriangle));
  // M-04 的"抹掉"在结构里就是目标为空：源照样被 gate 掉，没有位补上来。
  EXPECT_EQ(Find(set, Source::kMute)->targets, 0u);
  EXPECT_EQ(Find(set, Source::kMute)->keep_source, false);
  // M-03 的"保留"是源自己进不了 gate，与目标无关。
  EXPECT_EQ(Find(set, Source::kCross)->targets, BitOf(Source::kL3));
  EXPECT_EQ(Find(set, Source::kCross)->keep_source, true);
  EXPECT_EQ(Find(set, Source::kL2)->targets, BitOf(Source::kR2));
}

TEST_F(ButtonsSettings, SourcesAndTargetsShareTheOneTokenTable) {
  // 同一个 token 在键的位置和值的位置都认：表只有一张（C-03）。
  const FakeSettings settings{{"remap.dpad_up", "dpad_left"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.remaps.size(), 1u);
  EXPECT_EQ(set.remaps[0].src, Source::kDpadUp);
  EXPECT_EQ(set.remaps[0].targets, BitOf(Source::kDpadLeft));
}

// ---------------------------------------------------------------------------
// 忽略与回落：C-05 的沉默、G-06 的一条一报
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, UnknownGroupsAreIgnoredWithoutComment) {
  // 还没实现的功能组、别家的插件的键、以及少了那一层的裸组名：读不通就一个字
  // 都不说（C-05）。
  const FakeSettings settings{
      {"gyro.gesture.1", "circle"},
      {"stick.deadzone", "10"},
      {"idle.off_after_minutes", "30"},
      {"remap", "cross"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_TRUE(set.Empty());
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
  EXPECT_EQ(Lines(DS5_LOG_DEBUG), 0u);
}

TEST_F(ButtonsSettings, UnknownSourceKeyIsIgnored) {
  const FakeSettings settings{{"remap.cricle", "cross"}, {"remap.x", "cross"}};
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  // 键名不在 token 表里属于"不认识的键"：DEBUG 一行，不占用 WARNING 那档。
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
  EXPECT_EQ(Lines(DS5_LOG_DEBUG), 2u);
}

TEST_F(ButtonsSettings, KeyWithoutASourceIsReported) {
  // 组名后面那一段是空的：这不是"不认识的键"，是写坏了的一行，要说出来。
  const FakeSettings settings{{"remap.", "cross"}};
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 1u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("no key name after 'remap.'"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
}

TEST_F(ButtonsSettings, OnlyTheOffendingRuleIsDropped) {
  // G-06：三行读不通，各自留一行 WARNING 并且各归各自的原因；第四行照常成规则。
  const FakeSettings settings{
      {"remap.circle", "cricle"},      {"remap.square", "cross+l3"},
      {"remap.mute", "disable+keep"},  {"remap.l2", "r2"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.remaps.size(), 1u);
  EXPECT_EQ(set.remaps[0].src, Source::kL2);
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
  for (const std::string& line : g_lines[DS5_LOG_WARNING]) {
    EXPECT_NE(line.find("plugins: buttons: remap."), std::string::npos) << line;
    EXPECT_NE(line.find("this rule does nothing"), std::string::npos) << line;
  }
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("remap.circle"), std::string::npos);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][1].find("remap.square"), std::string::npos);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][2].find("remap.mute"), std::string::npos);
}

TEST_F(ButtonsSettings, ValueEchoesWhatTheUserWrote) {
  const FakeSettings settings{{"remap.circle", "✕"}};
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 1u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("value '✕'"), std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
}

TEST_F(ButtonsSettings, EmptySectionAndNullArrayBothMeanNotArmed) {
  // C-04：`buttons` 整节没写 = 整个插件不生效；空数组与 null 同途。
  const FakeSettings none{};
  Set set;
  EXPECT_FALSE(Parse(none, set));
  EXPECT_TRUE(set.Empty());

  const ds5Host host = MakeHost();
  Set other;
  EXPECT_FALSE(buttons::settings::Load(nullptr, other, host));
  EXPECT_TRUE(other.Empty());
}

TEST_F(ButtonsSettings, ParsingOverwritesWhateverWasInTheTable) {
  // init 可以被重复调用（Stop 之后重新 Start），上一轮的规则不能留下尾巴。
  const FakeSettings first{{"remap.circle", "cross"}};
  const FakeSettings second{{"remap.square", "triangle"}};
  Set set;
  ASSERT_TRUE(Parse(first, set));
  ASSERT_TRUE(Parse(second, set));
  ASSERT_EQ(set.remaps.size(), 1u);
  EXPECT_EQ(set.remaps[0].src, Source::kSquare);
}

// ---------------------------------------------------------------------------
// 映射的源与目标不对称：方向能当源，不能当目标
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, RemapSourceCanUseAStickDirection) {
  // I-01：摇杆方向是判出来的源，放在键的位置上是配置文档里的例子。
  const FakeSettings settings{{"remap.lstick_up", "dpad_up"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.remaps.size(), 1u);
  EXPECT_EQ(set.remaps[0].src, Source::kLstickUp);
  EXPECT_EQ(set.remaps[0].targets, BitOf(Source::kDpadUp));
}

TEST_F(ButtonsSettings, RemapTargetMustExistOnTheWire) {
  // G-03：八个摇杆方向没有对应的位，映射到它们等于什么都能不输出。
  const FakeSettings settings{
      {"remap.circle", "lstick_up"},
      {"remap.square", "rstick_left"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_TRUE(set.remaps.empty());
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 2u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("no bit on the wire to press"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
}

TEST_F(ButtonsSettings, RemapTargetRejectsEveryTouchpadForm) {
  // 同一个理由覆盖触摸板那一族：它们和摇杆方向一样是判出来的，线上没有那一位
  // 可以去置。
  const FakeSettings settings{
      {"remap.circle", "touch_left"},
      {"remap.square", "pad_right"},
      {"remap.triangle", "swipe_up"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_TRUE(set.remaps.empty());
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
}

// ---------------------------------------------------------------------------
// 触摸板那族（I-03、I-04）：能当源，划动不能当映射的源
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, RemapSourceCanUseATouchpadForm) {
  // I-03：左半点击与"左半有触点"是两个不同的源，配置里都摆在键名的位置上。
  const FakeSettings settings{
      {"remap.pad_left", "cross"},
      {"remap.pad_right", "circle"},
      {"remap.touch_left", "l3"},
      {"remap.touch_two", "dpad_up"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.remaps.size(), 4u);
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
  EXPECT_EQ(Find(set, Source::kPadLeft)->targets, BitOf(Source::kCross));
  EXPECT_EQ(Find(set, Source::kPadRight)->targets, BitOf(Source::kCircle));
  EXPECT_EQ(Find(set, Source::kTouchLeft)->targets, BitOf(Source::kL3));
  EXPECT_EQ(Find(set, Source::kTouchTwo)->targets, BitOf(Source::kDpadUp));
}

TEST_F(ButtonsSettings, RemapSourceRejectsASwipe) {
  // I-04 末句：划动是一次性事件，没有"按住"。remap 的形态是源按着目标就按着，
  // 套在事件位上等于把一次手势拖成一串按住，所以要那种输出该写 macro。
  const FakeSettings settings{
      {"remap.swipe_up", "cross"},
      {"remap.swipe_left", "dpad_left"},
      {"remap.pad_left", "cross"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.remaps.size(), 1u);
  EXPECT_EQ(set.remaps[0].src, Source::kPadLeft);
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 2u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("one-shot event"), std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
}

TEST_F(ButtonsSettings, TouchpadFormsCanTriggerRules) {
  // 同一个 token 在 `:` 左边是判据，落不进状态体也没关系：划动与半区都能当触发
  // 的项，起始半区与一到两指因此是"组合键的另一半"。
  const FakeSettings settings{
      {"shortcut.1", "swipe_up+touch_left: power_off"},
      {"shortcut.2", "swipe_down+touch_two>500: power_off"},
      {"macro.1", "swipe_right: cross"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.shortcuts.size(), 2u);
  ASSERT_EQ(set.macros.size(), 1u);
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
  EXPECT_EQ(set.shortcuts[0].trigger.terms,
            BitOf(Source::kSwipeUp) | BitOf(Source::kTouchLeft));
  EXPECT_EQ(set.shortcuts[1].trigger.timing, Timing::kLong);
  EXPECT_EQ(set.shortcuts[1].trigger.threshold_ms, 500u);
  EXPECT_EQ(set.macros[0].trigger.terms, BitOf(Source::kSwipeRight));
  EXPECT_EQ(set.macros[0].digits, BitOf(Source::kCross));
}

// ---------------------------------------------------------------------------
// turbo 组（T-02、T-03）
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, TurboGroupBecomesRules) {
  const FakeSettings settings{
      {"turbo.cross", "20"}, {"turbo.l1", "1"}, {"turbo.rpaddle", "60"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.turbos.size(), 3u);
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
  EXPECT_EQ(set.turbos[0].src, Source::kCross);
  EXPECT_EQ(set.turbos[0].hz, 20);
  // 区间的两端都是闭区间：1 与 60 都收（T-02）。
  EXPECT_EQ(set.turbos[1].hz, 1);
  EXPECT_EQ(set.turbos[2].hz, 60);
}

TEST_F(ButtonsSettings, TurboRejectsSourcesItCannotHammer) {
  // T-03：扳机还有行程、触摸板只有一个物理开关、摇杆方向压根不是线上的位。
  const FakeSettings settings{
      {"turbo.l2", "20"},   {"turbo.r2", "20"},      {"turbo.pad", "20"},
      {"turbo.lstick_up", "20"}, {"turbo.circle", "20"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.turbos.size(), 1u);
  EXPECT_EQ(set.turbos[0].src, Source::kCircle);
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 4u);
}

TEST_F(ButtonsSettings, TurboRejectsTheTouchpadForms) {
  // T-03 那条"只能配在数字键上"的判据是 IsWireBit，触摸板那一族全在界外：一次
  // 划动本来就是一次性的，没有可以按固定周期开合的东西。
  const FakeSettings settings{
      {"turbo.pad_left", "20"}, {"turbo.touch_two", "20"},
      {"turbo.swipe_up", "20"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_TRUE(set.turbos.empty());
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
}

TEST_F(ButtonsSettings, TurboFrequencyHasToBeInRange) {
  const FakeSettings settings{
      {"turbo.cross", "0"}, {"turbo.square", "61"}, {"turbo.triangle", "sixty"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_TRUE(set.turbos.empty());
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("presses per second between 1 and 60"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
}

// ---------------------------------------------------------------------------
// axis 组（M-12～M-14、I-02）
// ---------------------------------------------------------------------------

/// 规则表里那根轴的改写记录。
const buttons::rules::AxisRewrite& Ax(const Set& set, Axis axis) {
  return set.axis[buttons::ids::IndexOf(axis)];
}

TEST_F(ButtonsSettings, AxisSwapFillsBothDirections) {
  // 对调在结构里是"这一格读对方"，所以整根换是四格两两成对（M-12）。
  const FakeSettings settings{{"axis.swap_lstick_rstick", "both"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  EXPECT_EQ(Ax(set, Axis::kLstickX).read_from, Axis::kRstickX);
  EXPECT_EQ(Ax(set, Axis::kRstickX).read_from, Axis::kLstickX);
  EXPECT_EQ(Ax(set, Axis::kLstickY).read_from, Axis::kRstickY);
  EXPECT_EQ(Ax(set, Axis::kRstickY).read_from, Axis::kLstickY);
  // 扳机不在这句话的范围里：它没有"另一根"可以对调。
  EXPECT_EQ(Ax(set, Axis::kLTrigger).read_from, Axis::kCount);
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
}

TEST_F(ButtonsSettings, AxisSwapCanCoverOneAxisOnly) {
  const FakeSettings settings{{"axis.swap_lstick_rstick", "x"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  EXPECT_EQ(Ax(set, Axis::kLstickX).read_from, Axis::kRstickX);
  EXPECT_EQ(Ax(set, Axis::kRstickX).read_from, Axis::kLstickX);
  EXPECT_TRUE(Ax(set, Axis::kLstickY).IsIdentity());
  EXPECT_TRUE(Ax(set, Axis::kRstickY).IsIdentity());
}

TEST_F(ButtonsSettings, AxisInvertIsPerAxis) {
  // 摇杆四根和扳机两根都在轴名表里，所以反向的范围比居中大（M-13）。
  const FakeSettings settings{
      {"axis.invert_rstick_x", "true"}, {"axis.invert_ltrigger", "true"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  EXPECT_TRUE(Ax(set, Axis::kRstickX).invert);
  EXPECT_TRUE(Ax(set, Axis::kLTrigger).invert);
  EXPECT_FALSE(Ax(set, Axis::kRstickY).invert);
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
}

TEST_F(ButtonsSettings, AxisInvertFalseIsTheSameAsNotWritingIt) {
  // C-04：没有规则进表，整节就等于没写。
  const FakeSettings settings{{"axis.invert_lstick_x", "false"}};
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_TRUE(set.Empty());
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
}

TEST_F(ButtonsSettings, AxisCenterCoversBothAxesOfTheStick) {
  // 居中说的是整根摇杆：只居中一根会把它推到对角线上（I-02）。
  const FakeSettings settings{{"axis.center_lstick", "always"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  EXPECT_TRUE(Ax(set, Axis::kLstickX).always_center);
  EXPECT_TRUE(Ax(set, Axis::kLstickY).always_center);
  EXPECT_TRUE(Ax(set, Axis::kRstickX).IsIdentity());
}

TEST_F(ButtonsSettings, InvertAndCenterComposeOnTheSameAxis) {
  // 三件事各占记录里的一个开关，同时写不需要判冲突。
  const FakeSettings settings{
      {"axis.invert_lstick_x", "true"}, {"axis.center_lstick", "always"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  EXPECT_TRUE(Ax(set, Axis::kLstickX).invert);
  EXPECT_TRUE(Ax(set, Axis::kLstickX).always_center);
}

TEST_F(ButtonsSettings, AxisValueOutsideTheKnownWordsIsReported) {
  const FakeSettings settings{
      {"axis.swap_lstick_rstick", "diagonal"},
      {"axis.center_lstick", "hit"},
      {"axis.invert_lstick_x", "yes"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][1].find("has to be 'always'"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][1];
}

TEST_F(ButtonsSettings, UnknownAxisLeafIsIgnored) {
  // 扳机没有"整根"，所以 center_ltrigger 是不认识的键，不是写错的取值。
  const FakeSettings settings{
      {"axis.invert_lstik_x", "true"}, {"axis.center_ltrigger", "always"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
  EXPECT_EQ(Lines(DS5_LOG_DEBUG), 2u);
}

// ---------------------------------------------------------------------------
// 触发串：shortcut 与 macro 共用的那套语法（C-02）
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, ShortcutTimings) {
  const FakeSettings settings{
      {"shortcut.1", "create+options: power_off"},
      {"shortcut.2", "home*2: power_off"},
      {"shortcut.3", "l3>750: power_off"},
      {"shortcut.4", "circle~: power_off"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.shortcuts.size(), 4u);
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);

  EXPECT_EQ(set.shortcuts[0].trigger.timing, Timing::kChord);
  EXPECT_EQ(set.shortcuts[0].trigger.terms,
            BitOf(Source::kCreate) | BitOf(Source::kOptions));
  EXPECT_EQ(set.shortcuts[1].trigger.timing, Timing::kDouble);
  EXPECT_EQ(set.shortcuts[1].trigger.threshold_ms, buttons::rules::kDefaultDoubleMs);
  EXPECT_EQ(set.shortcuts[2].trigger.timing, Timing::kLong);
  EXPECT_EQ(set.shortcuts[2].trigger.threshold_ms, 750u);
  EXPECT_EQ(set.shortcuts[3].trigger.timing, Timing::kTap);
  EXPECT_EQ(set.shortcuts[3].trigger.threshold_ms, buttons::rules::kDefaultTapMs);
}

TEST_F(ButtonsSettings, TriggerIgnoresSpacesAroundTheTerms) {
  // 带引号的标量会留着空格，而触发串里的空格没有第二种含义。
  const FakeSettings settings{{"shortcut.1", " l3 + r3 : power_off"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.shortcuts.size(), 1u);
  EXPECT_EQ(set.shortcuts[0].trigger.terms,
            BitOf(Source::kL3) | BitOf(Source::kR3));
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
}

TEST_F(ButtonsSettings, OneTimingPerTrigger) {
  // 后缀描述的是整组键的时序：写在中段没有可解释的语义（K-01 vs K-02），写两次
  // 更不可能同时成立。
  const FakeSettings settings{
      {"shortcut.1", "l3~+r3: power_off"},
      {"shortcut.2", "l3~+r3*2: power_off"},
      {"shortcut.3", "home*3: power_off"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("in the middle of the group"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
  EXPECT_NE(g_lines[DS5_LOG_WARNING][1].find("more than one timing suffix"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][1];
  EXPECT_NE(g_lines[DS5_LOG_WARNING][2].find("neither '~', '*2' nor"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][2];
}

TEST_F(ButtonsSettings, ShortcutNeedsBothHalves) {
  const FakeSettings settings{
      {"shortcut.1", "create+options"},
      {"shortcut.2", "create+options:"},
      {"shortcut.3", "create+options: turn_off_bluetooth"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("has no ':' between the trigger"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
  EXPECT_NE(g_lines[DS5_LOG_WARNING][2].find("only 'power_off'"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][2];
}

TEST_F(ButtonsSettings, LongThresholdRangeIsChecked) {
  // '>' 后面的数三条都读不通：太短、太长、根本不是一个数。
  const FakeSettings settings{
      {"shortcut.1", "l3>20: power_off"},
      {"shortcut.2", "l3>10001: power_off"},
      {"shortcut.3", "l3>abc: power_off"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 3u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("between 50 and 10000 milliseconds"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
}

TEST_F(ButtonsSettings, UnknownTermInATriggerIsReported) {
  // 触发串里写错的键名是"这一条读不通"，不是"这个键我不读"：整条都在取值里。
  const FakeSettings settings{{"shortcut.1", "cricle+options: power_off"}};
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 1u);
  EXPECT_NE(g_lines[DS5_LOG_WARNING][0].find("a term that is not a key name"),
            std::string::npos)
      << g_lines[DS5_LOG_WARNING][0];
  EXPECT_EQ(Lines(DS5_LOG_DEBUG), 0u);
}

TEST_F(ButtonsSettings, MacroBecomesRule) {
  const FakeSettings settings{{"macro.1", "circle~: l3+dpad_up"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.macros.size(), 1u);
  EXPECT_EQ(set.macros[0].trigger.timing, Timing::kTap);
  // 冒号右边是"同时按下"（A-04），与左边的"与"共用同一个 '+'，两边处理者不同。
  EXPECT_EQ(set.macros[0].digits, BitOf(Source::kL3) | BitOf(Source::kDpadUp));
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
}

TEST_F(ButtonsSettings, MacroTargetsMustExistOnTheWire) {
  const FakeSettings settings{
      {"macro.1", "circle~: lstick_up"},
      {"macro.2", "circle~: pad_left"},
  };
  Set set;
  EXPECT_FALSE(Parse(settings, set));
  EXPECT_TRUE(set.macros.empty());
  ASSERT_EQ(Lines(DS5_LOG_WARNING), 2u);
}

TEST_F(ButtonsSettings, MacroTriggerCanUseAStickDirection) {
  // 方向在 `:` 左边是判据、在右边要落进状态体，所以只有右边有这条限制。
  const FakeSettings settings{{"macro.1", "lstick_up: cross"}};
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  ASSERT_EQ(set.macros.size(), 1u);
  EXPECT_EQ(set.macros[0].trigger.terms, BitOf(Source::kLstickUp));
  EXPECT_EQ(set.macros[0].digits, BitOf(Source::kCross));
}

// ---------------------------------------------------------------------------
// 需求第 7 节那份例子：整份读通，一条都不掉
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, ShippedExampleParsesCleanly) {
  const FakeSettings settings{
      {"remap.circle", "cross"},
      {"remap.square", "triangle+keep"},
      {"remap.l2", "r2"},
      {"remap.mute", "disable"},
      {"remap.lstick_up", "dpad_up"},
      {"remap.pad_left", "cross"},
      {"axis.swap_lstick_rstick", "both"},
      {"axis.invert_rstick_x", "true"},
      {"axis.center_lstick", "always"},
      {"turbo.cross", "20"},
      {"shortcut.1", "create+options: power_off"},
      {"shortcut.2", "home*2: power_off"},
      {"shortcut.3", "l3>750: power_off"},
      {"shortcut.4", "swipe_up+touch_left: power_off"},
      {"macro.1", "circle~: l3+dpad_up"},
      {"macro.2", "swipe_right: cross"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  EXPECT_EQ(set.remaps.size(), 6u);
  EXPECT_EQ(set.turbos.size(), 1u);
  EXPECT_EQ(set.shortcuts.size(), 4u);
  EXPECT_EQ(set.macros.size(), 2u);
  EXPECT_FALSE(Ax(set, Axis::kRstickX).IsIdentity());
  EXPECT_EQ(Lines(DS5_LOG_WARNING), 0u);
}

// ---------------------------------------------------------------------------
// 武装摘要：插件本体与配置校验工具共用的一行
// ---------------------------------------------------------------------------

TEST_F(ButtonsSettings, SummaryCountsEveryKindOfRule) {
  const FakeSettings settings{
      {"remap.circle", "cross"},
      {"axis.swap_lstick_rstick", "both"},
      {"axis.invert_rstick_x", "false"},
      {"turbo.r3", "30"},
      {"shortcut.1", "l3+r3>800: power_off"},
      {"macro.1", "touch_two: cross+circle"},
  };
  Set set;
  ASSERT_TRUE(Parse(settings, set));
  // 对调一次写满四格（两根摇杆各两根轴）；`false` 不留痕迹，所以轴改写是 4 条
  // 而不是 5 条——摘要与 Set::Empty 同一口径，数的是"真的会改什么"。
  EXPECT_EQ(buttons::settings::Summary(set),
            "1 remap(s), 4 axis rewrite(s), 1 turbo(s), 1 shortcut(s), "
            "1 macro(s)");
}

}  // namespace
