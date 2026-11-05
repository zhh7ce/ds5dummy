/**
 * @file daemon/options.cpp
 * @brief 命令行解析与 worker 配置快照的编解码实现
 * @details 命令行只剩 `--worker` 与 `--help`；配置的跨进程传递是"定长标量头 +
 *          长度前缀的字符串与插件条目"这一份自描述快照，由 Manager 编码、
 *          worker 解码，两侧共用这里的同一个函数对。
 * @details 不负责：任何 syscall（快照的 memfd 读写属于 handoff）、配置的消费方、
 *          取值合法性（配置层在更早的一步收口）。
 * @note 层次：daemon。线程模型：进程启动时单线程调用；无全局可变态。
 */

#include "daemon/options.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "daemon/config.h"

namespace ds5::daemon {
namespace {

/// @brief 快照的魔数，逐字节 "DS5W"。只用来判定"这个 fd 里装的是我们的配置"。
constexpr uint8_t kMagic[4] = {'D', 'S', '5', 'W'};

/// @brief 固定标量头的长度：magic + total_bytes + 4 个 i32 + f32 + 2 个 u8 +
///        2 字节保留位。其后的字符串与插件条目全部变长。
constexpr size_t kHeaderBytes = 4 + 4 + 4 * 4 + 4 + 1 + 1 + 2;

/**
 * @brief 追加一个本机字节序的 u32。
 * @param out 目标字节串。
 * @param value 待写入数值。
 * @note 两侧是同一个镜像、同一种架构，不需要做字节序转换；字段逐个写入而不是
 *       memcpy 整个 struct，避免结构体内存布局与填充位进入线格式。
 */
void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  uint8_t bytes[sizeof value];
  std::memcpy(bytes, &value, sizeof bytes);
  out.insert(out.end(), bytes, bytes + sizeof bytes);
}

/// @brief 追加一个 i32，按无符号位型写入（两侧同构，负号原样保留）。
void PutI32(std::vector<uint8_t>& out, int value) {
  PutU32(out, static_cast<uint32_t>(value));
}

/// @brief 按位型追加一个 float。
void PutF32(std::vector<uint8_t>& out, float value) {
  uint32_t bits = 0;
  static_assert(sizeof bits == sizeof value, "float must be 32 bits");
  std::memcpy(&bits, &value, sizeof bits);
  PutU32(out, bits);
}

/// @brief 追加一个长度前缀（u32）+ 原始字节的字符串；允许 NUL 与空串。
void PutString(std::vector<uint8_t>& out, const std::string& text) {
  PutU32(out, static_cast<uint32_t>(text.size()));
  const auto* const begin = reinterpret_cast<const uint8_t*>(text.data());
  out.insert(out.end(), begin, begin + text.size());
}

/**
 * @brief 顺序读取快照的游标，所有读取都带剩余长度校验。
 * @note 只在 Options::Decode 里使用；任何越界都让读取返回 false，由调用方
 *       统一翻成一条错误文本。
 */
class Reader {
 public:
  /**
   * @param data 首字节；调用方保证 size 与之匹配。
   * @param size 可读字节数。
   */
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ReadU8(uint8_t& value) { return Take(&value, sizeof value); }

  bool ReadU32(uint32_t& value) {
    uint8_t bytes[sizeof value];
    if (!Take(bytes, sizeof bytes)) return false;
    std::memcpy(&value, bytes, sizeof bytes);
    return true;
  }

  bool ReadI32(int& value) {
    uint32_t raw = 0;
    if (!ReadU32(raw)) return false;
    value = static_cast<int>(static_cast<int32_t>(raw));
    return true;
  }

  bool ReadF32(float& value) {
    uint32_t raw = 0;
    if (!ReadU32(raw)) return false;
    std::memcpy(&value, &raw, sizeof value);
    return true;
  }

  /// @brief 读一个长度前缀字符串；长度不得超过剩余字节数。
  bool ReadString(std::string& value) {
    uint32_t length = 0;
    if (!ReadU32(length)) return false;
    if (length > size_ - offset_) return false;
    value.assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    return true;
  }

  /// @brief 是否已读到快照末尾（用于拒绝尾随字节）。
  bool Done() const { return offset_ == size_; }

 private:
  /// @brief 取走 n 字节；不足 n 时不动游标并返回 false。
  bool Take(uint8_t* destination, size_t n) {
    if (n > size_ - offset_) return false;
    std::memcpy(destination, data_ + offset_, n);
    offset_ += n;
    return true;
  }

  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
};

}  // namespace

/**
 * @brief 解析命令行：只认 `--worker` 与 `--help`。
 * @param argc main 的参数个数；<=0 时按空参数表处理。
 * @param argv main 的参数数组，argv[0] 为程序名，不计入选项。
 * @return 配置结果；未知选项写入 error，且不再解析后面的参数。
 * @note 用户级设置已经全部搬到 YAML：旧开关在这里一律是"未知选项"，不会留下
 *       一个被静默忽略的入口。
 */
Options ParseArgs(int argc, char* argv[]) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string name = argv[i];
    if (name == "--worker") {
      o.worker = true;
    } else if (name == "--help") {
      o.show_help = true;
    } else {
      o.error = "Unknown option: " + name;
      return o;
    }
  }
  return o;
}

/**
 * @brief 编码生效配置；布局见 options.h 的 Encode 说明。
 * @return 自描述字节串；total_bytes 在最后回填。
 */
