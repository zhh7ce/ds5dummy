// DS5Dummy — 身份选择单测（daemon/identity.cpp）
//
// 覆盖纯策略 SelectWorkerIdentity 与 LookupHome。QueryGraphicalSessions 依赖
// sd-login 与真实登录会话，不属于单测范围。全部用例不需要 root 与手柄：
// 会话由测试手工构造，LookupHome 只查 root（passwd 里必然存在）与 uid_t 最大值
// （必然不存在），两者都不依赖具体发行版的账号布局。

#include "daemon/identity.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using ds5::daemon::IdentityDecision;
using ds5::daemon::IdentityEntry;
using ds5::daemon::IdentityMode;
using ds5::daemon::IdentityOutcome;
using ds5::daemon::SessionInfo;
using ds5::daemon::SystemConfig;

// 构造一个默认 active 的图形会话；type 缺省 wayland，primary_gid 传 0
// 表示未知。
SessionInfo MakeSession(uid_t uid, gid_t primary_gid,
                        const char* type = "wayland", bool active = true) {
  SessionInfo session;
  session.uid = uid;
  session.primary_gid = primary_gid;
  session.type = type;
  session.active = active;
  return session;
}

/** auto 模式的系统配置，不带白名单。 */
SystemConfig AutoConfig() {
  SystemConfig config;
  config.identity_mode = IdentityMode::kAuto;
  return config;
}

/** list 模式的系统配置；entries 按给定顺序成为 allowed_users。 */
SystemConfig ListConfig(std::vector<IdentityEntry> entries) {
  SystemConfig config;
  config.identity_mode = IdentityMode::kList;
  config.allowed_users = std::move(entries);
  return config;
}

/** 构造一条白名单条目。 */
IdentityEntry Entry(uid_t uid, gid_t gid) {
  IdentityEntry entry;
  entry.uid = uid;
  entry.gid = gid;
  return entry;
}

// ---------------------------------------------------------------------------
// auto 模式
// ---------------------------------------------------------------------------

TEST(IdentityAuto, NoSessionsRetries) {
  const IdentityDecision decision =
      ds5::daemon::SelectWorkerIdentity(AutoConfig(), {});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kRetry);
  EXPECT_FALSE(decision.why.empty());
  EXPECT_EQ(decision.identity.uid, 0u);
  EXPECT_EQ(decision.identity.gid, 0u);
  EXPECT_TRUE(decision.warnings.empty());
}

TEST(IdentityAuto, SingleSessionServesPasswdPrimaryGid) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      AutoConfig(), {MakeSession(1000, 1000)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
  EXPECT_EQ(decision.identity.gid, 1000u);
  EXPECT_TRUE(decision.why.empty());
  EXPECT_TRUE(decision.warnings.empty());
}

TEST(IdentityAuto, TwoDistinctUidsRetryAndListThem) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      AutoConfig(), {MakeSession(1000, 1000), MakeSession(1001, 1001)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kRetry);
  EXPECT_NE(decision.why.find("1000"), std::string::npos);
  EXPECT_NE(decision.why.find("1001"), std::string::npos);
}

TEST(IdentityAuto, DuplicateSessionsOfSameUidServeOnce) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      AutoConfig(),
      {MakeSession(1000, 1000, "wayland"), MakeSession(1000, 1000, "x11")});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
  EXPECT_EQ(decision.identity.gid, 1000u);
}

TEST(IdentityAuto, InactiveSessionsAreIgnored) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      AutoConfig(), {MakeSession(1000, 1000),
                     MakeSession(1001, 1001, "wayland", /*active=*/false)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
}

TEST(IdentityAuto, TtySessionsAreIgnored) {
  // 只有 ttys 会话 → 等同于没登录。
  const IdentityDecision only_tty = ds5::daemon::SelectWorkerIdentity(
      AutoConfig(), {MakeSession(1000, 1000, "tty")});
  EXPECT_EQ(only_tty.outcome, IdentityOutcome::kRetry);

  // 图形会话与 ttys 并存 → ttys 不参与候选计数。
  const IdentityDecision mixed = ds5::daemon::SelectWorkerIdentity(
      AutoConfig(), {MakeSession(1000, 1000), MakeSession(1001, 1001, "tty")});
  EXPECT_EQ(mixed.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(mixed.identity.uid, 1000u);
}

TEST(IdentityAuto, UnknownPrimaryGidRetries) {
  const IdentityDecision decision =
      ds5::daemon::SelectWorkerIdentity(AutoConfig(), {MakeSession(1000, 0)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kRetry);
  EXPECT_NE(decision.why.find("gid"), std::string::npos);
}

TEST(IdentityAuto, UidZeroIsNeverServed) {
  const IdentityDecision decision =
      ds5::daemon::SelectWorkerIdentity(AutoConfig(), {MakeSession(0, 1000)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kRetry);
  EXPECT_NE(decision.why.find("uid 0"), std::string::npos);
}

// ---------------------------------------------------------------------------
// list 模式
// ---------------------------------------------------------------------------

TEST(IdentityList, NoSessionsRetriesWhileWaitingForLogin) {
  const IdentityDecision decision =
      ds5::daemon::SelectWorkerIdentity(ListConfig({Entry(1000, 998)}), {});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kRetry);
  EXPECT_FALSE(decision.why.empty());
}

TEST(IdentityList, EmptyAllowedUsersDisablesInsteadOfCrashing) {
  // 配置解析器已禁止 list 模式空名单；策略层仍必须给出确定结果。
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({}), {MakeSession(1000, 1000)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kDisabled);
  EXPECT_FALSE(decision.why.empty());
}

TEST(IdentityList, SessionInListServesConfiguredGid) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 1000)}), {MakeSession(1000, 1000)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
  EXPECT_EQ(decision.identity.gid, 1000u);
  EXPECT_TRUE(decision.warnings.empty());
}

