/**
 * @file daemon/worker.h
 * @brief 降权 worker 的进程入口
 * @details worker 是数据面进程：认领 Manager 交接来的三个 FunctionFS fd，
 *          连上手柄的 BT 链路，把 BT ↔ USB 桥起来，并挂上 PipeWire 音频下行。
 *          它以桌面用户身份运行，全程不需要 root。
 * @details 不负责：configfs / mount / 描述符 / bind（都在 Manager 与 usb 模块）、
 *          fd 的挪位与降权本身（handoff + privdrop，发生在 execv 之前）。
 * @note 层次：daemon。进程内多线程（bt 传输线程、usb 三条阻塞线程、libpipewire
 *       的数据线程），本头文件只暴露单入口。
 */

#pragma once

#include "daemon/options.h"

namespace ds5::daemon {

/**
 * @brief 跑一个 worker 进程，直到退出条件成立。
 * @details 交接来的 fd 已经由 Manager 写好描述符并 bind 到 UDC，这里只认领它们，
 *          全程不碰 configfs / mount。
 * @param options worker 侧配置：三个 ep*_fd 必须是本进程里真实可用的 fd 号
 *                （正常只有 Manager 产出的 argv 满足）；bt_addr 恒为必填，
 *                worker 只有桥接这一种模式。
 * @return 0 正常退出（信号或链路断开）；1 认领失败、缺 bt_addr 等无法开始的配置。
 * @note 副作用：mount 之外的全部系统资源（L2CAP socket、PipeWire、线程），
 *       返回前自行 Stop 收尾。
 * @see RunManager —— 本函数的对端。
 */
int RunWorker(const Options& options);

}  // namespace ds5::daemon
