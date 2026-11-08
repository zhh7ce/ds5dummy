/**
 * @file usb.h
 * @brief usb 模块门面：FunctionFS 虚拟手柄的双半边入口与数据面线程
 *
 * 同一份二进制、两个进程半边，按"知道什么"划分而不是按"是谁"划分：
 * - root 半边（Manager 与 tools 直接调用）：Provision 一条命令完成
 *   configfs 装配 → 打开三个端点 → 写描述符 → bind UDC，失败自带回滚，
 *   调用方可直接重试；每个 gadget 由 (ffs_path, instance) 这一组坐标标识，
 *   同一 Manager 可并行持有 N 组（每只手柄一组），互不共享状态；
 * - worker 半边（降权后的 fork+execv 子进程）：Adopt 认领经 fd 交接固定到
 *   3/4/5 的三个端点 fd，随后 StartThreads 跑数据面。
 *
 * 数据面必须是三条线程而不是 poll 循环：FunctionFS 端点文件没有 .poll
 * 实现，poll() 永远报 ready，真正的阻塞发生在 ffs_epfile_io()/ffs_ep0_read()
 * 内核调用里，所以 ep0、IN、OUT 各配一条阻塞线程。转发策略（class 请求
 * 怎么答、OUT 报告送去哪）全部由调用方以 Handlers 回调注入：usb 层不认识
 * bt，也不认识报告内容——它只搬运字节并应答 USB enumeration 机制。
 *
 * @see ffs_device.h —— 端点 fd 的机制封装；gadget_setup.h —— configfs 装配。
 */

#pragma once

#include <linux/usb/ch9.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "core/debug.h"
#include "core/device/descriptors.h"

