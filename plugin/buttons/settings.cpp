/**
 * @file
 * @brief 配置文本的语法：五个功能组的分派与取值
 *
 * 一行配置只有三种下场：变成一条规则、被忽略（组名或键名不认识）、这一条不生效
 * 并留下一行 WARNING（取值读不通）。三种都不让服务退出（G-06）。
 *
 * 组内条目数不定，所以分派靠"遍历数组 + 比组前缀"，不靠精确查键；摊平后的数组
 * 也不保证同组相邻，因此不假设顺序。
 */

#include "settings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "ids.h"
#include "plugin/plugin_api.h"
#include "plugin/plugin_util.h"

namespace buttons {
namespace settings {
namespace {

// ---------------------------------------------------------------------------
// 组的拼写：摊平后的键形如 `remap.circle`，组名是第一个点之前那段。
// ---------------------------------------------------------------------------

constexpr std::string_view kRemapGroup = "remap.";
constexpr std::string_view kAxisGroup = "axis.";
constexpr std::string_view kTurboGroup = "turbo.";
constexpr std::string_view kShortcutGroup = "shortcut.";
constexpr std::string_view kMacroGroup = "macro.";

// ---------------------------------------------------------------------------
// axis 组（M-12～M-14、I-02）
// ---------------------------------------------------------------------------

/// `invert_<轴名>`：一根轴单独反向。
constexpr std::string_view kInvertLeaf = "invert_";

/// `center_<摇杆名>`：整根摇杆居中。
constexpr std::string_view kCenterLeaf = "center_";

/// `swap_lstick_rstick`：两根摇杆对调。
constexpr std::string_view kSwapLeaf = "swap_lstick_rstick";

constexpr std::string_view kTrueValue = "true";
constexpr std::string_view kFalseValue = "false";

/// 一直居中；"只在方向命中期间居中"是缺省，没有对应的写法。
constexpr std::string_view kAlwaysValue = "always";

/// 对调的三种范围。
constexpr std::string_view kSwapBoth = "both";
constexpr std::string_view kSwapX = "x";
constexpr std::string_view kSwapY = "y";

/// 值里的保留源键后缀（M-03）。
constexpr std::string_view kKeepSuffix = "+keep";

/// 值写成 disable 就是把这一位抹掉（M-04）：目标位集合为空，没有位补上来。
constexpr std::string_view kDisableValue = "disable";

// ---------------------------------------------------------------------------
// 触发串（设计文档 §7）
// ---------------------------------------------------------------------------

/// 后缀的引导字符：token 表里不含这三个，所以它们一出现就是后缀。
constexpr std::string_view kSuffixChars = "~*>";

/// 单击（K-02）。
constexpr std::string_view kTapSuffix = "~";

/// 双击（K-04）：只有这一种 `*…` 形态。
constexpr std::string_view kDoubleSuffix = "*2";

/// 长按（K-03）的引导字符，后面跟毫秒数。
constexpr char kLongSuffix = '>';

/// 触发串与结果之间的分隔符。
constexpr char kRuleSeparator = ':';

/// 本期唯一的动作（K-09）。
constexpr std::string_view kPowerOffAction = "power_off";

/**
 * @brief 键路径是不是以某一段开头。
 * @param text 键路径原文。
 * @param prefix 要比的那一段。
 * @return 以它开头 true。
 */
bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/**
 * @brief 去掉两侧的空白。
 * @param text 原文。
 * @return 两端都不留空格与制表符的那一段。
 * @details 取值在 YAML 里通常是带引号的串，引号内的首尾空格会原样交过来，而触发
 *          串里的空格没有第二种含义：`l3 + r3` 与 `l3+r3` 是同一句话。键名用不到
 *          这一步——宿主的 IsToken 已经保证键段里没有空格。
 */
std::string_view Trim(std::string_view text) {
  constexpr std::string_view kBlanks = " \t";
  const std::size_t begin = text.find_first_not_of(kBlanks);
  if (begin == std::string_view::npos) return {};
  return text.substr(begin, text.find_last_not_of(kBlanks) - begin + 1);
}

/**
 * @brief 拼一行"这一条不生效"的 WARNING。
 * @param key 配置里的键路径原文（不含插件名，插件名在行首）。
 * @param value 取值原文。
 * @param why 不生效的原因。
 * @return 可以直接交给 host->log 的单行文本。
 * @note 行首那份 `插件名: plugins: 插件名:` 是插件自称加上这条配置在 YAML 里的
 *       位置，与随包 idle 的文案同形；宿主给插件日志加不了前缀（它只收一个字符
 *       串），所以这段只能由插件自己写全。
 */
std::string Rejected(std::string_view key, std::string_view value,
                     const std::string& why) {
  return "buttons: plugins: buttons: " + std::string(key) + " value '" +
         std::string(value) + "' " + why + " — this rule does nothing";
}

/**
 * @brief 记一行"这个键我不读"（C-05 的沉默，只到 DEBUG）。
 * @param host 宿主能力表。
 * @param key 键路径原文。
 * @details 写错的键名和还没实现的键名在语法层分不开：报成 WARNING 就是把后者也
 *          说成前者的错。
 */
void Ignored(const ds5Host& host, std::string_view key) {
  host.log(DS5_LOG_DEBUG,
           ("buttons: plugins: buttons: " + std::string(key) +
            " is not a key this plugin reads")
               .c_str());
}

/**
 * @brief 标量文本 → [min_value, max_value] 内的整数。
 * @param text 取值原文。
 * @param min_value 允许的下限（含）。
 * @param max_value 允许的上限（含）。
 * @param out 成功时写入数值。
 * @return 读得通 true。
 * @details 借 plugin::ParseInt 的严格十进制（只认非负数字，越界即拒）；它收的是
 *          NUL 结尾的串，所以这里先落地一份。
 */
bool ParseNumber(std::string_view text, int min_value, int max_value, int& out) {
  const std::string owned(text);
  return plugin::ParseInt(owned.c_str(), min_value, max_value, out);
}

/**
 * @brief `+` 分隔的一串 token → 位集合。
 * @param text 触发项或目标键那一段。
 * @param out 成功时写入并起来的位。
 * @param why 失败时写入一小段说明，接在 WARNING 的取值回显后面。
 * @param wire_only 这一串是**输出**（映射或宏的目标）时 true：摇杆方向与触摸板那
 *        一族都是判出来的形态，线上没有对应的位可以去置，做不了输出（G-03）。触发
 *        项传 false：同一个 token 在 `:` 左边是"与"，在右边是"同时按下"，只有右边
 *        有这条限制。
 * @return 读得通 true。
 */
bool ParseTermList(std::string_view text, ids::Mask& out, std::string& why,
                   bool wire_only) {
  if (Trim(text).empty()) {
    why = "is empty";
    return false;
  }
  out = 0;
  std::size_t begin = 0;
  for (;;) {
    const std::size_t plus = text.find('+', begin);
    const std::string_view piece =
        Trim(plus == std::string_view::npos ? text.substr(begin)
                                            : text.substr(begin, plus - begin));
    ids::Source source;
    if (piece.empty() || !ids::ParseToken(piece, source)) {
      why = "has a term that is not a key name";
      return false;
    }
    if (wire_only && !ids::IsWireBit(source)) {
      why = "lists a judged key (a stick direction or a touchpad form), which "
            "has no bit on the wire to press";
      return false;
    }
    out |= ids::BitOf(source);
    if (plus == std::string_view::npos) return true;
    begin = plus + 1;
  }
}

/**
 * @brief 触发串：一组 token，末尾可以带一个时序后缀。
 * @param text 冒号左边那一段。
 * @param out 成功时写入触发（项集合 + 形态 + 阈值）。
 * @param why 失败原因。
 * @return 读得通 true。
 * @details 后缀的位置不用猜：token 表里没有 `~`、`*`、`>`，所以这三个字符一出现
 *          就是后缀的起点，而它必须到串尾——`l3~+r3` 那种混合时序因此在这一层就
 *          被拒（一个后缀描述的是整组键的时序）。
 */
bool ParseTrigger(std::string_view text, rules::Trigger& out, std::string& why) {
  out = rules::Trigger{};
  std::string_view terms = text;

  const std::size_t start = text.find_first_of(kSuffixChars);
  if (start != std::string_view::npos) {
    if (text.find_first_of(kSuffixChars, start + 1) != std::string_view::npos) {
      why = "carries more than one timing suffix";
      return false;
    }
    terms = text.substr(0, start);
    const std::string_view suffix = Trim(text.substr(start));
    // 后缀排在所有 token 之后才成立：`l3~+r3` 说的是"单击 l3"再加上"与 r3 的
    // 组合"，同一条规则里两者互斥（K-01 vs K-02），所以这不是一个可以猜的写法。
    if (suffix.find('+') != std::string_view::npos) {
      why = "puts a timing suffix in the middle of the group; one suffix "
            "describes all the keys in front of it";
      return false;
    }
    if (suffix == kTapSuffix) {
      out.timing = rules::Timing::kTap;
      out.threshold_ms = rules::kDefaultTapMs;
    } else if (suffix == kDoubleSuffix) {
      out.timing = rules::Timing::kDouble;
      out.threshold_ms = rules::kDefaultDoubleMs;
    } else if (suffix.front() == kLongSuffix) {
      int ms = 0;
      if (!ParseNumber(Trim(suffix.substr(1)), rules::kMinThresholdMs,
                       rules::kMaxThresholdMs, ms)) {
        why = "'>' has to be followed by a hold time between " +
              std::to_string(rules::kMinThresholdMs) + " and " +
              std::to_string(rules::kMaxThresholdMs) + " milliseconds";
        return false;
      }
      out.timing = rules::Timing::kLong;
      out.threshold_ms = static_cast<uint32_t>(ms);
    } else {
      why = "has a suffix that is neither '~', '*2' nor '>毫秒'";
      return false;
    }
  }

  return ParseTermList(terms, out.terms, why, false);
}

/**
 * @brief 一条 shortcut 或 macro 的取值：`触发串 : 结果`。
 * @param value 取值原文。
 * @param result_name 冒号右边那段在文案里的名字。
 * @param trigger 成功时写入解析好的触发。
 * @param result 成功时指向冒号右边那一段。
 * @param why 失败原因。
 * @return 切得开且触发读得通 true。
 */
bool ParseRule(std::string_view value, std::string_view result_name,
               rules::Trigger& trigger, std::string_view& result,
               std::string& why) {
  const std::size_t colon = value.find(kRuleSeparator);
  if (colon == std::string_view::npos) {
    why = std::string("has no '") + kRuleSeparator + "' between the trigger and " +
          std::string(result_name);
    return false;
  }
  if (!ParseTrigger(value.substr(0, colon), trigger, why)) return false;
  result = Trim(value.substr(colon + 1));
  return true;
}

// ---------------------------------------------------------------------------
// 逐组的取值
// ---------------------------------------------------------------------------

/**
 * @brief remap 一行的取值语法：`disable` 或 `目标键`，后者可以带 `+keep`。
 * @param value 键后面那份标量原文。
 * @param targets 成功时写入目标位集合；`disable` 时为空集。
 * @param keep_source 成功时写入是否保留源键。
 * @param why 失败原因。
 * @return 取值读得通 true。
 */
bool ParseRemapValue(std::string_view value, ids::Mask& targets,
                     bool& keep_source, std::string& why) {
  keep_source = false;
  if (Trim(value) == kDisableValue) {
    targets = 0;
    return true;
  }

  std::string_view name = Trim(value);
  const std::size_t plus = name.find('+');
  if (plus != std::string_view::npos) {
    if (name.find('+', plus + 1) != std::string_view::npos) {
      why = "lists more than one target; a source maps to a single key";
      return false;
    }
    if (Trim(name.substr(plus)) != kKeepSuffix) {
      why = "carries a '+…' suffix this plugin does not know (only '+keep')";
      return false;
    }
    keep_source = true;
    name = Trim(name.substr(0, plus));
  }

  return ParseTermList(name, targets, why, true);
}

/**
 * @brief `remap.<源>: <目标>` 一行。
 * @param out 规则表。
 * @param host 只用来记日志。
 * @param key 键路径原文。
 * @param leaf 组名之后那一段（可以是空）。
 * @param value 取值原文。
 */
void LoadRemap(rules::Set& out, const ds5Host& host, std::string_view key,
               std::string_view leaf, std::string_view value) {
  if (leaf.empty()) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "has no key name after '" +
                                      std::string(kRemapGroup) + "'")
                 .c_str());
    return;
  }

