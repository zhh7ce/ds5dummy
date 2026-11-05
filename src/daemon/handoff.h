/**
 * @file daemon/handoff.h
 * @brief root Manager 与降权 worker 之间的 fd 交接协议
 * @details 本模块负责把 Manager 手里的三条 FunctionFS 端点通道与一份配置快照
 *          安置到固定的 3/4/5/6 号、让它们活过 execv、关掉其余继承来的 fd，
 *          然后把身份切换交给 privdrop 并 execv 自身。交接完成后 worker 只需要
 *          按固定号取用：3/4/5 是端点，6 是配置快照，不必知道 Manager 当年拿到
 *          的是几号。
 * @details 不负责：gadget 的建立与描述符写入（usb 模块）、目标身份的选取
 *          （privdrop 模块）、快照的内容（options 模块）、进程退出码的语义解释
 *          （manager.cpp 的 ReportChildExit）。
 * @note 层次：daemon。线程模型：只在单线程上下文调用；PrepareAndExec 运行在
 *       fork 之后、execv 之前的子进程里，那条路径禁止内存分配与 loguru
 *       调用，因此每个核心判断都提供 const char** 的无分配重载。快照的
 *       memfd 读写发生在 fork 之前与 execv 之后，可以使用分配。
 */

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ds5::daemon {

/// @brief 交接后 worker 进程里 ep0（FunctionFS 控制端点）的固定 fd 号。
/// @details 3/4/5 而不是 0/1/2：stdio 占住前三号，端到端描述符会搅乱 worker 的
///          标准输入输出；从 3 起排也是最小的可用连续段。
inline constexpr int kEp0Target = 3;
/// @brief 交接后 interrupt-IN（设备→主机报告）的固定 fd 号。
inline constexpr int kEpInTarget = 4;
/// @brief 交接后 interrupt-OUT（主机→设备报告）的固定 fd 号。
inline constexpr int kEpOutTarget = 5;
/// @brief 交接后 worker 配置快照（memfd）的固定 fd 号。
/// @details 紧接三个端点之后，worker 的新镜像在初始化日志之前从这里读回整份
///          生效配置。快照本身是加过 seal 的匿名 memfd，内容与 fd 号都由
///          Manager 在 fork 前备好。
inline constexpr int kConfigFdTarget = 6;

/**
 * @brief Manager 手里那份交接 fd 的编号快照（fork 之后、挪位之前）。
 * @details 只是四个 int 的值语义快照，不持有 fd、不关闭 fd：源 fd 由 Manager
 *          的 slot 持有（usb::Provision 把端点所有权交给调用方），交接完成后
 *          父侧副本由 slot 自己关掉；本结构只描述"从哪儿挪到哪儿"。
 * @note 由 Manager 在 fork 前填好；子进程只读。
 */
struct HandoffPlan {
  int ep0_fd = -1;     ///< ep0 源 fd；-1 表示未设置，PlanOk 会拒绝。
  int ep_in_fd = -1;   ///< interrupt IN 源 fd。
  int ep_out_fd = -1;  ///< interrupt OUT 源 fd。
  int config_fd = -1;  ///< 配置快照 memfd 的源 fd。
};

// 下面每个函数都有两个版本：带 std::string& why 的给父进程/日志用，带
// const char** why 的是同一条判断的无分配版本——父进程有 loguru 的日志线程，
// fork 之后子进程不能假定 malloc 的锁是干净状态，所以 execv 之前只能用后者。

/**
 * @brief 检查交接计划本身是否可执行，不触碰任何 fd。
 * @param p 源 fd 快照，只读。
 * @param why 失败原因（字符串字面量写入 std::string）。
 * @return 合法 true；false 时 why 非空。
 * @note 纯判断，无副作用，可调用于任意进程。
 * @see ApplyPlan —— 先跑本检查再真正挪位。
 */
bool PlanOk(const HandoffPlan& p, std::string& why);
bool PlanOk(const HandoffPlan& p, const char** why);  ///< 无分配重载，见组注释。

/**
 * @brief 把四个源 fd 挪到 3/4/5/6，并保证它们活过 execv。
 * @details 挪位前先把"占住了别的通道目标号"的源 fd 用 F_DUPFD 逃到 7 以上，
 *          否则按顺序 dup2 会有一条通道的目标被中途覆盖；挪完再逐个清掉目标
 *          上的 FD_CLOEXEC，因为源 fd 可能带 O_CLOEXEC 打开。
 * @param p 源 fd 快照，只读；成功时 3/4/5/6 已指向对应通道。
 * @param why 失败原因。
 * @return 全部成功 true；任一步 syscall 失败 false，此时 fd 状态只保证
 *         "已完成的步骤不回滚"——调用方随后 _exit 或重建实例。
 * @warning 有副作用（改写进程 fd 表），只应在 fork 出的子进程里调用。
 * @see PlanOk（本函数第一步）、CloseAbove（本函数之后立即调用）。
 */
bool ApplyPlan(const HandoffPlan& p, std::string& why);
bool ApplyPlan(const HandoffPlan& p, const char** why);  ///< 无分配重载。

/**
 * @brief 关掉编号大于 keep_up_to 的全部 fd。
 * @details 3/4/5/6 已由 ApplyPlan 保下来，剩下的都是 Manager 留下的描述符
 *          （configfs 目录fd、FunctionFS 挂载、loguru 的日志文件等），一律不带进
 *          worker：worker 需要日志时以自己的身份重新打开同一个文件。
 * @param keep_up_to 保留的最大 fd 号，通常是 kConfigFdTarget；非负。
 * @param why 失败原因。
 * @return close_range 成功 true。
 * @error close_range 返回非 0（EINVAL/EBADF 等）时 false，why 带 errno。
 */
bool CloseAbove(int keep_up_to, std::string& why);
bool CloseAbove(int keep_up_to, const char** why);  ///< 无分配重载。

/**
 * @brief 把配置快照写进一个带 MFD_CLOEXEC|MFD_ALLOW_SEALING 的匿名 memfd。
 * @param blob 已编码的快照字节。
 * @param why 失败时写入原因。
 * @return 成功返回 fd（调用方持有并负责在 fork 之后关闭自己的副本）；失败 -1。
 * @details 写完加 `F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE|F_SEAL_SEAL`：交接
 *          之后内容不可变，worker 侧只会读。fd 带 CLOEXEC 只是为了在出错的
 *          路径上不泄漏；正常路径由 ApplyPlan 显式清掉它。
 * @note 在 fork 之前调用，允许分配内存。
 * @see ReadConfigFd, kConfigFdTarget。
 */
int MakeConfigFd(const std::vector<uint8_t>& blob, std::string& why);

/**
 * @brief 从交接来的 memfd 读回完整的配置快照。
 * @param fd 快照 fd，正常是 kConfigFdTarget。
 * @param why 失败时写入原因。
 * @return 快照内容；失败返回空 vector。
 * @details 先 fstat 取长度并对上限设防，再用 pread 逐段读满——不依赖文件偏移，
 *          因此 Manager 侧写完后不需要再 lseek。读完由调用方关闭 fd。
 * @note 在 worker 的新镜像里调用，允许分配内存。
 * @see MakeConfigFd。
 */
std::vector<uint8_t> ReadConfigFd(int fd, std::string& why);

/**
 * @brief 子进程的交接收尾：挪 fd → 关遗留 fd → 降权 → execv 自身。
 * @details 这是 fork 与 execv 之间唯一的入口，串起上面三个无分配重载和
 *          privdrop 的 DropTo。
 * @param p 源 fd 快照，只读。
 * @param uid worker 的目标 uid，须已由身份选择（identity 模块）判定非 0。
 * @param gid worker 的目标 gid。
 * @param argv execv 的参数数组，须以 nullptr 结尾且比本次调用活得久；
 *             必须在 fork 之前构造好。
 * @param self_path 可执行文件路径（/proc/self/exe 的读回值），非空。
 * @return 不返回。成功路径被新镜像接管；任何一步失败以
 *         kExecExitBase + ExecStage 的值 _exit。
 * @warning 只在 fork 出的子进程里调用；本函数及其调用链禁止内存分配与
 *          loguru 调用（fork 后 malloc 锁状态不可信）。
 * @see ExecStageName —— 父进程用退出码反查失败阶段。
 */
[[noreturn]] void PrepareAndExec(const HandoffPlan& p, uid_t uid, gid_t gid,
                                 const char* const argv[],
                                 const char* self_path);

/**
 * @brief PrepareAndExec 在 execv 之前的失败阶段，用于编码进退出码低位。
 * @details 值本身就是要 _exit 的偏移量：kExecExitBase + 阶段号。父进程据此
 *          区分"worker 还没跑起来"和"worker 跑过又退出了"。
 */
enum class ExecStage : int {
  kBadPlan = 1,     ///< 计划本身不合法：源 fd 未设置、撞 stdio 或彼此同号。
  kMove = 2,        ///< dup2/F_DUPFD/F_SETFD 失败，fd 没挪到位。
  kCloseRange = 3,  ///< close_range 失败，遗留 fd 没清干净。
  kDropPrivs = 4,   ///< setgroups/setgid/setuid 失败，身份没降下来。
  kExec = 5,        ///< execv 返回了：镜像切换失败（路径不可执行、argv 非法）。
};

/// @brief 交接失败退出码的基数。120 起步避开 worker 自身的 0/1/2，同时留在
///        shell 惯例的 126/127（不可执行/找不到命令）之外，不会混淆两类失败。
inline constexpr int kExecExitBase = 120;

/**
 * @brief 把子进程退出码翻译成人话。
 * @param exit_code waitpid 报告出来的 WEXITSTATUS 值。
 * @return 阶段描述；不在 kExecExitBase+ExecStage 范围内时返回 "exit <n>"，
 *         调用方据此判断这根本不是交接阶段的退出。
 * @note 纯函数，无副作用。
 * @see PrepareAndExec。
 */
std::string ExecStageName(int exit_code);

}  // namespace ds5::daemon
