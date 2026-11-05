/**
 * @file daemon/handoff.cpp
 * @brief fd 交接协议的实现
 * @details 全部逻辑是"把四条通道挪到固定号，同时保证子进程在 execv 之前不
 *          分配内存"。为此每个判断都写成一个只返回字符串字面量的核心函数
 *          （PlanCore/ApplyCore），对外再包一层 std::string 重载补 errno 上下文。
 *          配置快照的 memfd 读写不在 fork 与 execv 之间，因此可以用分配版。
 * @details 不负责：gadget 的建立与描述符（usb 模块）、身份选择的策略
 *          （privdrop 模块）、快照格式（options 模块）。
 * @note 层次：daemon。PrepareAndExec 之后的调用路径运行在 fork 出的子进程里，
 *       只允许 syscall 与字面量，禁止 new/std::string/loguru。
 */

#include "daemon/handoff.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "daemon/privdrop.h"

namespace ds5::daemon {
namespace {

/// @brief 四条通道的目标 fd 号，下标顺序与 HandoffPlan 的字段顺序一致
///        （0=ep0、1=interrupt IN、2=interrupt OUT、3=配置快照）。
const int kTargets[4] = {kEp0Target, kEpInTarget, kEpOutTarget, kConfigFdTarget};

/// @brief 快照的防御性长度上限：足够容纳任何合理的插件配置，又不让一个坏
///        memfd 把 worker 的启动路径变成一次超大分配。
constexpr off_t kMaxConfigBytes = 1 << 20;

/**
 * @brief 交接计划的合法性判定核心，同时也是无分配版 PlanOk 的实现。
 * @details 只看编号，不碰 fd 表，因此父进程和子进程都能安全调用。
 * @param p 源 fd 快照，只读。
 * @param why 失败时指向本函数内的字符串字面量（生命周期为整个程序），
 *            成功时不写。
 * @return 合法 true；false 时 why 非空。
 * @note 纯函数，无副作用，任意线程可调。
 * @see ApplyCore —— 第一步就是调用本函数。
 */
// 失败原因一律是字符串字面量：子进程在 execv 之前不能分配内存，errno 只能
// 由父进程按退出码再解释。
bool PlanCore(const HandoffPlan& p, const char** why) {
  const int src[4] = {p.ep0_fd, p.ep_in_fd, p.ep_out_fd, p.config_fd};

  for (int i = 0; i < 4; ++i) {
    if (src[i] < 0) {
      *why = "handoff fd is unset";
      return false;
    }
    // 0/1/2 是 stdio：占住它们会把 worker 的输入输出搅成一团，而 Manager
    // 正常不可能把端点或配置快照开在这三个号上。
    if (src[i] <= 2) {
      *why = "handoff fd collides with stdio";
      return false;
    }
  }

  // 四条通道是四个独立的文件描述，共用一个号就没有"谁是谁"了；挪位前必须
  // 两两不同。
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      if (src[i] == src[j]) {
        *why = "handoff fds cannot share one number";
        return false;
      }
    }
  }
  return true;
}

/**
 * @brief 这个源 fd 是不是"别的"通道的目标号。
 * @param fd 待判定的源 fd 号。
 * @param self_index 该源所属通道的下标，用于排除自己的目标号（源==自己的目标
 *                   是恒等，本来就不用挪，不算冲突）。
 * @return true 表示挪位过程中它会被别人覆盖，需要先逃位。
 * @note 纯谓词，无副作用。
 * @see ApplyCore 的逃位分支。
 */
// 是不是"别的"通道的目标号。源等于自己的目标号是恒等，不用挪。
bool is_foreign_target(int fd, int self_index) {
  for (int j = 0; j < 4; ++j) {
    if (j != self_index && fd == kTargets[j]) return true;
  }
  return false;
}

