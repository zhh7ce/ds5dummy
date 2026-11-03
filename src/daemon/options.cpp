/**
 * @file daemon/options.cpp
 * @brief worker 交接参数的解析与 argv 重建实现
 * @details 认得的名字集中在 kOpts 一张表：解析器查表判定"是不是选项、吃不吃
 *          一个值"，字段落位由 ParseArgv 的分支链完成，两张清单需要同步维护。
 *          用户级设置不在这里——它们来自配置文件，用法文本因此只打印两份配置
 *          的路径（见 internal::UsageText）。
 * @details 不负责：任何 syscall（本模块只处理字符串）、配置的消费方。
 * @note 层次：daemon。线程模型：进程启动时单线程调用；无全局可变态，
 *       kOpts 是 const 表。
 */

#include "daemon/options.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "daemon/config.h"

namespace ds5::daemon {
namespace {

/// @brief 选项形态：带值的开关，或不带值的开关。决定解析时是否消费下一个参数。
enum class Kind { Switch, Value };

/**
 * @brief 单个内部交接选项的定义。
 * @details 解析（find_opt）查这一张表判定名字与形态；用法文本不再由它生成，
 *          因为对用户只暴露配置文件。成员都是字符串字面量与枚举值，生命周期
 *          为整个程序。
 */
struct OptDef {
  const char* name;  ///< 长选项带 "--"，短选项带 "-"；必须与命令行上的写法逐字相同
  Kind kind;         ///< 是否消费下一个参数作为值
};

// 单一表源：解析器只认这里列出的名字；新增选项必须在这里与 ParseArgv 的
// 分支链里同时出现，否则会被"识别但不生效"。
const OptDef kOpts[] = {
    {"--bt-addr", Kind::Value},
    {"--bt-wait", Kind::Value},
    {"--worker", Kind::Switch},
    {"--ep0", Kind::Value},
    {"--ep-in", Kind::Value},
    {"--ep-out", Kind::Value},
    {"--no-audio", Kind::Switch},
    {"--usb-edge", Kind::Switch},
    {"--usb-report-interval-ms", Kind::Value},
    {"--haptics-gain", Kind::Value},
    {"--speaker-gain", Kind::Value},
    {"--sysfs-path", Kind::Value},
    {"-v", Kind::Value},
    {"--help", Kind::Switch},
};

/**
 * @brief 在 kOpts 里查名字对应的定义。
 * @param name 已含 "--"/"-" 前缀的选项字符串，逐字比较，不做缩写或大小写折叠。
 * @return 命中则指向表项（静态存储），否则 nullptr。
 * @note 纯查找，无副作用；表很短，线性扫描够用。
 */
const OptDef* find_opt(const std::string& name) {
  for (const OptDef& d : kOpts) {
    if (name == d.name) return &d;
  }
  return nullptr;
}

/**
 * @brief 解析非负十进制整数，用于所有 Kind::Value 的数字选项。
 * @param text 待解析字符串。
 * @param out 成功时写入数值，范围 [0, INT_MAX]。
 * @return 成功 true；空串、含非数字尾串、溢出 long 或超出 int 范围 false。
 * @details 拒绝的是负数与尾随垃圾；strtol 本身仍会接受前导空格、单独的 '+'
 *          号和前导零（例如 " 5"、"+5"、"007" 都会解析成 5），本函数没有
 *          额外拦下它们。
 * @note 纯函数，无副作用；errno 只在调用局部使用，不透出。
 * @see internal::ParseVerbosity —— 那里才做了逐字符校验。
 */
bool parse_number(const std::string& text, int& out) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE) return false;
  // 上限即 INT_MAX：LP64 上 long 比 int 宽，必须自己拦掉 int 放不下的值。
  if (v < 0 || v > 2147483647L) return false;
  out = static_cast<int>(v);
  return true;
}

/**
 * @brief 解析 [kHapticsGainMin, kHapticsGainMax] 内的十进制浮点数。
 * @param text 待解析字符串。
 * @param out 成功时写入增益倍率。
 * @return 成功 true；空串、尾随垃圾、溢出、NaN/Inf 或越界 false。
 * @details 与 parse_number 同口径允许前导空格、'+' 和前导零；`.nan`/`.inf`
 *          这类 strtod 认得的写法由 isfinite 拦下——一个 NaN 增益会让整段
 *          触觉量化成 NaN。边界取自 config.h，配置解析与本函数不会各自演化。
 */
