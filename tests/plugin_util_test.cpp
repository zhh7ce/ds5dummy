/**
 * @file
 * @brief plugin_util.h 的契约单测：配置键查找、整数解析、单调时钟
 *
 * 这三个小工具现在是插件作者共用的头文件 API，语义边界由这里钉住：精确匹配
 * 而不是前缀匹配、只收非负十进制、越界与超长文本一律失败且不改出参。idle 的
 * 用例只覆盖它自己怎么用，覆盖不到这些边界。
 */

#include <cstdint>

#include "gtest/gtest.h"
#include "plugin/plugin_util.h"

namespace {

TEST(FindSetting, NullArrayHasNoKeys) {
  EXPECT_EQ(plugin::FindSetting(nullptr, "anything"), nullptr);
}

TEST(FindSetting, MatchesTheExactKey) {
  const ds5Setting settings[] = {
      {"off_after_minutes", "30"}, {"verbose", "1"}, {nullptr, nullptr}};
  EXPECT_STREQ(plugin::FindSetting(settings, "off_after_minutes"), "30");
  EXPECT_STREQ(plugin::FindSetting(settings, "verbose"), "1");
}

TEST(FindSetting, DoesNotMatchPrefixesOrUnknownKeys) {
  const ds5Setting settings[] = {
      {"off_after_minutes_extra", "5"}, {nullptr, nullptr}};
  EXPECT_EQ(plugin::FindSetting(settings, "off_after_minutes"), nullptr);
  EXPECT_EQ(plugin::FindSetting(settings, "nope"), nullptr);
}

TEST(FindSetting, EmptyArrayHasNoKeys) {
  const ds5Setting settings[] = {{nullptr, nullptr}};
  EXPECT_EQ(plugin::FindSetting(settings, "anything"), nullptr);
}

TEST(ParseInt, AcceptsNonNegativeDecimals) {
  int value = -1;
  ASSERT_TRUE(plugin::ParseInt("0", 0, 60, value));
  EXPECT_EQ(value, 0);
  ASSERT_TRUE(plugin::ParseInt("30", 0, 60, value));
  EXPECT_EQ(value, 30);
  ASSERT_TRUE(plugin::ParseInt("60", 0, 60, value));
  EXPECT_EQ(value, 60);
  // 前导零是同一个数，不是八进制。
  ASSERT_TRUE(plugin::ParseInt("030", 0, 60, value));
  EXPECT_EQ(value, 30);
}

TEST(ParseInt, RejectsEverythingStrtoiWouldHaveAccepted) {
  int value = -1;
  // 空、空白、正负号、十六进制、小数、非数字：都不是这份配置的合法写法。
  for (const char* text :
       {"", " 30", "30 ", "+30", "-5", "0x1e", "3.5", "abc", "30x"}) {
    EXPECT_FALSE(plugin::ParseInt(text, 0, 60, value)) << text;
    EXPECT_EQ(value, -1) << text;  // 失败不写出参
  }
  EXPECT_FALSE(plugin::ParseInt(nullptr, 0, 60, value));
  EXPECT_EQ(value, -1);
}

TEST(ParseInt, EnforcesBothBounds) {
  int value = -1;
  EXPECT_FALSE(plugin::ParseInt("61", 0, 60, value));
  EXPECT_EQ(value, -1);
  EXPECT_FALSE(plugin::ParseInt("0", 1, 60, value));
  EXPECT_EQ(value, -1);
}

TEST(ParseInt, LongTextStopsAtTheBoundInsteadOfOverflowing) {
  int value = -1;
  EXPECT_FALSE(plugin::ParseInt("999999999999999999999999", 0, 60, value));
  EXPECT_EQ(value, -1);
}

TEST(NowMs, NeverGoesBackwards) {
  const uint64_t first = plugin::NowMs();
  const uint64_t second = plugin::NowMs();
  EXPECT_GE(second, first);
}

}  // namespace