/**
 * @brief 挪位 + 保活的核心实现，也是无分配版 ApplyPlan。
 * @details 三步：逃位（把占住别人目标号的源 dup 到 7 以上）、挪位（按通道顺序
 *          dup2 到 3/4/5/6）、保活（清掉目标上的 FD_CLOEXEC）。
 * @param p 源 fd 快照，只读。
 * @param why 失败时指向字符串字面量。
 * @return 全部成功 true；中途失败 false，已完成的步骤不回滚——调用方要么在
 *         子进程里 _exit，要么由父进程整站重建。
 * @warning 改写当前进程的 fd 表，只在 fork 出的子进程里调用。
 * @note 失败时 errno 保留的是最后一个失败 syscall 的码，中间没有别的调用，
 *       std::string 重载据此补上下文。
 * @see PlanCore, CloseAbove。
 */
bool ApplyCore(const HandoffPlan& p, const char** why) {
  if (!PlanCore(p, why)) return false;

  int src[4] = {p.ep0_fd, p.ep_in_fd, p.ep_out_fd, p.config_fd};

  // 源占住了别的通道的目标号：按 3→4→5→6 顺序 dup2 走到那一步时那个号已经
  // 被覆盖，两条通道会指向同一份数据。先把它 F_DUPFD 逃到 7 以上（挑最小空闲
  // 号，一定不与在用的源撞号），后面统一挪位。
  for (int i = 0; i < 4; ++i) {
    if (!is_foreign_target(src[i], i)) continue;
    const int moved = ::fcntl(src[i], F_DUPFD, kConfigFdTarget + 1);
    if (moved < 0) {
      *why = "fcntl(F_DUPFD) failed";
      return false;
    }
    src[i] = moved;
  }

  // 逐条通道挪到自己的目标号；源已经在目标号上就是恒等，跳过。
  for (int i = 0; i < 4; ++i) {
    if (src[i] == kTargets[i]) continue;
    if (::dup2(src[i], kTargets[i]) < 0) {
      *why = "dup2 failed";
      return false;
    }
  }

  // dup2 出来的新 fd 本来就不带 FD_CLOEXEC，这里再确认一遍：源 fd 带着
  // O_CLOEXEC 打开时，交接目标仍然必须活过 execv。
  for (int target : kTargets) {
    const int flags = ::fcntl(target, F_GETFD);
    if (flags < 0) {
      *why = "fcntl(F_GETFD) failed";
      return false;
    }
    if ((flags & FD_CLOEXEC) != 0 &&
        ::fcntl(target, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
      *why = "fcntl(F_SETFD) failed";
      return false;
    }
  }
  return true;
}

}  // namespace

/**
 * @brief 无分配版 PlanOk：直接转发到 PlanCore。
 * @see handoff.h 的组注释说明两个版本的分野。
 */
bool PlanOk(const HandoffPlan& p, const char** why) { return PlanCore(p, why); }

/**
 * @brief 父进程版 PlanOk：把字面量原因搬进 std::string。
 * @param p 源 fd 快照，只读。
 * @param why 失败时写入原因；成功时不写。
 * @return 合法 true。
 * @note 会分配内存，只给父进程用。
 */
bool PlanOk(const HandoffPlan& p, std::string& why) {
  const char* raw = nullptr;
  if (PlanCore(p, &raw)) return true;
  why = raw;
  return false;
}

/**
 * @brief 无分配版 ApplyPlan：转发到 ApplyCore（子进程路径）。
 * @param p 源 fd 快照，只读。
 * @param why 失败时指向字符串字面量。
 * @return 全部成功 true。
 * @warning 有副作用（改写 fd 表），只应在 fork 出的子进程里调用。
 */
bool ApplyPlan(const HandoffPlan& p, const char** why) {
  return ApplyCore(p, why);  // 纯转发，无额外行为（子进程路径）。
}

/**
 * @brief 父进程版 ApplyPlan：转发到 ApplyCore，失败时补 errno 上下文。
 * @param p 源 fd 快照，只读。
 * @param why 失败时写入 "原因: strerror (errno=n)"。
 * @return 全部成功 true。
 * @warning 有副作用（改写 fd 表）且会分配内存，只给父进程用。
 */