bool parse_gain(const std::string& text, float& out) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || errno == ERANGE) return false;
  if (!std::isfinite(v) || v < static_cast<double>(kHapticsGainMin) ||
      v > static_cast<double>(kHapticsGainMax)) {
    return false;
  }
  out = static_cast<float>(v);
  return true;
}

/**
 * @brief 把浮点增益写成能被 parse_gain 原样读回的最短十进制串。
 * @param gain 已解析的增益值。
 * @return 十进制文本；float 的最短往返表示不会超过 24 字符。
 * @details 不能用 std::to_string：它固定 6 位小数，0.123456789f 会写成
 *          0.123457，往返后不是同一个 float，违反 BuildWorkerArgv 的互逆契约。
 */
std::string format_gain(float gain) {
  char buf[32];
  const auto result = std::to_chars(buf, buf + sizeof buf, gain);
  return std::string(buf, result.ptr);
}

}  // namespace

/**
 * @brief 逐个参数查 kOpts 表并落进 Options 字段，出错即填 error 返回。
 * @param args 不含 argv[0] 的参数序列。
 * @return 配置结果；error 非空时后面参数不再解析，其余字段不可信。
 * @note 表里新增一项而这里没有对应分支时，该选项会被"识别但不生效"：
 *       名字合法、值被消费、字段保持默认。加选项时必须同时改两处。
 * @see BuildWorkerArgv —— 与本函数互逆。
 */
Options internal::ParseArgv(const std::vector<std::string>& args) {
  Options o;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& name = args[i];
    const OptDef* def = find_opt(name);
    if (def == nullptr) {
      o.error = "Unknown option: " + name;
      return o;
    }

    // 带值选项必须还能向后吃一个参数；i+1 越界即"末尾缺值"。
    std::string value;
    if (def->kind == Kind::Value) {
      if (i + 1 >= args.size()) {
        o.error = "Option " + name + " requires a value";
        return o;
      }
      value = args[++i];
    }

    if (name == "--bt-addr") {
      o.bt_addr = value;
    } else if (name == "--sysfs-path") {
      // 路径由 Manager 从 sysfs 读回，worker 只是转交给 PipeWire 属性，
      // 这里不做存在性校验：校验点在解析成功之前已经过了，再查一遍只会
      // 把"接口目录被拆了"这种运行期变化当成参数错误。
      o.host_sysfs_path = value;
    } else if (name == "-v") {
      int level = 0;
      if (!internal::ParseVerbosity(value, level)) {
        o.error =
            "Option -v needs OFF/FATAL/ERROR/WARNING/INFO/DEBUG/VERBOSE "
            "or an integer, got: " +
            value;
        return o;
      }
      o.verbosity = level;
    } else if (name == "--worker") {
      o.worker = true;
    } else if (name == "--no-audio") {
      o.no_audio = true;
    } else if (name == "--usb-edge") {
      o.usb_edge = true;
    } else if (name == "--usb-report-interval-ms") {
      // 与 YAML 那一侧同一张取值表：worker 拿到的是配置校验过的同一个值。
      int interval = 0;
      if (!parse_number(value, interval) ||
          std::ranges::find(kUsbReportIntervals, interval) ==
              std::ranges::end(kUsbReportIntervals)) {
        o.error = "Option --usb-report-interval-ms needs " +
                  UsbReportIntervalChoices() + ", got: " + value;
        return o;
      }
      o.usb_report_interval_ms = interval;
    } else if (name == "--haptics-gain") {
      float gain = 0.0f;
      if (!parse_gain(value, gain)) {
        o.error = "Option --haptics-gain needs a number in [0, 2], got: " +
                  value;
        return o;
      }
      o.haptics_gain = gain;
    } else if (name == "--speaker-gain") {
      int gain = 0;
      if (!parse_number(value, gain) || gain > kSpeakerGainMax) {
        o.error = "Option --speaker-gain needs an integer in [0, " +
                  std::to_string(kSpeakerGainMax) + "], got: " + value;
        return o;
      }
      o.speaker_gain = gain;
    } else if (name == "--help") {
      o.show_help = true;
    } else if (name == "--bt-wait" || name == "--ep0" || name == "--ep-in" ||
               name == "--ep-out") {
      // 四个数字选项共用一次严格校验，再按名字分发到各自的字段。
      int parsed = 0;
      if (!parse_number(value, parsed)) {
        o.error =
            "Option " + name + " needs a non-negative number, got: " + value;
        return o;
      }
      if (name == "--bt-wait") o.bt_wait_s = parsed;
      if (name == "--ep0") o.ep0_fd = parsed;
      if (name == "--ep-in") o.ep_in_fd = parsed;
      if (name == "--ep-out") o.ep_out_fd = parsed;
    }
  }

  // worker 的三个 fd 号只能由 Manager 写入，缺任何一个都意味着交接被手工
  // 拼出来的命令行绕过了——那种运行没有可用的数据面。
  if (o.worker && (o.ep0_fd < 0 || o.ep_in_fd < 0 || o.ep_out_fd < 0)) {
    o.error = "--worker requires --ep0, --ep-in and --ep-out";
  }
  return o;
}

