/**
 * @file usb.cpp
 * @brief usb 模块门面实现：双半边入口、数据面原语与线程起停装配
 *
 * 本文件只留门面：root 半边（Provision/Teardown/HasDummyUdc/IsSafeGadgetPath）
 * 与 worker 半边（Adopt/Close）的入口，加上数据面原语与 StartThreads/
 * StopThreads 装配。共享状态与三个循环不在本文件：状态在
 * usb_plane.cpp，IN/OUT 两条循环在 usb_inout.cpp，ep0 事件循环与应答原语
 * 在 usb_ep0.cpp。
 *
 * 三个端点都不能进 poll()（FunctionFS 端点文件没有 .poll，永远报 ready），
 * 阻塞点全在内核调用里，所以 IN/ep0/OUT 各一条线程；这替代了早期单线程
 * pump——主机一启用 interface，OUT 读就把整个循环 parked 住，ep0 控制
 * 传输全部无人应答。
 */

#include "usb/usb.h"

#include <pthread.h>

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>

#include "core/debug.h"
#include "core/device/identity.h"
#include "core/protocol/report.h"
#include "loguru.hpp"
#include "usb/ffs_device.h"
#include "usb/gadget_setup.h"
#include "usb/usb_plane.h"

namespace usb {
namespace {

/**
 * @brief Provision 任一步失败后的统一回滚：关掉本次打开的端点 fd，再整站拆掉。
 * @param device 本次 Provision 用的端点句柄（可能一个 fd 都没开成）。
 * @param gadget 本次 Provision 用的坐标（挂载点 + 实例名）。
 * @details 两步都是幂等的：FfsDevice::Close 对 -1 无操作（Open 的失败路径已经
 *          关过），GadgetTeardown 对"没绑过"与"目录本就不在"都按成功处理。
 *          所以回滚不必按"挂在哪一步"挑步骤，三条失败路径共用这一条。
 * @note 副作用：关 fd、写 configfs、detach FunctionFS 挂载。
 * @see Provision。
 */
void AbortProvision(detail::FfsDevice& device, const GadgetId& gadget) {
  device.Close();
  detail::GadgetTeardown(gadget.ffs_path, gadget.instance);
}

/// bind 之后等主机把设备枚举出来的总预算（毫秒）。内核 usb 核在新设备连上后
/// 异步分配地址、读描述符，这一段没有用户态参与；经验值 2 s 是给慢机器和开机
/// 繁忙时段留的余量，等满也不改变站点可用性（未实测具体耗时）。
constexpr int kHostEnumBudgetMs = 2000;

/// 枚举等待的轮询步长（毫秒）：等待本身只是几次 sysfs 读，步长粗一点不省什么，
/// 但也犯不着 1 ms 一次——最坏多睡 50 ms，Manager 的重试退避是 2000 ms。
constexpr int kHostEnumPollMs = 50;

/**
 * @brief 带预算地反复尝试解析本站的主机侧接口目录，直到拿到结果或预算用尽。
 * @param gadget 本站坐标。
 * @return 相对 /sys 的接口目录路径；预算内没等到返回空串。
 * @note 阻塞调用：只睡 sysfs 读取的轮询间隔，不碰端点 fd，所以此时数据面
 *       线程还没起也没关系（描述符已由内核 f_fs 就位，枚举不需要用户态应答）。
 *       设备目录与它下面的接口目录由同一次枚举先后建出，所以这条预算同时覆盖
 *       两者——等不到接口目录就等于等不到本站。
 * @see detail::FindHostDevicePath。
 */
std::string WaitForHostDevicePath(const GadgetId& gadget) {
  for (int waited = 0; waited < kHostEnumBudgetMs; waited += kHostEnumPollMs) {
    const std::string path = detail::FindHostDevicePath(
        gadget.instance, core::device::kDualSenseIdentity.vendor_id,
        core::device::kDualSenseIdentity.product_id);
    if (!path.empty()) return path;
    std::this_thread::sleep_for(std::chrono::milliseconds(kHostEnumPollMs));
  }
  return "";
}

/// SIGUSR1 空处理函数：唯一作用是让阻塞的系统调用以 EINTR 返回。
void Suppress(int) {}

}  // namespace

// ---- root 半边 ----

GadgetId ResolveGadgetId(const std::string& mac, const std::string& ffs_base) {
  std::string digits;
  digits.reserve(12);
  for (const char c : mac) {
    // 分隔符（':'、'-'）跳过，十六进制数字统一转小写以让同一 MAC 的两种写法
    // 派生出同一个实例名；其余字符直接判非法。
    if (c == ':' || c == '-') continue;
    if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
    digits.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (digits.size() != 12) return {};
  // 序列号直接复用那 12 位：它必须逐站唯一，而 digits 已经是本站的唯一键。
  return GadgetId{"ds5dummy-" + digits, ffs_base + "." + digits, digits};
}

bool Provision(Station& out, const GadgetId& gadget, uint8_t interval_ms) {
  // 端点句柄是本轮的局部值而不是 plane 里的常驻成员：Manager 会并发持有
  // 多个 gadget，每个 slot 的 fd 都归自己的 Provision 调用产出。FfsDevice
  // 析构不关 fd（见 ffs_device.h），所以成功后三份 fd 直接交给调用方。
  detail::FfsDevice device;
  // 顺序即依赖：configfs 就位 → 描述符进内核 → 内核才接受 bind。
  if (!detail::GadgetSetup(gadget.ffs_path, gadget.instance, gadget.serial)) {
    DS5DUMMY_LOG(ERROR, "[usb] GadgetSetup failed (instance {})",
                 gadget.instance.c_str());
    AbortProvision(device, gadget);
    return false;
  }
  if (!device.Open(gadget.ffs_path, gadget.serial, interval_ms)) {
    DS5DUMMY_LOG(ERROR, "[usb] FunctionFS open failed");
    AbortProvision(device, gadget);
    return false;
  }
  if (!detail::GadgetBind(gadget.instance)) {
    DS5DUMMY_LOG(ERROR, "[usb] GadgetBind failed (instance {})",
                 gadget.instance.c_str());
    AbortProvision(device, gadget);
    return false;
  }
  out.endpoints = Endpoints{device.Ep0Fd(), device.EpInFd(), device.EpOutFd()};
  // bind 成功站点就可用了，接口目录只是可选的关联线索：等不到留空并降级成
  // WARNING，绝不因此拆站（消费者只有音频 sink 的属性，缺它只是少一个键）。
  out.host_sysfs_path = WaitForHostDevicePath(gadget);
  if (out.host_sysfs_path.empty()) {
    DS5DUMMY_LOG(WARNING, "[usb] {} bound but host interface dir not found",
                 gadget.instance.c_str());
  }
  return true;
}

void Teardown(const std::string& ffs_path, const std::string& instance) {
  detail::GadgetTeardown(ffs_path, instance);
}

bool HasDummyUdc() { return !detail::FindDummyUdc().empty(); }

// ---- worker 半边 ----

bool Adopt(const Endpoints& inherited) {
  return plane::Get().device.Adopt(inherited.ep0, inherited.ep_in,
                                   inherited.ep_out);
}

void Close() { plane::Get().device.Close(); }

// ---- 数据面原语 ----

void PostInputReport(const uint8_t* report, size_t len) {
  // 空指针或零长度没有可发布的内容：整次调用作废，槽位保持上一份。
  if (report == nullptr || len == 0) return;

  plane::DataPlane& state = plane::Get();
  // 槽位是一份定长报告：超过线尺寸的部分截断，容量即 kUsbInputReportSize。
  const size_t copied = len < core::protocol::kUsbInputReportSize
                            ? len
                            : core::protocol::kUsbInputReportSize;
  {
    std::lock_guard<std::mutex> lock(state.input_mutex);
    memcpy(state.latest_input.data(), report, copied);
    state.pending_ready = true;
  }
  state.input_cv.notify_one();
}

void LatestInputReport(uint8_t* out, size_t cap) {
  plane::DataPlane& state = plane::Get();
  const size_t n =
      cap < core::protocol::kUsbInputReportSize
          ? cap
          : core::protocol::kUsbInputReportSize;
  std::lock_guard<std::mutex> lock(state.input_mutex);
  memcpy(out, state.latest_input.data(), n);
}

#if DS5DUMMY_DEBUG
uint64_t InputWrites() { return plane::Get().input_writes.load(); }
#endif

// ---- 三条阻塞线程 ----

bool StartThreads(const Handlers& handlers) {
  plane::DataPlane& state = plane::Get();
  if (state.running) {
    DS5DUMMY_LOG(ERROR, "[usb] data plane already running");
    return false;
  }

  state.handlers = handlers;

  // 槽位内容不在这里播种：报告格式是 bridge 的知识，第一份（空闲 0x01 报告）
  // 由 bridge::Init() 发布，时序上早于本函数。这里只复位标志与计数。
  state.pending_ready = false;
  state.link_up = false;
#if DS5DUMMY_DEBUG
  state.input_writes = 0;
#endif

  // 空处理函数即可：StopThreads 只需要阻塞调用以 EINTR 返回，不丢语义。
  struct sigaction action = {};
  action.sa_handler = Suppress;
  sigaction(SIGUSR1, &action, nullptr);

  state.running = true;
  try {
    state.in_thread = std::thread(plane::RunIn);
    state.ep0_thread = std::thread(plane::RunEp0);
    state.out_thread = std::thread(plane::RunOut);
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[usb] cannot start data-plane thread: {}", error.what());
    StopThreads();  // 就地收尾：已起来的线程不留半启动状态。
    return false;
  }
  return true;
}

void StopThreads() {
  plane::DataPlane& state = plane::Get();
  // 三条线程都可能 parked 在阻塞的 FunctionFS 调用里，那些等待可被信号
  // 打断：先置标志、广播 cv，再逐个 pthread_kill 唤醒，最后 join。
  state.running = false;
  state.input_cv.notify_all();
  auto wake = [](std::thread& thread) {
    if (thread.joinable()) pthread_kill(thread.native_handle(), SIGUSR1);
  };
  auto join = [](std::thread& thread) {
    if (thread.joinable()) thread.join();
  };
  wake(state.in_thread);
  wake(state.ep0_thread);
  wake(state.out_thread);
  join(state.in_thread);
  join(state.ep0_thread);
  join(state.out_thread);
  {
    std::lock_guard<std::mutex> lock(state.input_mutex);
    state.link_up = false;
  }
}

}  // namespace usb
