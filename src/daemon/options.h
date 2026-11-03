/**
 * @file daemon/options.h
 * @brief worker 交接用的内部命令行编解码
 * @details 用户级设置全部来自 YAML 配置（见 daemon/config.h），命令行只剩
 *          Manager 写、worker 读的那一组交接参数：三个继承 fd 号加上由配置
 *          推导出的生效值。解析结果与 argv 重建互为逆运算，是特权分离/交接里
 *          唯一能被单测完整覆盖的一环。本模块刻意不碰任何 syscall。
 * @details 不负责：配置文件的读取与身份选择（config / identity 模块）、参数
 *          的落地使用（各模块自己读 Options 字段）、fd 交接本身（handoff 模块）、
 *          日志级别的应用（main 与 Manager 写 loguru）。
 * @note 层次：daemon。线程模型：只在进程启动时单线程调用一次。
 */

#pragma once

#include <string>
#include <vector>

namespace ds5::daemon {

/**
 * @brief 解析后的进程配置，一个进程一生一份。
 * @details 默认值即 worker 侧"还没交接"的形态：worker 关闭、三个继承 fd 号为
 *          -1。ep*_fd 在 Manager 解析时保持 -1，由 Manager 在交接前显式写入目标
 *          fd 号（handoff 固定 3/4/5），再经 BuildWorkerArgv 变成命令行传下去
 *          ——所以 worker 解析到的号就是 handoff 挪过去的号。用户级设置
 *          （MAC、等待秒数、音频开关、两个增益、日志级别）由 Manager 从
 *          配置文件读出后写进这里交接，worker 自己不读配置文件。另有一个
 *          不来自配置、只来自本运行时刻的字段 host_sysfs_path：Manager 建站
 *          时从 sysfs 解析到的 USB 接口目录，同样经 argv 单向传给 worker。USB
 *          报告的发送间隔整个交下去：worker 的 IN 线程要靠它自己按这个间隔发，
 *          描述符里的 bInterval 只是给主机看的声明。
 * @note 纯数据，无行为；拷贝即配置快照。error 非空表示解析失败，其余字段不应
 *       被使用。
 * @see ParseArgs, BuildWorkerArgv。
 */
struct Options {
  bool worker = false;  ///< --worker：降权子进程入口，不建 gadget
  bool show_help = false;  ///< --help：只打印用法，不进入任何流程
  bool no_audio = false;   ///< --no-audio：不建 PipeWire sink，只桥接 HID
  /// --usb-edge：虚拟 USB 设备按 DualSense Edge 呈现（054c:0df2 + 453 字节
  /// report descriptor）。Manager 按发现阶段判出的型号置位；worker 侧只用来选
  /// ep0 应答的描述符，默认 false 即普通 DualSense。
  bool usb_edge = false;
  std::string bt_addr;     ///< --bt-addr：桥接必需的控制器 BD_ADDR，空=未指定
  /// -v：解析后的 loguru stderr 级别（越大越啰嗦）。刻意自己解析而不是让
  /// loguru::init 扫 argv——它遇到不认识的值是 CHECK_F 失败直接 abort 进程，
  /// 而 -v DEBUG 这种写法太自然了，必须是普通报错。
  int verbosity = 0;
  int bt_wait_s = 60;  ///< --bt-wait：worker 重试 BT 连接的秒数；0 = 不等
  /// --haptics-gain：触觉振幅倍率，[config::kHapticsGainMin,
  /// kHapticsGainMax]；1.0 = 原样。乘在编码线程的 int8 量化之前。
  float haptics_gain = 1.0f;
  /// --speaker-gain：控制器侧扬声器前置增益，[0, config::kSpeakerGainMax]；
  /// 2 是固件初值。写进 SetStateData 的 byte[37] 低 3 位，由手柄放大。
  int speaker_gain = 2;
  /// --usb-report-interval-ms：虚拟手柄两份 USB 输入报告之间的间隔（毫秒），
  /// 取值只能是 config.h 的 kUsbReportIntervals。1/2/4 是重发档——IN 线程按
  /// 这个间隔把持有的那一份反复发出去；0 是 real-time 档，只在新的蓝牙样本
  /// 到达时写一份。同一个数值在 Manager 建站时还写进描述符的 bInterval，
  /// 但那个字节不构成节奏——dummy_hcd 不兑现它，真实节奏只能由 worker 保证。
  int usb_report_interval_ms = 1;
  /// --sysfs-path：本站在主机侧被枚举出来的 USB 接口目录，相对 /sys（形如
  /// `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`）。Manager 建站时解析，
  /// worker 只把它写进 PipeWire sink 的 device.sysfs.path；空 = 没解析出来，
  /// worker 不写这个属性。worker 自己无法解析：那是 root 侧 configfs 的知识。
  std::string host_sysfs_path;

  /// --ep0/--ep-in/--ep-out：worker 侧继承到的 fd 号。Manager 解析时保持 -1，
  /// 由 BuildWorkerArgv 的调用方在交接前写入目标号（handoff 固定 3/4/5）。
  int ep0_fd = -1;     ///< ep0 fd 号
  int ep_in_fd = -1;   ///< interrupt-IN fd 号
  int ep_out_fd = -1;  ///< interrupt-OUT fd 号

