/**
 * @file daemon/identity.cpp
 * @brief 图形会话枚举、降权身份选择与家目录查询的实现
 * @details 会话查询与 passwd 查询各自独立：前者走 sd-login，后者只走 NSS。
 *          策略选择不触碰任何系统调用，输入是会话快照加系统配置，输出是三态
 *          决定，因此可以在无 logind、无 root 的环境里完整覆盖。
 * @details 不负责：配置解析（daemon/config）、降权动作（daemon/privdrop）、
 *          退出码映射（manager）。
 * @note 层次：daemon。线程模型：只在 Manager 启动阶段单线程调用；本文件
 *       无文件级状态。
 */

#include "daemon/identity.h"

#include <pwd.h>
#include <systemd/sd-login.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace ds5::daemon {
namespace {

/**
 * @brief 一个 uid 的 passwd 查询结果。
 * @note 纯数据；home 只在 found 为 true 时有意义，NSS 返回空 pw_dir 时也为空。
 */
struct PasswdInfo {
  bool found = false;     ///< 是否在 passwd 里查到该 uid
  gid_t primary_gid = 0;  ///< pw_gid；found 为 false 时保持 0
  std::string home;       ///< pw_dir；可能为空串
};

/**
 * @brief 用 getpwuid_r 读取 uid 的 passwd 记录（主组与家目录）。
 * @param uid 待查 uid，0 是合法输入（root）。
 * @param why 失败时写入单行英文原因；成功时清空。允许传 nullptr，表示调用方
 *            不关心原因（会话枚举只想要 gid）。
 * @return found 表示查到与否；未查到与查询失败都返回 found=false，用 why 区分。
 * @details 缓冲区从 sysconf(_SC_GETPW_R_SIZE_MAX) 的提示值起步（该名字不支持
 *          时 1 KiB；提示值超过 1 MiB 时按 1 MiB 截断），NSS 后端返回 ERANGE
 *          表示缓冲区不够，此时加倍重试，每次增长同样以 1 MiB 封顶；
 *          1 MiB 仍不够说明记录异常，按失败上报而不是无限长大。
 * @note 只读 NSS，每次调用独立分配缓冲区，不依赖静态存储。
 * @see LookupHome。
 */
PasswdInfo LookupPasswd(uid_t uid, std::string* why) {
  if (why != nullptr) why->clear();

  // 1 MiB 远超任何 NSS 后端的单条记录长度，用来给"缓冲区无限长大"兜底。
  // 它同时是硬顶：初始长度与每次 ERANGE 增长都不越过它。
  constexpr std::size_t kMaxPasswdBufBytes = 1u << 20;
  // sysconf 给的是提示值，不是保证值；不支持时（返回 -1）按 1 KiB 起步。
  // 提示值本身也可能大得离谱，先按硬顶截断再分配，保证任何一次分配都 ≤ 1 MiB。
  const long hint = ::sysconf(_SC_GETPW_R_SIZE_MAX);
  const std::size_t seed = hint > 0 ? static_cast<std::size_t>(hint) : 1024;
  std::vector<char> buf(std::min(seed, kMaxPasswdBufBytes));

  for (;;) {
    struct passwd entry{};
    struct passwd* result = nullptr;
    const int rc = ::getpwuid_r(uid, &entry, buf.data(), buf.size(), &result);
    if (rc == 0) {
      if (result == nullptr) {  // 调用成功但没有该 uid，与查询失败不同
        if (why != nullptr) {
          *why = "uid " + std::to_string(uid) + " not found in passwd";
        }
        return PasswdInfo{};
      }
      PasswdInfo info;
      info.found = true;
      info.primary_gid = entry.pw_gid;
      // pw_dir 按 POSIX 可以缺省；空串同样视为没有可用的家目录。
      if (entry.pw_dir != nullptr) info.home = entry.pw_dir;
      return info;
    }
    if (rc != ERANGE) {
      if (why != nullptr) {
        *why = "getpwuid_r failed: " + std::string(std::strerror(rc));
      }
      return PasswdInfo{};
    }
    if (buf.size() >= kMaxPasswdBufBytes) {
      if (why != nullptr) *why = "passwd record is unreasonably large";
      return PasswdInfo{};
    }
    // 加倍重试；硬顶夹住上界，所以最后一次增长落在 (size, kMaxPasswdBufBytes]
    // 区间内（不会像裸 *2 那样冲过文档承诺的 1 MiB）。
    buf.resize(std::min(buf.size() * 2, kMaxPasswdBufBytes));
  }
}

/** out 里是否已有该 uid 的会话；会话数是个位数，线性扫描足够。 */
bool ContainsUid(const std::vector<SessionInfo>& sessions, uid_t uid) {
  for (const SessionInfo& session : sessions) {
    if (session.uid == uid) return true;
  }
  return false;
}

/**
 * @brief 过滤出可降权的会话并按 uid 去重。
 * @param sessions 任意会话列表，允许含非 active、非图形会话与重复 uid。
 * @return 仅含 active 且 type 为 x11/wayland 的会话；同一 uid
 * 保留先出现的一个。
 * @details 同一用户可能同时挂着多个会话（多个 VT、多次登录），而策略层只按
 *          uid 决策，所以先在这里归一，后续比较"候选个数"才有意义。
 * @note 纯函数，不修改输入。
 */
std::vector<SessionInfo> FilterCandidates(
    const std::vector<SessionInfo>& sessions) {
  std::vector<SessionInfo> out;
  out.reserve(sessions.size());
  for (const SessionInfo& session : sessions) {
    // 非 active（例如已切走的 VT）或 ttys 之类非图形会话没有可供降权的桌面。
    if (!session.active) continue;
    if (session.type != "wayland" && session.type != "x11") continue;
    if (ContainsUid(out, session.uid)) continue;
    out.push_back(session);
  }
  return out;
}

/** 把候选 uid 拼成 "1000, 1001" 形式的列表，用于重试原因。 */
std::string JoinUids(const std::vector<SessionInfo>& sessions) {
  std::string text;
  for (const SessionInfo& session : sessions) {
    if (!text.empty()) text += ", ";
    text += std::to_string(session.uid);
  }
  return text;
}

/**
 * @brief 在名单里找 uid 对应的条目。
 * @param users 系统配置里的 allowed_users，可能为空或含重复 uid。
 * @param uid 待匹配的 uid。
 * @return 第一个 uid 匹配的条目；没有匹配时返回 nullptr。
 * @note 纯函数；重复 uid 取第一条是刻意行为，保证选择结果只由配置顺序决定。
 */
const IdentityEntry* FindEntry(const std::vector<IdentityEntry>& users,
                               uid_t uid) {
  for (const IdentityEntry& entry : users) {
    if (entry.uid == uid) return &entry;
  }
  return nullptr;
}

/**
 * @brief 校验选出的 uid/gid 对并填进 kServe 结果。
 * @param uid 候选 uid。
 * @param gid 候选主组。
 * @param decision 结果写入这里；失败时 outcome 保持 kRetry 并写 why。
 * @return 可以服务 true；uid 或 gid 为 0 时 false。
 * @details uid/gid 为 0 在配置解析阶段已被拒绝，这里再挡一次是因为策略函数
 *          是公开的纯函数，测试与将来的调用方都可能绕过解析器直接喂值。
 * @note 纯函数（只写 decision）。
 */
bool AcceptPair(uid_t uid, gid_t gid, IdentityDecision& decision) {
  if (uid == 0) {
    decision.why = "refusing to serve uid 0";
    return false;
  }
  if (gid == 0) {
    decision.why = "gid for uid " + std::to_string(uid) + " is 0";
    return false;
  }
  decision.outcome = IdentityOutcome::kServe;
  decision.identity = WorkerIdentity{uid, gid};
  return true;
}

}  // namespace

