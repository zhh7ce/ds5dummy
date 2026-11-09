/**
 * @file
 * @brief 规则表在内存里的形状
 *
 * 纯数据结构：配置文本怎么变成这些形状是 settings 的事，怎么按这些形状改一帧是
 * sense 与 pipeline 的事。这里只有形状本身，以及每个字段为什么非要有。
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ids.h"

namespace buttons {
namespace rules {

/**
 * @brief 一条映射：一个源 → 一组目标位。
 *
 * @details 条目之间没有任何引用关系，一帧只做一次"物理位 → 目标位"的搬运，
 *          所以链式映射不会叠加（M-06）：□→✕ 与 ✕→○ 是两条独立条目，按 □ 出 ○，
 *          按 ✕ 也出 ○。第二条的目的位在同一个快照上读不到第一条的结果，
 *          两跳因此表达不出来。
 */
struct Remap {
  /// 物理侧的那个键。没有默认值：条目只在解析出源之后才存在。
  ids::Source src;

  /**
   * @brief 目标位集合。
   * @details 多条规则指向同一个目标时，pipeline 只做一次按位或，多对一的 OR
   *          （M-05）因此不需要特判。空集就是"抹掉"（M-04），不需要第二个布尔量。
   *          源是扳机、目标里也有扳机时行程跟着搬（M-08/M-16）：那根轴由
   *          ids::TriggerAxisOf 从位上找出来，不在这里再记一遍。
   */
  ids::Mask targets = 0;

  /// 保留源键（M-03）：条目不把自己的源抹掉，目标跟着一起按下。
  bool keep_source = false;
};

/**
 * @brief 一根轴的静态改写：值从哪来、要不要反向、要不要一直居中。
 *
 * @details 三个开关合在一条记录里，是因为配置里的三个叶子（`swap_*` / `invert_*`
 *          / `center_*`）本来就能同时成立——"右摇杆 X 换过来之后再反向"是一句
 *          有意义的话，不需要判冲突。pipeline 因此对每根轴只做一次
 *          "读 read_from → 按 invert 翻 → 按 always_center 覆盖"。
 * @note 六根轴各占 Set 里的一格，是定长表而不是 vector：pipeline 不必每帧找
 *       有没有针对这根轴的条目，"这根轴没被改写"就是默认构造的那个形状。
 */
struct AxisRewrite {
  /**
   * @brief 取值来源那根轴，M-12 的互换在这里就是"读对方"。
   * @note Axis::kCount 表示"读这根轴自己"，也就是没有互换。选这个值而不是让
   *       默认构造填成 kLstickX，是为了让"没改写"有唯一形状（见 IsIdentity）。
   *       读的是快照，所以互换永远按同一帧的原始值一次算完（M-15）：先改写左 X
   *       再读左 X 那种边改边写在这里表达不出来。
   */
  ids::Axis read_from = ids::Axis::kCount;

  /// 反向这根轴，并且让静置点仍在静置（M-13/M-14，见 ids::Invert）。
  bool invert = false;

  /// 一直居中（I-02 的 always）。只在方向命中期间居中是运行期的事，走 FrameDecision。
  bool always_center = false;

  /**
   * @brief 这根轴有没有被任何规则动过。
   * @return 三个开关都是缺省值时 true。
   */
  bool IsIdentity() const {
    return read_from == ids::Axis::kCount && !invert && !always_center;
  }
};

/**
 * @brief 一个键的连点（T-01）。
 * @details 只有周期，没有占空比：一帧只能表达一次开或关，所以缺省就是均匀的两半。
 *          达成频率只会低于配置值（T-02 的"宁可偏低"）——sense 按经过的毫秒数推
 *          相位，不按帧数推，因为帧率是 100～120 Hz 的浮动值。
 */
struct Turbo {
  ids::Source src;    ///< 要连点的那个键：只有线上真存在的位可以连点（T-03）。
  uint16_t hz;        ///< 每秒开合次数。
};

/// 触发形态，就是需求 K-01～K-04 的那四种。
enum class Timing : uint8_t {
  kChord,    ///< 组合：全部按下即命中，不等时间。
  kTap,      ///< 单击：松手时按下时长短于阈值才算。
  kDouble,   ///< 双击：两次按下间隔短于窗口才算。
  kLong,     ///< 长按：按下超过阈值即触发，不等松手。
};