  std::string error;  ///< 空 = 解析成功；非空是给用户看的错误文本
};

/**
 * @brief 解析完整 argc/argv，内部跳过 argv[0] 后交给 internal::ParseArgv。
 * @param argc main 的参数个数；<=0 时按空参数表处理。
 * @param argv main 的参数数组，argv[0] 为程序名，不计入选项。
 * @return 配置结果；失败时 Options.error 非空且其余字段不可信。
 * @note 未知选项与缺值都通过 error 报告，不打印、不退出，由调用方决定用法提示。
 * @see internal::ParseArgv（直接喂字符串数组的入口）。
 */
Options ParseArgs(int argc, char* argv[]);

/**
 * @brief 由 o 重建一份可被 internal::ParseArgv 原样读回的 argv。
 * @param o 已解析的配置。ep*_fd 必须已是 worker 侧的目标号，否则产出的
 *          "--ep0 -1" 之类的负值会被解析器判为非法。
 * @param self_path 作为 argv[0] 的可执行文件路径。
 * @return 完整 argv（含 argv[0]）。
 * @details 用于 Manager 把配置交接给 fork+execv 的 worker：产出与解析互为逆
 *          运算，因此 worker 与 Manager 对同一次运行看到同一份配置。
 * @note show_help 与 error 是解析期产物，不参与重建；--worker 一律写入。
 */
std::vector<std::string> BuildWorkerArgv(const Options& o,
                                         const std::string& self_path);

/**
 * @brief 把字符串数组转成 execv 需要的、NULL 结尾的 char* 数组。
 * @param av 字符串数组。
 * @return NULL 结尾的指针数组。
 * @warning 返回的指针指向 av 内各元素的缓冲区，故 av 必须比返回值活得久。
 * @warning 必须在 fork 之前调用（子进程不得再分配内存）。
 */
std::vector<char*> ToArgv(const std::vector<std::string>& av);

/**
 * @brief 用法文本本体（不含程序名）。
 * @return 多行用法说明。
 * @warning 本项目只提供 internal::UsageText 的定义，命名空间直下的这个声明
 *          没有实现，调用它会链接失败。
 * @see internal::UsageText。
 */
std::string UsageText();

/**
 * @brief 把用法文本连同程序名打到 stdout。
 * @param prog 程序名；nullptr 时回落 "ds5dummy"。
 * @note 副作用：写 stdout。用法属于正常输出，走 stdout 而非 stderr。
 * @see internal::UsageText。
 */
void PrintUsage(const char* prog);

// ---------------------------------------------------------------------------
// 实现细节 — core 之外只有 tests/options_test.cpp 直接使用
// ---------------------------------------------------------------------------

namespace internal {

/**
 * @brief 解析 args（不含 argv[0]）。
 * @param args 参数列表，已剥掉程序名。
 * @return 配置结果；失败时 Options.error 非空且首个错误即返回。
 * @details 与 BuildWorkerArgv 互为逆运算：后者产出的 argv 必须能被本函数原样
 *          读回。main 走 ParseArgs；本函数留作直接喂字符串数组的入口（测试用）。
 * @note 选项名由表判定，字段落位由本函数内的分支链判定，两处需同步维护。
 */
Options ParseArgv(const std::vector<std::string>& args);

/**
 * @brief 把 loguru 的级别名或整数字符串解析成写入 g_stderr_verbosity 的数值。
 * @param text 级别名（OFF/FATAL/ERROR/WARNING/INFO/DEBUG/VERBOSE/TRACE）或
 *             整数串；空串非法。
 * @param out 成功时写入解析出的数值；失败时不写。
 * @return 成功 true；不认识的值 false。
 * @details 数值与 loguru::Verbosity 枚举逐项对应（OFF=-9 起，TRACE=2 止），
 *          整数形式不做夹取，便于临时调到枚举之外的级别。配置文件的
 *          log_level 也复用本函数，两侧因此不会出现两套级别表。
 * @see ParseArgv 的 -v 分支。
 */
bool ParseVerbosity(const std::string& text, int& out);

/**
 * @brief 把 ParseVerbosity 解析出的数值还原成配置文件里能写的级别名。
 * @param level 级别数值，语义与 ParseVerbosity 的输出相同。
 * @return 静态存储的级别名（OFF/FATAL/ERROR/WARNING/INFO/DEBUG/VERBOSE）；
 *         数值不在这七个取值之内时返回 nullptr，调用方应改为打印数字。
 * @note 返回的七个名字与 ParseVerbosity 的数值一一对应，但不保留别名：TRACE 与
 *       VERBOSE 同为 2，这里只回 VERBOSE。返回值指向字符串字面量，调用方不负责
 *       释放，也不得写入。
 * @see ParseVerbosity —— 逆向的解析入口。
 */
const char* VerbosityName(int level);

/**
 * @brief 用法文本本体（不含程序名）：一句话说明加两份配置的路径。
 * @return 多行用法说明，首行是 "Usage: ds5dummy [--worker]"。
 * @details 选项表不再出现在用法里——对用户暴露的只有配置文件，命令行参数是
 *          Manager 与 worker 之间的内部契约。两份路径来自 daemon/config.h 的
 *          常量，与真正的读取点同源。
 * @see PrintUsage —— 会替换首行的程序名。
 */
std::string UsageText();

}  // namespace internal

}  // namespace ds5::daemon
