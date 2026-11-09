/**
 * @file tools/buttons_check.cpp
 * @brief buttons 插件的配置校验器：不装载 .so，只跑一遍插件自己的语法层
 * @details 两种输入形态。给一份文件时，按 daemon 同样的两层叠加取出某一节的
 *          插件配置，再把 `buttons.` 前缀剥掉、交给 buttons::settings::Load 解析；
 *          给 `--layer` 时，从标准输入读一段相对 `buttons` 的子树，交给
 *          buttons_config::CheckLayer。两种形态走的都是与 worker 里完全相同的那段
 *          代码，不另写一份 YAML 规则。
 * @details 输出分两段：逐条的三种下场（变成规则 / 整行忽略 / 这一条不生效），
 *          以及整节的武装摘要。`--json` 换成同一份内容的机器可读形态，供
 *          tools/buttons_web.py 按行回填。
 * @details 不负责：装载插件、连接手柄、改写配置。校验器只读输入，不写任何东西。
 * @note 层次：tools。不需要 root、手柄或 PipeWire，也不读真实系统配置目录。
 */

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "buttons_config.h"
#include "daemon/config.h"

namespace {

/// 退出码：这一节会武装，且没有一条取值被拒。
constexpr int kOk = 0;
/// 退出码：有取值被拒，或整节没有可解析的规则（插件不会武装）。
constexpr int kNotUsable = 1;
/// 退出码：用法错误、输入读不了、YAML 解析失败、--mac 不在配置里。
constexpr int kUsageError = 2;

/// 配置里这一节的名字，也是校验的目标。
constexpr std::string_view kPluginName = "buttons";

/**
 * @brief 打用法。
 * @param stream 目标流：--help 走 stdout，用法错误走 stderr。
 */
void PrintUsage(std::FILE* stream) {
  std::fprintf(
      stream,
      "用法: buttons_check <用户配置.yaml> [--mac AA:BB:CC:DD:EE:FF] [--json]\n"
      "      buttons_check --layer <层标识> [--json] < buttons子树\n"
      "\n"
      "用 buttons 插件自己的语法层检查 global.plugins.buttons 那一节：每一行会变成\n"
      "规则、被整行忽略，还是取值被拒，并给出整节的武装摘要。\n"
      "\n"
      "  --mac MAC      检查 controllers.<MAC> 那一节（缺省检查 defaults/global 合并后的\n"
      "                 结果，也就是没有专属条目的手柄拿到的配置）\n"
      "  --layer ID     改从标准输入读一段 `buttons` 之下的子树（缩进相对 buttons），\n"
      "                 按 ID 指定的位置单独校验：global 或 controllers.<MAC>\n"
      "  --json         输出机器可读的一份 JSON，而不是给人看的文本\n"
      "\n"
      "退出码: 0 = 会武装且没有取值被拒；1 = 有取值被拒，或整节没有可解析的规则；\n"
      "        2 = 用法、输入或 YAML 错误。\n");
}

/// 把一段任意 UTF-8 文本包成 JSON 字符串。
void JsonString(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const unsigned char ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\u%04x", ch);
          out += buffer;
        } else {
          out.push_back(raw);
        }
    }
  }
  out.push_back('"');
}

/// 往 JSON 里写一份报告（逐条结局与整节摘要）。
void JsonReport(std::string& out, const buttons_config::Report& report) {
  out += "\"entries\":[";
  for (std::size_t i = 0; i < report.entries.size(); ++i) {
    const buttons_config::Entry& entry = report.entries[i];
    if (i > 0) out.push_back(',');
    out += "{\"key\":";
    JsonString(out, entry.row.key);
    out += ",\"value\":";
    JsonString(out, entry.row.value);
    out += ",\"rejected\":";
    out += entry.rejected ? "true" : "false";
    out += ",\"ignored\":";
    out += entry.ignored ? "true" : "false";
    out += ",\"armed\":";
    out += entry.armed ? "true" : "false";
    out += ",\"summary\":";
    JsonString(out, entry.summary);
    out += ",\"messages\":[";
    for (std::size_t m = 0; m < entry.messages.size(); ++m) {
      if (m > 0) out.push_back(',');
      JsonString(out, entry.messages[m]);
    }
    out += "]}";
  }
  out += "],\"rejected\":";
  out += std::to_string(report.rejected);
  out += ",\"ignored\":";
  out += std::to_string(report.ignored);
  out += ",\"armed\":";
  out += report.armed ? "true" : "false";
  out += ",\"summary\":";
  JsonString(out, report.summary);
}