namespace usb {

/**
 * @brief 三个 FunctionFS 端点 fd 的搬运结构。
 *
 * Provision 成功后填入本进程真实 fd；经 handoff 交接到 worker 侧后固定为
 * 3/4/5。默认 -1 表示"未持有"。
 */
struct Endpoints {
  int ep0 = -1;     ///< ep0 控制端点：事件流 + class/标准请求应答通道。
  int ep_in = -1;   ///< interrupt IN 端点：设备→主机输入报告。
  int ep_out = -1;  ///< interrupt OUT 端点：主机→设备输出报告。
};

/**
 * @brief 一次 Provision 的产出：三个端点 fd + 本站在主机侧的 sysfs 接口目录。
 *
 * host_sysfs_path 用**去掉 /sys 前缀**的形态（形如
 * `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`），与 PipeWire 真设备上
 * `device.sysfs.path` 的写法一致，可直接透传给音频 sink 当设备关联线索。它是
 * 接口目录而不是设备目录：按硬件身份回溯的消费者沿**父链**找 `usb_device`，
 * 从设备目录出发只会命中 root hub。解析不到就是空串——它只是可选信息，不影响
 * 站点是否可用。
 *
 * @see Provision —— 唯一的产出方；audio::Start —— 最终的消费方（经 worker 配置快照）。
 */
struct Station {
  Endpoints endpoints;       ///< 三个 FunctionFS 端点 fd，所有权归调用方。
  std::string host_sysfs_path;  ///< 主机侧枚举出的接口目录，相对 /sys；可为空。
};

// ---- root 半边 ----

/**
 * @brief 一台虚拟手柄的站点坐标：configfs/FunctionFS 两个路径 + 序列号。
 *
 * 同一 Manager 可为多只手柄各建一站，站与站之间唯一不共享的就是这三个字符串；
 * Provision/Teardown 必须拿到同一组值，否则会拆到别的站。
 */
struct GadgetId {
  /// configfs gadget 目录名，同时是 ffs function 实例名与挂载源。
  std::string instance;
  /// FunctionFS 挂载点目录。
  std::string ffs_path;
  /// USB 序列号（iSerialNumber 的内容），与 instance/ffs_path 同源于 MAC。
  std::string serial;
};

/**
 * @brief 由手柄 MAC 派生本站的 gadget 实例名、FunctionFS 挂载点与序列号。
 *
 * 实例名取 MAC 的全部 12 位十六进制（`ds5dummy-<12 hex>`），挂载点取
 * `<ffs_base>.<12 hex>`：两者都随 MAC 唯一，撞名的后果是后建的一站把前一站
 * 当成残留拆掉，所以不用"末几位"这类缩写。
 *
 * 序列号就是那 12 位十六进制本身（形如真机的 12 字符序列号）。它必须逐站
 * 唯一：主机侧把 iSerialNumber 当作设备的实例标识，两只手柄共用一个序列号
 * 会被认成同一台设备反复插拔。
 *
 * @param mac 手柄 BD_ADDR，大小写与分隔符不限（如 "AA:BB:CC:DD:EE:FF"）；
 *            必须恰好含 12 个十六进制数字。
 * @param ffs_base 系统配置里的 FunctionFS 挂载根（如 /dev/ffs-ds5）。
 * @return 派生出的坐标；@p mac 不含 12 个十六进制数字时三个字段都为空串，
 *         调用方据此判配置错误。@p ffs_base 的合法性不在此校验——它还要过
 *         IsSafeGadgetPath。
 */
GadgetId ResolveGadgetId(const std::string& mac, const std::string& ffs_base);

/**
 * @brief root 侧整站 bring-up：detail::GadgetSetup → 开端点+写描述符 →
 *        detail::GadgetBind。
 *
 * 三步顺序不可交换：内核只有在 function 描述符就位后才接受 bind。任何一步
 * 失败都按同一套动作回滚（关 fd、必要时解绑、清 configfs），调用方直接重试
 * 本函数即可，无需先调 Teardown。
 *
 * @param out 成功时填入本站的产出：三个端点 fd（所有权外交给调用方，本层不
 *            保存副本）与主机侧枚举出来的设备 sysfs 路径。失败时内容不保证。
 * @param gadget 本站坐标（实例名、挂载点、序列号），必须是同一次
 *               ResolveGadgetId 的产物；三者缺一即与本站不对应，会拆到别站。
 * @param profile 本站型号档案：configfs 里的 VID/PID/产品串与写进 ep0 的 report
 *                descriptor 都取自它，两处必须同源，否则主机按错的布局解析。
 *                型号由发现阶段的 HID VID/PID 判定（core::device::ProfileFor）。
 * @param interval_ms 两个中断端点的 bInterval（毫秒），直接取配置的
 *                    usb_report_interval_ms（1/2/4）。它只是描述符里的声明：
 *                    dummy_hcd 不照它轮询（见 StartThreads），实际发送节拍
 *                    由 worker 的 IN 线程保证。手柄经蓝牙上报的节奏由固件
 *                    决定，本参数改不了。real-time 档传 1：主机仍按 1 ms
 *                    轮询，新样本到达的延迟最小，只是没有新样本时 worker
 *                    不排数据。
 * @return 成功 true；false 时已完成回滚（含 detach 本组的 FunctionFS 挂载）。
 * @note 仅 root 权限可调用（写 configfs、mount）。UDC 由 GadgetBind 从空闲的
 *       dummy_udc.N 里挑一个，所以并发 gadget 数受 dummy_hcd 的 num 限制。
 * @note bind 之后本函数会**同步等待**主机侧把设备枚举出来（上限见实现里的
 *       预算）：那段等待只是读 sysfs，描述符由内核 f_fs 直接应答，不需要
 *       用户态参与。等不到也只把 host_sysfs_path 留空并记 WARNING——站点
 *       本身是好的，缺的只是一个可选的关联线索。
 * @see Teardown —— 对称的整站拆除，坐标必须与本次调用一致。
 */
bool Provision(Station& out, const GadgetId& gadget,
               const core::device::DeviceProfile& profile,
               uint8_t interval_ms);

/**
 * @brief 单个 gadget 的整站拆除：unbind + 清 configfs + detach 挂载。
 * @param ffs_path 该 gadget 用的 FunctionFS 挂载点。
 * @param instance 该 gadget 的实例名。
 * @note 仅 root 半边调用；与 Provision 的回滚共用底层两段实现。传入的坐标
 *       必须与当初 Provision 的一致，拆错坐标会伤到另一个 gadget。
 */
void Teardown(const std::string& ffs_path, const std::string& instance);

/**
 * @brief /sys/class/udc 下是否存在 dummy_udc.N 虚拟 UDC。
 * @return 存在 true；Manager 预检 dummy_hcd 模块是否已加载时用。
 */
bool HasDummyUdc();

/**
 * @brief 校验即将进入 mkdir/mount/symlink 的 gadget 路径或实例名。
 *
 * 白名单只放行 字母/数字/. _ / -，并显式拒绝含 ".." 的串——这两个字符串
 * 绝不能来自外部输入，GadgetSetup 里还会再挡一遍。
 *
 * @param path 待校验串，非空且仅含白名单字符、不含 ".." 才通过。
 * @return 安全 true。
 */
bool IsSafeGadgetPath(const std::string& path);

// ---- worker 半边 ----

/**
 * @brief 认领交接来的三个端点 fd：只校验可用且互不相同，不 open、不写描述符。
 * @param inherited 交接后的 fd 三元组（worker 侧即 3/4/5）。
 * @param profile 本站型号档案：ep0 线程据此应答 GET_DESCRIPTOR(Report) 与 HID
 *                class descriptor。必须与 Manager 建站时用的是同一份，所以由
 *                Manager 经 worker 配置快照的 usb_model 传下来。
 * @return 合法 true；任一 fd 不可用或彼此相同 false。
 */
bool Adopt(const Endpoints& inherited,
           const core::device::DeviceProfile& profile);

/**
 * @brief 关闭本进程持有的三个 fd 副本。
 * @details worker 那份是最后一个引用时，f_fs 会自动解绑 gadget。
 */
void Close();

// ---- 数据面原语 ----

/**
 * @brief 线程安全地覆盖持有的输入报告：新一份替换旧一份，不排队。
 *
 * usb 侧只保留一份最新状态。IN 写线程在链路 up 后把它阻塞写进 ep_in；重发
 * 档下写完成就立刻重发同一份，一次写要等主机取走一份，所以节奏就是主机轮询
 * 节奏（bInterval），两次蓝牙样本之间主机收到的是逐字节相同的重复。real-time
 * 档下每份只写一次。两档都以最新覆盖为准：迟到的旧报告不会被补发，主机的
 * 滞后上限因此是一个蓝牙上报周期。
 *
 * @param report 整份报告的字节，至少 len 字节可读，首字节即 report ID——报告的
 *               组装归 bridge，本层只搬运字节；nullptr 视为无内容。
 * @param len    @p report 的字节数，超出单份容量的部分被截断，不足时尾部清零。
 *               len 为 0 或 report 为 null 时整次发布作废，持有的状态保持原样。
 */
void PostInputReport(const uint8_t* report, size_t len);

/**
 * @brief 拷贝最近一份输入报告（含 report ID 字节），供 ep0 GET_REPORT 应答。
 * @param out 目标缓冲，至少能收 min(cap, 64) 字节。
 * @param cap 缓冲区容量；小于 64 时按 cap 截断拷贝。
 */
void LatestInputReport(uint8_t* out, size_t cap);

#if DS5DUMMY_DEBUG
/**
 * @brief IN 写线程累计的成功写入次数（进程生命周期内的单调计数）。
 * @return 计数；StartThreads 重新开始时清零。
 */
uint64_t InputWrites();
#endif

// ---- ep0 应答原语（class 请求回调里用；方向语义见各方法注释）----

/**
 * @brief 一条 ep0 SETUP 的应答通道：handler 用它完成 data/status stage。
 *
 * 由 usb 的 ep0 线程在每条 SETUP 时构造——class 请求随回调交给策略层，
 * 标准请求由本层就地使用——回调返回即失效：handler 只能把引用用在本轮
 * 回调里，存下来或挪到别的线程再用都是未定义行为（会踩到下一条 SETUP
 * 的 data stage）。f_fs 的应答怪癖（无数据 OUT 请求的零长 read、STALL 的
 * read/write 方向语义）全部收在这四个方法里，handler 不用知道。
 *
 * @see Handlers::on_class_request —— 门面上唯一能看到本类型的回调。
 */
class Ep0Responder {
 public:
  /**
   * @brief 持有一条 SETUP 的应答通道；构造归 ep0 线程。
   * @param ep0_fd 当前 ep0 端点 fd。
   * @warning 外部只见引用：自己构造一个 responder 等于给任意 fd 发应答，
   *          与"只在本轮回调内有效"的契约无关，但没有任何合法用途。
   */
  explicit Ep0Responder(int ep0_fd) : ep0_fd_(ep0_fd) {}

