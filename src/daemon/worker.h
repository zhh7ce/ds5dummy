/**
 * @file daemon/worker.h
 * @brief 降权 worker 的进程入口
 * @details worker 是数据面进程：认领 Manager 放在固定 fd 上的三个 FunctionFS
 *          端点，连上手柄的 BT 链路，把 BT ↔ USB 桥起来，并挂上 PipeWire 音频下行。
 *          它以桌面用户身份运行，全程不需要 root。
 * @details 不负责：configfs / mount / 描述符 / bind（都在 Manager 与 usb 模块）、
 *          fd 的挪位、配置快照的读取与降权本身（handoff + privdrop + main，
 *          都发生在 execv 前后、本函数之前）。
 * @note 层次：daemon。进程内多线程（bt 传输线程、usb 三条阻塞线程、libpipewire
 *       的数据线程），本头文件只暴露单入口。
 */

#pragma once

#include "daemon/options.h"

namespace ds5::daemon {

/**
 * @brief 跑一个 worker 进程，直到退出条件成立。
 * @details 三个端点 fd 的编号由交接约定钉死（handoff 的 3/4/5），Manager 在
 *          execv 前已经写好描述符并 bind 到 UDC，这里只认领它们，全程不碰
 *          configfs / mount。
 * @param options worker 侧配置：由 main 从 Manager 下发的配置快照解出，不再经
 *                argv；三个端点 fd 不在这里，固定取交接约定的编号。
 *                bt_addr 恒为必填，worker 只有桥接这一种模式。
 * @return 0 正常退出（信号或链路断开）；1 认领失败、缺 bt_addr 等无法开始的配置。
 * @note 副作用：mount 之外的全部系统资源（L2CAP socket、PipeWire、线程），
 *       返回前自行 Stop 收尾。
 * @see RunManager —— 本函数的对端。
 */
int RunWorker(const Options& options);

}  // namespace ds5::daemon