/// 读干标准输入。
bool ReadStdin(std::string& out) {
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof buffer, stdin)) > 0) {
    out.append(buffer, got);
  }
  return std::ferror(stdin) == 0;
}

/// 打一段给人看的报告。
void PrintReport(std::FILE* stream, const buttons_config::Report& report,
                 bool note_empty_section) {
  if (note_empty_section && report.entries.empty()) {
    std::fprintf(stream, "\n插件会拿到空配置，不会武装：一个字节都不改。\n");
    return;
  }
  std::fprintf(stream, "\n");
  for (const buttons_config::Entry& entry : report.entries) {
    std::fprintf(stream, "  %-24s = %-24s -> %s\n", entry.row.key.c_str(),
                 entry.row.value.c_str(), entry.summary.c_str());
    for (const std::string& line : entry.messages) {
      std::fprintf(stream, "      %s\n", line.c_str());
    }
  }
  std::fprintf(stream, "\n");
  if (report.rejected > 0 || report.ignored > 0) {
    std::fprintf(stream, "%d 条不生效，%d 条被忽略。\n", report.rejected,
                 report.ignored);
  }
  if (report.armed) {
    std::fprintf(stream, "armed with %s\n", report.summary.c_str());
  } else {
    std::fprintf(stream, "整节没有可解析的规则：插件不会武装，一个字节都不改。\n");
  }
}