  ids::Source source;
  if (!ids::ParseToken(leaf, source)) {
    Ignored(host, key);
    return;
  }

  // I-04：划动是一次性事件，没有"按住"的那一段。remap 的形态是"源按着、目标就按着"，
  // 套在事件位上等于把一次手势拖成一串按住；要的正是那一次输出时该写 macro。
  if (ids::IsSwipe(source)) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "maps a swipe, which is a one-shot event with "
                                  "no 'held down' to map; press a key for a set "
                                  "of strokes with macro.N instead")
                 .c_str());
    return;
  }

  rules::Remap rule{source, 0, false};
  std::string why;
  if (!ParseRemapValue(value, rule.targets, rule.keep_source, why)) {
    host.log(DS5_LOG_WARNING, Rejected(key, value, why).c_str());
    return;
  }
  out.remaps.push_back(rule);
}

/**
 * @brief `turbo.<键>: <每秒次数>` 一行。
 * @param out 规则表。
 * @param host 只用来记日志。
 * @param key 键路径原文。
 * @param leaf 组名之后那一段。
 * @param value 取值原文。
 */
void LoadTurbo(rules::Set& out, const ds5Host& host, std::string_view key,
               std::string_view leaf, std::string_view value) {
  if (leaf.empty()) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "has no key name after '" +
                                      std::string(kTurboGroup) + "'")
                 .c_str());
    return;
  }

  ids::Source source;
  if (!ids::ParseToken(leaf, source)) {
    Ignored(host, key);
    return;
  }

  // T-03：连点只在数字键上。扳机的形态是行程、触摸板只有一个物理开关，摇杆方向
  // 压根不是线上的位，三者都表达不出"按固定周期出现与消失"。
  if (!ids::IsWireBit(source) || source == ids::Source::kL2 ||
      source == ids::Source::kR2 || source == ids::Source::kPad) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "is not one of the digital keys turbo can "
                                  "hammer (triggers, touchpad and stick "
                                  "directions are excluded)")
                 .c_str());
    return;
  }

  int hz = 0;
  std::string why;
  if (!ParseNumber(Trim(value), rules::kMinHz, rules::kMaxHz, hz)) {
    why = "has to be a number of presses per second between " +
          std::to_string(rules::kMinHz) + " and " +
          std::to_string(rules::kMaxHz);
    host.log(DS5_LOG_WARNING, Rejected(key, value, why).c_str());
    return;
  }
  out.turbos.push_back(rules::Turbo{source, static_cast<uint16_t>(hz)});
}