std::vector<SessionInfo> QueryGraphicalSessions() {
  std::vector<SessionInfo> out;

  // sd_get_sessions 分配外层的 NULL 结尾数组以及数组里每个字符串，调用方必须
  // 逐个 free() 字符串再 free() 数组（见 sd_get_sessions(3)）；只释放数组会漏
  // 掉全部会话名。失败时返回负数，ret 参数不保证被写，所以指针先置空。
  char** sessions = nullptr;
  const int count = ::sd_get_sessions(&sessions);
  if (count < 0) return out;

  for (int i = 0; i < count; ++i) {
    const char* session = sessions[i];
    if (session == nullptr) continue;

    // 只要 class=user：greeter、display-manager 的会话不是登录用户本身。
    char* clazz = nullptr;
    if (::sd_session_get_class(session, &clazz) < 0) continue;
    const bool is_user = clazz != nullptr && std::strcmp(clazz, "user") == 0;
    std::free(clazz);
    if (!is_user) continue;

    // 只要图形会话；ttys 会话没有桌面环境可交给 worker 使用。
    char* type = nullptr;
    if (::sd_session_get_type(session, &type) < 0) continue;
    const bool graphical =
        type != nullptr &&
        (std::strcmp(type, "wayland") == 0 || std::strcmp(type, "x11") == 0);
    std::string session_type = graphical ? std::string(type) : std::string();
    std::free(type);
    if (!graphical) continue;

    // 非 active 的会话（已切到别的 VT）仍是登录状态，但不是当前桌面。
    if (::sd_session_is_active(session) <= 0) continue;

    uid_t uid = 0;
    if (::sd_session_get_uid(session, &uid) < 0) continue;

    SessionInfo info;
    info.uid = uid;
    info.type = std::move(session_type);
    info.active = true;
    // 主组查不到时留 0（"未知"，不是 gid 0）：auto 模式据此 kRetry，
    // list 模式不看这个值，照配置里的 gid 服务。
    info.primary_gid = LookupPasswd(uid, nullptr).primary_gid;
    out.push_back(std::move(info));
  }

  // 先逐个释放字符串，再释放数组本身：两者是 sd_get_sessions 分别分配的独立
  // 块；反过来先 free() 数组就再也拿不到字符串指针，逐个字符串的堆块会泄漏。
  for (int i = 0; i < count; ++i) std::free(sessions[i]);
  std::free(sessions);

  // sd-login 可能为同一 uid 报出多个图形会话；策略层只按 uid 决策，先归一。
  return FilterCandidates(out);
}

