/**
 * @file daemon/config.cpp
 * @brief 启动配置的 YAML 解析与加固读取实现
 * @details 解析器是严格模式：schema 之外的键、类型不符的值、以及跨字段约束
 *          （identity 的 mode: list 需要 users、bt_addr: list 需要 controllers）
 *          都报错，避免拼错后静默失效。文件读取全部不跟随符号链接，并校验
 *          普通文件、属主、权限与大小上限。失败原因沿 why 文本返回，本模块不写日志。
 * @details 不负责：日志输出、passwd 查询、身份选择策略、配置的消费。唯一的
 *          NSS 查询是白名单 gid 的存在性校验（getgrgid_r）：gid 写错时在降权前
 *          报配置错误，而不是等到 setgid 失败才发现。
 * @note 层次：daemon。线程模型：Manager 启动阶段单线程调用；无全局状态。
 */

#include "daemon/config.h"

#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "daemon/options.h"

namespace ds5::daemon {
namespace {

/// 配置大小上限（字节）：正常配置连同注释只有几十行，64 KiB 是宽松上限。
constexpr std::size_t kMaxConfigBytes = 65536;

/**
 * @brief usb_model 的文本 ↔ 型号映射，解析与横幅回显共用这一张表。
 * @details std::nullopt 代表 auto：不覆盖，由发现阶段按真手柄的 HID VID/PID
 *          定型号。
 */
struct UsbModelChoice {
  const char* text;  ///< 配置文本里的取值
  std::optional<core::device::ControllerModel> model;  ///< 对应型号
};

/// usb_model 的三个合法取值；顺序即 UsbModelChoices 的输出顺序。
constexpr UsbModelChoice kUsbModelChoices[] = {
    {"auto", std::nullopt},
    {"dualsense", core::device::ControllerModel::kDualSense},
    {"edge", core::device::ControllerModel::kEdge},
};

/**
 * @brief 文件描述符的 RAII 持有者。
 * @details 读取路径上有多个提前返回点（非普通文件、属主不符、读失败），用析构
 *          统一关闭，避免每条分支手写 close。所有权独占，因此禁止拷贝。
 * @note 仅本 TU 使用；fd_ 为 -1 表示空。
 */
class UniqueFd {
 public:
  UniqueFd() = default;
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  /**
   * @brief 接管一个已打开的 fd（-1 表示置空），先关闭旧值。
   * @param fd 新拥有的描述符编号；所有权转移给本对象。
   */
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

