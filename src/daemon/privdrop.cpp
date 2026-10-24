/**
 * @file daemon/privdrop.cpp
 * @brief 降权实现
 * @details 一次成功的降权由四个 syscall 组成，任何一个失败都必须让调用方
 *          立刻终止进程：半途切换的身份比 root 更难推理。最后一个 setuid(0)
 *          是反向验证，把"悄悄没降成"变成硬失败。
 * @details 不负责：目标身份的选取（identity 模块负责）、fd 交接（handoff
 *          模块）；本模块只看数字，不查用户/组库。
 * @note 层次：daemon。DropCore 之后没有可回滚的余地；无分配版供 fork 后的
 *       子进程使用。
 */

#include "daemon/privdrop.h"

#include <grp.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace ds5::daemon {
namespace {

/**
 * @brief 降权序列核心，也是无分配版 DropTo 的实现。
 * @param uid 目标 uid，非 0。
 * @param gid 目标 gid，非 0。
 * @param why 失败时指向本函数内的字符串字面量（生命周期为整个程序）。
 * @param err 失败时的 errno；反向验证分支写 0，表示"不是 syscall 报错，而是
 *            报错的调用竟然成功了"。
 * @return 全部成功 true；任一步失败 false，此时身份处于半途状态。
 * @warning 不可逆：成功返回后本进程再也拿不回 root。
 * @note 只允许在 fork 出的子进程（execv 之前）或确定要退出的路径上调用。
 * @see DropTo。
 */
bool DropCore(uid_t uid, gid_t gid, const char** why, int* err) {
  // 补充组一律清空（探针里验证过的序列）：只换 uid/gid 会把 wheel、input 这类
  // 组原样带进子进程，等于降权没降干净。
  if (::setgroups(0, nullptr) != 0) {
    *err = errno;
    *why = "setgroups failed";
    return false;
  }
  if (::setgid(gid) != 0) {
    *err = errno;
    *why = "setgid failed";
    return false;
  }
  if (::setuid(uid) != 0) {
    *err = errno;
    *why = "setuid failed";
    return false;
  }
  // setuid(root→普通用户) 会把有效/保存的 uid 一起换掉，之后任何一次
  // 重新拿回 root 的尝试都会失败；再验证一遍纯粹是把"悄悄没降成"变成硬失败。
  if (::setuid(0) == 0) {
    *err = 0;
    *why = "still able to become root after setuid";
    return false;
  }
  return true;
}

}  // namespace

/**
 * @brief 无分配版 DropTo：转发到 DropCore，丢弃 errno 细节。
 * @param uid 目标 uid。
 * @param gid 目标 gid。
 * @param why 失败时指向字符串字面量；成功时不写。
 * @return 成功 true。
 * @note 子进程失败后只能靠退出码定位阶段，errno 无人可读，所以这里不往外传。
 */
bool DropTo(uid_t uid, gid_t gid, const char** why) {
  int err = 0;
  return DropCore(uid, gid, why, &err);
}

/**
 * @brief 父进程版 DropTo：转发到 DropCore，失败时补 errno 与目标身份。
 * @param uid 目标 uid，会写进错误文本便于排查。
 * @param gid 目标 gid。
 * @param why 失败时写入 "原因: strerror (errno=n) [uid=.. gid=..]"。
 * @return 成功 true；false 时 why 非空。
 * @note 会分配内存，只给父进程用。
 * @see DropCore 的 err==0 分支（反向验证失败时不带 errno 文本）。
 */
bool DropTo(uid_t uid, gid_t gid, std::string& why) {
  const char* raw = nullptr;
  int err = 0;
  if (DropCore(uid, gid, &raw, &err)) return true;
  why = raw;
  if (err != 0) {
    why += ": " + std::string(std::strerror(err)) +
           " (errno=" + std::to_string(err) + ")";
  }
  why += " [uid=" + std::to_string(uid) + " gid=" + std::to_string(gid) + "]";
  return false;
}

}  // namespace ds5::daemon