/// 一行 JSON 错误，`--json` 模式下的用法/输入错误也用它。
void PrintJsonError(std::string_view error) {
  std::string out = "{\"ok\":false,\"error\":";
  JsonString(out, error);
  out += "}\n";
  std::fputs(out.c_str(), stdout);
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string mac;
  std::string layer;
  bool json = false;
  bool layer_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage(stdout);
      return kOk;
    }
    if (arg == "--json") {
      json = true;
      continue;
    }
    if (arg == "--mac") {
      if (i + 1 >= argc) {
        const std::string error = "--mac 后面要跟一个 MAC 地址";
        if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
        return kUsageError;
      }
      mac = argv[++i];
      continue;
    }
    if (arg == "--layer") {
      if (i + 1 >= argc) {
        const std::string error = "--layer 后面要跟一个层标识";
        if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
        return kUsageError;
      }
      layer = argv[++i];
      layer_mode = true;
      continue;
    }
    if (!arg.empty() && arg.front() == '-') {
      const std::string error = "不认识的选项 " + std::string(arg);
      if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
      return kUsageError;
    }
    if (!path.empty()) {
      const std::string error = "一次只能检查一份配置文件";
      if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
      return kUsageError;
    }
    path = argv[i];
  }
  if (layer_mode && !path.empty()) {
    const std::string error = "--layer 从标准输入读配置，不能再给文件";
    if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
    return kUsageError;
  }
  if (layer_mode && !mac.empty()) {
    const std::string error = "--layer 的层标识已经包含手柄，不能再给 --mac";
    if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
    return kUsageError;
  }
  if (!layer_mode && path.empty()) {
    PrintUsage(stderr);
    if (json) PrintJsonError("缺少输入");
    return kUsageError;
  }
  if (!mac.empty() && !ds5::daemon::IsValidBtAddr(mac)) {
    const std::string error = "'" + mac + "' 不是 XX:XX:XX:XX:XX:XX 形式的 MAC";
    if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
    return kUsageError;
  }

  if (layer_mode) {
    std::string block;
    if (!ReadStdin(block)) {
      const std::string error = "读不了标准输入";
      if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
      return kUsageError;
    }
    const buttons_config::Checked checked =
        buttons_config::CheckLayer(layer, block);
    if (json) {
      std::string out = "{\"ok\":";
      out += checked.yaml_ok ? "true" : "false";
      out += ",\"mode\":\"layer\",\"layer\":";
      JsonString(out, layer);
      out += ",\"yaml_ok\":";
      out += checked.yaml_ok ? "true" : "false";
      out += ",\"yaml_error\":";
      JsonString(out, checked.yaml_error);
      out += ",\"report\":{";
      JsonReport(out, checked.report);
      out += "}}\n";
      std::fputs(out.c_str(), stdout);
    } else if (!checked.yaml_ok) {
      std::fprintf(stderr, "buttons_check: %s\n", checked.yaml_error.c_str());
    } else {
      std::fprintf(stdout, "%s 下 plugins.%s\n", layer.c_str(),
                   std::string(kPluginName).c_str());
      PrintReport(stdout, checked.report, true);
    }
    if (!checked.yaml_ok) return kUsageError;
    return (checked.report.armed && checked.report.rejected == 0) ? kOk
                                                                  : kNotUsable;
  }

  std::ifstream file(path);
  if (!file) {
    const std::string error = "打不开 " + path;
    if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
    return kUsageError;
  }
  const std::string text{std::istreambuf_iterator<char>(file),
                         std::istreambuf_iterator<char>()};

  ds5::daemon::UserConfig config;
  std::string why;
  if (!ds5::daemon::ParseUserConfig(text, config, why)) {
    const std::string error = path + ": " + why;
    if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
    return kUsageError;
  }

  // 没有 --mac 时检查 defaults：它已经是"global + 内置默认"叠加之后的结果，
  // 正是没有专属条目的手柄拿到的配置。
  const ds5::daemon::ControllerConfig* controller = &config.defaults;
  std::string section = "defaults";
  if (!mac.empty()) {
    const std::string canonical = ds5::daemon::CanonicalBtAddr(mac);
    const auto found = config.controllers.find(canonical);
    if (found == config.controllers.end()) {
      std::string error = path + " 里没有 controllers." + canonical + " 这一条";
      if (!config.controllers.empty()) {
        error += "；可选的 MAC:";
        for (const auto& entry : config.controllers) {
          error += " " + entry.first;
        }
      }
      if (json) PrintJsonError(error); else std::fprintf(stderr, "buttons_check: %s\n", error.c_str());
      return kUsageError;
    }
    controller = &found->second;
    section = "controllers." + canonical;
  }

  const std::vector<buttons_config::Row> rows =
      buttons_config::RowsOf(controller->plugins);
  const buttons_config::Report report = buttons_config::Check(rows);

  if (json) {
    std::string out = "{\"ok\":true,\"mode\":\"file\",\"path\":";
    JsonString(out, path);
    out += ",\"section\":";
    JsonString(out, section);
    out += ",\"layer\":";
    JsonString(out, mac.empty() ? "global" : section);
    out += ",\"rows\":";
    out += std::to_string(rows.size());
    out += ",\"yaml_ok\":true,\"yaml_error\":\"\",\"report\":{";
    JsonReport(out, report);
    out += "}}\n";
    std::fputs(out.c_str(), stdout);
  } else {
    std::printf("%s\n  %s 下 plugins.%.*s 共 %zu 条\n", path.c_str(),
                section.c_str(), static_cast<int>(kPluginName.size()),
                kPluginName.data(), rows.size());
    PrintReport(stdout, report, true);
  }

  return (report.armed && report.rejected == 0) ? kOk : kNotUsable;
}