  /**
   * @brief 在 ep0 上应答一段 IN 数据（setup 后的 data stage）。
   * @param data 待发送字节。
   * @param len 字节数，须已由主机 wLength 截断。
   * @return write 成功 true；失败已记日志。
   */
  bool SendIn(const void* data, size_t len);

  /**
   * @brief 在 ep0 上读取 OUT 请求携带的数据段。
   * @param buf 接收缓冲。
   * @param cap 容量。
   * @return read 的字节数，出错 -1（errno 保留）。
   */
  ssize_t ReadData(void* buf, size_t cap);

  /**
   * @brief 应答一个无数据段的 OUT 请求：读 0 字节把零长 status stage 排上队。
   * @details f_fs 把"完成 OUT setup 的应答"暴露为一次长度为 0 的 read。
   */
  void AckDatalessOut();

  /**
   * @brief 对当前 setup 回 STALL。
   * @param device_to_host 请求方向位：true 时在 IN setup 上 stall（用 0 长
   *        read 触发），false 时在 OUT setup 上 stall（用 0 长 write 触发）。
   */
  void Stall(bool device_to_host);

 private:
  int ep0_fd_;  ///< ep0 端点 fd；ep0 线程在构造时快照。
};

// ---- 三条阻塞线程 ----

/**
 * @brief USB 数据面线程注入的策略回调集合。
 *
 * 全部回调都在对应的数据面线程上执行：on_class_request 与 on_ep0_closed
 * 在 ep0 线程，on_output_report 与 on_output_closed 在 OUT 线程。回调里
 * 不得长时间阻塞——那会停掉整条 ep0/OUT 通道。
 */
struct Handlers {
  /// TYPE_CLASS SETUP 到达时回调；应答经随附的 responder 完成，它只在
  /// 本轮回调内有效。
  std::function<void(const usb_ctrlrequest&, Ep0Responder&)> on_class_request;
  /// OUT 端点收到报告时回调（缓冲区首字节即 report ID）。
  std::function<void(const uint8_t* data, size_t len)> on_output_report;
  /// ep0 读通道关闭或出错时回调（控制面已不可用）。
  std::function<void()> on_ep0_closed;
  /// OUT 端点单独坏掉或 EOF 时回调，给上层统一退出的机会。
  std::function<void()> on_output_closed;
};

/**
 * @brief 拉起 IN/ep0/OUT 三条数据面线程。
 *
 * 落下链路状态与发送节奏、复位诊断计数（持有的输入报告不由本函数播种也不
 * 清空，第一份由 bridge::Init() 发布并一直保留到下一次覆盖），再安装 SIGUSR1
 * 空处理函数（StopThreads 靠它打断三个阻塞点），最后启动线程。
 *
 * @param handlers 策略回调，按值拷贝保存进模块单例。
 * @param report_interval IN 线程的发送节拍：两份重发之间的间隔（1/2/4 ms，
 *        即主机一侧的 1000/500/250 Hz）；0 = real-time 档，只在有新的发布时
 *        写一份。
 * @details 重发档的节拍由本模块按墙上时钟自己排：dummy_hcd 不兑现描述符里
 *        中断端点的 bInterval（它的 scan_urb 对 PIPE_INTERRUPT 就写着"polls
 *        too fast"），主机取完一份立刻重新排队，所以阻塞写本身不构成节奏，
 *        不排就会把手柄的一份状态以几千帧每秒灌给主机。
 * @return 全部启动 true；已有数据面在跑或任一线程起不来 false——失败时
 *         已就地调用 StopThreads 收尾。
 * @note 每进程单实例；重复调用被拒绝而不是叠加线程。
 * @note 合法取值由配置层收口（daemon/config.h 的 kUsbReportIntervals），
 *       本模块只按"0 或正整数毫秒"解释它。
 * @see StopThreads —— 对称收尾。
 */
bool StartThreads(const Handlers& handlers,
                  std::chrono::milliseconds report_interval);

/**
 * @brief 停掉三条数据面线程并 join。
 *
 * 置 running=false、广播条件变量，然后向每个仍可能阻塞在 FunctionFS 调用
 * 里的线程发 SIGUSR1（这些等待都可被信号打断），再依次 join。
 *
 * @note 只能从主线程调用；重复调用安全（线程不可 joinable 时跳过）。
 * @see StartThreads。
 */
void StopThreads();

}  // namespace usb
