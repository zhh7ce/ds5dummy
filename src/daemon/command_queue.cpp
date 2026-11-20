/**
 * @file daemon/command_queue.cpp
 * @brief 队列本体：一把短锁护环、一张只属于主循环的 pid 表，与 fork 出的 shell
 *
 * 状态全是文件作用域变量：一个 worker 进程只服务一只手柄，队列没有第二个实例，
 * 所以不需要类（与 bt::plane / usb::plane 同一个理由）。
 *
 * 锁只有一把，只护环：Post 在 bt 读线程上进来，Drain 在 worker 主循环上进来，
 * 两边都只在 memcpy 期间持锁。pid 表只由 Drain 读写，不需要锁；home 在 Start
 * 之后只读。
 *
 * fork 之后子进程只调异步信号安全的函数：那一刻别的线程可能正持有任意一把锁
 * （fork 只复制调用线程），分配与日志都可能在子进程里死锁。
 */

#include "daemon/command_queue.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>

#include "core/debug.h"
#include "loguru.hpp"
#include "plugin/plugin_api.h"

namespace ds5::daemon::command_queue {
namespace {

/// 命令的工作目录；Start 之后只读，fork 出的子进程也直接读它。
std::string home;

std::mutex ring_mutex;  ///< 只护环，不护 pid 表。

/// 定长条目环：每条一条命令行，宽度与插件侧的拒收上限是同一个常量。
std::array<std::array<char, kPluginCommandSize>, kCapacity> ring{};
std::size_t ring_head = 0;  ///< 下一个读出。
std::size_t ring_tail = 0;  ///< 下一个写入。
std::size_t ring_count = 0;

/// 已启动、尚未回收的子进程；只在 worker 主循环上读写，0 = 空槽。
std::array<pid_t, kLiveMax> pids{};

/**
 * @brief 回收已经退出的子进程，腾出 pid 槽。
 * @details 只等自己启动的那几个 pid，绝不 waitpid(-1)：L2CAP 后端的 RunGdbus
 *          自己 fork 一个 gdbus 并在 waitpid(pid) 上阻塞等它，抢着回收会让它
 *          拿到 ECHILD。ECHILD 在这里表示"别处已经收走"，同样把槽清掉。
 * @note 返回 0 表示还在跑、-1/EINTR 表示这一轮收不到，两种情况都留着槽，下一轮
 *       Drain 再看。
 */
void Reap() {
  for (pid_t& pid : pids) {
    if (pid == 0) continue;
    const pid_t result = ::waitpid(pid, nullptr, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) pid = 0;
  }
}

/**
 * @brief fork 一条命令，把 pid 记进空槽。
 * @param text 已经拷到 Drain 栈上的命令文本，子进程里读同一份。
 * @return 起来了 true；pid 槽满或 fork 失败 false（这一条丢弃，不重试）。
 * @note 子进程不留回执：不等待、不查退出码，它在 systemd 的 cgroup 里随服务一起
 *       被停掉（cgroup 是 fork 之后继承的，setsid 不换它）。
 */
bool Spawn(const char* text) {
  pid_t* slot = nullptr;
  for (pid_t& pid : pids) {
    if (pid == 0) {
      slot = &pid;
      break;
    }
  }
  if (slot == nullptr) {
    DS5DUMMY_LOG(ERROR,
                 "[worker] {} plugin commands are already running; dropping "
                 "the newest one",
                 kLiveMax);
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    DS5DUMMY_LOG(ERROR, "[worker] fork for a plugin command failed: {}",
                 std::strerror(errno));
    return false;
  }
  if (pid == 0) {
    ::setsid();
    // 工作目录退化（家目录不可达）不拦这条命令，留在继承来的目录里就行。
    (void)::chdir(home.c_str());
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      if (devnull != STDIN_FILENO) ::close(devnull);
    }
    ::execl("/bin/sh", "sh", "-c", text, static_cast<char*>(nullptr));
    ::_exit(127);
  }

  *slot = pid;
  return true;
}

}  // namespace

void Start(std::string home_dir) { home = std::move(home_dir); }

bool Post(const char* text) {
  if (text == nullptr) return false;

  const std::size_t length = std::strlen(text);
  if (length >= kPluginCommandSize) {
    DS5DUMMY_LOG(ERROR,
                 "[worker] plugin command of {} bytes exceeds the {} byte "
                 "limit (kPluginCommandSize - 1); dropped",
                 length, kPluginCommandSize - 1);
    return false;
  }

  bool full = false;
  {
    const std::lock_guard<std::mutex> lock(ring_mutex);
    if (ring_count == kCapacity) {
      full = true;
    } else {
      std::memcpy(ring[ring_tail].data(), text, length + 1);
      ring_tail = (ring_tail + 1) % kCapacity;
      ++ring_count;
    }
  }
  if (full) {
    // 日志在锁外打：丢命令不在常态路径上，这条不该再占着环的锁。
    DS5DUMMY_LOG(ERROR,
                 "[worker] plugin command queue is full ({} entries); dropping "
                 "the newest one",
                 kCapacity);
    return false;
  }
  return true;
}

void Drain() {
  Reap();

  char text[kPluginCommandSize];
  for (;;) {
    {
      const std::lock_guard<std::mutex> lock(ring_mutex);
      if (ring_count == 0) return;
      std::memcpy(text, ring[ring_head].data(), kPluginCommandSize);
      ring_head = (ring_head + 1) % kCapacity;
      --ring_count;
    }
    Spawn(text);
  }
}

#ifdef DS5DUMMY_TESTING
void ResetForTesting() {
  {
    const std::lock_guard<std::mutex> lock(ring_mutex);
    ring_head = 0;
    ring_tail = 0;
    ring_count = 0;
  }
  pids.fill(0);
  home = "/";
}

std::size_t LiveForTesting() {
  std::size_t live = 0;
  for (const pid_t pid : pids) {
    if (pid != 0) ++live;
  }
  return live;
}
#endif

}  // namespace ds5::daemon::command_queue