/**
 * @brief `axis.` 一组的三种叶子：`invert_<轴>`、`center_<摇杆>`、`swap_lstick_rstick`。
 * @param out 规则表（改的是那张定长轴表）。
 * @param host 只用来记日志。
 * @param key 键路径原文。
 * @param leaf 组名之后那一段。
 * @param value 取值原文。
 * @details 三行落在同一根轴上也不冲突：每根轴的记录是"读哪根 → 要不要反向 →
 *          要不要一直居中"三件事拼起来的，pipeline 一次算完（M-15 的"同一帧原始
 *          值"由读取点保证，与配置怎么写无关）。
 */
void LoadAxis(rules::Set& out, const ds5Host& host, std::string_view key,
              std::string_view leaf, std::string_view value) {
  if (leaf.empty()) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "has no key name after '" +
                                      std::string(kAxisGroup) + "'")
                 .c_str());
    return;
  }

  const std::string_view setting = Trim(value);

  if (StartsWith(leaf, kInvertLeaf)) {
    ids::Axis axis;
    if (!ids::ParseAxisToken(leaf.substr(kInvertLeaf.size()), axis)) {
      Ignored(host, key);
      return;
    }
    if (setting == kTrueValue) {
      out.axis[ids::IndexOf(axis)].invert = true;
      return;
    }
    // false 与"没写这一行"同义（C-04：未出现的源自动恒等），所以它不进表。
    if (setting != kFalseValue) {
      host.log(DS5_LOG_WARNING,
               Rejected(key, value, "has to be 'true' or 'false'").c_str());
    }
    return;
  }

  if (StartsWith(leaf, kCenterLeaf)) {
    ids::Axis stick_x;
    if (!ids::ParseStickToken(leaf.substr(kCenterLeaf.size()), stick_x)) {
      Ignored(host, key);
      return;
    }
    if (setting != kAlwaysValue) {
      host.log(DS5_LOG_WARNING,
               Rejected(key, value,
                        "has to be 'always'; centering while a bound direction "
                        "is hit happens by itself")
                   .c_str());
      return;
    }
    // 居中说的是整根摇杆：X、Y 两格一起写，只居中一根会把摇杆推到对角线上。
    out.axis[ids::IndexOf(stick_x)].always_center = true;
    out.axis[ids::IndexOf(ids::StickOther(stick_x))].always_center = true;
    return;
  }

  if (leaf != kSwapLeaf) {
    Ignored(host, key);
    return;
  }

  if (setting != kSwapBoth && setting != kSwapX && setting != kSwapY) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "has to be 'both', 'x' or 'y'").c_str());
    return;
  }

  // 对调说的是"这一格读对方"，四根摇杆轴两两成对；按轴换就是只写其中一对。
  // 用 ids 的那两个配对函数而不是用下标的奇偶：布局前提已经钉在 ids 里。
  for (const ids::Axis stick_x : {ids::Axis::kLstickX, ids::Axis::kRstickX}) {
    const ids::Axis stick_y = ids::StickOther(stick_x);
    if (setting != kSwapY) {
      out.axis[ids::IndexOf(stick_x)].read_from = ids::SwapPartner(stick_x);
    }
    if (setting != kSwapX) {
      out.axis[ids::IndexOf(stick_y)].read_from = ids::SwapPartner(stick_y);
    }
  }
}