bool ApplyPlan(const HandoffPlan& p, std::string& why) {
  const char* raw = nullptr;
  if (ApplyCore(p, &raw)) return true;
  // core 里最后那个失败调用就是 errno 的来源，中间没有别的 syscall。
  const int err = errno;
  why = raw;
  if (err != 0) {
    why += ": " + std::string(std::strerror(err)) +
           " (errno=" + std::to_string(err) + ")";
  }
  return false;
}

/**
 * @brief 无分配版 CloseAbove：一次 close_range 关掉 keep_up_to 之上的全部 fd。
 * @param keep_up_to 保留的最大 fd 号；范围 [0, INT_MAX]。
 * @param why 失败时指向字符串字面量。
 * @return 成功 true；close_range 失败 false。
 * @warning 不可逆：被关掉的 fd 在本进程里再也回不来。
 * @note 上界用 ~0U（unsigned 全 1）：close_range 允许上界超过实际 fd 表上限，
 *       超出部分直接忽略，所以不必先查 RLIMIT_NOFILE。
 */
bool CloseAbove(int keep_up_to, const char** why) {
  if (::close_range(static_cast<unsigned>(keep_up_to) + 1u, ~0U, 0) != 0) {
    *why = "close_range failed";
    return false;
  }
  return true;
}

/**
 * @brief 父进程版 CloseAbove：转发并补 errno。
 * @param keep_up_to 保留的最大 fd 号。
 * @param why 失败时写入 "原因: strerror (errno=n)"。
 * @return 成功 true。
 * @note 会分配内存，只给父进程用。
 */
bool CloseAbove(int keep_up_to, std::string& why) {
  const char* raw = nullptr;
  if (CloseAbove(keep_up_to, &raw)) return true;
  const int err = errno;
  why = std::string(raw) + ": " + std::strerror(err) +
        " (errno=" + std::to_string(err) + ")";
  return false;
}

/**
 * @brief 写配置快照的 memfd；内容写完即封存。
 * @param blob 已编码的快照。
 * @param why 失败时写入原因。
 * @return fd；失败 -1。
 * @details 四个 seal 一起加：内容不可增删改，交接之后 worker 只能读。MFD_ALLOW_SEALING
 *          是加 seal 的前提，没有它 F_ADD_SEALS 一律 EPERM。fd 的 CLOEXEC 由
 *          ApplyPlan 在子进程里显式清掉；这里带 MFD_CLOEXEC 是为了"建好但交接失败"
 *          的路径上不把 fd 泄漏给随后的 exec。
 */
