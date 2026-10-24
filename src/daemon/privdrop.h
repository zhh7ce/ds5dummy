/**
 * @file daemon/privdrop.h
 * @brief 降权：整条链路上唯一的身份切换点
 * @details Manager 必须持 root 才能建 gadget，而数据面（FunctionFS 读写、
 *          PipeWire、Bluetooth）不需要任何特权，因此 fork 出的 worker 在 execv
 *          之前把身份换成目标用户。目标身份由 identity 模块从 logind 图形会话
 *          选出，本模块只回答"怎么切过去"（DropTo），不再有第二套身份来源。
 * @details 不负责：目标身份的选取（identity 模块）、fd 交接（handoff 模块）、
 *          gadget 的建立（usb 模块）。
 * @note 层次：daemon。DropTo 的无分配重载运行在 fork 后、execv 前的子进程里，
 *       禁止内存分配与 loguru 调用。
 */

#pragma once

#include <sys/types.h>

#include <string>

namespace ds5::daemon {

/**
 * @brief 把当前进程切到目标 uid/gid，并清掉所有补充组。
 * @details setgroups → setgid → setuid，顺序不可调换：先清补充组，否则
 *          wheel/input 这类组会原样带进子进程，等于降权没降干净；setgid 必须
 *          先于 setuid，一旦丢掉 root 身份就再也改不了组。
 * @param uid 目标 uid，非 0（降成 0 等于没降，由调用方负责拒绝）。
 * @param gid 目标 gid，非 0。
 * @param why 失败原因；std::string 版额外附带 errno 文本与 uid/gid 数值。
 * @return 成功 true。失败 false，进程身份停在半途状态，调用方应立刻终止
 *         （子进程路径直接 _exit）。
 * @warning 成功后本进程无法再回到 root，切换本身不可逆。
 * @warning 最后会用 setuid(0) 再做一次反向验证：它必须失败。若竟然成功，
 *          说明前面没降干净，同样报 false。
 * @see PrepareAndExec（子进程路径）、kExecExitBase + ExecStage::kDropPrivs。
 */
bool DropTo(uid_t uid, gid_t gid, std::string& why);
bool DropTo(uid_t uid, gid_t gid, const char** why);  ///< 无分配重载，供 execv 前的子进程用。

}  // namespace ds5::daemon