/**
 * @brief `shortcut.<编号>: "<触发串>: power_off"` 一行。
 * @param out 规则表。
 * @param host 只用来记日志。
 * @param key 键路径原文。
 * @param leaf 编号那一段（只是标签，本身不参与判定）。
 * @param value 取值原文。
 */
void LoadShortcut(rules::Set& out, const ds5Host& host, std::string_view key,
                  std::string_view leaf, std::string_view value) {
  if (leaf.empty()) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "has no key name after '" +
                                      std::string(kShortcutGroup) + "'")
                 .c_str());
    return;
  }

  rules::Shortcut rule;
  std::string_view action;
  std::string why;
  if (!ParseRule(value, "the action", rule.trigger, action, why)) {
    host.log(DS5_LOG_WARNING, Rejected(key, value, why).c_str());
    return;
  }
  if (action != kPowerOffAction) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value,
                      "is not an action this plugin performs (only 'power_off')")
                 .c_str());
    return;
  }
  out.shortcuts.push_back(rule);
}

/**
 * @brief `macro.<编号>: "<触发串>: <一组键>"` 一行。
 * @param out 规则表。
 * @param host 只用来记日志。
 * @param key 键路径原文。
 * @param leaf 编号那一段。
 * @param value 取值原文。
 */
void LoadMacro(rules::Set& out, const ds5Host& host, std::string_view key,
               std::string_view leaf, std::string_view value) {
  if (leaf.empty()) {
    host.log(DS5_LOG_WARNING,
             Rejected(key, value, "has no key name after '" +
                                      std::string(kMacroGroup) + "'")
                 .c_str());
    return;
  }

  rules::Macro rule;
  std::string_view targets;
  std::string why;
  if (!ParseRule(value, "the keys to press", rule.trigger, targets, why)) {
    host.log(DS5_LOG_WARNING, Rejected(key, value, why).c_str());
    return;
  }
  // 冒号右边是"同时按下"（A-04），与左边的"与"用同一个 `+`，两边的处理者不同。
  if (!ParseTermList(targets, rule.digits, why, true)) {
    host.log(DS5_LOG_WARNING, Rejected(key, value, why).c_str());
    return;
  }
  out.macros.push_back(rule);
}

}  // namespace

