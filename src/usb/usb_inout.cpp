/**
 * @file usb_inout.cpp
 * @brief IN 写线程与 OUT 读线程的循环本体
 *
 * 两条循环都睡在自己的阻塞系统调用里（FunctionFS 端点没有 .poll），
 * 只能由 StopThreads 的 SIGUSR1 以 EINTR 打断；结束条件与唤醒合取见
 * 各循环注释。共享状态经 `usb::plane::Get()` 现取，谁都不缓存跨轮引用。
 *
 * @see usb_plane.h —— 状态与并发契约；usb.cpp —— 起停装配。
 */

#include "usb/usb_plane.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

#include "core/debug.h"
#include "core/device/descriptors.h"
#include "loguru.hpp"

namespace usb {
namespace {

/**
 * @brief IN 写线程主循环：全进程唯一允许阻塞式 FunctionFS 数据写的地方。
 *
 * interface 未启用时发布只覆盖持有的状态、不碰端点，避免把报告写进没有消费者
 * 的管道，所以两种节奏都以"链路 up"为前提。
 *
 * 重发档（resend_interval 非零）按墙上时钟自己排发送时刻，醒来后把持有的那
 * 一份再阻塞写一次，两次蓝牙样本之间主机收到的是逐字节相同的重复。不能靠
 * "写要等主机取走一份"来定节奏：dummy_hcd 不兑现描述符里中断端点的 bInterval
 * （它的 scan_urb 对 PIPE_INTERRUPT 自己就写着 polls too fast），主机取完一份
 * 立刻重新排队请求，阻塞写几乎不等待，不排时刻表就会把一份状态以几千帧每秒
 * 灌出去。发布也不提前叫醒这一档——真 USB 设备不会因为内部状态变了就多送一帧。
 *
 * real-time 档（resend_interval 为 0）只在 pending_ready 置位时写一份，写期间
 * 新样本照常覆盖持有状态，下一轮再取最新一份。
 *
 * 两档都在锁内拷走报告、解锁后阻塞写，锁不跨越阻塞调用。
 */
void InWriterLoop() {
  plane::DataPlane& state = plane::Get();
  std::array<uint8_t, core::protocol::kUsbInputReportSize> report{};

  std::unique_lock<std::mutex> lock(state.input_mutex);
  // 重发档下一份报告的发送时刻：只在锁内读写，初值是线程起来的这一刻。
  auto next_send = std::chrono::steady_clock::now();
  while (state.running) {
    // real-time 档没有自己的时钟，必须有上次写完之后的新发布；重发档只看链路。
    // 谓词含 !running：StopThreads 的 notify_all 也必须能叫醒睡死的循环。
    const bool realtime = state.resend_interval.count() == 0;
    state.input_cv.wait(lock, [&state, realtime] {
      return !state.running ||
             (state.link_up && (realtime ? state.pending_ready : true));
    });
    if (!state.running) break;

    if (!realtime) {
      // 睡到发送时刻；被 stop 或链路掉下去才提前醒。
      state.input_cv.wait_until(lock, next_send, [&state] {
        return !state.running || !state.link_up;
      });
      if (!state.running) break;
      if (!state.link_up) continue;  // 链路断了：回去等它重新起来。
      const auto due = next_send + state.resend_interval;
      const auto now = std::chrono::steady_clock::now();
      // 落后超过一格（阻塞写、唤醒抖动）就从现在重排，不连发补欠下的帧。
      next_send = due < now ? now + state.resend_interval : due;
    }

    report = state.LatestInput();
    state.pending_ready = false;
    lock.unlock();

    // 写失败（EINTR/ESHUTDOWN/短写）不退出：链路若仍在，下一轮按最新持有的
    // 状态重试；链路已断则由上面的等待谓词让循环睡下。
    if (state.device.WriteInputReport(report.data(), report.size())) {
#if DS5DUMMY_DEBUG
      ++state.input_writes;
#endif
    }
    lock.lock();
  }
}

/**
 * @brief OUT 读线程主循环：阻塞读主机报告并转发给策略层。
 *
 * 缓冲按端点的 wMaxPacketSize 开，比 0x02 报告本身大：主机发一个比排队请求
 * 缓冲大的短包时，udc（dummy_hcd）把那份请求判成 -EOVERFLOW，读侧看到的就不是
 * "报告太长"而是"端点坏了"。按 maxpacket 开缓冲后同样的包只是一次普通短读，
 * 多出来的字节交给策略层，它只取前面的有效 payload。
 *
 * EINTR（StopThreads 打信号）与 ESHUTDOWN（主机撤掉 configuration 时
 * ffs_func_eps_disable() 击杀在途 I/O）都是可继续状态：前者回循环顶看
 * running，后者下一次 read 会重新 parked 直到链路回来。其余错误终止
 * 循环并按"OUT 单独坏掉"上报。
 */
void OutLoop() {
  plane::DataPlane& state = plane::Get();
  std::array<uint8_t, core::device::kUsbEpMaxPacketSize> buffer{};
  while (state.running) {
    const ssize_t n =
        state.device.ReadOutputReport(buffer.data(), buffer.size());
    if (n > 0) {
      if (state.handlers.on_output_report) {
        state.handlers.on_output_report(buffer.data(), static_cast<size_t>(n));
      }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == ESHUTDOWN) continue;
    if (n < 0) {
      DS5DUMMY_LOG(ERROR, "[usb] ep_out read error: {}", strerror(errno));
    }
    break;  // n==0 或非可恢复错误：OUT 通道结束。
  }

  // running 仍为 true 说明不是正常 stop，而是 OUT 端点单独坏掉——必须让
  // 上层有机会统一退出，而不是等 ep0 或 BT 先报错。
  if (state.running && state.handlers.on_output_closed) {
    state.handlers.on_output_closed();
  }
}

}  // namespace
}  // namespace usb

namespace usb::plane {

void RunIn() { InWriterLoop(); }

void RunOut() { OutLoop(); }

}  // namespace usb::plane
