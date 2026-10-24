/**
 * @file ffs_device.cpp
 * @brief FunctionFS 端点打开、描述符写入与读写实现
 *
 * ---------------------------------------------------------------------------
 * FunctionFS ep0 载荷格式（内核 f_fs.c 规定的两段写入，顺序固定）：
 * 1. Descriptors 段：magic、总长、flags、每个被 flag 置位的速度块一个计数，
 *    随后是该计数个描述符。只允许 function 自身的描述符——放 configuration
 *    descriptor 会被内核拒绝（composite 从 configfs 自己构造它）。
 * 2. Strings 段：magic、总长、字符串数、语言数；每种语言一个 LE16 langid
 *    后跟该数量个 NUL 结尾字符串。
 * ---------------------------------------------------------------------------
 */

#include "usb/ffs_device.h"

#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "core/debug.h"
#include "core/device/descriptors.h"
#include "loguru.hpp"

namespace usb {
namespace detail {

bool FfsDevice::WriteFfsDescriptors(const std::string& serial,
                                    uint8_t interval_ms) {
  // 段 1 头：V2 magic + 声明只有 full-speed 一块描述符。
  struct {
    uint32_t magic;
    uint32_t length;
    uint32_t flags;
    uint32_t fs_count;
  } header = {
      FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
      sizeof(header) + sizeof(core::device::kFfsFsDescriptors),
      FUNCTIONFS_HAS_FS_DESC,
      core::device::kFfsFsDescCount,
  };

  uint8_t desc_buf[sizeof(header) + sizeof(core::device::kFfsFsDescriptors)];
  memcpy(desc_buf, &header, sizeof(header));
  memcpy(desc_buf + sizeof(header), core::device::kFfsFsDescriptors,
         sizeof(core::device::kFfsFsDescriptors));
  // 两个 bInterval 是描述符里唯一随配置变化的字节；偏移由 descriptors.h
  // 的常量给出，并在那里用 static_assert 钉在端点描述符内。
  desc_buf[sizeof(header) + core::device::kFfsEpInIntervalOffset] = interval_ms;
  desc_buf[sizeof(header) + core::device::kFfsEpOutIntervalOffset] =
      interval_ms;

  // 描述符先于任何 setup 到达 ep0，主机 enumeration 才看得见 function。
  ssize_t written = ::write(ep0_fd_, desc_buf, sizeof(desc_buf));
  if (written != static_cast<ssize_t>(sizeof(desc_buf))) {
    DS5DUMMY_LOG(ERROR, "[ffs] Failed to write descriptors to ep0: {}",
          strerror(errno));
    return false;
  }
  DS5DUMMY_LOG(INFO, "[ffs] Wrote {} bytes of descriptors to ep0", sizeof(desc_buf));

  // 段 2：本模块的描述符不引用任何字符串索引，这张表只为满足内核的
  // 形式要求；设备真字符串走 configfs（gadget_setup），不在此处生效。
  // 三个条目的形状与 configfs 侧一一对应（厂商/产品/序列号），序列号按站
  // 传入而不是取常量——两处取值一致，读 f_fs 事件的工具才不会看到错位。
  const char* strings[] = {core::device::kManufacturer, core::device::kProduct,
                           serial.c_str()};
  uint16_t langid = 0x0409;  // en-US，与 configfs strings/0x409 目录一致。

  struct {
    uint32_t magic;
    uint32_t length;
    uint32_t str_count;
    uint32_t lang_count;
  } str_header = {
      FUNCTIONFS_STRINGS_MAGIC,
      0,  // length 依赖下方逐串累加，先置 0。
      static_cast<uint32_t>(sizeof(strings) / sizeof(strings[0])),
      1,  // 只注册一种语言。
  };

  // 总长 = 头 + langid + 每串（含 NUL 结尾，它是段格式的一部分）。
  size_t str_total = sizeof(str_header) + sizeof(langid);
  for (const char* s : strings) {
    str_total += strlen(s) + 1;
  }
  str_header.length = static_cast<uint32_t>(str_total);

  std::vector<uint8_t> str_buf(str_total);
  memcpy(str_buf.data(), &str_header, sizeof(str_header));
  size_t str_off = sizeof(str_header);
  memcpy(str_buf.data() + str_off, &langid, sizeof(langid));
  str_off += sizeof(langid);
  for (const char* s : strings) {
    size_t len = strlen(s) + 1;
    memcpy(str_buf.data() + str_off, s, len);
    str_off += len;
  }

  written = ::write(ep0_fd_, str_buf.data(), str_buf.size());
  if (written != static_cast<ssize_t>(str_buf.size())) {
    DS5DUMMY_LOG(ERROR, "[ffs] Failed to write strings to ep0: {}", strerror(errno));
    return false;
  }
  DS5DUMMY_LOG(INFO, "[ffs] Wrote {} bytes of strings to ep0", str_buf.size());

  return true;
}

bool FfsDevice::Open(const std::string& ffs_path, const std::string& serial,
                     uint8_t interval_ms) {
  std::string ep0_path = ffs_path + "/ep0";
  std::string ep_in_path = ffs_path + "/ep1";
  std::string ep_out_path = ffs_path + "/ep2";

  DS5DUMMY_LOG(INFO, "[ffs] Opening FunctionFS at {}", ffs_path.c_str());

  // ep0：之后 ep0 线程睡在 ffs_ep0_read() 里等 SETUP，必须能在主机控制
  // 超时前应答，所以打开即阻塞是常态而非异常。
  // 三处 O_CLOEXEC 是 fd 交接的前提：Manager 侧 fork+execv 前由
  // ApplyPlan() 清掉 dup 到 3/4/5 目标上的该标志，其余 fd 一律不出子进程。
  ep0_fd_ = ::open(ep0_path.c_str(), O_RDWR | O_CLOEXEC);
  if (ep0_fd_ < 0) {
    DS5DUMMY_LOG(ERROR, "[ffs] Failed to open ep0: {}", strerror(errno));
    return false;
  }

  // 描述符与字符串必须在 bind 之前就位。
  if (!WriteFfsDescriptors(serial, interval_ms)) {
    Close();
    return false;
  }

  // IN：ffs_epfile_io() 无论 O_ 标志如何都会同步等待主机消费，因此只有
  // IN 写线程允许碰它。
  ep_in_fd_ = ::open(ep_in_path.c_str(), O_WRONLY | O_CLOEXEC);
  if (ep_in_fd_ < 0) {
    DS5DUMMY_LOG(ERROR, "[ffs] Failed to open ep1 (IN): {}", strerror(errno));
    Close();
    return false;
  }

  // OUT：同样的阻塞数据路径，独占一条读线程；poll 在它身上没有意义
  // （端点文件无 .poll，永远报 ready）。
  ep_out_fd_ = ::open(ep_out_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (ep_out_fd_ < 0) {
    DS5DUMMY_LOG(ERROR, "[ffs] Failed to open ep2 (OUT): {}", strerror(errno));
    Close();
    return false;
  }

  DS5DUMMY_LOG(INFO, "[ffs] All endpoints opened successfully");
  return true;
}

bool FfsDevice::Adopt(int ep0_fd, int ep_in_fd, int ep_out_fd) {
  // 交接来的 fd 只认领：不 open、不写描述符（root 半边已经做完这两步）。
  const int fds[3] = {ep0_fd, ep_in_fd, ep_out_fd};
  for (int fd : fds) {
    if (fd < 0 || ::fcntl(fd, F_GETFD) < 0) {
      DS5DUMMY_LOG(ERROR, "[ffs] Adopted fd {} is not usable: {}", fd,
            strerror(errno));
      return false;
    }
  }
  if (ep0_fd == ep_in_fd || ep0_fd == ep_out_fd || ep_in_fd == ep_out_fd) {
    DS5DUMMY_LOG(ERROR, "[ffs] Adopted fds must be distinct");
    return false;
  }
  ep0_fd_ = ep0_fd;
  ep_in_fd_ = ep_in_fd;
  ep_out_fd_ = ep_out_fd;
  DS5DUMMY_LOG(INFO, "[ffs] Adopted inherited endpoints (ep0={} ep1={} ep2={})",
        ep0_fd_, ep_in_fd_, ep_out_fd_);
  return true;
}

bool FfsDevice::WriteInputReport(const uint8_t* data, size_t len) {
  if (ep_in_fd_ < 0) return false;
  // 阻塞直到主机消费：ffs_epfile_io() 的同步路径等待 io_data->done，与
  // O_NONBLOCK 无关，所以本调用只允许出现在 IN 线程上。
  ssize_t written = ::write(ep_in_fd_, data, len);
  if (written < 0) {
    if (errno == EINTR || errno == ESHUTDOWN) return false;  // stop/拆链：静默丢帧
    DS5DUMMY_LOG(ERROR, "[ffs] write_input_report failed: {}", strerror(errno));
    return false;
  }
  return static_cast<size_t>(written) == len;  // 短写同样按失败计，不补写
}

ssize_t FfsDevice::ReadOutputReport(uint8_t* buf, size_t bufsize) {
  if (ep_out_fd_ < 0) return -1;
  return ::read(ep_out_fd_, buf, bufsize);
}

void FfsDevice::Close() {
  if (ep0_fd_ >= 0) {
    ::close(ep0_fd_);
    ep0_fd_ = -1;
  }
  if (ep_in_fd_ >= 0) {
    ::close(ep_in_fd_);
    ep_in_fd_ = -1;
  }
  if (ep_out_fd_ >= 0) {
    ::close(ep_out_fd_);
    ep_out_fd_ = -1;
  }
}

}  // namespace detail
}  // namespace usb