std::string Summary(const rules::Set& rules) {
  const auto axes =
      std::count_if(std::begin(rules.axis), std::end(rules.axis),
                    [](const rules::AxisRewrite& entry) {
                      return !entry.IsIdentity();
                    });
  return std::to_string(rules.remaps.size()) + " remap(s), " +
         std::to_string(axes) + " axis rewrite(s), " +
         std::to_string(rules.turbos.size()) + " turbo(s), " +
         std::to_string(rules.shortcuts.size()) + " shortcut(s), " +
         std::to_string(rules.macros.size()) + " macro(s)";
}

bool Load(const ds5Setting* settings, rules::Set& out, const ds5Host& host) {
  out = rules::Set{};
  if (settings == nullptr) return false;

  for (const ds5Setting* entry = settings; entry->key != nullptr; ++entry) {
    const std::string_view key(entry->key);
    const std::string_view value(entry->value == nullptr ? "" : entry->value);

    // 组名不认识一律忽略（C-05）：别家的插件的键、还没实现的功能组，在这一层
    // 都只是"不是我读的那一行"，一个字都不说。
    if (StartsWith(key, kRemapGroup)) {
      LoadRemap(out, host, key, key.substr(kRemapGroup.size()), value);
    } else if (StartsWith(key, kAxisGroup)) {
      LoadAxis(out, host, key, key.substr(kAxisGroup.size()), value);
    } else if (StartsWith(key, kTurboGroup)) {
      LoadTurbo(out, host, key, key.substr(kTurboGroup.size()), value);
    } else if (StartsWith(key, kShortcutGroup)) {
      LoadShortcut(out, host, key, key.substr(kShortcutGroup.size()), value);
    } else if (StartsWith(key, kMacroGroup)) {
      LoadMacro(out, host, key, key.substr(kMacroGroup.size()), value);
    }
  }
  return !out.Empty();
}

}  // namespace settings
}  // namespace buttons
