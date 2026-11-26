/**
 * @file daemon/control_socket.cpp
 * @brief 控制 socket 实现：一发即走的 AF_UNIX 数据报，主循环切片取一条
 *
 * 状态全是文件作用域变量：Manager 进程只有一个监听点，和 command_queue 一样
 * 不需要类。监听 fd 非阻塞；一次 sendto 就是一个完整报文，recvfrom 要么拿到
 * 整条命令、要么什么都没有，所以没有连接状态、没有"等一行"的过程，也不需要对
 * 客户端设读超时——一个不发包的用户对服务端完全不存在。
 *
 * 收到 `abort` 时本模块只请求异常退出，不碰站点、不碰 worker——收摊仍由
 * Manager 的现有循环做，这样信号入口与 socket 入口共用同一条清理路径。
 */

#include "daemon/control_socket.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>

#include "core/debug.h"
#include "daemon/shutdown.h"
#include "loguru.hpp"

namespace ds5::daemon::control_socket {
namespace {

/// @brief 一条命令的最大字节数，含结尾换行；超长报文只记日志、不执行。
constexpr std::size_t kMaxCommandBytes = 64;

int g_fd = -1;            ///< 监听 fd；-1 表示未初始化或已关闭。
bool g_bound = false;     ///< 是否留下了需要本模块删除的 socket 文件。

/**
 * @brief 把路径写进 sockaddr_un，并挡住超长路径。
 * @param addr 输出的地址结构；调用方零初始化后传入。
 * @param why 失败原因。
 * @return 可 bind true。
 */
bool MakeAddress(sockaddr_un& addr, std::string& why) {
  const std::size_t length = std::strlen(kSocketPath);
  if (length >= sizeof addr.sun_path) {
    why = std::string("control socket path is too long: ") + kSocketPath;
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, kSocketPath, length);
  return true;
}

/**
 * @brief 去掉首尾空白并转小写，得到用于精确比较的命令名。
 * @note 只处理 ASCII 空白与字母；控制字符原样留在结果里，不会匹配 `abort`。
 */
std::string Normalize(const std::string& raw) {
  std::size_t begin = 0;
  std::size_t end = raw.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(raw[begin])) != 0) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) {
    --end;
  }
  std::string out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    out.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(raw[i]))));
  }
  return out;
}

}  // namespace

bool Init(std::string& why) {
  if (::mkdir(kDirectory, 0755) != 0) {
    if (errno != EEXIST) {
      why = std::string("mkdir ") + kDirectory + ": " + std::strerror(errno);
      return false;
    }
    struct stat dir_info{};
    if (::stat(kDirectory, &dir_info) != 0 || !S_ISDIR(dir_info.st_mode)) {
      why = std::string(kDirectory) + " exists but is not a directory";
      return false;
    }
  }

  sockaddr_un addr{};
  if (!MakeAddress(addr, why)) return false;

  // 崩溃残留：只有确认没有人在监听的 socket 文件才能删。目录是 root 写的，
  // 普通用户塞不进符号链接，这里仍用 lstat 而不是 stat 以防万一。
  // 数据报 socket 的 connect 同样会在"文件在但无人 bind"时返回 ECONNREFUSED，
  // 所以探测逻辑与流式版本一致（已在本地验证）。
  struct stat old{};
  if (::lstat(kSocketPath, &old) == 0) {
    if (!S_ISSOCK(old.st_mode)) {
      why = std::string(kSocketPath) + " exists and is not a socket";
      return false;
    }
    const int probe = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe < 0) {
      why = std::string("socket for stale check: ") + std::strerror(errno);
      return false;
    }
    const int connected =
        ::connect(probe, reinterpret_cast<const sockaddr*>(&addr),
                  static_cast<socklen_t>(sizeof addr));
    const int connect_error = errno;
    ::close(probe);
    if (connected == 0 || (connected < 0 && connect_error != ECONNREFUSED)) {
      why = std::string(kSocketPath) + " is still in use";
      return false;
    }
    if (::unlink(kSocketPath) != 0) {
      why = std::string("unlink stale ") + kSocketPath + ": " +
            std::strerror(errno);
      return false;
    }
  } else if (errno != ENOENT) {
    why = std::string("lstat ") + kSocketPath + ": " + std::strerror(errno);
    return false;
  }

  g_fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (g_fd < 0) {
    why = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  if (::bind(g_fd, reinterpret_cast<const sockaddr*>(&addr),
             static_cast<socklen_t>(sizeof addr)) != 0) {
    why = std::string("bind ") + kSocketPath + ": " + std::strerror(errno);
    ::close(g_fd);
    g_fd = -1;
    return false;
  }
  if (::chmod(kSocketPath, 0666) != 0) {
    why = std::string("chmod ") + kSocketPath + ": " + std::strerror(errno);
    ::unlink(kSocketPath);
    ::close(g_fd);
    g_fd = -1;
    return false;
  }
  // 数据报没有 listen()：bind 完就能收。
  g_bound = true;
  return true;
}

void PollOnce(int timeout_ms) {
  if (g_fd < 0) return;
  pollfd pfd{};
  pfd.fd = g_fd;
  pfd.events = POLLIN;
  const int ready = ::poll(&pfd, 1, timeout_ms);
  if (ready == 0) return;
  if (ready < 0) {
    if (errno != EINTR) {
      DS5DUMMY_LOG(WARNING, "[control] poll listener: {}", std::strerror(errno));
    }
    return;
  }
  if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
    DS5DUMMY_LOG(WARNING, "[control] listener error event {:#x}",
                 static_cast<unsigned>(pfd.revents));
    return;
  }
  if ((pfd.revents & POLLIN) == 0) return;

  char buffer[kMaxCommandBytes];
  // MSG_TRUNC 让返回值仍是报文原长，方便识别超长命令；多余字节被丢弃。
  const ssize_t got = ::recvfrom(g_fd, buffer, sizeof buffer,
                                 MSG_DONTWAIT | MSG_TRUNC, nullptr, nullptr);
  if (got < 0) {
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      DS5DUMMY_LOG(WARNING, "[control] recvfrom: {}", std::strerror(errno));
    }
    return;
  }
  if (got > static_cast<ssize_t>(sizeof buffer)) {
    DS5DUMMY_LOG(WARNING, "[control] datagram longer than {} bytes; ignored",
                 kMaxCommandBytes);
    return;
  }

  const std::string raw(buffer, static_cast<std::size_t>(got));
  const std::string command = Normalize(raw);
  if (command == "abort") {
    DS5DUMMY_LOG(INFO, "[control] abort received; exiting with -1 for restart");
    RequestAbnormalExit();
  } else {
    DS5DUMMY_LOG(WARNING, "[control] unknown command ({} bytes)", raw.size());
  }
}

void Close() {
  if (g_fd >= 0) {
    ::close(g_fd);
    g_fd = -1;
  }
  if (g_bound) {
    if (::unlink(kSocketPath) != 0 && errno != ENOENT) {
      DS5DUMMY_LOG(WARNING, "[control] unlink {}: {}", kSocketPath,
                   std::strerror(errno));
    }
    g_bound = false;
  }
}

}  // namespace ds5::daemon::control_socket
