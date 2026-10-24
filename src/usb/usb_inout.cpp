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
 * 等待条件把"链路 up"和"有新值"合取：interface 未启用时发布只覆盖槽位、
 * 不写端点，避免把报告写进没有消费者的管道。醒来后先在锁内整槽拷贝并
 * 清 pending，再解锁去做最长可达毫秒级的阻塞写——写期间新发布照常进槽，
 * 下一轮被消费，锁不跨越阻塞调用。
 */
void InWriterLoop() {
  plane::DataPlane& state = plane::Get();
  std::array<uint8_t, core::protocol::kUsbInputReportSize> report{};

  std::unique_lock<std::mutex> lock(state.input_mutex);
  while (state.running) {
    // 谓词含 !running：StopThreads 的 notify_all 也必须能叫醒睡死的循环。
    state.input_cv.wait(lock, [&state] {
      return !state.running || (state.link_up && state.pending_ready);
    });
    if (!state.running) break;
    report = state.latest_input;
    state.pending_ready = false;
    lock.unlock();

    // 短写/中断只丢这一帧，不退出：下一份发布永远是最新状态。
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
