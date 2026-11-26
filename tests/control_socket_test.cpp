/**
 * @file
 * @brief Manager 控制 socket 的单测：文件模式、一发即走的命令、退出码与目录清理
 *
 * 测试路径由 CMake 的 `DS5DUMMY_CONTROL_SOCKET_DIR` 指到临时目录，不碰真实的
 * /run/ds5dummy。每条用例先重置退出状态与监听状态，再从 Init 开始，条与条之间
 * 不共享状态；客户端用 AF_UNIX 数据报直接发一条，不依赖 socat、也没有回执。
 */

#include "daemon/control_socket.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

#include "daemon/shutdown.h"
#include "gtest/gtest.h"

namespace {

namespace fs = std::filesystem;

using ds5::daemon::ShutdownExitCode;
using ds5::daemon::ShutdownRequested;
using ds5::daemon::control_socket::Close;
using ds5::daemon::control_socket::Init;
using ds5::daemon::control_socket::kDirectory;
using ds5::daemon::control_socket::kSocketPath;
using ds5::daemon::control_socket::PollOnce;

class ControlSocketTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ds5::daemon::ResetForTesting();
    Close();
    std::string why;
    ASSERT_TRUE(Init(why)) << why;
  }

  void TearDown() override {
    Close();
    ds5::daemon::ResetForTesting();
  }
};

/**
 * @brief 发一个数据报就关掉客户端；数据报没有连接状态和回执。
 */
bool SendCommand(const char* command) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::size_t length = std::strlen(kSocketPath);
  if (length >= sizeof addr.sun_path) return false;
  std::memcpy(addr.sun_path, kSocketPath, length);

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  const std::size_t bytes = std::strlen(command);
  const bool ok =
      ::sendto(fd, command, bytes, 0, reinterpret_cast<const sockaddr*>(&addr),
               static_cast<socklen_t>(sizeof addr)) == static_cast<ssize_t>(bytes);
  ::close(fd);
  return ok;
}

TEST_F(ControlSocketTest, InitCreatesWorldWritableSocketAndCloseKeepsDirectory) {
  struct stat info{};
  ASSERT_EQ(::stat(kSocketPath, &info), 0);
  EXPECT_TRUE(S_ISSOCK(info.st_mode));
  EXPECT_EQ(info.st_mode & 0777, 0666);

  Close();
  EXPECT_EQ(::stat(kSocketPath, &info), -1);
  EXPECT_EQ(errno, ENOENT);
  EXPECT_TRUE(fs::is_directory(kDirectory));
}

TEST_F(ControlSocketTest, UnknownCommandIsIgnoredWithoutExit) {
  ASSERT_TRUE(SendCommand("bogus\n"));
  PollOnce(200);
  EXPECT_FALSE(ShutdownRequested());
  EXPECT_EQ(ShutdownExitCode(), 0);
}

TEST_F(ControlSocketTest, IdlePollDoesNotRequestExit) {
  PollOnce(0);
  EXPECT_FALSE(ShutdownRequested());
  EXPECT_EQ(ShutdownExitCode(), 0);
}

TEST_F(ControlSocketTest, AbortRequestsAbnormalExit) {
  ASSERT_TRUE(SendCommand("  ABORT \r\n"));
  PollOnce(200);
  EXPECT_TRUE(ShutdownRequested());
  EXPECT_EQ(ShutdownExitCode(), -1);
}

}  // namespace
