/**
 * @file daemon/identity.h
 * @brief 从 systemd-logind 的图形会话推导 worker 的降权身份
 * @details 启动契约里的 identity.mode 有两种策略：auto 要求恰好一个活跃图形
 *          会话，list 只接受 /etc 配置里显式列出的 uid。本模块把"会话枚举"与
 *          "策略套用"拆开：QueryGraphicalSessions 碰 sd-login 与 NSS，
 *          SelectWorkerIdentity 是纯函数（可在无 root、无 logind 的环境里
 *          单测），LookupHome 只查 passwd 拿家目录。
 * @details 不负责：配置文件的读取与校验（daemon/config），降权 syscall 本身
 *          （daemon/privdrop），以及退出码映射——Manager 把 kRetry 映射为 1、
 *          kDisabled 映射为 0。
 * @note 层次：daemon。线程模型：只在 root Manager 启动阶段、provision 之前
 *       单线程调用；模块本身不持有全局状态，但 sd-login 与 NSS 查询不是
 *       热路径，也不保证可重入，不要并发调用。
 * @see daemon/config.h —— SystemConfig 与 IdentityEntry 的来源。
 * @see daemon/privdrop.h —— 消费 WorkerIdentity 的 DropTo。
 */

#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "daemon/config.h"

namespace ds5::daemon {

/**
 * @brief 一个活跃图形会话，作为降权身份的候选。
 * @details type 只保留 "wayland" 与 "x11"（枚举阶段已过滤）；primary_gid 是
 *          passwd 里的主组，list 模式用它和配置里显式给的 gid 比对。查不到
 *          passwd 记录时 primary_gid 为 0，表示"未知"，不是"gid 0"。
 * @note 纯数据；线上由 QueryGraphicalSessions 填充，测试手工构造。
 */
struct SessionInfo {
  uid_t uid = 0;          ///< 登录用户的 uid；0 是 root，策略层拒绝服务
  gid_t primary_gid = 0;  ///< passwd 主组；0 = getpwuid_r 未查到
  std::string type;       ///< 会话类型，只可能是 "wayland" 或 "x11"
  bool active = false;    ///< 会话是否 active；非 active 的会话在策略层被忽略
};

/**
 * @brief 通过策略筛选后要降权到的一对 uid/gid。
 * @details 两个字段都必须非 0 才会被 SelectWorkerIdentity 产出；降权序列会
 *          清空补充组，所以主组选择对 worker 能访问哪些设备有实际影响。
 * @note 纯数据。
 */
struct WorkerIdentity {
  uid_t uid = 0;  ///< worker 的目标 uid，非 0
  gid_t gid = 0;  ///< worker 的目标主组，非 0
};

/**
 * @brief 身份选择的三态结果，直接对应 Manager 的退出行为。
 * @details kRetry 用于"稍后会自行消失"的状态（尚未登录、候选不唯一），
 *          Manager 以非零码退出，由 systemd 每 2 s 重拉；kDisabled 表示当前
 *          登录用户没有启用本服务，Manager 以 0 退出且不会被重启。
 */
enum class IdentityOutcome {
  kServe,     ///< 有唯一目标，IdentityDecision::identity 有效
  kRetry,     ///< 暂时无解，应以 1 退出等待重拉；why 说明原因
  kDisabled,  ///< 登录用户未启用本服务，应以 0 退出；why 说明原因
};

/**
 * @brief 身份选择的结果与面向用户的说明。
 * @details identity 只在 kServe 时有效（其余情况保持零值）。warnings 在
 *          kServe 时也可能非空——list 模式下配置的 gid 与 passwd 主组不一致
 *          只告警不拒绝，由 Manager 逐条打 WARNING。why 只在 kRetry/kDisabled
 *          时非空，是单行英文原因（运行期日志与配置报错都是英文）。
 * @note 纯数据；warnings 的顺序与发现顺序一致。
 */
struct IdentityDecision {
  IdentityOutcome outcome = IdentityOutcome::kRetry;  ///< 缺省按"等待重试"处理
  WorkerIdentity identity;                            ///< 仅 kServe 有效
  std::vector<std::string> warnings;  ///< 不阻止服务、但需要让用户看到的偏差
  std::string why;                    ///< kRetry/kDisabled 的单行原因
};

/**
 * @brief 枚举当前活跃的图形用户会话。
 * @return 活跃图形会话列表，按 uid 去重并保留枚举顺序里先出现的一个；任何
 *         sd-login 失败都返回空列表，不区分"没有会话"与"查不了"。
 * @details 过滤条件是 class == "user"、type ∈ {x11, wayland} 且
 *          sd_session_is_active() > 0。primary_gid 通过 getpwuid_r 填充，查不到
 *          时为 0 表示"未知"：只有 auto 模式会因此判 kRetry，list 模式不看它，
 *          一律使用配置里显式给出的 gid（仅在主组非 0 且不一致时告警）。
 *          sd_get_sessions 返回的数组与其中每个字符串都由本函数逐个 free()。
 * @note 会调用 sd-login 与 NSS，只读、不写日志；不保证并发安全。空返回是
 *       Manager 的"还没登录"信号，不是错误。
 * @see SelectWorkerIdentity —— 消费者。
 */
std::vector<SessionInfo> QueryGraphicalSessions();

/**
 * @brief 按系统配置的身份策略从会话列表里选出唯一的降权目标。
 * @param config 系统配置快照，只读 identity_mode 与 allowed_users。
 * @param sessions 会话列表，可以含重复 uid、非 active 与非图形会话；本函数
 *                 自己过滤并去重，不信任上层已清洗。
 * @return 三态结果：kServe 带非零 uid/gid；kRetry 用于 0 个候选、候选不唯一、
 *         候选的 uid 或最终选定的 gid 为 0，以及 auto 模式下主组未知；
 *         kDisabled 用于 list 模式下登录用户不在名单内（含名单为空）。
 *         warnings 只在 list 模式、passwd 主组非 0 且与配置 gid 不一致时出现。
 * @details auto：过滤后恰好一个 uid → 用它与其 passwd 主组；0 个 → kRetry
 *          （等待登录）；多个 → kRetry 并列出候选 uid；passwd 主组未知
 *          （primary_gid == 0）→ kRetry。"主组未知即重试"只属于 auto 模式：
 *          list 模式不依赖 passwd 主组，始终使用配置条目里的 gid。list：0 个
 *          候选 → kRetry；名单内 0 个匹配 → kDisabled；恰好一个 → 用条目里显式
 *          配置的 gid，与 passwd 主组（非 0 时）不一致则追加一条 warning；多个
 *          → kRetry 并列出 uid。同一 uid 在 allowed_users 里重复时取第一条。
 *          任何路径都不产出 uid 0 或 gid 0。
 * @note 纯函数：不碰文件系统、NSS、日志与全局状态，任意线程可调。
 * @see QueryGraphicalSessions, LookupHome。
 */
IdentityDecision SelectWorkerIdentity(const SystemConfig& config,
                                      const std::vector<SessionInfo>& sessions);

/**
 * @brief 查目标用户的家目录。
 * @param uid 目标用户 uid；0 是 root，也是合法输入。
 * @param home 成功时写入非空的家目录绝对路径；失败时清空。
 * @param why 失败时写入单行英文原因（用户不存在、查询失败、家目录为空或记录
 *            过大）；成功时清空。
 * @return 查到非空家目录 true，否则 false。
 * @details 用 getpwuid_r；缓冲区从 sysconf(_SC_GETPW_R_SIZE_MAX) 的提示值
 *          起步（不支持时 1 KiB），NSS 返回 ERANGE 时加倍重试，上限 1 MiB。
 * @note 只读 NSS，会分配内存；结果来自 passwd，调用方仍需按不可信路径的
 *       方式使用（daemon/config 的加固读取）。
 */
bool LookupHome(uid_t uid, std::string& home, std::string& why);

}  // namespace ds5::daemon
