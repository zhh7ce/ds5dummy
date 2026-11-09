/**
 * @file
 * @brief 映射与轴改写的执行：数字位一趟、六根轴一趟，最后一处写回缓冲
 *
 * 两趟都在同一帧的快照上算，写出只在末尾的 EncodeFrame：M-06（链式不叠加）与
 * M-15（同帧原始值）在这个形状下是结构性的——本函数没有任何一处回读 state，
 * 一条规则的产物不会成为另一条规则的输入。
 */

#include "pipeline.h"

#include <algorithm>
#include <cstddef>

namespace buttons {
namespace pipeline {
namespace {

/// 能落到轴上的键位只有这两个扳机位；需求 M-16 说的"扳机的模拟量与数字位是
/// 同一个键的两半"在这里就是同一趟里各管一半。
constexpr ids::Source kTriggerDigits[] = {ids::Source::kL2, ids::Source::kR2};

/**
 * @brief 这条映射在这一帧有没有事可做。
 * @param rule 那条映射。
 * @param digits 映射层这一帧看到的位。
 * @param frame 快照。
 * @return 源按下（或源扳机有行程）时 true。
 * @details 扳机的"按下"有两半：数字位要压过阈值才置位，游戏读深度读的是轴值。
 *          只看数字位的话，"缓慢压 L2、R2 跟着动"（M-08 与实机验收 2）会在
 *          阈值之前整个不发生。
 */
bool Active(const rules::Remap& rule, ids::Mask digits, const ids::Frame& frame) {
  if ((digits & ids::BitOf(rule.src)) != 0) return true;
  const ids::Axis source_axis = ids::TriggerAxisOf(rule.src);
  return source_axis != ids::Axis::kCount &&
         frame.axis[ids::IndexOf(source_axis)] != ids::kTriggerReleased;
}

/**
 * @brief 一条映射给目标扳机带来的行程值。
 * @param rule 那条映射。
 * @param frame 快照。
 * @return 源是扳机时是源轴的实际深度（M-08 前半），否则按下为全按、松开为松开
 *         （M-08 末句"非扳机的键驱动扳机时是全按"）。
 * @note 取值只从快照读：`l2: r2` 与 `r2: l2` 同时在时，两根轴各拿到对方在这一
 *       帧的原始深度，而不是自己刚写出去的值（M-15）。
 */
uint8_t TravelValue(const rules::Remap& rule, const ids::Frame& frame) {
  const ids::Axis source_axis = ids::TriggerAxisOf(rule.src);
  if (source_axis != ids::Axis::kCount) return frame.axis[ids::IndexOf(source_axis)];
  return ids::kTriggerPressed;
}

/**
 * @brief 一条映射的轴侧后果：这一帧要抹掉哪根轴、给哪根轴多少行程。
 * @param rule 那条映射。
 * @param frame 快照。
 * @param hidden 输出：被换掉的扳机所在的那一格置位（M-09）。
 * @param travel 输出：目标扳机这一帧拿到的深度。
 * @param has_travel 输出：这一格有没有人投过值。
 * @details 数字位那侧的多对一是 OR（M-05），模拟量这侧唯一的对应形状就是取最深
 *          的那个：三条规则都投右扳机时，主机看到的是一个右扳机，不是三个。
 */
void ShapeAxes(const rules::Remap& rule, ids::Mask digits, const ids::Frame& frame,
               bool (&hidden)[ids::kAxisCount],
               uint8_t (&travel)[ids::kAxisCount],
               bool (&has_travel)[ids::kAxisCount]) {
  if (!rule.keep_source) {
    const ids::Axis source_axis = ids::TriggerAxisOf(rule.src);
    if (source_axis != ids::Axis::kCount) hidden[ids::IndexOf(source_axis)] = true;
  }
  if (!Active(rule, digits, frame)) return;
  for (const ids::Source digit : kTriggerDigits) {
    if ((rule.targets & ids::BitOf(digit)) == 0) continue;
    const std::size_t target = ids::IndexOf(ids::TriggerAxisOf(digit));
    const uint8_t value = TravelValue(rule, frame);
    if (!has_travel[target] || value > travel[target]) travel[target] = value;
    has_travel[target] = true;
  }
}

/**
 * @brief 一根轴这一帧该写出去的值：先选来源，再整形。
 * @param rewrite 这根轴的静态改写（互换的来处、要不要反向、要不要一直居中）。
 * @param decision 判定结果里的强制值那一档。
 * @param frame 快照。
 * @param index 这根轴的下标。
 * @param hidden 这根轴属于一个被换掉的扳机（M-09）。
 * @param travel 这根轴从映射拿到的行程。
 * @param has_travel 上一行有没有值。
 * @return 主机这一帧在这根轴上读到的字节。
 * @details 优先级是 强制（居中 / 宏全按）> 映射行程 > 抹掉 > 互换 > 快照里的自己，
 *          四档全部只在"取值"这一步里竞争；反向与一直居中是之后的整形，对选出来
 *          的值起作用。整形排在最后有个附带好处：静置点 128 是反向的不动点
 *          （M-14），所以"居中"与"反向"谁先谁后结果一样，这一族组合不必再判。
 */
uint8_t AxisValue(const rules::AxisRewrite& rewrite,
                  const sense::FrameDecision& decision, const ids::Frame& frame,
                  std::size_t index, bool hidden, uint8_t travel, bool has_travel) {
  uint8_t value;
  if (decision.forces_axis[index]) {
    value = decision.forced_axis[index];
  } else if (has_travel) {
    // 映射给这一格多加了一个驱动源，不是把这一格自己的手指抹掉：没人换掉这根
    // 扳机时取两者里更深的（主机看到的是一根扳机，不是两个来源）。hidden 那一
    // 档在下面，它说的是另一件事：这一格属于一个被换掉的源（M-15）。
    value = hidden ? travel : std::max(travel, frame.axis[index]);
  } else if (hidden) {
    value = ids::kTriggerReleased;
  } else {
    // 读的是快照里的另一根轴，不是刚写出去的那一格：这就是 M-15。
    const ids::Axis source =
        rewrite.read_from == ids::Axis::kCount
            ? static_cast<ids::Axis>(index)   // 下标小于 kCount，就是 Axis 之一
            : rewrite.read_from;
    value = frame.axis[ids::IndexOf(source)];
  }
  if (rewrite.invert) value = ids::Invert(value);
  if (rewrite.always_center) value = ids::kStickCenter;
  return value;
}

}  // namespace

void Apply(const rules::Set& rules, const sense::FrameDecision& decision,
           const ids::Frame& frame, uint8_t* state) {
  ids::Mask gate = 0;
  ids::Mask inject = 0;

  // 这根轴上的模拟量属于一个被换掉的扳机（M-09）：游戏读扳机读的是轴值，只抹
  // 数字位等于什么都没抹掉，所以源扳机的轴取松开值而不是快照。
  bool hidden[ids::kAxisCount] = {};
  uint8_t travel[ids::kAxisCount] = {};
  bool has_travel[ids::kAxisCount] = {};

  for (const rules::Remap& rule : rules.remaps) {
    const bool erased = !rule.keep_source;   // M-02 的默认是"换掉"。
    // 这一帧源的那个位是不是真的按着。扳机那两半各走各的（M-16）：浅压时数字位没
    // 有置起来，搬的只有轴那一半，所以这里看的是位而不是 Active。
    const bool held = (decision.digits & ids::BitOf(rule.src)) != 0;
    // 抹掉的是源在线上那一位：判出来的位里只有左/右半点击有对应的线上位（pad），
    // 其余的没有自己的字节（GateBitsOf 返回 0）。gate 因此要跟着"这一位真的按着"
    // 走——pad_left 那条规则抹的是 pad 那一位，而它能被别人按着：右半点击不该被
    // 一条只绑了左半的规则顺手抹掉。绑在线上位上的规则两种写法等价，因为没按下
    // 的位本来就不在 digits 里。
    if (erased && held) gate |= ids::GateBitsOf(rule.src);
    ShapeAxes(rule, decision.digits, frame, hidden, travel, has_travel);
    // 数字位跟着数字位搬（M-16）：+keep 因此只是"这条规则不把自己的源放进
    // gate"（M-03），不需要为它开第二个分支。
    if (held) inject |= rule.targets;
  }

  ids::Frame out;
  // 先抹后加：多个源映到同一目标自然是 OR（M-05），十字键的按轴合并与只抹一个
  // 方向（M-10/M-11）也都是这个掩码上的位操作。宏注入的位排在最后，不参与映射
  // （它要的就是那组键本身）。
  out.digits = (decision.digits & ~gate) | inject | decision.inject;

  for (std::size_t a = 0; a < ids::kAxisCount; ++a) {
    out.axis[a] = AxisValue(rules.axis[a], decision, frame, a, hidden[a], travel[a],
                            has_travel[a]);
  }

  ids::EncodeFrame(out, state);
}

}  // namespace pipeline
}  // namespace buttons