/**
 * @brief 剥掉 argv[0] 后交给 internal::ParseArgv。
 * @param argc main 的参数个数；<=0 时按空参数表处理。
 * @param argv main 的参数数组；每个元素必须是非空 C 字符串（std::string
 *             构造会解引用它们）。
 * @return 配置结果；失败时 error 非空。
 * @note 会分配内存，只在 main 的启动路径调用。
 */
Options ParseArgs(int argc, char* argv[]) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  return internal::ParseArgv(args);
}

/**
 * @brief 把配置重写成 worker 能吃的一套 argv（与 internal::ParseArgv 互逆）。
 * @param o 已解析的配置；ep*_fd 须已是 worker 侧的目标号（负值会产出
 *          "--ep0 -1"，解析端判非法）。
 * @param self_path argv[0]，通常是 /proc/self/exe 的读回值。
 * @return 完整 argv。固定槽位（--worker、三个 fd 号、--bt-wait、-v）总是写出，
 *         其余字段只在非默认时写出。
 * @note --help 与 error 是解析期产物，不回写。
 * @see ToArgv —— 紧接着把它变成 execv 需要的指针数组。
 */
std::vector<std::string> BuildWorkerArgv(const Options& o,
                                         const std::string& self_path) {
  std::vector<std::string> av{self_path,    "--worker",
                              "--ep0",      std::to_string(o.ep0_fd),
                              "--ep-in",    std::to_string(o.ep_in_fd),
                              "--ep-out",   std::to_string(o.ep_out_fd),
                              "--bt-wait",  std::to_string(o.bt_wait_s)};
  // 可选段：与解析端的默认值一一对应，等于默认就不写，保持命令行简短。
  if (!o.bt_addr.empty()) av.insert(av.end(), {"--bt-addr", o.bt_addr});
  if (!o.host_sysfs_path.empty()) {
    av.insert(av.end(), {"--sysfs-path", o.host_sysfs_path});
  }
  av.insert(av.end(), {"-v", std::to_string(o.verbosity)});
  if (o.no_audio) av.push_back("--no-audio");
  if (o.usb_edge) av.push_back("--usb-edge");
  if (o.usb_report_interval_ms != 1) {
    av.insert(av.end(), {"--usb-report-interval-ms",
                         std::to_string(o.usb_report_interval_ms)});
  }
  if (o.haptics_gain != 1.0f) {
    av.insert(av.end(), {"--haptics-gain", format_gain(o.haptics_gain)});
  }
  if (o.speaker_gain != 2) {
    av.insert(av.end(), {"--speaker-gain", std::to_string(o.speaker_gain)});
  }
  return av;
}

/**
 * @brief 转成 execv 要的 NULL 结尾指针数组，指针复用 av 的缓冲区。
 * @param av 字符串数组，必须比返回的指针数组活得久。
 * @return size == av.size()+1 的数组，末元素为 nullptr。
 * @warning const_cast 是必需的：execv 的签名带 const char* const*，而内核
 *          不会写这些缓冲区。
 * @warning 必须在 fork 之前调用（子进程不得再分配内存）。
 */
std::vector<char*> ToArgv(const std::vector<std::string>& av) {
  std::vector<char*> out;
  out.reserve(av.size() + 1);  // +1 留给末尾的 nullptr 哨兵
  for (const std::string& a : av) out.push_back(const_cast<char*>(a.c_str()));
  out.push_back(nullptr);
  return out;
}

/**
 * @brief 级别名或整数字符串 → loguru verbosity 数值。
 * @param text 级别名或整数串；空串非法。
 * @param out 成功时写入数值；失败时不写。
 * @return 成功 true。
 * @details 名字分支逐一对齐 loguru 的 Verbosity 枚举（OFF=-9、FATAL=-3、
 *          ERROR=-2、WARNING=-1、INFO=0、DEBUG=1、VERBOSE/TRACE=2）。
 *          整数分支先手写数字校验再走 strtol：strtol 单独用会接受前导空格、
 *          "+"、"0x" 前缀和尾随字符，这里一律拒绝；符号只允许一个前导 '-'。
 *          数值范围不额外夹取，超出 loguru 枚举的整数按原值透传。
 * @see kOpts 的 -v 说明列。
 */
