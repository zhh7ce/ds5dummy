// DS5Dummy — fd 交接纯逻辑单测（handoff.cpp）
//
// 只覆盖不动真 fd、不 execv 的两个纯函数：
//   - PlanOk：对 ds5::daemon::HandoffPlan 的合法性判定（负号/stdio/重复）+
//   两个重载
//     （std::string& 有分配版给父进程，const char** 无分配版给 fork
//     后子进程）。
//   - ExecStageName：把 PrepareAndExec 的 120+stage 退出码翻译回人话。
// ApplyPlan/CloseAbove/PrepareAndExec 会真的 dup2/close_range/execv，在测试
// 进程里调用会搅掉它自己的 fd，归 Manager↔worker 的集成/硬件验收。

#include "daemon/handoff.h"

#include <gtest/gtest.h>

#include <string>

TEST(PlanOk, AcceptsDistinctNonStdioFds) {
  std::string why;
  EXPECT_TRUE(ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 9}, why))
      << why;
  EXPECT_TRUE(why.empty());
}

TEST(PlanOk, AcceptsSourceEqualOwnTarget) {
  // ep0 已经在 3 是恒等，不算冲突（apply 会跳过挪位）。
  std::string why;
  EXPECT_TRUE(ds5::daemon::PlanOk(
      ds5::daemon::HandoffPlan{ds5::daemon::kEp0Target, 8, 9}, why))
      << why;
}

TEST(PlanOk, RejectsUnsetNegativeFd) {
  const char* why = nullptr;
  EXPECT_FALSE(ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{-1, 4, 5}, &why));
  EXPECT_STREQ(why, "endpoint fd is unset");
}

TEST(PlanOk, RejectsStdioCollision) {
  std::string why;
  EXPECT_FALSE(ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{0, 4, 5}, why));
  EXPECT_NE(why.find("stdio"), std::string::npos) << why;

  why.clear();
  EXPECT_FALSE(ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{2, 4, 5}, why));
  EXPECT_NE(why.find("stdio"), std::string::npos) << why;
}

TEST(PlanOk, RejectsSharedFd) {
  const char* why = nullptr;
  EXPECT_FALSE(ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 7, 8}, &why));
  EXPECT_STREQ(why, "the three endpoints cannot share one fd");

  EXPECT_FALSE(ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 7}, &why));
  EXPECT_FALSE(ds5::daemon::PlanOk(ds5::daemon::HandoffPlan{7, 8, 8}, &why));
}

TEST(PlanOk, BothOverloadsAgree) {
  // 无分配版和 string 版必须给出同一个结论与同一句话。
  const ds5::daemon::HandoffPlan bad{1, 4, 5};
  const char* raw = nullptr;
  std::string str;
  EXPECT_FALSE(ds5::daemon::PlanOk(bad, &raw));
  EXPECT_FALSE(ds5::daemon::PlanOk(bad, str));
  EXPECT_EQ(std::string(raw), str);
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