int MakeConfigFd(const std::vector<uint8_t>& blob, std::string& why) {
  const int fd =
      ::memfd_create("ds5dummy-worker-config", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    why = std::string("memfd_create: ") + std::strerror(errno);
    return -1;
  }

  size_t written = 0;
  while (written < blob.size()) {
    const ssize_t n =
        ::write(fd, blob.data() + written, blob.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      why = std::string("write config snapshot: ") + std::strerror(errno);
      ::close(fd);
      return -1;
    }
    if (n == 0) {
      why = "write config snapshot: short write";
      ::close(fd);
      return -1;
    }
    written += static_cast<size_t>(n);
  }

  if (::fcntl(fd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
    why = std::string("seal config snapshot: ") + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

/**
 * @brief 读回配置快照；长度先过防御上限，再逐段 pread 读满。
 * @param fd 快照 fd。
 * @param why 失败时写入原因。
 * @return 快照内容；失败返回空 vector（合法快照至少有一个头，不可能为空）。
 * @details 用 pread 而不是 read：不依赖文件偏移，Manager 侧写完不必再 lseek，
 *          也不会有"谁把偏移读走了"的隐式状态。
 */
std::vector<uint8_t> ReadConfigFd(int fd, std::string& why) {
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    why = std::string("fstat config snapshot: ") + std::strerror(errno);
    return {};
  }
  if (!S_ISREG(info.st_mode) || info.st_size <= 0) {
    why = "config snapshot fd is not a non-empty regular file";
    return {};
  }
  if (info.st_size > kMaxConfigBytes) {
    why = "config snapshot is larger than 1 MiB";
    return {};
  }

  std::vector<uint8_t> blob(static_cast<size_t>(info.st_size));
  size_t offset = 0;
  while (offset < blob.size()) {
    const ssize_t n = ::pread(fd, blob.data() + offset, blob.size() - offset,
                              static_cast<off_t>(offset));
    if (n < 0) {
      if (errno == EINTR) continue;
      why = std::string("read config snapshot: ") + std::strerror(errno);
      return {};
    }
    if (n == 0) {
      why = "read config snapshot: unexpected end of file";
      return {};
    }
    offset += static_cast<size_t>(n);
  }
  return blob;
}

/**
 * @brief 子进程交接收尾：四步失败即 _exit，成功则被 execv 的新镜像接管。
 * @details 步骤顺序即依赖顺序：fd 先就位，再清场，最后降权（降权后无法再动
 *          fd 之外的事）；execv 放在最末尾，之后本进程身份已是目标 uid/gid。
 * @param p 源 fd 快照（Manager fork 时的编号），只读。
 * @param uid worker 目标 uid，须非 0。
 * @param gid worker 目标 gid，须非 0。
 * @param argv nullptr 结尾的参数数组，须 fork 前构造好。
 * @param self_path 本可执行文件路径，非空。
 * @return 不返回。
 * @error 每步失败以 kExecExitBase + 对应 ExecStage 值 _exit；errno 在此丢弃，
 *        由父进程按退出码反查阶段（ExecStageName）。
 * @warning 禁止在本调用链里加内存分配或 loguru 调用：fork 后 malloc 锁状态
 *          不可信。
 * @see ExecStageName, ManagerSupervisor::StartWorker。
 */
void PrepareAndExec(const HandoffPlan& p, uid_t uid, gid_t gid,
                    const char* const argv[], const char* self_path) {
  const char* why = nullptr;

  if (!PlanCore(p, &why))
    ::_exit(kExecExitBase + static_cast<int>(ExecStage::kBadPlan));
  if (!ApplyCore(p, &why))
    ::_exit(kExecExitBase + static_cast<int>(ExecStage::kMove));
  if (!CloseAbove(kConfigFdTarget, &why))
    ::_exit(kExecExitBase + static_cast<int>(ExecStage::kCloseRange));
  if (!DropTo(uid, gid, &why))
    ::_exit(kExecExitBase + static_cast<int>(ExecStage::kDropPrivs));

  // execv 成功就不返回；返回只可能是失败，fallthrough 到 _exit 报告阶段 5。
  ::execv(self_path, const_cast<char* const*>(argv));
  ::_exit(kExecExitBase + static_cast<int>(ExecStage::kExec));
}

/**
 * @brief 退出码 → 失败阶段的可读名。
 * @param exit_code 子进程的 WEXITSTATUS 值。
 * @return 阶段描述串；落在枚举范围外时返回 "exit <n>"，告诉调用方这次退出
 *         与交接无关（worker 已经跑起来了）。
 * @note 纯函数，无副作用。
 * @see PrepareAndExec, kExecExitBase。
 */
std::string ExecStageName(int exit_code) {
  switch (exit_code - kExecExitBase) {
    case static_cast<int>(ExecStage::kBadPlan):
      return "bad fd plan";
    case static_cast<int>(ExecStage::kMove):
      return "dup2/fcntl failed";
    case static_cast<int>(ExecStage::kCloseRange):
      return "close_range failed";
    case static_cast<int>(ExecStage::kDropPrivs):
      return "setgroups/setgid/setuid failed";
    case static_cast<int>(ExecStage::kExec):
      return "execv failed";
    default:
      return "exit " + std::to_string(exit_code);
  }
}

}  // namespace ds5::daemon
