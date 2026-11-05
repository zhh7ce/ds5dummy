/**
 * @file daemon/options.h
 * @brief 进程命令行与 worker 配置快照的编解码
 * @details 用户级设置全部来自 YAML 配置（见 daemon/config.h），命令行只剩两个
 *          用户可见开关：`--worker` 与 `--help`。Manager 与 worker 之间的配置
 *          不走命令行：Manager 把生效值编成一份变长二进制快照（定长标量头 +
 *          长度前缀的字符串与插件条目），写进一个 memfd 经 handoff 交下去，
 *          worker 的新镜像读回同一份快照。两侧共享同一份结构定义，因此不需要
 *          ABI 版本号，也不存在"把已解析的配置展平再解析"的往返。
 * @details 不负责：配置文件的读取与身份选择（config / identity 模块）、参数
 *          的落地使用（各模块自己读 Options 字段）、fd 交接本身（handoff 模块）、
 *          日志级别的应用（main 与 Manager 写 loguru）。
 * @note 层次：daemon。线程模型：只在进程启动时单线程调用一次。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "daemon/config.h"

namespace ds5::daemon {

/**
 * @brief 解析后的进程配置，一个进程一生一份。
 * @details 数据面设置整份放在 controller 里，类型就是配置层的 ControllerConfig：
 *          Manager 把叠加好的每站设置直接赋进去，worker 解出来的也是同一个
 *          结构，两侧不维护第二份字段表，也不做改名或取反。这里只额外放四个
 *          进程级字段：命令行角色、用法开关、解析错误与日志级别。
 *          Manager 在拷贝之前已经把 controller 里的运行期事实填齐——bt_addr、
 *          host_sysfs_path，以及用发现结果解决掉 auto 之后的 usb_model。
 * @note error 非空表示解析失败，其余字段不应被使用。Encode/Decode 是它唯一的
 *       行为，只负责跨 exec 的字节化与还原，不解释任何字段含义。
 * @see ParseArgs。
 */
struct Options {
  bool worker = false;     ///< --worker：降权子进程入口，不建 gadget
  bool show_help = false;  ///< --help：只打印用法，不进入任何流程
  /// loguru stderr 级别（越大越啰嗦）。Manager 从配置读出后写进快照，worker
  /// 的新镜像直接沿用——execv 会重置 loguru 的全部状态，这个值必须跟着快照走。
  int verbosity = 0;
  /// 本站生效的手柄级设置。字段含义与默认值见 daemon/config.h 的
  /// ControllerConfig；Manager 在 StartSlot 里把 bt_addr、host_sysfs_path 与
  /// 解决掉 auto 的 usb_model 填齐后整份拷进来，两侧不再逐字段对照。
  ControllerConfig controller;

  std::string error;  ///< 空 = 解析成功；非空是给用户看的错误文本

  /**
   * @brief 把 verbosity 与 controller 编成交给 worker 的二进制快照。
   * @return 自描述的字节串：固定标量头 + 长度前缀的字符串与插件条目。
   * @details 布局（全部本机字节序，两侧是同一个镜像）：
   *          - `u32 magic`（ASCII "DS5W"）
   *          - `u32 total_bytes`（整份快照的长度，解码端据此拒绝截断与尾随数据）
   *          - `i32 verbosity`、`i32 bt_wait_s`、`i32 speaker_gain`、
   *            `i32 usb_report_interval_ms`、`f32 haptics_gain`
   *          - `u8 audio`、`u8 usb_model`（0 = 未定，1 = 普通，2 = Edge）、
   *            `u8 reserved[2]`
   *          - `u32 bt_addr_len` + 字节、`u32 sysfs_len` + 字节
   *          - `u32 plugin_count`，随后每项 `u32 key_len` + 字节、
   *            `u32 value_len` + 字节。
   *          plugins 是有序 map，同一份配置重复编码得到逐字节相同的快照。
   *          worker / show_help / error 是进程内产物，不编码。
   * @note 只定长的是头里的标量；字符串与插件条目完全变长，插件键的数量没有上限。
   * @see Decode。
   */
  std::vector<uint8_t> Encode() const;

  /**
   * @brief 从快照补回 verbosity 与 controller。
   * @param data 快照首字节，来自交接下来的 memfd。
   * @param size 快照长度（memfd 的 st_size）。
   * @param why 失败时写入面向用户的整句错误原因。
   * @return 结构完整 true；magic 不符、长度不一致、字符串越界或尾随字节 false。
   * @details 只写被编码的那两个字段；worker / show_help / error 保持原值（调用方
   *          在读取快照前已经用 ParseArgs 填好了它们）。失败时本对象不被修改。
   *          只做结构校验，不做取值校验：快照由同一个镜像的 Encode 写出，配置层
   *          在更早的一步已经把取值收口。
   * @note 会分配内存（字符串与 map），只在 worker 启动路径调用。
   * @see Encode。
   */
  bool Decode(const uint8_t* data, size_t size, std::string& why);
};

/**
 * @brief 解析命令行：只认 `--worker` 与 `--help`，跳过 argv[0]。
 * @param argc main 的参数个数；<=0 时按空参数表处理。
 * @param argv main 的参数数组，argv[0] 为程序名，不计入选项。
 * @return 配置结果；失败时 Options.error 非空且其余字段不可信。
 * @note 未知选项通过 error 报告，不打印、不退出，由调用方决定用法提示。用户级
 *       设置已经全部搬到 YAML，旧命令行开关因此落到"未知选项"而不是被静默忽略。
 */
Options ParseArgs(int argc, char* argv[]);

/**
 * @brief 把用法文本连同程序名打到 stdout。
 * @param prog 程序名；nullptr 时回落 "ds5dummy"。
 * @note 副作用：写 stdout。用法属于正常输出，走 stdout 而非 stderr。
 * @see internal::UsageText。
 */
void PrintUsage(const char* prog);

// ---------------------------------------------------------------------------
// 实现细节 — daemon 模块之外只有 tests/options_test.cpp 直接使用
// ---------------------------------------------------------------------------

namespace internal {

/**
 * @brief 把 loguru 的级别名或整数字符串解析成写入 g_stderr_verbosity 的数值。
 * @param text 级别名（OFF/FATAL/ERROR/WARNING/INFO/DEBUG/VERBOSE/TRACE）或
 *             整数串；空串非法。
 * @param out 成功时写入解析出的数值；失败时不写。
 * @return 成功 true；不认识的值 false。
 * @details 数值与 loguru::Verbosity 枚举逐项对应（OFF=-9 起，TRACE=2 止），
 *          整数形式不做夹取，便于临时调到枚举之外的级别。配置文件的
 *          log_level 也复用本函数，两侧因此不会出现两套级别表。
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
 * @return 多行用法说明，首行是 "Usage: ds5dummy [--help]"。
 * @details 命令行不再向用户暴露配置项——对用户暴露的只有配置文件；`--worker`
 *          是 Manager 的内部入口，不列出来。两份路径来自 daemon/config.h 的
 *          常量，与真正的读取点同源。
 * @see PrintUsage —— 会替换首行的程序名。
 */
std::string UsageText();

}  // namespace internal

}  // namespace ds5::daemon
