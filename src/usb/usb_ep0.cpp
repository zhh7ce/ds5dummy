/**
 * @file usb_ep0.cpp
 * @brief ep0 事件线程的循环本体与 Ep0Responder 的方法实现
 *
 * ep0 是控制端点：事件流（SETUP/链路启停/挂起恢复）读走之后，标准
 * GET_DESCRIPTOR 属于 USB enumeration 机制、在这里用 core::device 的
 * 描述符常量应答；class 请求原样转交 handlers.on_class_request，怎么答
 * 是转发层策略（bridge）。Ep0Responder 声明在门面 usb.h、方法体在本文件：
 * 它只对"当前正阻塞在 ep0 线程上的那一条 setup"有意义，跨线程调用会踩
 * 别人的 data stage。
 *
 * @see usb_plane.h —— 状态与并发契约；usb.cpp —— 起停装配。
 */

#include "usb/usb_plane.h"

#include <linux/usb/functionfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>

#include "core/debug.h"
#include "core/device/descriptors.h"
#include "loguru.hpp"

namespace usb {
namespace {

/// 吞掉 [[nodiscard]]/warn_unused_result 的 fire-and-forget ep0 应答结果。
template <typename T>
inline void DiscardResult(T) {}

/**
 * @brief 处理一个 ep0 事件：SETUP 分发、链路启停、挂起恢复。
 *
 * 每条 SETUP 先构造一条 Ep0Responder（应答通道只在本次 setup 的生命期内
 * 有效）：class 请求把它连同 setup 交给 handlers.on_class_request——怎么答
 * 是转发层策略；标准 GET_DESCRIPTOR(HID/Report) 属于 enumeration 机制，
 * 留在本层用 core::device 的描述符常量直接应答，其余未知请求 STALL。
 *
 * @return true 继续下一轮 read；false ep0 通道已坏或被关闭，调用方收尾。
 */
bool HandleEp0Event() {
  plane::DataPlane& state = plane::Get();
  struct usb_functionfs_event event;
  const ssize_t n = ::read(state.device.Ep0Fd(), &event, sizeof(event));
  if (n != static_cast<ssize_t>(sizeof(event))) {
    if (n == 0) {
      DS5DUMMY_LOG(ERROR, "[usb] ep0 closed: {}", strerror(errno));
      return false;
    }
    if (n < 0 && errno != EINTR) {
      DS5DUMMY_LOG(ERROR, "[usb] ep0 read error: {}", strerror(errno));
      return false;
    }
    return true;  // EINTR/短读：不是致命通道错误，回来继续等。
  }

  switch (event.type) {
    case FUNCTIONFS_SETUP: {
      const struct usb_ctrlrequest& request = event.u.setup;
      const uint8_t request_type = request.bRequestType & USB_TYPE_MASK;
      const bool in = request.bRequestType & USB_DIR_IN;
      DS5DUMMY_DLOG(1,
                    "[ep0] SETUP type={} req=0x{:02x} wValue=0x{:04x} "
                    "wIndex=0x{:04x} wLength={}",
                    request_type >> 5, request.bRequest, request.wValue,
                    request.wIndex, request.wLength);
      Ep0Responder responder{state.device.Ep0Fd()};
      if (request_type == USB_TYPE_CLASS) {
        // 内容无关，整条 setup 转交策略层。
        if (state.handlers.on_class_request) {
          state.handlers.on_class_request(request, responder);
        }
      } else if (request_type == USB_TYPE_STANDARD &&
                 request.bRequest == USB_REQ_GET_DESCRIPTOR && in) {
        // interface 目标的 GET_DESCRIPTOR 会到用户态：HID class 与 report
        // 描述符由我们应答——这是设备 enumeration 机制，归本层而不是策略侧。
        const uint8_t desc_type = static_cast<uint8_t>(request.wValue >> 8);
        const uint8_t* desc = nullptr;
        size_t desc_len = 0;
        // HID class descriptor 里唯一随型号变化的是 wDescriptorLength；reply 按
        // 冻结块切片后在栈上覆写这两个字节，与 FfsDevice 写给 f_fs 的那份同源。
        uint8_t hid_desc[core::device::kFfsHidDescLength];

        if (desc_type == core::device::kDescriptorTypeHid) {
          memcpy(hid_desc,
                 core::device::kFfsFsDescriptors +
                     core::device::kFfsHidDescOffset,
                 sizeof(hid_desc));
          const uint16_t w_desc_len = state.profile->report_descriptor_size;
          constexpr uint32_t kLenField =
              core::device::kFfsWDescLenOffset -
              core::device::kFfsHidDescOffset;
          hid_desc[kLenField] = w_desc_len & 0xFF;
          hid_desc[kLenField + 1] = (w_desc_len >> 8) & 0xFF;
          desc = hid_desc;
          desc_len = sizeof(hid_desc);
        } else if (desc_type == core::device::kDescriptorTypeReport) {
          desc = state.profile->report_descriptor;
          desc_len = state.profile->report_descriptor_size;
        }

        if (!desc) {
          DS5DUMMY_LOG(ERROR, "[ep0] GET_DESCRIPTOR type=0x{:02X} — stalling",
                desc_type);
          responder.Stall(true);
          break;
        }
        // wLength 截断：主机可以合法地只要前缀，多一是协议违规。
        const size_t len =
            request.wLength < desc_len ? request.wLength : desc_len;
        responder.SendIn(desc, len);
        if (desc_type == core::device::kDescriptorTypeReport) {
          DS5DUMMY_LOG(INFO, "[ep0] served HID report descriptor ({} bytes)", len);
        }
      } else {
        DS5DUMMY_LOG(ERROR,
              "[ep0] unhandled request type=0x{:02X} bRequest=0x{:02X} "
              "dir={}",
              request_type, request.bRequest, in ? "IN" : "OUT");
        // stall 方向跟请求方向一致：IN 用 read、OUT 用 write（见 Ep0Responder::Stall）。
        responder.Stall(in);
      }
      break;
    }

    case FUNCTIONFS_ENABLE:
      DS5DUMMY_LOG(INFO, "[ep0] USB interface enabled");
      {
        std::lock_guard<std::mutex> lock(state.input_mutex);
        state.link_up = true;
      }
      // 链路 up 本身可能点亮 InWriterLoop 等待的合取条件。
      state.input_cv.notify_one();
      break;

    case FUNCTIONFS_DISABLE:
    case FUNCTIONFS_UNBIND: {
      std::lock_guard<std::mutex> lock(state.input_mutex);
      state.link_up = false;  // 之后的发布只进槽不写端点。
    }
      DS5DUMMY_LOG(INFO, "[ep0] USB interface down (event {})", event.type);
      break;

    case FUNCTIONFS_BIND:
      break;  // 建 function 时数据面无事可做。

    case FUNCTIONFS_SUSPEND:
      DS5DUMMY_LOG(INFO, "[ep0] USB suspend");
      break;

    case FUNCTIONFS_RESUME:
      DS5DUMMY_LOG(INFO, "[ep0] USB resume");
      break;

    default:
      break;  // 内核新增事件类型：忽略比误动作安全。
  }
  return true;
}

/**
 * @brief ep0 事件线程主循环：读事件→分发，直到通道坏或数据面停。
 *
 * 退出路径统一把 link_up 落下（没有 ep0 就没有控制面，主机侧状态不再可信），
 * 然后回调 on_ep0_closed 通知上层。
 */
void Ep0Loop() {
  plane::DataPlane& state = plane::Get();
  while (state.running && HandleEp0Event()) {
  }
  {
    std::lock_guard<std::mutex> lock(state.input_mutex);
    state.link_up = false;
  }
  if (state.handlers.on_ep0_closed) state.handlers.on_ep0_closed();
}

}  // namespace

// ---- ep0 应答原语（Ep0Responder 的方法体）----

bool Ep0Responder::SendIn(const void* data, size_t len) {
  if (::write(ep0_fd_, data, len) < 0) {
    DS5DUMMY_LOG(ERROR, "[ep0] IN response failed: {}", strerror(errno));
    return false;
  }
  return true;
}

ssize_t Ep0Responder::ReadData(void* buf, size_t cap) {
  return ::read(ep0_fd_, buf, cap);
}

// 无数据 OUT 请求的应答 = 把零长 status stage 排队。f_fs 把它暴露成一次
// 0 字节 read：__ffs_ep0_queue_wait 路径会对 OUT 方向的 pending setup
// 直接完成 status（0xDEADBABE 占位缓冲就是这条路径的内核侧痕迹）。
void Ep0Responder::AckDatalessOut() {
  char status;
  DiscardResult(::read(ep0_fd_, &status, 0));
}

void Ep0Responder::Stall(bool device_to_host) {
  char unused;
  if (device_to_host) {
    // IN setup 上的 stall 由一次 read() 触发（内核走 __ffs_ep0_stall）。
    DiscardResult(::read(ep0_fd_, &unused, 0));
  } else {
    // OUT setup 上的 stall 由一次 write(0) 触发。
    DiscardResult(::write(ep0_fd_, nullptr, 0));
  }
}

}  // namespace usb

namespace usb::plane {

void RunEp0() { Ep0Loop(); }

}  // namespace usb::plane