TEST(IdentityList, SessionNotInListDisables) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 1000)}), {MakeSession(1001, 1001)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kDisabled);
  EXPECT_FALSE(decision.why.empty());
}

TEST(IdentityList, TwoAllowedUsersPresentRetryAndListThem) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 1000), Entry(1001, 1001)}),
      {MakeSession(1000, 1000), MakeSession(1001, 1001)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kRetry);
  EXPECT_NE(decision.why.find("1000"), std::string::npos);
  EXPECT_NE(decision.why.find("1001"), std::string::npos);
}

TEST(IdentityList, MultipleSessionsOfOneAllowedUserServeOnce) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 998)}),
      {MakeSession(1000, 998, "wayland"), MakeSession(1000, 998, "x11")});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
  EXPECT_EQ(decision.identity.gid, 998u);
  EXPECT_TRUE(decision.warnings.empty());
}

TEST(IdentityList, GidMismatchServesWithWarning) {
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 4242)}), {MakeSession(1000, 1000)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
  EXPECT_EQ(decision.identity.gid, 4242u);
  ASSERT_EQ(decision.warnings.size(), 1u);
  EXPECT_NE(decision.warnings[0].find("4242"), std::string::npos);
  EXPECT_NE(decision.warnings[0].find("1000"), std::string::npos);
}

TEST(IdentityList, InactiveAllowedSessionDoesNotCountAsMatch) {
  // 名单内用户只是挂着未激活的会话（例如已切走的 VT），当前活跃的桌面属于
  // 名单外用户：匹配集为空 → 该登录用户未启用，kDisabled 而不是拿旧会话服务。
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 1000)}),
      {MakeSession(1000, 1000, "wayland", /*active=*/false),
       MakeSession(1001, 1001)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kDisabled);
  EXPECT_FALSE(decision.why.empty());
}

TEST(IdentityList, DuplicateEntriesForSameUidUseTheFirstGid) {
  // 同一 uid 写了两条（gid 不同）时取第一条，选择结果只由配置顺序决定；
  // primary_gid 与第一条一致，所以不应产生 gid 不一致的告警。
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 998), Entry(1000, 4242)}),
      {MakeSession(1000, 998)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
  EXPECT_EQ(decision.identity.gid, 998u);
  EXPECT_TRUE(decision.warnings.empty());
}

TEST(IdentityList, UnknownPrimaryGidStillServesConfiguredGid) {
  // primary_gid == 0 是"passwd 没查到"，不是 gid 0：list 模式不依赖它，
  // 照配置 gid 服务，并且因为无法比对而不产生告警。
  const IdentityDecision decision = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 998)}), {MakeSession(1000, 0)});
  EXPECT_EQ(decision.outcome, IdentityOutcome::kServe);
  EXPECT_EQ(decision.identity.uid, 1000u);
  EXPECT_EQ(decision.identity.gid, 998u);
  EXPECT_TRUE(decision.warnings.empty());
  EXPECT_TRUE(decision.why.empty());
}

TEST(IdentityList, UidZeroOrGidZeroIsNeverServed) {
  const IdentityDecision uid_zero = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(0, 1000)}), {MakeSession(0, 1000)});
  EXPECT_EQ(uid_zero.outcome, IdentityOutcome::kRetry);

  const IdentityDecision gid_zero = ds5::daemon::SelectWorkerIdentity(
      ListConfig({Entry(1000, 0)}), {MakeSession(1000, 1000)});
  EXPECT_EQ(gid_zero.outcome, IdentityOutcome::kRetry);
}

// ---------------------------------------------------------------------------
// LookupHome
// ---------------------------------------------------------------------------

TEST(IdentityLookupHome, RootHasNonEmptyHome) {
  std::string home;
  std::string why;
  ASSERT_TRUE(ds5::daemon::LookupHome(0, home, why)) << why;
  EXPECT_FALSE(home.empty());
  EXPECT_TRUE(why.empty());
}

TEST(IdentityLookupHome, UnknownUidFailsAndClearsHome) {
  // uid_t 的最大值不会被分配给任何系统账号（passwd/uid 分配都远小于它），
  // 所以这是一个稳定"不存在"的 uid，不依赖具体发行版的账号布局。
  const uid_t unknown = std::numeric_limits<uid_t>::max();
  std::string home = "sentinel";  // 失败时必须被清空，不能留下旧值
  std::string why;
  ASSERT_FALSE(ds5::daemon::LookupHome(unknown, home, why))
      << "uid " << static_cast<unsigned long long>(unknown)
      << " unexpectedly resolved in this environment";
  EXPECT_TRUE(home.empty());
  EXPECT_FALSE(why.empty());
}

}  // namespace
