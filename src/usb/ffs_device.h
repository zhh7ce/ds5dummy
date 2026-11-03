/**
 * @file ffs_device.h
 * @brief FunctionFS 端点 fd 的持有与 I/O 封装
 *
 * 持有 ep0/ep_in/ep_out 三个 FunctionFS 端点 fd，封装打开、认领、读写与
 * 关闭。是 usb 模块内部的机制层之一（另两层是 gadget_setup 与门面 usb.cpp），
 * 收在 namespace usb::detail 里，不跨门面暴露。
 *
 * 设备级字符串（厂商/产品/序列号）的唯一来源是 configfs，见 gadget_setup.h
 * 的 GadgetSetup；本类的 Open() 只为通过内核 f_fs 对字符串表的格式校验，
 * 不额外维护第二份设备身份。
 *
 * @note 名为"封装"但不是 RAII：析构函数不关 fd，收尾必须显式调 Close()
 *       （门面 usb::Close/Provision 失败路径负责）。这样 fd 能在 fork+execv
 *       交接后由 worker 认领而不触发任何隐式关闭。
 * @note 三条数据面的阻塞语义（ep0 事件读、IN 写、OUT 读各会睡在内核里）
 *       决定了调用方必须把它们放在各自的线程上——见 usb.cpp 的三条循环。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace usb {
namespace detail {

/**
 * @brief 一组 FunctionFS 端点 fd 的所有权句柄。
 *
 * 生命周期由外部驱动：root 侧 Open() 建立，worker 侧 Adopt() 认领，Close()
 * 释放；三个成员独立记录、-1 表示未持有。线程模型：fd 本身无锁，靠"哪个
 * 线程用哪个 fd"的约定分域（ep0/IN/OUT 各归其线程）。
 */
class FfsDevice {
 public:
  /**
   * @brief 打开 ffs_path 下的三个端点并向 ep0 写入描述符与字符串表。
   * @param ffs_path FunctionFS 挂载目录（GadgetSetup 已 mount 好）。
   * @param serial 本站的 USB 序列号，只用作 ep0 字符串表的第三个条目以填满
   *               内核要求的格式；主机实际读到的 iSerialNumber 来自 configfs
   *               （见 GadgetSetup）。传本站坐标里的同一个值，别让两处不一致。
   * @param interval_ms 两个中断端点的 bInterval（毫秒）；全速总线一帧 1 ms，
   *               所以配置的毫秒数原样就是这个字节。调用方保证它是非零的
   *               1/2/4（real-time 档由调用方换成 1 再传进来）。它只是声明——
   *               dummy_hcd 不照它轮询，见 usb::StartThreads。
   * @return 成功 true；任一步失败时已 Close() 回收本次打开的 fd。
   * @note 仅 root 半边调用；写描述符必须先于内核接受 gadget bind。
   */
  bool Open(const std::string& ffs_path, const std::string& serial,
            uint8_t interval_ms);

  /**
   * @brief 关闭全部已持有的端点 fd 并复位为 -1；可重复调用。
   * @details worker 侧这是最后一个引用时，f_fs 自动解绑 gadget。
   */
  void Close();

  /**
   * @brief 认领已打开的三个 fd（fd 交接场景），不 open 文件、不写描述符。
   * @param ep0_fd ep0 fd。
   * @param ep_in_fd interrupt IN fd。
   * @param ep_out_fd interrupt OUT fd。
   * @return 三个 fd 都可用（fcntl F_GETFD 成功）且互不相同时 true。
   */
  bool Adopt(int ep0_fd, int ep_in_fd, int ep_out_fd);

  /**
   * @brief 向 interrupt IN 端点写一份报告；调用线程会一直阻塞到主机消费。
   * @param data 报告字节（含 report ID 首字节）。
   * @param len 报告长度。
   * @return 完整写入 true；EINTR/ESHUTDOWN/短写 false。
   * @warning 只允许 usb 的 IN 写线程调用（唯一合法阻塞点约定）。
   */
  bool WriteInputReport(const uint8_t* data, size_t len);

  /**
   * @brief 从 interrupt OUT 端点阻塞读取一份主机报告。
   * @param buf 接收缓冲区。
   * @param bufsize 缓冲区容量。
   * @return 读到的字节数；出错 -1，errno 原样保留给调用方分流。
   * @warning 只允许 OUT 读线程调用。
   */
  ssize_t ReadOutputReport(uint8_t* buf, size_t bufsize);

  /// interrupt IN 端点 fd；未持有时 -1。
  int EpInFd() const { return ep_in_fd_; }
  /// interrupt OUT 端点 fd；未持有时 -1。
  int EpOutFd() const { return ep_out_fd_; }
  /// ep0 控制端点 fd；未持有时 -1。
  int Ep0Fd() const { return ep0_fd_; }

 private:
  int ep0_fd_ = -1;     ///< 控制端点：事件流与 setup 应答通道。
  int ep_in_fd_ = -1;   ///< interrupt IN 端点。
  int ep_out_fd_ = -1;  ///< interrupt OUT 端点。

  /**
   * @brief 把 FunctionFS 两段载荷（描述符块 + 字符串表）先后写进 ep0。
   * @param serial 本站序列号，字符串表的第三个条目。
   * @param interval_ms 写进 EP1 IN / EP2 OUT 两处 bInterval 的毫秒数。
   * @return 两段都完整写入 true；失败已记日志。
   * @details 冻结在 core::device::kFfsFsDescriptors 里的字节先拷进局部缓冲，
   *          再按 core::device::kFfsEpIn/OutIntervalOffset 覆写这两个字节——
   *          除它们之外整块描述符与常量逐字节相同。
   * @note 仅由 Open() 在 ep0 打开后调用一次。
   */
  bool WriteFfsDescriptors(const std::string& serial, uint8_t interval_ms);
};

}  // namespace detail
}  // namespace usb