  /// 当前持有的 fd 编号；-1 表示空。
  int get() const noexcept { return fd_; }
  /// 是否持有有效 fd。
  bool valid() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;  ///< 拥有的描述符，-1 为空
};

/**
 * @brief 给错误文本拼上定位前缀："<where>: <detail>"。
 * @param where 出错位置（如 "system config: identity"）。
 * @param detail 具体原因。
 * @return 面向用户的单行错误文本。
 */
std::string At(const std::string& where, const std::string& detail) {
  return where + ": " + detail;
}

/**
 * @brief 校验映射的每个键都在本层 schema 里。
 * @param map 待检查的节点；调用方保证 IsMap()。
 * @param allowed 本层允许的键集合。
 * @param where 错误文本里的定位（如 "user config"）。
 * @param why 失败时写入"未知键 + 键名"。
 * @return 全部已知时 true。
 * @details 键必须标量：`? [a, b]` 这类复合键直接判错，而不是让后面的
 *          as<std::string>() 抛异常。未知键报错是防止拼错后静默失效。
 */
bool CheckKeys(const YAML::Node& map,
               std::initializer_list<std::string_view> allowed,
               const std::string& where, std::string& why) {
  for (auto it = map.begin(); it != map.end(); ++it) {
    const YAML::Node key = it->first;
    if (!key.IsScalar()) {
      why = At(where, "mapping keys must be scalars");
      return false;
    }
    const std::string text = key.Scalar();
    bool known = false;
    for (std::string_view candidate : allowed) {
      if (candidate == text) {
        known = true;
        break;
      }
    }
    if (!known) {
      why = At(where, "unknown key '" + text + "'");
      return false;
    }
  }
  return true;
}

/**
 * @brief 把标量节点读成字符串。
 * @param node 已确认存在的节点；Null 与容器都算类型错误。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入标量原文。
 * @param why 失败时的原因。
 * @return 成功 true。
 * @note YAML 的未加引号标量（如 `5`）也会按文本接受；配置里的字符串键不要求引号。
 */
bool ReadString(const YAML::Node& node, const std::string& where,
                const char* key, std::string& out, std::string& why) {
  if (!node.IsScalar()) {
    why = At(where, std::string("key '") + key + "' must be a string");
    return false;
  }
  out = node.Scalar();
  return true;
}

/**
 * @brief 把标量节点读成布尔值。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入结果。
 * @param why 失败时的原因。
 * @return 成功 true；容器或无法识别的标量（如 `yes please`）false。
 */
bool ReadBool(const YAML::Node& node, const std::string& where, const char* key,
              bool& out, std::string& why) {
  if (!node.IsScalar()) {
    why = At(where, std::string("key '") + key + "' must be a boolean");
    return false;
  }
  try {
    out = node.as<bool>();
  } catch (const YAML::Exception&) {
    why = At(where, std::string("key '") + key + "' must be a boolean");
    return false;
  }
  return true;
}

/**
 * @brief 把标量节点读成 64 位整数。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入数值。
 * @param why 失败时的原因。
 * @return 成功 true；容器、浮点、带尾随字符的标量（如 `12abc`）false。
 * @details 先按 long long 转换再交给调用方做范围检查，避免直接 as<int>()
 *          在不同平台上先溢出再截断。
 */
bool ReadInt(const YAML::Node& node, const std::string& where, const char* key,
             long long& out, std::string& why) {
  if (!node.IsScalar()) {
    why = At(where, std::string("key '") + key + "' must be an integer");
    return false;
  }
  try {
    out = node.as<long long>();
  } catch (const YAML::Exception&) {
    why = At(where, std::string("key '") + key + "' must be an integer");
    return false;
  }
  return true;
}

/**
 * @brief 读非负 int（bt_wait_s 用）。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入 [0, INT_MAX] 内的值。
 * @param why 失败时的原因。
 * @return 成功 true；负数或超出 int 范围 false。
 */
bool ReadNonNegativeInt(const YAML::Node& node, const std::string& where,
                        const char* key, int& out, std::string& why) {
  long long value = 0;
  if (!ReadInt(node, where, key, value, why)) return false;
  if (value < 0) {
    why = At(where, std::string("key '") + key + "' must not be negative");
    return false;
  }
  // 上限按 int 收口：下游 Options 的字段就是 int，超出会静默回绕。
  if (value > static_cast<long long>(std::numeric_limits<int>::max())) {
    why = At(where, std::string("key '") + key + "' is too large");
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

/**
 * @brief 把标量节点读成 [kHapticsGainMin, kHapticsGainMax] 内的触觉增益。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入增益倍率。
 * @param why 失败时的原因。
 * @return 成功 true；容器、非数字、NaN/Inf 或越界 false。
 * @details YAML 的 `.nan`/`.inf` 能通过 as<double>()，但一旦参与量化就会把整段
 *          触觉变成 NaN，夹取也救不回来，所以用 isfinite 单独拦下。
 */
bool ReadHapticsGain(const YAML::Node& node, const std::string& where,
                     const char* key, float& out, std::string& why) {
  if (!node.IsScalar()) {
    why = At(where, std::string("key '") + key + "' must be a number");
    return false;
  }
  double value = 0.0;
  try {
    value = node.as<double>();
  } catch (const YAML::Exception&) {
    why = At(where, std::string("key '") + key + "' must be a number");
    return false;
  }
  if (!std::isfinite(value) || value < static_cast<double>(kHapticsGainMin) ||
      value > static_cast<double>(kHapticsGainMax)) {
    why = At(where, std::string("key '") + key +
                        "' must be between 0.0 and 2.0");
    return false;
  }
  out = static_cast<float>(value);
  return true;
}

/**
 * @brief 把标量节点读成 [0, kSpeakerGainMax] 内的整数扬声器增益。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入线上字段值。
 * @param why 失败时的原因。
 * @return 成功 true；容器、非整数或越界 false。
 * @details 3 位字段放不下 8 以上；负数没有线上对应值，一并拒绝。
 */
bool ReadSpeakerGain(const YAML::Node& node, const std::string& where,
                     const char* key, int& out, std::string& why) {
  long long value = 0;
  if (!ReadInt(node, where, key, value, why)) return false;
  if (value < 0 || value > kSpeakerGainMax) {
    why = At(where, std::string("key '") + key + "' must be between 0 and " +
                      std::to_string(kSpeakerGainMax));
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

/**
 * @brief 把一个插件名之下的嵌套映射摊平成点分路径，就地合并进 settings。
 * @param node 已确认 IsMap() 的那一层。
 * @param where 错误定位。
 * @param path 这一层的完整扁平前缀（插件名，或插件名往下已拼好的若干段），也是
 *             写进 @p settings 的那些键的前缀；错误文本在它前面再加 `plugins.`，
 *             报成配置文件里的位置。
 * @param depth 本层写出的那些键在插件名以下的段数：插件名之下的第一层键是 1
 *              （@p path 此时就是插件名本身），往下每进一层加 1。
 * @param settings 入参是下层的配置，出参是叠加本层之后的结果。
 * @param why 失败时的原因。
 * @return 成功 true；某一层的键名不是 token、叶子不是非空标量、或嵌套超过
 *         kMaxPluginDepth 时 false。
 * @details 递归只做一件事：键名是 token 就把 `path + '.' + key` 往下带，遇到
 *          映射继续深入，遇到标量就写进 @p settings。段名不允许含 `.`（过
 *          plugin::IsToken），所以"写成嵌套"与"写成一个带点的键"不会撞成同一
 *          个扁平键——后者在这里直接被拒，扁平路径因此是唯一形态；下游按
 *          `插件名.` 前缀整段剥掉它（plugin::detail::BuildSettings），前缀匹配
 *          不会有第二处候选。
 *          空映射（`{}`）贡献零条键，不算错误：它与"这一组没写"同形。只写组名
 *          不写值（YAML 的 null）不算空映射，和叶子写空一样被拒。
 * @note 深度上限只收栈这一维：递归别名（`&a {b: *a}`）在 yaml-cpp 里能构出环，
 *       没有上限这里就会无限递归下去。条目总数另有配置文件的 64 KiB 上限兜着。
 * @see ReadPlugins —— 本函数唯一的调用方（插件名那一层）。
 */
bool FlattenPluginSettings(const YAML::Node& node, const std::string& where,
                           const std::string& path, int depth,
                           plugin::Settings& settings, std::string& why) {
  const std::string located = "plugins." + path;
  for (auto it = node.begin(); it != node.end(); ++it) {
    const YAML::Node key_node = it->first;
    if (!key_node.IsScalar()) {
      why = At(where, located + " keys must be setting names");
      return false;
    }
    const std::string key = key_node.Scalar();
    if (!plugin::IsToken(key)) {
      why = At(where, located + " key '" + key +
                         "' must be 1..32 characters of letters, digits, '_' "
                         "or '-' (nest deeper instead of writing a dotted key)");
      return false;
    }
    const std::string child = path + plugin::kKeySeparator + key;
    const YAML::Node value = it->second;
    if (value.IsMap()) {
      if (depth >= kMaxPluginDepth) {
        why = At(where, "plugins." + child + " nests deeper than " +
                        std::to_string(kMaxPluginDepth) +
                        " key levels under a plugin name");
        return false;
      }
      if (!FlattenPluginSettings(value, where, child, depth + 1, settings,
                                 why)) {
        return false;
      }
      continue;
    }
    if (!value.IsScalar() || value.Scalar().empty()) {
      why = At(where, "plugins." + child + " must be a non-empty scalar");
      return false;
    }
    settings[child] = value.Scalar();
  }
  return true;
}

/**
 * @brief 读一个插件层（`plugins:` 那一节）并就地合并进配置。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param settings 入参是下层的配置，出参是叠加本层之后的结果。
 * @param why 失败时的原因。
 * @return 成功 true；形状不对 false。
 * @details 只校验形状，不看键名含义：`plugins:` 的键属于各个插件，配置这一层
 *          认识它们就会把插件列表变成自己的一份依赖。规则只有两条——第一层是
 *          插件名（必须过 plugin::IsToken），它以下是**任意层**的嵌套映射，每
 *          一段名字都要过 IsToken，叶子必须是**非空标量**并原样存成文本。取值
 *          合不合插件的心意由插件自己判（写错取值的报错在 worker 日志里，不在
 *          启动前的配置校验里）。
 *          嵌套在读取时就摊平成点分路径（`buttons.remap.circle`），层数对下游
 *          不可见：扁平键只有插件名那一段被认出来（它以下整段原样带走），递给
 *          插件时按 `插件名.` 前缀整段剥掉（plugin::detail::BuildSettings），
 *          交接 argv 搬的还是 `名.键路径=值` 一条字符串。所以加深只发生在配置
 *          文件里，不发生在交接格式里；层数上限是 kMaxPluginDepth，收的是递归
 *          别名与超长路径，不是形状。
 *          合并是逐扁平键覆盖：本层写了 `buttons.axis.invert_rstick_x` 就替换
 *          下层的同名键，同组里没写的键、别的组与别的插件都原样留下。
 * @see FlattenPluginSettings —— 插件名以下那些层的递归。
 * @see plugin/plugin.h —— 扁平配置的形状与插件如何读它。
 */
bool ReadPlugins(const YAML::Node& node, const std::string& where,
                 plugin::Settings& settings, std::string& why) {
  if (node.IsNull()) return true;  // `plugins:` 只写键不写值 = 本层不覆盖
  if (!node.IsMap()) {
    why = At(where, "key 'plugins' must be a mapping of plugin name to "
                    "settings");
    return false;
  }
  for (auto it = node.begin(); it != node.end(); ++it) {
    const YAML::Node name_node = it->first;
    if (!name_node.IsScalar()) {
      why = At(where, "plugins keys must be plugin names");
      return false;
    }
    const std::string name = name_node.Scalar();
    if (!plugin::IsToken(name)) {
      why = At(where, "plugin name '" + name +
                      "' must be 1..32 characters of letters, digits, '_' "
                      "or '-'");
      return false;
    }
    const YAML::Node settings_node = it->second;
    if (!settings_node.IsMap()) {
      why = At(where, "plugins." + name + " must be a mapping of key to value");
      return false;
    }
    // 第一层键（插件名之下）就是 depth=1：上限数的是插件名以下的层数。
    if (!FlattenPluginSettings(settings_node, where, name, 1, settings, why)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief 读两份 USB 输入报告之间的间隔：0（real-time）或 1/2/4 毫秒。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入间隔；0 表示 real-time。
 * @param why 失败时的原因。
 * @return 成功 true；容器、非整数或不在四个取值内 false。
 * @details 取值表是 config.h 的 kUsbReportIntervals。这个毫秒数全程直用、
 *          不做换算：它既是全速中断端点 bInterval 的字节值，也是 IN 线程
 *          两份重发之间的间隔。0 不是间隔，是"只在收到新蓝牙样本时发一份"
 *          的模式，描述符仍取最快的 1 ms。表外的值要么落不到整数 bInterval
 *          上、要么语义未定义，不做就近取整——写错就报错。
 */
bool ReadUsbReportInterval(const YAML::Node& node, const std::string& where,
                           const char* key, int& out, std::string& why) {
  long long value = 0;
  if (!ReadInt(node, where, key, value, why)) return false;
  if (std::ranges::find(kUsbReportIntervals, value) ==
      std::ranges::end(kUsbReportIntervals)) {
    why = At(where, std::string("key '") + key + "' must be " +
                        UsbReportIntervalChoices());
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

/**
 * @brief 读虚拟 USB 设备的型号：auto（跟随发现结果）、dualsense 或 edge。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入型号；auto 写 std::nullopt。
 * @param why 失败时的原因。
 * @return 成功 true；容器或表外取值 false。
 * @details 取值表是 kUsbModelChoices。型号不是自由字符串：PID、产品串与
 *          report descriptor 是三个必须同源的量，只接受这两份编译期档案。
 */
bool ReadUsbModel(const YAML::Node& node, const std::string& where,
                  const char* key,
                  std::optional<core::device::ControllerModel>& out,
                  std::string& why) {
  std::string text;
  if (!ReadString(node, where, key, text, why)) return false;
  for (const UsbModelChoice& choice : kUsbModelChoices) {
    if (text == choice.text) {
      out = choice.model;
      return true;
    }
  }
  why = At(where, std::string("key '") + key + "' must be " +
                      UsbModelChoices() + ", got '" + text + "'");
  return false;
}

/**
 * @brief 在一个配置层之上应用手柄级键块（global: 或 controllers.<MAC>）。
 * @param node 键块节点；调用方保证非 Null。
 * @param where 错误定位（"user config: global" 或 "user config: controllers.<MAC>"）。
 * @param cfg 入参是下层（内置默认或 global）的快照，出参是叠加本层后的值。
 * @param why 失败时的原因。
 * @return 成功 true。
 * @details 只写出现的键，未出现的键保留入参里的值——三层叠加就是这样逐层
 *          覆盖出来的。允许的键与 worker 的交接参数一一对应，两个例外是
 *          usb_model（Manager 把它叠加成型号，再折成 `--usb-edge`）与 plugins
 *          （整份交给 worker，键的含义由插件自己读）。
 */
bool ApplyControllerKeys(const YAML::Node& node, const std::string& where,
                         ControllerConfig& cfg, std::string& why) {
  if (!node.IsMap()) {
    why = At(where, "must be a mapping");
    return false;
  }
  if (!CheckKeys(node,
                 {"bt_wait_s", "audio", "haptics_gain",
                  "speaker_gain", "usb_report_interval_ms", "usb_model",
                  "plugins"},
                 where, why)) {
    return false;
  }
  const YAML::Node bt_wait = node["bt_wait_s"];
  if (bt_wait && !ReadNonNegativeInt(bt_wait, where, "bt_wait_s", cfg.bt_wait_s,
                                     why)) {
    return false;
  }
  const YAML::Node audio = node["audio"];
  if (audio && !ReadBool(audio, where, "audio", cfg.audio, why)) return false;
  const YAML::Node haptics_gain = node["haptics_gain"];
  if (haptics_gain &&
      !ReadHapticsGain(haptics_gain, where, "haptics_gain", cfg.haptics_gain,
                       why)) {
    return false;
  }
  const YAML::Node speaker_gain = node["speaker_gain"];
  if (speaker_gain &&
      !ReadSpeakerGain(speaker_gain, where, "speaker_gain", cfg.speaker_gain,
                       why)) {
    return false;
  }
  const YAML::Node usb_report_interval = node["usb_report_interval_ms"];
  if (usb_report_interval &&
      !ReadUsbReportInterval(usb_report_interval, where,
                             "usb_report_interval_ms",
                             cfg.usb_report_interval_ms, why)) {
    return false;
  }
  const YAML::Node usb_model = node["usb_model"];
  if (usb_model &&
      !ReadUsbModel(usb_model, where, "usb_model", cfg.usb_model, why)) {
    return false;
  }
  const YAML::Node plugins = node["plugins"];
  if (plugins && !ReadPlugins(plugins, where, cfg.plugins, why)) {
    return false;
  }
  return true;
}

/**
 * @brief 把节点读成正的 uid/gid。
 * @tparam Id uid_t 或 gid_t，必须是无符号整数类型。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param key 键名，只用于错误文本。
 * @param out 成功时写入数值。
 * @param why 失败时的原因。
 * @return 成功 true；0、负数或超出 Id 表示范围 false。
 * @details 0 是 root，配置里一律拒绝；上限用 Id 自身的 numeric_limits 收口，
 *          避免 long long 转窄类型时截断。
 */
template <typename Id>
bool ReadPositiveId(const YAML::Node& node, const std::string& where,
                    const char* key, Id& out, std::string& why) {
  long long value = 0;
  if (!ReadInt(node, where, key, value, why)) return false;
  if (value <= 0) {
    why = At(where, std::string("key '") + key + "' must be greater than 0");
    return false;
  }
  const long long max = static_cast<long long>(std::numeric_limits<Id>::max());
  if (value > max) {
    why = At(where, std::string("key '") + key + "' is out of range");
    return false;
  }
  out = static_cast<Id>(value);
  return true;
}

/**
 * @brief 判断文本是否是 internal::ParseVerbosity 会按整数解释的形态。
 * @param text 待判断的原始标量文本。
 * @return 可选一个前导 '-' 后跟至少一个十进制数字时 true。
 * @details 只做形态判断，不关心数值大小；名字（INFO 等）一律 false。与
 *          ParseVerbosity 的整数分支保持同一判据，避免两边各自演化。
 */
bool LooksLikeInteger(const std::string& text) {
  if (text.empty()) return false;
  // 前导负号跳过；"-" 与 "-x" 都不算整数（负号后必须至少有一位数字）。
  const std::size_t start = text[0] == '-' ? 1 : 0;
  if (start == text.size()) return false;
  for (std::size_t i = start; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  return true;
}

/**
 * @brief 读 log_level：级别名或整数，语义与 -v 参数共用。
 * @param node 已确认存在的节点。
 * @param where 错误定位。
 * @param out 成功时写入 loguru verbosity 数值。
 * @param why 失败时的原因。
 * @return 成功 true；容器、无法识别的名字或超出 int 范围的整数 false。
 * @details 级别名集合与取值语义完全交给 internal::ParseVerbosity，配置与
 *          命令行因此不会出现两套级别表。但 ParseVerbosity 的整数分支在
 *          64 位上是 long → int 的无检查强转（如 4294967296 会回绕成 0），
 *          所以在委托之前先对"形似整数"的文本做一次 [INT_MIN, INT_MAX]
 *          范围校验；名字形态不含数字前缀，不受这次预检影响。
 * @see daemon::internal::ParseVerbosity。
 */
bool ReadLogLevel(const YAML::Node& node, const std::string& where, int& out,
                  std::string& why) {
  if (!node.IsScalar()) {
    why = At(where,
             "key 'log_level' must be a level name or an integer");
    return false;
  }
  const std::string text = node.Scalar();
  if (LooksLikeInteger(text)) {
    errno = 0;
    char* end = nullptr;
    // strtoll 在 LP64 上比 int 宽，正好用它区分"整数值"与"int 放不下"。
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || errno == ERANGE ||
        value < static_cast<long long>(std::numeric_limits<int>::min()) ||
        value > static_cast<long long>(std::numeric_limits<int>::max())) {
      why = At(where, "key 'log_level' is out of range for an integer level");
      return false;
    }
  }
  if (!internal::ParseVerbosity(text, out)) {
    why = At(where,
             "key 'log_level' must be OFF/FATAL/ERROR/WARNING/INFO/DEBUG/"
             "VERBOSE/TRACE or an integer");
    return false;
  }
  return true;
}

/**
 * @brief 解析系统配置里的 identity 段。
 * @param node identity 节点；调用方保证非 Null。
 * @param cfg 解析结果写入这里（identity_mode 与 allowed_users）。
 * @param why 失败时的原因。
 * @return 成功 true。
 * @details 允许的键是 mode 与 users。mode 缺省 auto；users 只在 list 模式有意义，
 *          所以 auto 模式带非空 users 也判错——否则这份名单会被静默忽略。
 *          每条 users 是裸 uid 或一个 mapping：mapping 里 uid 必填，gid 省略时
 *          取 uid（单用户桌面上 uid 与主组同号是常态）。所有 id 都必须大于 0。
 */
bool ParseIdentity(const YAML::Node& node, SystemConfig& cfg, std::string& why) {
  const std::string where = "system config: identity";
  if (!node.IsMap()) {
    why = At(where, "must be a mapping");
    return false;
  }
  if (!CheckKeys(node, {"mode", "users"}, where, why)) return false;

  IdentityMode mode = IdentityMode::kAuto;
  const YAML::Node mode_node = node["mode"];
  if (mode_node) {
    if (!mode_node.IsScalar()) {
      why = At(where, "key 'mode' must be 'auto' or 'list'");
      return false;
    }
    const std::string text = mode_node.Scalar();
    if (text == "auto") {
      mode = IdentityMode::kAuto;
    } else if (text == "list") {
      mode = IdentityMode::kList;
    } else {
      why = At(where, "key 'mode' must be 'auto' or 'list', got '" + text + "'");
      return false;
    }
  }

  std::vector<IdentityEntry> users;
  const YAML::Node users_node = node["users"];
  if (users_node && !users_node.IsNull()) {
    if (!users_node.IsSequence()) {
      why = At(where, "key 'users' must be a sequence");
      return false;
    }
    users.reserve(users_node.size());
    for (std::size_t i = 0; i < users_node.size(); ++i) {
      const YAML::Node entry = users_node[i];
      const std::string entry_where =
          where + ".users[" + std::to_string(i) + "]";
      IdentityEntry item;
      // 裸整数是 "uid 与主组同号" 的简写：users: [1000] 等价于
      // {uid: 1000, gid: 1000}。写成别的标量（abc、0、负数）由
      // ReadPositiveId 按 uid 报错。
      if (entry.IsScalar()) {
        if (!ReadPositiveId(entry, entry_where, "uid", item.uid, why)) {
          return false;
        }
        item.gid = static_cast<gid_t>(item.uid);
        users.push_back(item);
        continue;
      }
      if (!entry.IsMap()) {
        why = At(entry_where, "must be a uid or a mapping");
        return false;
      }
      if (!CheckKeys(entry, {"uid", "gid"}, entry_where, why)) return false;
      const YAML::Node uid_node = entry["uid"];
      if (!uid_node) {
        why = At(entry_where, "key 'uid' is required");
        return false;
      }
      if (!ReadPositiveId(uid_node, entry_where, "uid", item.uid, why)) {
        return false;
      }
      // gid 省略时取 uid：setgid 只要求一个存在的组号，与 uid 同号是
      // 单用户桌面的常态；万一与 passwd 主组不同，identity 会打 WARNING。
      const YAML::Node gid_node = entry["gid"];
      if (!gid_node) {
        item.gid = static_cast<gid_t>(item.uid);
      } else if (!ReadPositiveId(gid_node, entry_where, "gid", item.gid, why)) {
        return false;
      }
      users.push_back(item);
    }
  }

  if (mode == IdentityMode::kList && users.empty()) {
    why = At(where, "mode 'list' requires a non-empty 'users' list");
    return false;
  }
  if (mode == IdentityMode::kAuto && !users.empty()) {
    why = At(where, "'users' is only valid with mode 'list'");
    return false;
  }

  cfg.identity_mode = mode;
  cfg.allowed_users = std::move(users);
  return true;
}

/**
 * @brief 校验已打开的配置文件：普通文件 + 64 KiB 上限，并回填 stat。
 * @param fd 已打开的描述符。
 * @param display 错误文本里的路径展示。
 * @param st 成功时写入 fstat 结果，供属主/权限检查复用。
 * @param why 失败时的原因。
 * @return 通过 true。
 * @note 大小检查只是提前失败；ReadAll 还会按实际读到的字节数再拦一次，
 *       因为 fstat 之后文件仍可能被追加。
 */
bool CheckConfigFile(int fd, const std::string& display, struct stat& st,
                     std::string& why) {
  if (::fstat(fd, &st) != 0) {
    why = "cannot stat " + display + ": " + std::strerror(errno);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    why = display + " is not a regular file";
    return false;
  }
  if (st.st_size > static_cast<off_t>(kMaxConfigBytes)) {
    why = display + " is larger than 64 KiB";
    return false;
  }
  return true;
}

/**
 * @brief 从当前偏移读到文件末尾，超限即失败。
 * @param fd 已打开的描述符。
 * @param display 错误文本里的路径展示。
 * @param text 成功时写入全文（不含结尾哨兵，内容按字节原样保留）。
 * @param why 失败时的原因。
 * @return 成功 true。
 * @note 读失败只对 EINTR 重试；文本按字节追加，不做编码转换。
 */
bool ReadAll(int fd, const std::string& display, std::string& text,
             std::string& why) {
  text.clear();
  char buf[4096];  // 4 KiB 读块：栈占用可忽略，常规文件一次就能填满
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      why = "cannot read " + display + ": " + std::strerror(errno);
      return false;
    }
    if (n == 0) break;  // EOF
    text.append(buf, static_cast<std::size_t>(n));
    if (text.size() > kMaxConfigBytes) {
      why = display + " is larger than 64 KiB";
      return false;
    }
  }
  return true;
}

/**
 * @brief 在 dirfd 下打开一个路径分量，区分"不存在"与"其它错误"。
 * @param dirfd 父目录描述符。
 * @param name 分量名（不含 '/'）。
 * @param flags openat 标志；调用方保证含 O_NOFOLLOW。
 * @param display 错误文本里的完整路径展示。
 * @param out 成功时接管打开的描述符。
 * @param why 失败时的原因。
 * @return kOk 成功；kAbsent 分量为 ENOENT/ENOTDIR；其余 kError。
 * @details ELOOP（分量是符号链接）归入 kError：符号链接是明确的"不安全"，
 *          不能被当成"用户没配置"而静默跳过。
 */
LoadResult OpenChild(int dirfd, const char* name, int flags,
                     const std::string& display, UniqueFd& out,
                     std::string& why) {
  const int fd = ::openat(dirfd, name, flags);
  if (fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) return LoadResult::kAbsent;
    why = "cannot open " + display + ": " + std::strerror(errno);
    return LoadResult::kError;
  }
  out.reset(fd);
  return LoadResult::kOk;
}

/**
 * @brief 打开一个目录分量并校验它不是符号链接。
 * @param dirfd 父目录描述符；家目录本身用 AT_FDCWD。
 * @param name 分量名或绝对路径。
 * @param display 错误文本里的完整路径展示。
 * @param out 成功时接管打开的描述符（O_PATH，可直接作后续 openat 的 dirfd）。
 * @param why 失败时的原因。
 * @return kOk 成功；kAbsent 分量不存在；kError 符号链接、非目录或其它错误。
 * @details 不用 O_DIRECTORY|O_NOFOLLOW：内核在这种组合下对符号链接报
 *          ENOTDIR，与"分量不存在"无法区分。O_PATH|O_NOFOLLOW 会把符号链接
 *          本身打开，随后 fstat 就能明确分出"链接 / 普通文件 / 目录"三类，
 *          符号链接一律按不安全拒绝。O_PATH 不需要目录的读权限，而下面的
 *          config.yaml 仍以 O_RDONLY 打开，读权限检查没有放松。
 */
LoadResult OpenDirNoFollow(int dirfd, const char* name,
                           const std::string& display, UniqueFd& out,
                           std::string& why) {
  const int fd = ::openat(dirfd, name, O_PATH | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) return LoadResult::kAbsent;
    why = "cannot open " + display + ": " + std::strerror(errno);
    return LoadResult::kError;
  }
  out.reset(fd);
  struct stat st {};
  if (::fstat(out.get(), &st) != 0) {
    why = "cannot stat " + display + ": " + std::strerror(errno);
    return LoadResult::kError;
  }
  if (S_ISLNK(st.st_mode)) {
    why = display + " is a symbolic link";
    return LoadResult::kError;
  }
  if (!S_ISDIR(st.st_mode)) {
    why = display + " is not a directory";
    return LoadResult::kError;
  }
  return LoadResult::kOk;
}

/**
 * @brief 判断 gid 能否在组库里解析到。
 * @param gid 待查的组 id；调用方保证 > 0。
 * @param error 查询机制本身失败（非"不存在"）时写入原因；组不存在时保持为空。
 * @return 组存在 true；组不存在或查询失败 false（用 error 是否为空区分两者）。
 * @details 用 getgrgid_r 而不是 getgrgid：前者可重入，也不依赖静态缓冲区。
 *          NSS 后端（sssd、LDAP 等）返回 ERANGE 表示调用方给的缓冲区不够，
 *          此时把缓冲区加倍重试；达到上限仍不够就按查询失败上报，避免死循环。
 * @note 只读 NSS，无副作用；gid_t 本身是无符号类型，范围已在解析期收口。
 */
bool GroupExists(gid_t gid, std::string& error) {
  error.clear();
  // 先在 sysconf 给的提示值上开缓冲区；不支持该名字时用 1 KiB 起步。
  const long hint = ::sysconf(_SC_GETGR_R_SIZE_MAX);
  std::size_t size = hint > 0 ? static_cast<std::size_t>(hint) : 1024;
  // 1 MiB 远超任何 NSS 后端的单条记录长度，用来给"无限长大"兜底。
  constexpr std::size_t kMaxGroupBufBytes = 1u << 20;
  std::vector<char> buf(size);
  for (;;) {
    struct group entry {};
    struct group* result = nullptr;
    const int rc = ::getgrgid_r(gid, &entry, buf.data(), buf.size(), &result);
    if (rc == 0) return result != nullptr;  // result 为空 = 组不存在
    if (rc != ERANGE) {
      error = std::strerror(rc);
      return false;
    }
    if (buf.size() >= kMaxGroupBufBytes) {
      error = "group database record is unreasonably large";
      return false;
    }
    buf.resize(buf.size() * 2);
  }
}

/**
 * @brief 校验白名单里每个 gid 都存在于组库。
 * @param users 解析出的白名单条目。
 * @param why 失败时写入原因，文本带条目下标与 gid。
 * @return 全部存在 true；任一条目缺失或查询失败 false。
 * @details 放在读取阶段而不是 ParseSystemConfig：解析函数按契约是纯函数，
 *          不碰文件系统与 NSS。gid 写错的代价是降权时才在 setgid 处失败，
 *          提前到配置阶段能把原因直接指到那一行。
 */
bool CheckGroupIds(const std::vector<IdentityEntry>& users, std::string& why) {
  for (std::size_t i = 0; i < users.size(); ++i) {
    std::string lookup_error;
    if (GroupExists(users[i].gid, lookup_error)) continue;
    const std::string where =
        "system config: identity.users[" + std::to_string(i) + "]";
    if (lookup_error.empty()) {
      why = At(where,
               ".gid " + std::to_string(users[i].gid) +
                   " does not exist in the group database");
    } else {
      why = At(where, ".gid " + std::to_string(users[i].gid) +
                          " cannot be looked up: " + lookup_error);
    }
    return false;
  }
  return true;
}

}  // namespace

bool IsValidBtAddr(const std::string& addr) {
  if (addr.size() != 17) return false;  // 6 个字节 × 2 位 + 5 个冒号
  for (std::size_t i = 0; i < addr.size(); ++i) {
    const char c = addr[i];
    // 每 3 个字符一组的第 3 个（下标 2、5、…、14）是冒号，其余是十六进制数字。
    if ((i + 1) % 3 == 0) {
      if (c != ':') return false;
      continue;
    }
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    const bool upper = c >= 'A' && c <= 'F';
    if (!digit && !lower && !upper) return false;
  }
  return true;
}

std::string UsbReportIntervalChoices() {
  // 0 是模式不是间隔，因此带括注；其余档位是纯毫秒数，末项用 "or" 连接。
  std::string text = "0 (real-time)";
  for (std::size_t i = 1; i < std::size(kUsbReportIntervals); ++i) {
    text += i + 1 == std::size(kUsbReportIntervals) ? " or " : ", ";
    text += std::to_string(kUsbReportIntervals[i]);
  }
  return text;
}

std::string UsbModelChoices() {
  std::string text;
  for (std::size_t i = 0; i < std::size(kUsbModelChoices); ++i) {
    if (i != 0) {
      text += i + 1 == std::size(kUsbModelChoices) ? " or " : ", ";
    }
    text += kUsbModelChoices[i].text;
  }
  return text;
}

const char* UsbModelName(
    const std::optional<core::device::ControllerModel>& model) {
  for (const UsbModelChoice& choice : kUsbModelChoices) {
    if (choice.model == model) return choice.text;
  }
  // 表里一定有 auto（std::nullopt）；到这里说明表被改坏了。
  return kUsbModelChoices[0].text;
}

std::string CanonicalBtAddr(const std::string& addr) {
  std::string out = addr;
  for (char& c : out) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

/**
 * @brief 解析系统配置文本（schema：ffs_path/discovery_interval_s/log_level/identity）。
 * @param text YAML 文本；空文本按空文档处理。
 * @param out 成功时写入结果；失败时不修改。
 * @param why 失败时的单行原因；成功时清空。
 * @return 成功 true。
 * @note 解析先落在局部对象里，只有全部校验通过才覆盖 out；YAML 库抛出的异常
 *       在这里统一转成 why，不会穿出本函数。
 */
bool ParseSystemConfig(const std::string& text, SystemConfig& out,
                       std::string& why) {
  why.clear();
  SystemConfig cfg;
  try {
    const YAML::Node root = YAML::Load(text);
    // 空文件 / 全是注释的文档加载后是 Null，等价于"全部取默认值"。
    if (root && !root.IsNull()) {
      if (!root.IsMap()) {
        why = "system config: document root must be a mapping";
        return false;
      }
      if (!CheckKeys(root,
                     {"ffs_path", "discovery_interval_s", "log_level",
                      "identity"},
                     "system config", why)) {
        return false;
      }

      const YAML::Node ffs_path = root["ffs_path"];
      if (ffs_path &&
          !ReadString(ffs_path, "system config", "ffs_path", cfg.ffs_path,
                      why)) {
        return false;
      }

      // 扫描间隔按秒收口，再交给 Manager 换算成毫秒节拍。0 会让监督循环空转，
      // 所以下限是 1；上限取 INT_MAX/1000，保证乘出来的毫秒数不回绕。
      const YAML::Node interval = root["discovery_interval_s"];
      if (interval) {
        long long seconds = 0;
        if (!ReadInt(interval, "system config", "discovery_interval_s", seconds,
                     why)) {
          return false;
        }
        constexpr long long kMaxIntervalSeconds =
            std::numeric_limits<int>::max() / 1000;
        if (seconds < 1 || seconds > kMaxIntervalSeconds) {
          why = "system config: key 'discovery_interval_s' must be between 1 "
                "and " + std::to_string(kMaxIntervalSeconds);
          return false;
        }
        cfg.discovery_interval_s = static_cast<int>(seconds);
      }

      const YAML::Node log_level = root["log_level"];
      if (log_level &&
          !ReadLogLevel(log_level, "system config", cfg.log_level, why)) {
        return false;
      }

      // `identity:` 这种只写键不写值的形式按"未配置"处理，其余非映射都是错误。
      const YAML::Node identity = root["identity"];
      if (identity && !identity.IsNull() &&
          !ParseIdentity(identity, cfg, why)) {
        return false;
      }
    }
  } catch (const std::exception& e) {
    why = std::string("system config: ") + e.what();
    return false;
  }
  out = std::move(cfg);
  return true;
}

/**
 * @brief 解析用户配置文本（schema：enabled/bt_addr/log_level/plugins/global/
 *        controllers）。
 * @param text YAML 文本；空文本按空文档处理。
 * @param out 成功时写入结果；失败时不修改。
 * @param why 失败时的单行原因；成功时清空。
 * @return 成功 true。
 * @details root 级键（ffs_path、uid、gid 等）在这份 schema 里不存在，
 *          一律按未知键拒绝。bt_addr 取 auto/list/XX:XX:XX:XX:XX:XX 三者之一，
 *          缺省 auto；global 与 controllers.<MAC> 只接受七个手柄级键（含
 *          plugins），且逐层叠加成每只手柄的最终值。bt_addr: list 要求
 *          controllers 非空；bt_addr: <MAC> 时 controllers 里不能出现别的
 *          MAC——那些条目永远不会生效，静默忽略等于让用户以为自己配上了。
 * @note 解析先落在局部对象里，只有全部校验通过才覆盖 out。
 */
bool ParseUserConfig(const std::string& text, UserConfig& out,
                     std::string& why) {
  why.clear();
  UserConfig cfg;
  try {
    const YAML::Node root = YAML::Load(text);
    if (root && !root.IsNull()) {
      if (!root.IsMap()) {
        why = "user config: document root must be a mapping";
        return false;
      }
      if (!CheckKeys(root, {"enabled", "bt_addr", "log_level", "plugins",
                            "global", "controllers"},
                     "user config", why)) {
        return false;
      }

      const YAML::Node enabled = root["enabled"];
      if (enabled &&
          !ReadBool(enabled, "user config", "enabled", cfg.enabled, why)) {
        return false;
      }

      // bt_addr 是"服务哪些手柄"的开关：两个字面量选模式，其余按 MAC 校验。
      const YAML::Node bt_addr = root["bt_addr"];
      if (bt_addr) {
        std::string text_addr;
        if (!ReadString(bt_addr, "user config", "bt_addr", text_addr, why)) {
          return false;
        }
        if (text_addr == "auto") {
          cfg.bt_mode = BtTargetMode::kAuto;
        } else if (text_addr == "list") {
          cfg.bt_mode = BtTargetMode::kList;
        } else if (IsValidBtAddr(text_addr)) {
          cfg.bt_mode = BtTargetMode::kSingle;
          cfg.bt_addr = CanonicalBtAddr(text_addr);
        } else {
          why = "user config: 'bt_addr' must be 'auto', 'list' or "
                "XX:XX:XX:XX:XX:XX, got '" + text_addr + "'";
          return false;
        }
      }

      const YAML::Node log_level = root["log_level"];
      if (log_level) {
        // 只有键存在才写 optional：-1 是合法的 WARNING，不能拿它当"未配置"。
        int level = 0;
        if (!ReadLogLevel(log_level, "user config", level, why)) return false;
        cfg.log_level = level;
      }

      // 根级 `plugins:` 是插件那一族的默认层：先合进 defaults，global.plugins
      // 与 controllers.<MAC>.plugins 再在它之上逐键覆盖。默认层是空的：装载由
      // 插件目录决定（见 plugin/plugin.h），这一节只负责把键交给同名插件。
      const YAML::Node root_plugins = root["plugins"];
      if (root_plugins &&
          !ReadPlugins(root_plugins, "user config", cfg.defaults.plugins,
                       why)) {
        return false;
      }

      // `global:` 只写键不写值时按"无覆盖"处理，与 identity: 同一种宽容。
      const YAML::Node global = root["global"];
      if (global && !global.IsNull() &&
          !ApplyControllerKeys(global, "user config: global", cfg.defaults,
                               why)) {
        return false;
      }

      const YAML::Node controllers = root["controllers"];
      if (controllers && !controllers.IsNull()) {
        if (!controllers.IsMap()) {
          why = "user config: 'controllers' must be a mapping of MAC to "
                "settings";
          return false;
        }
        for (auto it = controllers.begin(); it != controllers.end(); ++it) {
          const YAML::Node key = it->first;
          if (!key.IsScalar()) {
            why = "user config: controllers keys must be MAC addresses";
            return false;
          }
          const std::string raw = key.Scalar();
          if (!IsValidBtAddr(raw)) {
            why = "user config: controllers key '" + raw +
                  "' is not a MAC address";
            return false;
          }
          const std::string mac = CanonicalBtAddr(raw);
          // 归一后同名的两个键（AA:.. 与 aa:..）会让后一个静默覆盖前一个。
          if (cfg.controllers.count(mac) != 0) {
            why = "user config: duplicate controllers entry for " + mac;
            return false;
          }
          // 每个条目从 global 层起步，只覆盖写了的键。
          ControllerConfig entry = cfg.defaults;
          if (!it->second.IsNull() &&
              !ApplyControllerKeys(it->second,
                                   "user config: controllers." + mac, entry,
                                   why)) {
            return false;
          }
          cfg.controllers.emplace(mac, entry);
        }
      }
    }
  } catch (const std::exception& e) {
    why = std::string("user config: ") + e.what();
    return false;
  }

  // 跨字段约束在全部键读完后再判：list 的白名单就是 controllers 的键集。
  if (cfg.bt_mode == BtTargetMode::kList && cfg.controllers.empty()) {
    why = "user config: 'bt_addr: list' requires a non-empty 'controllers' map";
    return false;
  }
  if (cfg.bt_mode == BtTargetMode::kSingle) {
    for (const auto& [mac, unused] : cfg.controllers) {
      if (mac != cfg.bt_addr) {
        why = "user config: controllers entry '" + mac +
              "' is not the configured bt_addr " + cfg.bt_addr;
        return false;
      }
    }
  }

  out = std::move(cfg);
  return true;
}

/**
 * @brief 打开、校验并解析系统配置文件。
 * @param path 配置文件路径。
 * @param out 成功时写入结果；kAbsent/kError 时不修改。
 * @param why 失败时的原因；kAbsent 与成功时清空。
 * @return kOk/kAbsent/kError。
 * @note O_NOFOLLOW 只保护最后一级分量；/etc 由 root 管理，不做逐级加固。
 * @note 解析成功后还会查组库确认白名单 gid 存在；gid 不存在是配置错误。
 */
LoadResult LoadSystemConfig(const std::string& path, SystemConfig& out,
                            std::string& why) {
  why.clear();
  UniqueFd file;
  file.reset(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!file.valid()) {
    if (errno == ENOENT) return LoadResult::kAbsent;
    why = "cannot open " + path + ": " + std::strerror(errno);
    return LoadResult::kError;
  }

  struct stat st {};
  if (!CheckConfigFile(file.get(), path, st, why)) return LoadResult::kError;
  std::string text;
  if (!ReadAll(file.get(), path, text, why)) return LoadResult::kError;

  SystemConfig parsed;
  if (!ParseSystemConfig(text, parsed, why)) return LoadResult::kError;
  // 解析只保证 gid 是正整数；能不能在组库解析出来要在这里查（§4 的配置错误）。
  if (!CheckGroupIds(parsed.allowed_users, why)) return LoadResult::kError;
  out = std::move(parsed);
  return LoadResult::kOk;
}

/**
 * @brief 逐级打开并解析目标用户的配置文件。
 * @param owner_uid 期望的文件属主；root（0）也接受。
 * @param home 目标用户家目录。
 * @param out 成功时写入结果；kAbsent/kError 时不修改。
 * @param why 失败时的原因；kAbsent 与成功时清空。
 * @return kOk/kAbsent/kError。
 * @note 每一级目录都带 O_NOFOLLOW 并显式拒绝符号链接；文件还要求属主为
 *       owner_uid 或 root，且组/其他用户不可写。
 */
LoadResult LoadUserConfig(uid_t owner_uid, const std::string& home,
                          UserConfig& out, std::string& why) {
  why.clear();
  constexpr int kFileFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;

  // home 本身也拒绝符号链接：家目录是链接时宁可报错，也不跟随到别处。
  UniqueFd home_dir;
  LoadResult result = OpenDirNoFollow(AT_FDCWD, home.c_str(), home, home_dir,
                                      why);
  if (result != LoadResult::kOk) return result;

  // kUserConfigDir 是相对家目录的两级路径，而逐级 openat 需要分开的分量；
  // 多于或少于两级都说明常量被改坏了，编译期就拦下而不是运行期拼出怪路径。
  constexpr std::string_view kUserDir = kUserConfigDir;
  constexpr std::size_t kUserDirSlash = kUserDir.find('/');
  static_assert(kUserDirSlash != std::string_view::npos &&
                    kUserDir.find('/', kUserDirSlash + 1) ==
                        std::string_view::npos,
                "kUserConfigDir must be a two-component relative path");
  const std::string user_dir(kUserDir);  // 完整相对路径，用于错误文本
  const std::string first_part = user_dir.substr(0, kUserDirSlash);
  const std::string second_part = user_dir.substr(kUserDirSlash + 1);

  UniqueFd config_dir;
  result = OpenDirNoFollow(home_dir.get(), first_part.c_str(),
                           home + "/" + first_part, config_dir, why);
  if (result != LoadResult::kOk) return result;

  UniqueFd app_dir;
  result = OpenDirNoFollow(config_dir.get(), second_part.c_str(),
                           home + "/" + user_dir, app_dir, why);
  if (result != LoadResult::kOk) return result;

  const std::string display = home + "/" + user_dir + "/" + kUserConfigFileName;
  UniqueFd file;
  result = OpenChild(app_dir.get(), kUserConfigFileName, kFileFlags, display,
                     file, why);
  if (result != LoadResult::kOk) return result;

  // 文件级加固：普通文件、属主正确、组/其他不可写、大小受限。
  struct stat st {};
  if (!CheckConfigFile(file.get(), display, st, why)) {
    return LoadResult::kError;
  }
  if (st.st_uid != owner_uid && st.st_uid != 0) {
    why = display + " is not owned by the target user or root";
    return LoadResult::kError;
  }
  // 组/其他可写意味着第三方能篡改 root 将要读取的内容，直接拒绝。
  if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    why = display + " must not be group- or other-writable";
    return LoadResult::kError;
  }

  std::string text;
  if (!ReadAll(file.get(), display, text, why)) return LoadResult::kError;

  UserConfig parsed;
  if (!ParseUserConfig(text, parsed, why)) return LoadResult::kError;
  out = std::move(parsed);
  return LoadResult::kOk;
}

}  // namespace ds5::daemon
