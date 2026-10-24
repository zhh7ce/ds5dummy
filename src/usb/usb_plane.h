/**
 * @file usb_plane.h
 * @brief 数据面三条线程共享的运行状态与三个循环的入口
 *
 * 只在 `src/usb/` 内部使用：`usb.cpp` 的 StartThreads/StopThreads 用
 * RunIn/RunEp0/RunOut 把三条线程拉起来，循环本体分别在 `usb_inout.cpp`
 * 与 `usb_ep0.cpp`；上层模块只看 `usb.h`。与 `bt::plane` 同构：两个方向
 * 共用的状态收在一个进程级 static 结构里，谁都不拥有谁，由 usb.cpp 建、
 * 由它销。
 *
 * 并发契约：
 * - `input_mutex` 保护 latest_input/pending_ready/link_up，`input_cv` 是
 *   发布→唤醒 IN 写线程的条件变量；
 * - `running` 与 `input_writes`（仅 `DS5DUMMY_DEBUG` 构建）是 atomic；
 * - `handlers` 只在 StartThreads 置 running 之前写、StopThreads join 之后
 *   随下一轮覆盖，读侧无须加锁（相位约定，同 bt 的 feature 缓存）；
 * - `in_thread`/`ep0_thread`/`out_thread` 三个句柄只由主线程的
 *   StartThreads/StopThreads 碰；
 * - `device` 只在 worker 进程里被认领：主线程 Adopt 一次、退出前 Close 一次，
 *   三个循环只经它读写端点。root 侧（Manager）不碰它——每个 gadget 的端点
 *   fd 归 Manager 自己的 slot 持有，见 usb.h 的 Provision。
 *
 * @see usb.h —— 门面；ffs_device.h、gadget_setup.h —— root 侧机制层。
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "core/debug.h"
#include "core/protocol/report.h"
#include "usb/ffs_device.h"
#include "usb/usb.h"

namespace usb::plane {

/** @brief 数据面共用的全部状态；一个 worker 进程只服务一只手柄，单实例。 */
struct DataPlane {
  detail::FfsDevice device;  ///< 三个端点 fd 的持有者（worker 侧认领得到）。

  std::atomic<bool> running{false};  ///< 数据面线程的存活标志。
  std::mutex input_mutex;            ///< 保护 latest_input/pending/link。
  std::condition_variable input_cv;  ///< 发布→唤醒 IN 写线程。
  std::array<uint8_t, core::protocol::kUsbInputReportSize> latest_input{};  ///< 覆盖式最新报告槽。
  bool pending_ready = false;  ///< 槽里有未被消费的新值。
  bool link_up = false;        ///< 主机已启用 interface（FUNCTIONFS_ENABLE）。

  std::thread in_thread;   ///< IN 写线程：唯一的阻塞数据写发生地。
  std::thread ep0_thread;  ///< ep0 事件线程：SETUP 应答与链路事件。
  std::thread out_thread;  ///< OUT 读线程：主机报告转发。

#if DS5DUMMY_DEBUG
  std::atomic<uint64_t> input_writes{0};  ///< IN 成功写计数（发布口径在 usb）。
#endif
  Handlers handlers;  ///< 策略回调，StartThreads 时装入。
};

/** @brief 进程内唯一数据面实例（函数级静态，首次调用时构造）。 */
DataPlane& Get();

/**
 * @brief IN 写线程本体：等链路 up 且槽位有新值，阻塞写 ep_in。
 * @note 只由 StartThreads 以 std::thread 启动一次。
 */
void RunIn();

/**
 * @brief ep0 事件线程本体：阻塞读事件、分发 SETUP 与链路事件。
 * @note 只由 StartThreads 以 std::thread 启动一次。
 */
void RunEp0();

/**
 * @brief OUT 读线程本体：阻塞读主机报告并交给 on_output_report。
 * @note 只由 StartThreads 以 std::thread 启动一次。
 */
void RunOut();

}  // namespace usb::plane
