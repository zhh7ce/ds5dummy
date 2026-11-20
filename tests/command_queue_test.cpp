/**
 * @file
 * @brief 宿主命令队列的单测：入队上限、FIFO、排空只跑一次、pid 回收与并发上限
 *
 * 队列真的 fork `/bin/sh`：命令只往临时文件里追加内容，不需要 root 或手柄。
 * 环与 pid 表是进程级的文件作用域状态，所以每条用例先 ResetForTesting()，拆用例
 * 前再等所有子进程退出并被 Drain 回收，不给后面的用例留僵尸。
 */

#include "daemon/command_queue.h"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "plugin/plugin_api.h"

namespace {

namespace fs = std::filesystem;

using ds5::daemon::command_queue::Drain;
using ds5::daemon::command_queue::kCapacity;
using ds5::daemon::command_queue::kLiveMax;
using ds5::daemon::command_queue::LiveForTesting;
using ds5::daemon::command_queue::Post;
using ds5::daemon::command_queue::ResetForTesting;
using ds5::daemon::command_queue::Start;

// 每个用例一个独立目录：进程号 + 自增序号，避免并行跑 ctest 时互相清目录。
int next_dir = 0;

class CommandQueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    dir_ = fs::temp_directory_path() /
           ("ds5dummy-command-queue-" + std::to_string(::getpid()) + "-" +
            std::to_string(next_dir++));
    fs::create_directories(dir_);
    Start(dir_.string());
  }

  void TearDown() override {
    DrainUntil([] { return LiveForTesting() == 0; });
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  /**
   * @brief 轮询 Drain，直到 done() 成立或超时。
   * @param done 判据；每次 Drain 之后看一次。
   * @return 超时前成立 true。
   * @note 超时 2 s：命令都是毫秒级的 sh 启动加一次写文件，留足 CI 的调度余量。
   */
  static bool DrainUntil(const std::function<bool()>& done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      Drain();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
  }

  /// 追加一行字面量 text 到本用例目录下的 name。
  std::string AppendLiteral(const std::string& name,
                            const std::string& text) const {
    return "printf '" + text + "\\n' >> \"" + (dir_ / name).string() + "\"";
  }

  /// 追加一行 "<tag>:<自己的 pid>" 到 name。
  /// @details pid 随 fork 顺序单调递增，把 tag 与 pid 对上就能证明"第几条命令先
  ///          被 fork"，而不依赖三条命令谁先写到文件——写入顺序是调度噪声。
  std::string AppendTagged(const std::string& name,
                           const std::string& tag) const {
    return "printf \"" + tag + ":$$\\n\" >> \"" + (dir_ / name).string() +
           "\"";
  }

  /// 读出 name 里的全部行；文件不存在时为空。
  std::vector<std::string> ReadLines(const std::string& name) const {
    std::vector<std::string> lines;
    std::ifstream in(dir_ / name);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
  }

  fs::path dir_;
};

TEST_F(CommandQueueTest, PostRejectsNullAndOversize) {
  EXPECT_FALSE(Post(nullptr));

  // 511 字节是可执行的最大长度：命令本体用 touch 跑得起来，补空格凑到边界。
  const std::string base = "touch \"" + (dir_ / "limit").string() + "\"";
  ASSERT_LT(base.size(), kPluginCommandSize - 1);
  const std::string at_limit =
      base + std::string(kPluginCommandSize - 1 - base.size(), ' ');
  ASSERT_EQ(at_limit.size(), kPluginCommandSize - 1);
  EXPECT_TRUE(Post(at_limit.c_str()));
  EXPECT_TRUE(DrainUntil([&] { return fs::exists(dir_ / "limit"); }));

  // 512 字节（含结尾 NUL 就是 kPluginCommandSize）：拒收，不截断。
  EXPECT_FALSE(Post((at_limit + " ").c_str()));
}

TEST_F(CommandQueueTest, PostRejectsNewestWhenRingIsFull) {
  for (std::size_t i = 0; i < kCapacity; ++i) {
    EXPECT_TRUE(Post("true")) << "entry " << i;
  }
  EXPECT_FALSE(Post("true"));
}

TEST_F(CommandQueueTest, DrainStartsEachEntryOnceInOrder) {
  ASSERT_TRUE(Post(AppendTagged("fifo", "0").c_str()));
  ASSERT_TRUE(Post(AppendTagged("fifo", "1").c_str()));
  ASSERT_TRUE(Post(AppendTagged("fifo", "2").c_str()));

  Drain();
  ASSERT_TRUE(DrainUntil([&] { return ReadLines("fifo").size() == 3; }));

  // 环已空：再排两次不会重复执行，也不会多出条目。
  Drain();
  Drain();
  const std::vector<std::string> lines = ReadLines("fifo");
  ASSERT_EQ(lines.size(), 3u);

  // 按 tag 取 pid，pid 的顺序就是 fork 的顺序。
  long pid[3] = {};
  for (const std::string& line : lines) {
    const std::size_t colon = line.find(':');
    ASSERT_NE(colon, std::string::npos) << line;
    pid[std::stoi(line.substr(0, colon))] = std::stol(line.substr(colon + 1));
  }
  EXPECT_LT(pid[0], pid[1]);
  EXPECT_LT(pid[1], pid[2]);
}

TEST_F(CommandQueueTest, DrainReapsExitedCommands) {
  ASSERT_TRUE(Post("true"));
  Drain();
  // 槽只在 Drain 的 Reap 里清掉，所以子进程即便已退出，这里仍然占着槽。
  EXPECT_EQ(LiveForTesting(), 1u);
  EXPECT_TRUE(DrainUntil([] { return LiveForTesting() == 0; }));
}

TEST_F(CommandQueueTest, DropsCommandsBeyondLiveLimit) {
  // 每条命令先写一行再睡 1 s：写行证明它起来了，睡觉把它按在"在跑"状态里，
  // 64 个槽因此能在几百毫秒内被真正占满。
  const std::string command =
      AppendLiteral("live", "x") + "; sleep 1";

  for (std::size_t round = 0; round < kLiveMax / kCapacity; ++round) {
    for (std::size_t i = 0; i < kCapacity; ++i) {
      ASSERT_TRUE(Post(command.c_str()));
    }
    Drain();
  }
  ASSERT_EQ(LiveForTesting(), kLiveMax);

  // 第 65 条入队成功但拿不到槽：丢弃，不再产生进程。
  ASSERT_TRUE(Post(command.c_str()));
  Drain();
  EXPECT_EQ(LiveForTesting(), kLiveMax);

  ASSERT_TRUE(DrainUntil(
      [&] { return ReadLines("live").size() >= kLiveMax; }));
  EXPECT_EQ(ReadLines("live").size(), kLiveMax);
}

}  // namespace