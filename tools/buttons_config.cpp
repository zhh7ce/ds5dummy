/**
 * @file tools/buttons_config.cpp
 * @brief 校验 buttons 配置的共用部分：逐条探测与整节复算
 * @details 所有判定都走 plugin/buttons 的 settings::Load 与 settings::Summary，
 *          不在这里重复任何语法、键名或取值区间。
 */

#include "buttons_config.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "daemon/config.h"
#include "plugin/buttons/rules.h"
#include "plugin/buttons/settings.h"
#include "plugin/plugin_api.h"

namespace buttons_config {
namespace {

/// 摊平后属于本插件的键都带这个前缀，剥掉之后才是插件看到的键路径。
constexpr std::string_view kPluginPrefix = "buttons.";

/// 一次 Load 的日志：先攒起来，由调用方决定怎么显示。
struct Capture {
  bool warning = false;
  bool debug = false;
  std::vector<std::string> lines;
};

/// 当前这一轮 Load 的日志去向。ds5Host::log 没有 userdata，工具又是单线程跑完，
/// 所以只能放在文件作用域里。
Capture g_capture;

/// 整节复算时关掉日志：逐条那一轮已经把每一行都记过了。
bool g_quiet = false;

/// 级别的可见写法。
const char* LevelName(int level) {
  switch (level) {
    case DS5_LOG_ERROR: return "error";
    case DS5_LOG_WARNING: return "warning";
    case DS5_LOG_INFO: return "info";
    default: return "debug";
  }
}

/// 假宿主的日志槽：把插件排好版的一行连同级别前缀一起收下。
void Log(int level, const char* message) {
  if (g_quiet) return;
  if (level == DS5_LOG_ERROR || level == DS5_LOG_WARNING) g_capture.warning = true;
  if (level == DS5_LOG_DEBUG) g_capture.debug = true;
  g_capture.lines.push_back(std::string(LevelName(level)) + ": " + message);
}

/**
 * @brief 假宿主的关机通道。
 * @return 恒 0（没发出去）。
 * @details 校验只跑 settings::Load，不跑 init，这个槽不会被调到；给一个明确的
 *          "失败"返回值，免得留一个空函数指针给人误读。
 */
int PowerOff() {
  return 0;
}

/// 一份填好的假宿主表。
ds5Host Host() {
  ds5Host host{};
  host.abi_version = kPluginAbiVersion;
  host.log = &Log;
  host.power_off = &PowerOff;
  return host;
}

/**
 * @brief 一条配置的结局写法。
 * @param capture 这一轮 Load 的日志。
 * @param parsed 这一轮 Load 解出的规则表。
 * @param armed Load 的返回值。
 * @return 跟在行尾的一小段说明。
 */
std::string OutcomeText(const Capture& capture, const buttons::rules::Set& parsed,
                        bool armed) {
  if (capture.warning) return "这一条不生效";
  if (capture.debug) return "被忽略：插件不读这个键";
  // 没有任何日志也没有规则：插件对不认识的组名保持沉默（C-05），所以它也可能是
  // 一句取值合法但什么也不改的配置（如 `invert_lstick_x: false`），两者分不开。
  if (!armed) return "不产生规则（未知组名，或取值等于空操作）";
  return buttons::settings::Summary(parsed);
}

/**
 * @brief 把一段 `buttons` 子树拼成只含它自己的最小用户配置文档。
 * @param layer_id 这一节的位置：`global` 或 `controllers.<原文键>`。
 * @param block 相对 `buttons` 的子行原文；空串就是空节。
 * @param indent 成功时写入子行的目标缩进。
 * @return 拼好的 YAML 文本；layer_id 不认识时为空串。
 * @details 只复制行、不重新序列化，页面里的引号与注释因此原样进入解析器；解析器
 *          报的错也就指向用户真正写的那一行。
 */
std::string SyntheticDocument(const std::string& layer_id,
                              const std::string& block, int& indent) {
  constexpr std::string_view kControllers = "controllers.";
  std::string text;
  if (layer_id == "global") {
    text = "global:\n  plugins:\n    buttons:\n";
    indent = 6;
  } else if (layer_id.size() > kControllers.size() &&
             layer_id.compare(0, kControllers.size(), kControllers) == 0) {
    text = "controllers:\n  \"" + layer_id.substr(kControllers.size()) +
           "\":\n    plugins:\n      buttons:\n";
    indent = 8;
  } else {
    indent = 0;
    return {};
  }

  const std::string pad(static_cast<std::size_t>(indent), ' ');
  std::size_t begin = 0;
  while (begin < block.size()) {
    const std::size_t end = block.find('\n', begin);
    const std::string line = end == std::string::npos
                                 ? block.substr(begin)
                                 : block.substr(begin, end - begin);
    if (line.find_first_not_of(' ') != std::string::npos) text += pad;
    text += line;
    text.push_back('\n');
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return text;
}

/**
 * @brief 取出拼出来的那份文档里本插件自己的配置袋。
 * @param layer_id 层标识。
 * @param config 解析结果。
 * @return 指向那一层的插件袋；控制器条目意外缺失时 nullptr。
 */
const std::map<std::string, std::string>* PluginBag(
    const std::string& layer_id, const ds5::daemon::UserConfig& config) {
  constexpr std::string_view kControllers = "controllers.";
  if (layer_id == "global") {
    return &config.defaults.plugins;
  }
  if (layer_id.size() <= kControllers.size() ||
      layer_id.compare(0, kControllers.size(), kControllers) != 0) {
    return nullptr;
  }
  const std::string canonical =
      ds5::daemon::CanonicalBtAddr(layer_id.substr(kControllers.size()));
  const auto found = config.controllers.find(canonical);
  if (found == config.controllers.end()) return nullptr;
  return &found->second.plugins;
}

}  // namespace

std::vector<Row> RowsOf(const std::map<std::string, std::string>& settings) {
  std::vector<Row> rows;
  for (const auto& [key, value] : settings) {
    if (key.size() <= kPluginPrefix.size()) continue;
    if (key.compare(0, kPluginPrefix.size(), kPluginPrefix) != 0) continue;
    rows.push_back(Row{key.substr(kPluginPrefix.size()), value});
  }
  return rows;
}

Report Check(const std::vector<Row>& rows) {
  const ds5Host host = Host();
  Report report;
  report.entries.reserve(rows.size());

  for (const Row& row : rows) {
    const ds5Setting one[] = {{row.key.c_str(), row.value.c_str()},
                              {nullptr, nullptr}};
    buttons::rules::Set parsed;
    g_capture = Capture{};
    const bool armed = buttons::settings::Load(one, parsed, host);

    Entry entry;
    entry.row = row;
    entry.rejected = g_capture.warning;
    entry.ignored = g_capture.debug;
    entry.armed = armed;
    entry.summary = OutcomeText(g_capture, parsed, armed);
    entry.messages = std::move(g_capture.lines);
    if (entry.rejected) ++report.rejected;
    if (entry.ignored) ++report.ignored;
    report.entries.push_back(std::move(entry));
  }

  // 整节复算一次，日志已经逐条记过，这一遍静默。
  std::vector<ds5Setting> array;
  array.reserve(rows.size() + 1);
  for (const Row& row : rows) {
    array.push_back(ds5Setting{row.key.c_str(), row.value.c_str()});
  }
  array.push_back(ds5Setting{nullptr, nullptr});
  buttons::rules::Set all;
  g_quiet = true;
  report.armed = buttons::settings::Load(array.data(), all, host);
  g_quiet = false;
  if (report.armed) report.summary = buttons::settings::Summary(all);
  return report;
}

Checked CheckLayer(const std::string& layer_id, const std::string& block) {
  Checked checked;
  int indent = 0;
  const std::string document = SyntheticDocument(layer_id, block, indent);
  if (document.empty()) {
    checked.yaml_ok = false;
    checked.yaml_error = "不认识的层 " + layer_id;
    return checked;
  }

  // 空节（插件名之下没有键）宿主本来就不接受；而"没有这一节"与"空节"对插件
  // 是同一件事——拿到空配置、不武装。所以这里不拼文档，直接给出这个结论。
  if (block.find_first_not_of(" \t\n") == std::string::npos) {
    return checked;
  }

  ds5::daemon::UserConfig config;
  if (!ds5::daemon::ParseUserConfig(document, config, checked.yaml_error)) {
    checked.yaml_ok = false;
    return checked;
  }

  const std::map<std::string, std::string>* bag = PluginBag(layer_id, config);
  if (bag == nullptr) {
    checked.yaml_ok = false;
    checked.yaml_error = "这一层没有出现在单独拼出的配置里";
    return checked;
  }
  checked.report = Check(RowsOf(*bag));
  return checked;
}

}  // namespace buttons_config