std::vector<uint8_t> Options::Encode() const {
  const ControllerConfig& c = controller;
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + c.bt_addr.size() + c.host_sysfs_path.size());
  out.insert(out.end(), kMagic, kMagic + sizeof kMagic);
  PutU32(out, 0);  // total_bytes：整份写完后再回填，解码端用它对账。
  PutI32(out, verbosity);
  PutI32(out, c.bt_wait_s);
  PutI32(out, c.speaker_gain);
  PutI32(out, c.usb_report_interval_ms);
  PutF32(out, c.haptics_gain);
  out.push_back(c.audio ? 1 : 0);
  // usb_model 是 optional：0 = 未定（auto，正常只在测试/默认构造里出现），
  // 1 = 普通 DualSense，2 = Edge。Manager 交接前已解决 auto。
  uint8_t model_code = 0;
  if (c.usb_model) {
    model_code = *c.usb_model == core::device::ControllerModel::kEdge ? 2 : 1;
  }
  out.push_back(model_code);
  out.push_back(0);  // 保留位：留出对齐/未来加标量的位置，解码端跳过。
  out.push_back(0);
  PutString(out, c.bt_addr);
  PutString(out, c.host_sysfs_path);
  // 插件配置是有序 map：同一份配置重复编码得到逐字节相同的快照。
  PutU32(out, static_cast<uint32_t>(c.plugins.size()));
  for (const auto& [key, value] : c.plugins) {
    PutString(out, key);
    PutString(out, value);
  }
  const uint32_t total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 4, &total, sizeof total);
  return out;
}

/**
 * @brief 解码快照；只校验结构，不校验取值。
 * @param data 快照首字节。
 * @param size 快照长度。
 * @param why 失败时写入原因。
 * @return 结构完整 true；失败时本对象不被修改。
 * @details 四类拒绝：长度不足头、magic 不符、total_bytes 与实长不等（含截断与
 *          尾随数据）、字符串长度越过剩余字节。total_bytes 紧跟 magic，先对账再
 *          读其余字段，截断与追加都能在动任何字段之前拒掉。plugins 的条目数由
 *          数据本身界定，超出实际内容时按截断处理。
 */
bool Options::Decode(const uint8_t* data, size_t size, std::string& why) {
  if (data == nullptr || size < kHeaderBytes) {
    why = "worker config snapshot is truncated";
    return false;
  }
  if (std::memcmp(data, kMagic, sizeof kMagic) != 0) {
    why = "worker config snapshot has a bad magic";
    return false;
  }

  Reader reader(data + sizeof kMagic, size - sizeof kMagic);
  uint32_t total = 0;
  if (!reader.ReadU32(total)) {
    why = "worker config snapshot is truncated";
    return false;
  }
  if (total != size) {
    why = "worker config snapshot length does not match its header";
    return false;
  }

  int parsed_verbosity = 0;
  int bt_wait_s = 0;
  int speaker_gain = 0;
  int interval_ms = 0;
  float haptics_gain = 0.0f;
  uint8_t audio = 0;
  uint8_t model_code = 0;
  uint8_t reserved[2] = {0, 0};
  std::string bt_addr;
  std::string host_sysfs_path;
  uint32_t plugin_count = 0;

  if (!reader.ReadI32(parsed_verbosity) ||
      !reader.ReadI32(bt_wait_s) || !reader.ReadI32(speaker_gain) ||
      !reader.ReadI32(interval_ms) || !reader.ReadF32(haptics_gain) ||
      !reader.ReadU8(audio) || !reader.ReadU8(model_code) ||
      !reader.ReadU8(reserved[0]) || !reader.ReadU8(reserved[1]) ||
      !reader.ReadString(bt_addr) || !reader.ReadString(host_sysfs_path) ||
      !reader.ReadU32(plugin_count)) {
    why = "worker config snapshot is truncated";
    return false;
  }

  ControllerConfig parsed;
  if (model_code == 1) {
    parsed.usb_model = core::device::ControllerModel::kDualSense;
  } else if (model_code == 2) {
    parsed.usb_model = core::device::ControllerModel::kEdge;
  } else if (model_code != 0) {
    // 编码端只会写 0/1/2；其余值说明快照已损坏，不是"未知型号"。
    why = "worker config snapshot has a bad usb model";
    return false;
  }

  plugin::Settings plugins;
  for (uint32_t i = 0; i < plugin_count; ++i) {
    std::string key;
    std::string value;
    if (!reader.ReadString(key) || !reader.ReadString(value)) {
      why = "worker config snapshot is truncated";
      return false;
    }
    plugins[std::move(key)] = std::move(value);
  }
  if (!reader.Done()) {
    why = "worker config snapshot has trailing bytes";
    return false;
  }

  parsed.bt_wait_s = bt_wait_s;
  parsed.speaker_gain = speaker_gain;
  parsed.usb_report_interval_ms = interval_ms;
  parsed.haptics_gain = haptics_gain;
  parsed.audio = audio != 0;
  parsed.bt_addr = std::move(bt_addr);
  parsed.host_sysfs_path = std::move(host_sysfs_path);
  parsed.plugins = std::move(plugins);
  verbosity = parsed_verbosity;
  controller = std::move(parsed);
  return true;
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
 * @return 多行文本，首行固定 "Usage: ds5dummy [--help]"，之后列出系统与
 *         用户配置的位置。
 * @details 不再列选项：对用户暴露的只有 YAML 配置；`--worker` 是 Manager 与
 *          worker 之间的内部入口，不写进用法。两条路径取自 daemon/config.h 的
 *          常量，与实际读取点同源，不会各写一份。
 * @note 首行的程序名是占位，展示前由 PrintUsage 换成实际 argv[0]。
 */
std::string internal::UsageText() {
  std::string text = "Usage: ds5dummy [--help]\n";
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