bool internal::ParseVerbosity(const std::string& text, int& out) {
  if (text.empty()) return false;
  // 数值和 loguru 的 Verbosity 枚举一致（loguru.hpp 中 enum Verbosity）。
  if (text == "OFF") {
    out = -9;
    return true;
  }
  if (text == "FATAL") {
    out = -3;
    return true;
  }
  if (text == "ERROR") {
    out = -2;
    return true;
  }
  if (text == "WARNING") {
    out = -1;
    return true;
  }
  if (text == "INFO") {
    out = 0;
    return true;
  }
  if (text == "DEBUG") {
    out = 1;
    return true;
  }
  if (text == "VERBOSE" || text == "TRACE") {
    out = 2;
    return true;
  }
  // 整数串：允许一个前导 '-'，其余字符必须全是数字；起始下标据此跳过符号位。
  for (size_t i = (text[0] == '-' ? 1 : 0); i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  // 只剩符号（""或"-"）时上面那个循环一次都没跑，这里补一次"必须有数字"的检查。
  if (text.size() == (text[0] == '-' ? 1u : 0u)) return false;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || errno == ERANGE) return false;
  out = static_cast<int>(v);
  return true;
}

/**
 * @brief 级别数值 → 配置里写的级别名，供启动横幅直接展示。
 * @param level 级别数值，语义与 ParseVerbosity 的输出相同。
 * @return 静态存储的级别名；未知数值返回 nullptr。
 * @details 名字集合与 ParseVerbosity 的名字表逐项对应，两处必须一起改：新增
 *          一个可配置的级别名时，解析与命名各加一行。2 同时由 VERBOSE 与 TRACE
 *          解析而来，这里统一回写 VERBOSE——TRACE 只是别名，没有独立的日志语义。
 *          只按精确数值匹配、不做 loguru::Verbosity 的类型转换，整数级别因此
 *          不会被 int8_t 截断后冒充相邻级别。
 * @see internal::ParseVerbosity。
 */
const char* internal::VerbosityName(int level) {
  // case 值就是 ParseVerbosity 里写下的字面量；不在表内的整数一律返回 nullptr，
  // 由调用方打印数字，绝不做枚举转换后的近似匹配。
  switch (level) {
    case -9: return "OFF";
    case -3: return "FATAL";
    case -2: return "ERROR";
    case -1: return "WARNING";
    case 0: return "INFO";
    case 1: return "DEBUG";
    case 2: return "VERBOSE";
  }
  return nullptr;
}

/**
 * @brief 生成用法文本：一行用法加两份配置的路径。
 * @return 多行文本，首行固定 "Usage: ds5dummy [--worker]"，之后列出系统与
 *         用户配置的位置。
 * @details 不再列选项：命令行是 Manager 与 worker 之间的内部契约，对用户暴露
 *          的只有 YAML 配置。两条路径取自 daemon/config.h 的常量，与实际读取
 *          点同源，不会各写一份。
 * @note 首行的程序名是占位，展示前由 PrintUsage 换成实际 argv[0]。
 */
std::string internal::UsageText() {
  std::string text = "Usage: ds5dummy [--worker]\n";
  text += "Configuration:\n";
  text += std::string("  system: ") + kSystemConfigPath + "\n";
  text += std::string("  user:   ~/") + kUserConfigDir + "/" +
          kUserConfigFileName + "\n";
  return text;
}

/**
 * @brief 把 UsageText 首行的程序名换成实际 argv[0] 后打到 stdout。
 * @param prog 程序名；nullptr 时回落 "ds5dummy"（空串会原样输出空名）。
 * @note 副作用：写 stdout（用法属于正常输出）。
 * @see internal::UsageText。
 */
void PrintUsage(const char* prog) {
  std::string u = internal::UsageText();
  // 只替换首行：find('\n') 是行尾位置，replace 到该位置为止，换行符本身留下。
  const size_t nl = u.find('\n');
  u.replace(0, nl,
            std::string("Usage: ") + (prog == nullptr ? "ds5dummy" : prog));
  std::fputs(u.c_str(), stdout);
}

}  // namespace ds5::daemon