/**
 * @brief 一次触发：一组源加上它们的时序形态。
 * @note 整组只有一个时序（C-02）：`+` 左边是"与"，后缀描述这一整组键，所以
 *       `l3~+r3` 那种混合时序在语法层就被拒，不占这里的形状。
 */
struct Trigger {
  ids::Mask terms = 0;              ///< 要一起按下的那些源。
  Timing timing = Timing::kChord;   ///< 时序形态。
  uint32_t threshold_ms = 0;        ///< 阈值或窗口，按 timing 解释；组合不看这个值。
};

/// 命中之后做什么。本期只有让手柄自己关机（K-09）。
enum class Action : uint8_t {
  kPowerOff,
};

/**
 * @brief 一条组合键：命中之后请宿主做一件事。
 * @details 动作不在这里执行——识别在输入钩子里，执行在心跳里（K-10），所以这条
 *          记录只放"做哪件事"，不放怎么调用。
 */
struct Shortcut {
  Trigger trigger;
  Action action = Action::kPowerOff;
};

/**
 * @brief 一条宏：命中之后把一组键按住一段脉冲时长，然后自动释放（A-01）。
 * @details 目标之间是"同时按下"（A-04），不描述先后与释放顺序。目标位里含扳机时
 *          那一根的模拟量按全按给出（A-03）：扳机的两半是同一个键，
 *          ids::TriggerAxisOf 从位上就能找到那根轴，所以这里不需要第二个字段。
 * @note 脉冲时长不在配置里：本期没有逐条覆盖它的语法，sense 直接用
 *       kDefaultPulseMs。留着这个字段就等于留一个没人能拧的旋钮。
 */
struct Macro {
  Trigger trigger;
  ids::Mask digits = 0;     ///< 脉冲期间按下的那些位。
};

/**
 * @brief 一份配置解析完得到的全部规则。
 * @note 五张表都是"init 之后只读、运行期只在 bt 读线程上读"，容量在 init 定死，
 *       运行期不增不减——这是这一层不加任何锁的依据。
 */
struct Set {
  std::vector<Remap> remaps;
  std::vector<Turbo> turbos;
  std::vector<Shortcut> shortcuts;
  std::vector<Macro> macros;

  /// 六根轴的静态改写，下标由 ids::IndexOf 给出；默认构造即"不改写"。
  AxisRewrite axis[ids::kAxisCount];

  /**
   * @brief 整份配置里没有任何生效的规则。
   * @return 没有任何生效规则时 true；init 据此返回 0，等于整个插件这一轮不生效
   *         （C-04：`buttons` 整节没写、或只写了不认识的东西，都走这里）。
   * @note 轴表按内容判，不按"有没有写过这一组"判：只写一句 `invert_rstick_x`
   *       同样是要生效的配置，而解析失败的条目根本不会进表。
   */
  bool Empty() const {
    return remaps.empty() && turbos.empty() && shortcuts.empty() && macros.empty() &&
           std::all_of(std::begin(axis), std::end(axis),
                       [](const AxisRewrite& entry) { return entry.IsIdentity(); });
  }
};

/// 单击的缺省阈值（K-02）：按下时长短于这个值的松手算单击。
constexpr uint32_t kDefaultTapMs = 300;

/// 双击的缺省窗口（K-04）：两次按下间隔短于这个值算双击。
constexpr uint32_t kDefaultDoubleMs = 300;

/// 宏脉冲的缺省时长（A-02）：游戏每帧只采样一次，所以要覆盖好几帧。80 ms 在
/// 100～120 Hz 下是 8～10 帧，也是 Studio 的同值。
constexpr uint32_t kDefaultPulseMs = 80;

/// 阈值与窗口的合法区间；超出即这一条不生效 + WARNING（G-06）。
constexpr int kMinThresholdMs = 50;
constexpr int kMaxThresholdMs = 10000;

/// 连点频率的合法区间（T-02 的上限来自帧率的一半）。
constexpr int kMinHz = 1;
constexpr int kMaxHz = 60;

}  // namespace rules
}  // namespace buttons
