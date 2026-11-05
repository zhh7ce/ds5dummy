// DS5Dummy — fd 交接纯逻辑单测（handoff.cpp）
//
// 覆盖不动进程 fd 表的 PlanOk 与两个 memfd 辅助函数：
//   - PlanOk：对 ds5::daemon::HandoffPlan 的合法性判定（负号/stdio/重复）+
//     两个重载（std::string& 有分配版给父进程，const char** 无分配版给 fork
//     后子进程）。
//   - MakeConfigFd/ReadConfigFd：真实 memfd 的写入、封存与读回；测试进程只用
//     自己新建的 fd，不碰任何外部资源。
//   - ExecStageName：把 PrepareAndExec 的 120+stage 退出码翻译回人话。
// ApplyPlan/CloseAbove/PrepareAndExec 会真的 dup2/close_range/execv，在测试
// 进程里调用会搅掉它自己的 fd，归 Manager↔worker 的集成/硬件验收。

#include "daemon/handoff.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

TEST(PlanOk, AcceptsDistinctNonStdioFds) {
  std::string why;
  EXPECT_TRUE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 9, 10}, why))
      << why;
  EXPECT_TRUE(why.empty());
}

TEST(PlanOk, AcceptsSourceEqualOwnTarget) {
  // ep0 已经在 3、快照已经在 6 都是恒等，不算冲突（apply 会跳过挪位）。
  std::string why;
  EXPECT_TRUE(ds5::daemon::PlanOk(
      ds5::daemon::HandoffPlan{ds5::daemon::kEp0Target, 8, 9,
                               ds5::daemon::kConfigFdTarget},
      why))
      << why;
}

TEST(PlanOk, RejectsUnsetNegativeFd) {
  const char* why = nullptr;
  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{-1, 4, 5, 6}, &why));
  EXPECT_STREQ(why, "handoff fd is unset");

  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 9, -1}, &why));
  EXPECT_STREQ(why, "handoff fd is unset");
}

TEST(PlanOk, RejectsStdioCollision) {
  std::string why;
  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{0, 4, 5, 6}, why));
  EXPECT_NE(why.find("stdio"), std::string::npos) << why;

  why.clear();
  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 9, 2}, why));
  EXPECT_NE(why.find("stdio"), std::string::npos) << why;
}

TEST(PlanOk, RejectsSharedFd) {
  const char* why = nullptr;
  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 7, 8, 9}, &why));
  EXPECT_STREQ(why, "handoff fds cannot share one number");

  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 7, 9}, &why));
  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 9, 9}, &why));
  EXPECT_FALSE(
      ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 9, 7}, &why));
}

TEST(PlanOk, BothOverloadsAgree) {
  // 无分配版和 string 版必须给出同一个结论与同一句话。
  const ds5::daemon::HandoffPlan bad{1, 4, 5, 6};
  const char* raw = nullptr;
  std::string str;
  EXPECT_FALSE(ds5::daemon::PlanOk(bad, &raw));
  EXPECT_FALSE(ds5::daemon::PlanOk(bad, str));
  EXPECT_EQ(std::string(raw), str);
}

// 快照的写入与读回：内容逐字节一致，fd 由测试自己关闭。
TEST(ConfigFd, RoundTripsBlob) {
  const std::vector<uint8_t> blob{1, 2, 3, 0, 255, 42};
  std::string why;
  const int fd = ds5::daemon::MakeConfigFd(blob, why);
  ASSERT_GE(fd, 0) << why;
  const std::vector<uint8_t> got = ds5::daemon::ReadConfigFd(fd, why);
  ::close(fd);
  ASSERT_EQ(got.size(), blob.size()) << why;
  EXPECT_EQ(got, blob);
}

// 封存之后不能再写：快照在交接那一刻就应该不可变。
TEST(ConfigFd, IsSealedAgainstWrites) {
  std::string why;
  const int fd = ds5::daemon::MakeConfigFd({1, 2, 3}, why);
  ASSERT_GE(fd, 0) << why;
  const uint8_t byte = 0x7F;
  EXPECT_LT(::write(fd, &byte, 1), 0);
  EXPECT_EQ(errno, EPERM);
  ::close(fd);
}

// 不是普通文件（这里用管道读端）一律拒绝：快照必须是可 fstat 定长的 memfd。
TEST(ConfigFd, RejectsNonRegularFile) {
  int fds[2] = {-1, -1};
  ASSERT_EQ(::pipe(fds), 0);
  std::string why;
  const std::vector<uint8_t> got = ds5::daemon::ReadConfigFd(fds[0], why);
  ::close(fds[0]);
  ::close(fds[1]);
  EXPECT_TRUE(got.empty());
  EXPECT_FALSE(why.empty());
}

TEST(ExecStageName, MapsExitCodes) {
  EXPECT_EQ(ds5::daemon::ExecStageName(ds5::daemon::kExecExitBase + 1),
            "bad fd plan");
  EXPECT_EQ(ds5::daemon::ExecStageName(ds5::daemon::kExecExitBase + 2),
            "dup2/fcntl failed");
  EXPECT_EQ(ds5::daemon::ExecStageName(ds5::daemon::kExecExitBase + 3),
            "close_range failed");
  EXPECT_EQ(ds5::daemon::ExecStageName(ds5::daemon::kExecExitBase + 4),
            "setgroups/setgid/setuid failed");
  EXPECT_EQ(ds5::daemon::ExecStageName(ds5::daemon::kExecExitBase + 5),
            "execv failed");
}

TEST(ExecStageName, FallsBackForUnmappedCode) {
  EXPECT_EQ(ds5::daemon::ExecStageName(0), "exit 0");
  EXPECT_EQ(ds5::daemon::ExecStageName(127), "exit 127");
}
