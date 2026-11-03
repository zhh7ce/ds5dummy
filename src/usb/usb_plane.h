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
 * - `input_mutex` 保护 input_report/link_up/resend_interval/pending_ready，
 *   `input_cv` 是发布与链路启用→唤醒 IN 写线程的条件变量；覆盖发布与读最近
 *   一份都是成员函数，调用方持锁后调用；
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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "core/debug.h"
#include "core/device/descriptors.h"
#include "core/protocol/report.h"
#include "usb/ffs_device.h"
#include "usb/usb.h"

namespace usb::plane {

/** @brief 数据面共用的全部状态；一个 worker 进程只服务一只手柄，单实例。 */
struct DataPlane {
  detail::FfsDevice device;  ///< 三个端点 fd 的持有者（worker 侧认领得到）。
  /// 本站型号档案：ep0 线程据此应答 GET_DESCRIPTOR(Report) 与 HID class
  /// descriptor。worker 侧由 Adopt 在起线程之前写入，之后只读（相位约定同
  /// handlers）；默认指向普通 DualSense。
  const core::device::DeviceProfile* profile = &core::device::kDualSenseProfile;

  std::atomic<bool> running{false};  ///< 数据面线程的存活标志。
  std::mutex input_mutex;  ///< 保护 input_report 与 link_up。
  std::condition_variable input_cv;  ///< 发布与链路启用→唤醒 IN 写线程。
  /// 持有一份最新输入报告：新发布就地覆盖，IN 线程每轮取它去写端点。
  std::array<uint8_t, core::protocol::kUsbInputReportSize> input_report{};
  bool link_up = false;  ///< 主机已启用 interface（FUNCTIONFS_ENABLE）。
  /// IN 线程的发送节奏，两份重发之间的间隔；StartThreads 起线程前写入，
  /// 之后只读。`count() == 0` 是 real-time 档：只在 PublishInput 之后写一份。
  /// 非零是重发档：链路 up 时按这个间隔自节奏地写持有的那一份——主机并不
  /// 照描述符的 bInterval 轮询（dummy_hcd 不兑现），节奏只能在这里自己保证；
  /// 这个间隔与那个字节是同一个毫秒数，不做换算。
  std::chrono::milliseconds resend_interval{0};
  /// 自上次 IN 写之后是否又有新发布；只在 real-time 档（resend_interval 为 0）
  /// 参与等待谓词，重发档不看它。
  bool pending_ready = false;

  /**
   * @brief 覆盖持有的输入报告；不排队，后一份替换前一份。
   * @param report 整份报告的字节，首字节即 report ID；超过线尺寸的部分截断，
   *               不足线尺寸时尾部清零。
   * @param len    @p report 的字节数，必须大于 0。
   * @note 调用方须持 `input_mutex`；本函数同时置 pending_ready，real-time 档
   *       的 IN 线程据此醒来写这一份。
   */
  void PublishInput(const uint8_t* report, size_t len);

  /**
   * @brief 最近一次发布的报告。
   * @return 只读引用，内容长度恒为 kUsbInputReportSize；从未发布过时是全零。
   * @note 调用方须持 `input_mutex`；IN 循环每轮用它写端点，ep0 的
   *       GET_REPORT(INPUT) 也读它，两处看到的是同一份状态。
   */
  const std::array<uint8_t, core::protocol::kUsbInputReportSize>& LatestInput()
      const;

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
 * @brief IN 写线程本体：等链路 up（重发档）或有新发布（real-time 档），再把
 *        最近一份报告阻塞写 ep_in。
 * @note 重发档按 resend_interval 排墙上时钟的发送时刻，写完一份睡到下一格；
 *       real-time 档写一份就回去等下一次发布。只由 StartThreads 以
 *       std::thread 启动一次。
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
