/**
 * @file main.cpp
 * @brief DS5Dummy 的唯一入口：一个二进制两个角色
 * @details 默认是常驻 root Manager，--worker 是它 fork+execv 出来的降权子进程。
 *          命令行只剩 `--worker` 与 `--help`；用户级设置来自 YAML 配置，由
 *          Manager 读取后编成一份快照写进 memfd，经 handoff 挪到 worker 的 6 号
 *          fd。worker 的新镜像在初始化日志之前把这份快照解码回 Options。
 * @details 不负责：任何具体流程——配置解析与特权动作在 manager.cpp，数据面在
 *          worker.cpp。日志级别在这里只落一个初始值：Manager 拿到配置后再按
 *          配置覆盖，worker 直接沿用快照里的 verbosity。
 * @note 层次：进程入口，最先执行的一段代码。线程模型：单线程起，之后由被调用
 *       方各起各的。
 */

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "daemon/handoff.h"
#include "daemon/manager.h"
#include "daemon/options.h"
#include "daemon/shutdown.h"
#include "daemon/worker.h"
#include "loguru.hpp"

/**
 * @brief 解析参数、解码交接快照、初始化日志、按 --worker 分派到两个角色之一。
 * @param argc 参数个数。
 * @param argv 参数数组，argv[0] 为程序名。
 * @return 所选角色的退出码；0 正常，1 启动条件不成立，2 命令行或交接快照错误。
 * @note 副作用：安装 SIGINT/SIGTERM handler、初始化 loguru（含其日志线程）。
 * @warning handler 必须在任何阻塞 IO 之前装好，否则 Ctrl-C 只能等 IO 自然结束
 *          才被看到。
 * @see RunManager, RunWorker。
 */
int main(int argc, char* argv[]) {
  // Interleaving the gadget/bridge/audio milestones is the diagnosis; when
  // stdout is a pipe libc buffers it and only the stderr lines survive.
  setvbuf(stdout, nullptr, _IOLBF, 0);

  // 先解析再初始化日志：loguru::init 自己会扫 argv 里的 -v，而它的名字表只认
  // OFF/INFO/WARNING/ERROR/FATAL，遇到不认识的值会 CHECK_F 直接 abort。现在
  // 命令行只剩两个开关，但这条顺序仍然成立——日志级别的唯一来源是配置。
  ds5::daemon::Options o = ds5::daemon::ParseArgs(argc, argv);
  if (o.show_help) {
    ds5::daemon::PrintUsage(argv[0]);
    return 0;
  }
  if (!o.error.empty()) {
    std::fprintf(stderr, "ds5dummy: %s\n", o.error.c_str());
    ds5::daemon::PrintUsage(argv[0]);
    return 2;
  }

  if (o.worker) {
    // 配置不走命令行：Manager 把它编成快照，经 handoff 固定在 6 号 fd 上交下来。
    std::string why;
    const std::vector<uint8_t> blob =
        ds5::daemon::ReadConfigFd(ds5::daemon::kConfigFdTarget, why);
    const bool config_ok =
        !blob.empty() && o.Decode(blob.data(), blob.size(), why);
    if (!config_ok) {
      std::fprintf(stderr, "ds5dummy: cannot read worker config: %s\n",
                   why.c_str());
      return 2;
    }
    ::close(ds5::daemon::kConfigFdTarget);
  }

  // 级别先落 stderr：worker 的 verbosity 来自快照，这一句就是它的最终值；
  // Manager 这里只是初始值，拿到配置后再覆盖（RunManager）。
  loguru::g_stderr_verbosity = static_cast<loguru::Verbosity>(o.verbosity);
  // loguru::init 要求 argv[argc] == nullptr，而真实 argv 后面还有我们的参数，
  // 所以只给它程序名——目的就是不让它碰命令行。
  char* init_argv[2] = {argv[0], nullptr};
  int init_argc = 1;
  loguru::init(init_argc, init_argv);

  ds5::daemon::InstallSignalHandlers();

  return o.worker ? ds5::daemon::RunWorker(o) : ds5::daemon::RunManager(o);
}