IdentityDecision SelectWorkerIdentity(
    const SystemConfig& config, const std::vector<SessionInfo>& sessions) {
  IdentityDecision decision;  // 缺省 kRetry，所有失败分支直接填 why 返回
  const std::vector<SessionInfo> candidates = FilterCandidates(sessions);

  // 无论哪种模式，"没有活跃图形会话"都意味着用户还没登录：等 systemd 重拉。
  if (candidates.empty()) {
    decision.why = "no active graphical session";
    return decision;
  }

  if (config.identity_mode != IdentityMode::kList) {
    // auto：候选 uid 必须恰好一个；多个说明无法替用户做决定。
    if (candidates.size() > 1) {
      decision.why = "multiple active graphical sessions (uids " +
                     JoinUids(candidates) + ")";
      return decision;
    }
    const SessionInfo& only = candidates.front();
    // auto 用 passwd 主组；gid 0 表示 passwd 查询失败，此时不能降权。
    if (only.primary_gid == 0) {
      decision.why = "primary gid for uid " + std::to_string(only.uid) +
                     " could not be resolved";
      return decision;
    }
    AcceptPair(only.uid, only.primary_gid, decision);
    return decision;
  }

  // list：只保留出现在 allowed_users 里的会话；名单外的登录用户等于没启用。
  std::vector<SessionInfo> matched;
  matched.reserve(candidates.size());
  for (const SessionInfo& session : candidates) {
    if (FindEntry(config.allowed_users, session.uid) != nullptr) {
      matched.push_back(session);
    }
  }

  if (matched.empty()) {
    // 空名单也走这里：配置解析已禁止 list 模式空名单，策略层仍然按
    // "没有匹配用户"处理，不崩溃、不服务。
    decision.outcome = IdentityOutcome::kDisabled;
    decision.why = "no active graphical session belongs to an allowed user";
    return decision;
  }
  if (matched.size() > 1) {
    decision.why =
        "multiple allowed graphical sessions (uids " + JoinUids(matched) + ")";
    return decision;
  }

  const SessionInfo& only = matched.front();
  const IdentityEntry* entry = FindEntry(config.allowed_users, only.uid);
  // 上面刚按同一条件筛过，entry 不可能为空；保留判空是为了不让解引用依赖
  // 两段代码之间的一致性。
  if (entry == nullptr) {
    decision.why = "allowed user entry disappeared during selection";
    return decision;
  }
  // 降权用条目里的 gid：补充组会被清空，主组必须由配置显式决定。
  if (!AcceptPair(entry->uid, entry->gid, decision)) return decision;

  // 配置 gid 与 passwd 主组不一致不阻止服务，但通常意味着配置写错了。
  if (only.primary_gid != 0 && only.primary_gid != entry->gid) {
    decision.warnings.push_back("configured gid " + std::to_string(entry->gid) +
                                " for uid " + std::to_string(entry->uid) +
                                " differs from passwd primary gid " +
                                std::to_string(only.primary_gid));
  }
  return decision;
}

bool LookupHome(uid_t uid, std::string& home, std::string& why) {
  home.clear();
  const PasswdInfo info = LookupPasswd(uid, &why);
  if (!info.found) return false;  // why 已由 LookupPasswd 填好
  if (info.home.empty()) {
    why = "uid " + std::to_string(uid) + " has an empty home directory";
    return false;
  }
  home = info.home;
  why.clear();
  return true;
}

}  // namespace ds5::daemon
