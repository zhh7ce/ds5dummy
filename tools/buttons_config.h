/**
 * @file tools/buttons_config.h
 * @brief 校验 buttons 配置的共用部分：把一行配置交给插件的语法层，收回它的下场
 * @details 唯一的调用方是命令行的 buttons_check（`--json` 与 `--layer` 两种输入
 *          形态）；本地网页 tools/buttons_web.py 通过它取判定，不另写一份语法。
 *          判定本身只在这里跑一遍——包括逐条探测（一行单独解析，看它产生规则、
 *          被忽略还是被拒）与整节复算（同一格轴被多行写到时的合并结果）。
 * @note 层次：tools。单线程调用；ds5Host::log 没有 userdata，所以一次 Check
 *       期间的日志落在本模块的文件级状态里，不能并发调用。
 */

#pragma once

#include <map>
#include <string>
#include <vector>

namespace buttons_config {

/// 一份配置里的一行：键路径已经去掉 `buttons.` 前缀，值是 YAML 标量原文。
struct Row {
  std::string key;
  std::string value;
};

/// 一行的结局：它自己单独解析时发生了什么。
struct Entry {
  Row row;
  bool rejected = false;  ///< 取值读不通，插件留了一行 WARNING
  bool ignored = false;   ///< 键名不认识，插件留了一行 DEBUG
  bool armed = false;     ///< 这一行自己就能让插件武装
  std::string summary;    ///< 这一行产生的规则组成；armed 时有效
  /// 插件这一行的日志原文，每条带 `error:` / `warning:` / `debug:` 前缀。
  std::vector<std::string> messages;
};

/// 整节的结局。
struct Report {
  std::vector<Entry> entries;  ///< 与传入的 rows 一一对应
  int rejected = 0;            ///< 取值被拒的行数
  int ignored = 0;             ///< 被整行忽略的行数
  bool armed = false;          ///< 整节有没有可解析的规则
  std::string summary;         ///< 整节合并之后的规则组成；armed 时有效
};

/**
 * @brief 从一份插件配置袋里取出本插件的那一段。
 * @param settings `插件名.点分键路径 → 标量文本`；宿主也按这个形状交给插件。
 * @return 剥掉 `buttons.` 前缀的行，顺序就是扁平键序。
 * @details 过滤与去前缀和宿主交给插件的那一步完全相同（plugin::detail::
 *          BuildSettings），校验工具因此不会看到插件看不到的键。
 */
std::vector<Row> RowsOf(const std::map<std::string, std::string>& settings);

/**
 * @brief 逐行判定，再整节复算一次。
 * @param rows RowsOf 的结果，或网页里现解析出来的同一形状。
 * @return 每行的结局与整节的摘要。
 * @details 逐行判定是"一行单独解析"：插件对每条规则的拒绝与忽略都只挂在那一行
 *          上，单独跑才能把日志归到具体的键，而不必去解析日志文本。整节复算则是
 *          真正会交给插件的那张表——同一格轴被两行写到时，只有整节这一遍才是
 *          合并后的结果，所以摘要取它。
 */
Report Check(const std::vector<Row>& rows);

/// 网页壳里一段待编辑配置的校验结果。
struct Checked {
  /// 这一段按这一层的位置能不能被宿主读进去：YAML 语法、插件名、嵌套形状与
  /// 标量取值都在这里判定，判定的是 daemon 解析器。
  bool yaml_ok = true;
  /// yaml_ok=false 时的一行原因（daemon 给的文本，已带出错键名）。
  std::string yaml_error;
  /// YAML 通过之后的逐条结局；yaml_ok=false 时为空。
  Report report;
};

/**
 * @brief 把网页上编辑的一段 `buttons` 子树放到它那一层单独校验。
 * @param layer_id 这一节在配置里的位置：`global` 或
 *        `controllers.<原文键>`（与 `tools/buttons_web.py` 的层标识同一套写法）。
 * @param block `buttons` 之下各行的原文，缩进相对 `buttons`；空串表示空节。
 * @return YAML 层的结果与语法层的结果。
 * @details 校验的是**整份配置里的这一段**，所以先按这一层的位置拼一份只含它的
 *          最小文档，再交给 daemon::ParseUserConfig——键名拼法、嵌套层数、叶子
 *          必须非空标量这些宿主侧规则因此与真正启动时完全一致，参数层不另写。
 *          拼出来的文档里没有别的层，因此取到的那份插件袋就是这一段自己的键。
 */
Checked CheckLayer(const std::string& layer_id, const std::string& block);

}  // namespace buttons_config
