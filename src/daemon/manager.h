/**
 * @file daemon/manager.h
 * @brief 常驻 root 监督进程入口
 * @details Manager 是全链路唯一持 root 的角色：读两份配置、从 logind 图形会话
 *          选出降权目标、建 gadget、mount FunctionFS、写描述符、bind 到 UDC，
 *          然后把数据面交给降权 worker。它自己不碰任何 HID/音频数据。
 * @details 不负责：配置文件与身份策略的实现（config / identity 模块）、数据面
 *          （worker 进程内的 bt/bridge/audio/usb）。
 * @note 层次：daemon。需以 root 运行（RunManager 内部校验 euid）。
 */

#pragma once

#include "daemon/options.h"

namespace ds5::daemon {

/**
 * @brief 起一站虚拟 USB：读配置选身份 → 建实例 → fork 降权 worker → 等它退出
 *        → 整站拆掉 → 退避后重来。
 * @param options 命令行解析出的内部参数。Manager 只把它们当作 worker 配置的
 *                骨架：bt_addr / bt_wait_s / no_audio / verbosity 全部被配置文件
 *                里的生效值覆盖，ffs 路径与身份来自系统配置。
 * @return 进程退出码：0 表示按 Ctrl-C 正常收摊，或当前登录用户没有启用本服务
 *         （配置 disabled / 不在白名单）；1 表示启动条件不成立（非 root、配置
 *         错误、无唯一目标用户、ffs 路径非法、无 dummy UDC）。
 * @note 阻塞直到收到退出信号；worker 崩溃不会让它返回，只会退避重试。
 * @see RunWorker —— 被 fork+execv 起来的对端。
 */
int RunManager(const Options& options);

}  // namespace ds5::daemon
