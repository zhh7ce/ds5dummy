/**
 * @file
 * @brief buttons 触发判定的单测：K-01..K-05/K-07/K-08 的时序、T-01..T-05 的连点、
 *        A-01..A-05 的宏脉冲，以及 I-01/I-02 的摇杆方向
 *
 * 这一层不碰那 63 个字节：用例把快照一帧一帧喂进去、把虚拟时钟一格一格推上去，
 * 断言说的是"这一帧判定交出去的那组位与那几个强制值"。规则表直接摆（不经
 * settings），字节上的结果怎么落地由 buttons_pipeline_test.cpp 负责。
 *
 * 无 root、无手柄。
 */

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "plugin/buttons/ids.h"
#include "plugin/buttons/rules.h"
#include "plugin/buttons/sense.h"

namespace {

using buttons::ids::Axis;
using buttons::ids::BitOf;
using buttons::ids::Frame;
using buttons::ids::IndexOf;
using buttons::ids::kAxisCount;
using buttons::ids::kStickCenter;
using buttons::ids::kTriggerPressed;
using buttons::ids::kTriggerReleased;
using buttons::ids::Mask;
using buttons::ids::Source;
using buttons::rules::Action;
using buttons::rules::Macro;
using buttons::rules::Remap;
using buttons::rules::Set;
using buttons::rules::Shortcut;
using buttons::rules::Timing;
using buttons::rules::Trigger;
using buttons::rules::Turbo;
using buttons::sense::Evaluate;
using buttons::sense::FrameDecision;
using buttons::sense::Reset;

/// 一条组合键的两位：`create+options` 那种形状。
Mask Both(Source first, Source second) {
  return BitOf(first) | BitOf(second);
}

/// 一份静置的快照：四根摇杆轴在中心、两个扳机松开、没有任何位按下。
Frame AtRest() {
  Frame frame;
  for (std::size_t i = 0; i < kAxisCount; ++i) {
    frame.axis[i] = i < IndexOf(Axis::kLTrigger) ? kStickCenter : kTriggerReleased;
  }
  return frame;
}

/**
 * @brief 按下某几位、其余静置的快照。
 * @param digits 那些位。
 * @return 可以直接交给 Evaluate 的帧。
 */
Frame Pressed(Mask digits) {
  Frame frame = AtRest();
  frame.digits = digits;
  return frame;
}

/**
 * @brief 左摇杆摆在某个位置的快照。
 * @param x 左 X 的线上值。
 * @param y 左 Y 的线上值。
 * @return 其余一切静置的帧。
 */
Frame LeftStick(uint8_t x, uint8_t y) {
  Frame frame = AtRest();
  frame.axis[IndexOf(Axis::kLstickX)] = x;
  frame.axis[IndexOf(Axis::kLstickY)] = y;
  return frame;
}

/**
 * @brief 一根手指按在触摸板某处的快照。
 * @param digits 同一帧里按下的线上位（半区点击要带 pad）。
 * @param x 接触点的横坐标，0..4095。
 * @param y 接触点的纵坐标，向下增长。
 * @return 第一个触点接触、其余一切静置的帧。
 */
Frame Touch(Mask digits, uint16_t x, uint16_t y) {
  Frame frame = Pressed(digits);
  frame.contact[0].touching = true;
  frame.contact[0].x = x;
  frame.contact[0].y = y;
  return frame;
}

/**
 * @brief 两根手指分别按在两处的快照。
 * @param digits 同一帧里按下的线上位。
 * @param x0 第一根手指的横坐标。
 * @param y0 第一根手指的纵坐标。
 * @param x1 第二根手指的横坐标。
 * @param y1 第二根手指的纵坐标。
 * @return 两个触点都接触的那一帧。
 */
Frame TwoTouch(Mask digits, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  Frame frame = Touch(digits, x0, y0);
  frame.contact[1].touching = true;
  frame.contact[1].x = x1;
  frame.contact[1].y = y1;
  return frame;
}

/// 离中线足够远的两个横坐标：左半与右半（中线那条 ±8% 的带在 2048 附近）。
constexpr uint16_t kLeftX = 300;
constexpr uint16_t kRightX = 3800;

/// 一次划动从这一点走到那一点：相距 1000 个纵向刻度，折算到横向刻度是 562 个，
/// 过 ids::kSwipeMinSpan（491）那条线。
constexpr uint16_t kSwipeFromY = 2000;
constexpr uint16_t kSwipeToY = 1000;

/**
 * @brief 一份规则表加它该有的跨帧状态，按用例给的时刻逐帧判定。
 * @details 时钟完全由调用方决定：这一层本来就把时刻当参数收，所以单测里不需要
 *          任何"只在测试构建里存在"的拨表入口。
 */
class Driver {
 public:
  explicit Driver(Set rules) : rules_(std::move(rules)) { Reset(state_, rules_); }

  /**
   * @brief 判定一帧。
   * @param now_ms 这一帧的时刻。
   * @param frame 这一帧的快照。
   * @return 判定结果。
   */
  FrameDecision At(uint64_t now_ms, const Frame& frame) {
    return Evaluate(rules_, frame, now_ms, state_);
  }

 private:
  Set rules_;
  buttons::sense::State state_;
};

/**
 * @brief 一条触发。
 * @param timing 时序形态。
 * @param terms 要一起按下那些源。
 * @param threshold_ms 阈值或窗口；组合键不看它。
 * @return 填好的触发。
 */
Trigger Trig(Timing timing, Mask terms, uint32_t threshold_ms) {
  Trigger trigger;
  trigger.timing = timing;
  trigger.terms = terms;
  trigger.threshold_ms = threshold_ms;
  return trigger;
}

/**
 * @brief 只有一条组合键的规则表。
 * @param trigger 那条触发。
 * @return 命中之后请宿主机头的表。
 */
Set ShortcutOnly(const Trigger& trigger) {
  Set set;
  Shortcut shortcut;
  shortcut.trigger = trigger;
  shortcut.action = Action::kPowerOff;
  set.shortcuts.push_back(shortcut);
  return set;
}

/**
 * @brief 只有一条宏的规则表。
 * @param trigger 那条触发。
 * @param targets 脉冲期间按下的那些位。
 * @return 规则表。
 */
Set MacroOnly(const Trigger& trigger, Mask targets) {
  Set set;
  Macro macro;
  macro.trigger = trigger;
  macro.digits = targets;
  set.macros.push_back(macro);
  return set;
}

/**
 * @brief 只有一条连点的规则表。
 * @param src 要连点的那个键。
 * @param hz 每秒开合次数。
 * @return 规则表。
 */
Set TurboOnly(Source src, uint16_t hz) {
  Set set;
  Turbo turbo;
  turbo.src = src;
  turbo.hz = hz;
  set.turbos.push_back(turbo);
  return set;
}

/// 一条把某个键映到另一个键的映射；它唯一的用途是让那个源算"被绑过"（I-02）。
Set RemapOnly(Source src, Mask targets) {
  Set set;
  Remap rule;
  rule.src = src;
  rule.targets = targets;
  set.remaps.push_back(rule);
  return set;
}

/// 这一帧这根轴被强制取值了吗。
bool Forced(const FrameDecision& decision, Axis axis) {
  return decision.forces_axis[IndexOf(axis)];
}

/// 这一帧这根轴被强制成什么。
uint8_t ForcedValue(const FrameDecision& decision, Axis axis) {
  return decision.forced_axis[IndexOf(axis)];
}

// ---------------------------------------------------------------------------
// K-01/K-08：组合键
// ---------------------------------------------------------------------------

TEST(ButtonsSense, ChordFiresOncePerHold) {
  const Mask terms = Both(Source::kCreate, Source::kOptions);
  Driver driver(ShortcutOnly(Trig(Timing::kChord, terms, 0)));

  // 只按下组合里一个键不算命中。
  EXPECT_FALSE(driver.At(0, Pressed(BitOf(Source::kCreate))).power_off);
  EXPECT_TRUE(driver.At(8, Pressed(terms)).power_off);
  // 还按着不再出第二次：一次按下只出一次动作。
  EXPECT_FALSE(driver.At(16, Pressed(terms)).power_off);
  EXPECT_FALSE(driver.At(24, Pressed(terms)).power_off);
  // 全松开之后闸门重新打开（K-08），下一次按下照样命中。
  EXPECT_FALSE(driver.At(32, AtRest()).power_off);
  EXPECT_TRUE(driver.At(40, Pressed(terms)).power_off);
}

TEST(ButtonsSense, PartialReleaseDoesNotReArmTheGate) {
  // K-08 说的是"这组键必须全部松开"：留着一个键按着再敲另一个键，不该一下一下
  // 地出动作。
  const Mask terms = Both(Source::kCreate, Source::kOptions);
  Driver driver(ShortcutOnly(Trig(Timing::kChord, terms, 0)));

  EXPECT_TRUE(driver.At(0, Pressed(terms)).power_off);
  EXPECT_FALSE(driver.At(8, Pressed(BitOf(Source::kCreate))).power_off);
  EXPECT_FALSE(driver.At(16, Pressed(terms)).power_off);
  EXPECT_FALSE(driver.At(24, Pressed(BitOf(Source::kOptions))).power_off);
  EXPECT_FALSE(driver.At(32, Pressed(terms)).power_off);
  EXPECT_FALSE(driver.At(40, AtRest()).power_off);
  EXPECT_TRUE(driver.At(48, Pressed(terms)).power_off);
}

TEST(ButtonsSense, ChordNeedsEveryTerm) {
  Driver driver(ShortcutOnly(Trig(Timing::kChord,
                                  BitOf(Source::kL1) | BitOf(Source::kR1) |
                                      BitOf(Source::kL2),
                                  0)));
  EXPECT_FALSE(driver.At(0, Pressed(Both(Source::kL1, Source::kR1))).power_off);
  EXPECT_TRUE(driver.At(8, Pressed(BitOf(Source::kL1) | BitOf(Source::kR1) |
                                   BitOf(Source::kL2)))
                  .power_off);
}

// ---------------------------------------------------------------------------
// K-02/K-05：单击
// ---------------------------------------------------------------------------

TEST(ButtonsSense, QuickReleaseIsATap) {
  Driver driver(ShortcutOnly(Trig(Timing::kTap, BitOf(Source::kL3), 300)));

  // 按下的那些帧不出动作：按住多久只有松手这一刻才知道。
  EXPECT_FALSE(driver.At(0, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_FALSE(driver.At(50, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_TRUE(driver.At(100, AtRest()).power_off);
}

TEST(ButtonsSense, SlowReleaseIsNotATap) {
  Driver driver(ShortcutOnly(Trig(Timing::kTap, BitOf(Source::kL3), 300)));

  EXPECT_FALSE(driver.At(0, Pressed(BitOf(Source::kL3))).power_off);
  // 松手时已经按住 400 ms，超过 300 的阈值：这一轮什么都不是。
  EXPECT_FALSE(driver.At(400, AtRest()).power_off);
  // 再来一轮：这一帧只是按下，还不看结果。
  EXPECT_FALSE(driver.At(410, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_TRUE(driver.At(480, AtRest()).power_off);
}

// ---------------------------------------------------------------------------
// K-04：双击
// ---------------------------------------------------------------------------

TEST(ButtonsSense, SecondPressInsideTheWindowFires) {
  Driver driver(ShortcutOnly(Trig(Timing::kDouble, BitOf(Source::kL3), 300)));

  EXPECT_FALSE(driver.At(0, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_FALSE(driver.At(20, AtRest()).power_off);
  // 第二下：距第一次按下的起点 100 ms，在窗口内。
  EXPECT_TRUE(driver.At(100, Pressed(BitOf(Source::kL3))).power_off);
}

TEST(ButtonsSense, SecondPressOutsideTheWindowDoesNotFire) {
  Driver driver(ShortcutOnly(Trig(Timing::kDouble, BitOf(Source::kL3), 300)));

  EXPECT_FALSE(driver.At(0, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_FALSE(driver.At(20, AtRest()).power_off);
  EXPECT_FALSE(driver.At(400, Pressed(BitOf(Source::kL3))).power_off);
  // 这一下没成，但它的松手照样把第二下武装起来：从它算起的下一次才算双击。
  EXPECT_FALSE(driver.At(420, AtRest()).power_off);
  EXPECT_TRUE(driver.At(500, Pressed(BitOf(Source::kL3))).power_off);
}

TEST(ButtonsSense, FourQuickTapsAreTwoDoubles) {
  // 一次双击消耗两下：命中之后的那次松手不重新武装，所以四下出两次而不是三次。
  Driver driver(ShortcutOnly(Trig(Timing::kDouble, BitOf(Source::kL3), 300)));
  int fires = 0;
  for (uint64_t start = 0; start < 4; ++start) {
    const uint64_t base = start * 50;
    fires += driver.At(base, Pressed(BitOf(Source::kL3))).power_off ? 1 : 0;
    fires += driver.At(base + 20, AtRest()).power_off ? 1 : 0;
  }
  EXPECT_EQ(fires, 2);
}

// ---------------------------------------------------------------------------
// K-03/K-05：长按
// ---------------------------------------------------------------------------

TEST(ButtonsSense, LongPressFiresAtTheThresholdWithoutRelease) {
  Driver driver(ShortcutOnly(Trig(Timing::kLong, BitOf(Source::kL3), 750)));

  EXPECT_FALSE(driver.At(0, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_FALSE(driver.At(700, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_TRUE(driver.At(750, Pressed(BitOf(Source::kL3))).power_off);
  // 已经出过动作，继续按着不再出第二次。
  EXPECT_FALSE(driver.At(800, Pressed(BitOf(Source::kL3))).power_off);
  // 长按不看松手（K-05）：收手那一刻不会再补一次。
  EXPECT_FALSE(driver.At(900, AtRest()).power_off);
}

TEST(ButtonsSense, ReleaseBeforeTheLongPressThresholdFiresNothing) {
  Driver driver(ShortcutOnly(Trig(Timing::kLong, BitOf(Source::kL3), 750)));

  EXPECT_FALSE(driver.At(0, Pressed(BitOf(Source::kL3))).power_off);
  EXPECT_FALSE(driver.At(700, AtRest()).power_off);
}

// ---------------------------------------------------------------------------
// K-07：触发判定读的是塑形之前的物理位
// ---------------------------------------------------------------------------

TEST(ButtonsSense, InjectedBitsNeverFeedTheTrigger) {
  // 一条宏的目标位正好是另一条组合键的全部要求：宏那几毫秒压住的位不该把邻居
  // 触发掉——判定读的是快照，注入的结果不参与判定。
  Set rules = MacroOnly(Trig(Timing::kChord, BitOf(Source::kCircle), 0),
                        BitOf(Source::kL3));
  Shortcut chained;
  chained.trigger = Trig(Timing::kChord, BitOf(Source::kL3), 0);
  chained.action = Action::kPowerOff;
  rules.shortcuts.push_back(chained);

  Driver driver(std::move(rules));
  const FrameDecision hit = driver.At(0, Pressed(BitOf(Source::kCircle)));
  EXPECT_TRUE((hit.inject & BitOf(Source::kL3)) != 0);
  EXPECT_FALSE(hit.power_off);
  const FrameDecision held = driver.At(8, Pressed(BitOf(Source::kCircle)));
  EXPECT_TRUE((held.inject & BitOf(Source::kL3)) != 0);
  EXPECT_FALSE(held.power_off);

  // 玩家自己按下 L3 时那条组合键照样工作：判据没有坏，只是不看注入。
  EXPECT_FALSE(driver.At(200, AtRest()).power_off);
  EXPECT_TRUE(driver.At(210, Pressed(BitOf(Source::kL3))).power_off);
}

TEST(ButtonsSense, TurboFramesDoNotFeedTheTrigger) {
  // 同一个键既连点又被一条组合键要求：连点把位抹掉的那些帧不该让组合键判成"已
  // 松开"，也不该让它的闸门提前打开。
  Set rules = TurboOnly(Source::kCross, 10);
  Shortcut chord;
  chord.trigger = Trig(Timing::kChord, BitOf(Source::kCross), 0);
  chord.action = Action::kPowerOff;
  rules.shortcuts.push_back(chord);

  Driver driver(std::move(rules));
  EXPECT_TRUE(driver.At(0, Pressed(BitOf(Source::kCross))).power_off);
  // 连点的"关"半周期：交给映射的位里没有 ✕，但判定仍然认为它按着。
  const FrameDecision off = driver.At(50, Pressed(BitOf(Source::kCross)));
  EXPECT_EQ(off.digits & BitOf(Source::kCross), 0u);
  EXPECT_FALSE(off.power_off);
  EXPECT_FALSE(driver.At(100, Pressed(BitOf(Source::kCross))).power_off);
}

// ---------------------------------------------------------------------------
// T-01/T-02：连点的相位
// ---------------------------------------------------------------------------

TEST(ButtonsSense, FirstFrameOfAPressIsStillPressed) {
  Driver driver(TurboOnly(Source::kCircle, 10));
  // 半周期 50 ms：按下后的第一帧是"开"，不是一次玩家没按过的空拍。
  EXPECT_NE(driver.At(0, Pressed(BitOf(Source::kCircle))).digits &
            BitOf(Source::kCircle),
            0u);
}

TEST(ButtonsSense, TurboBlinksAtTheHalfPeriod) {
  Driver driver(TurboOnly(Source::kCircle, 10));
  const Mask circle = BitOf(Source::kCircle);

  EXPECT_NE(driver.At(0, Pressed(circle)).digits & circle, 0u);
  EXPECT_NE(driver.At(49, Pressed(circle)).digits & circle, 0u);
  EXPECT_EQ(driver.At(50, Pressed(circle)).digits & circle, 0u);
  EXPECT_EQ(driver.At(99, Pressed(circle)).digits & circle, 0u);
  EXPECT_NE(driver.At(100, Pressed(circle)).digits & circle, 0u);
}

TEST(ButtonsSense, ReleasingStopsTheBlinkAndRestartThePhase) {
  Driver driver(TurboOnly(Source::kCircle, 10));
  const Mask circle = BitOf(Source::kCircle);

  EXPECT_NE(driver.At(0, Pressed(circle)).digits & circle, 0u);   // 开
  EXPECT_EQ(driver.At(60, Pressed(circle)).digits & circle, 0u);  // 翻成关
  EXPECT_EQ(driver.At(70, AtRest()).digits & circle, 0u);         // 松手立即停
  // 重新按下从"开"起算，而不是接着上一轮的关半周期。
  EXPECT_NE(driver.At(80, Pressed(circle)).digits & circle, 0u);
}

TEST(ButtonsSense, TurboNeverInventsAPress) {
  Driver driver(TurboOnly(Source::kCircle, 10));
  // 没人按着的时候，连点一个位都不添。
  for (uint64_t t = 0; t < 500; t += 10) {
    EXPECT_EQ(driver.At(t, AtRest()).digits, 0u);
  }
}

TEST(ButtonsSense, ALateFrameDoesNotCauseAFlickerBurst) {
  // 读线程某一帧迟到很久时，相位从现在重新起算并且一个沿都不补：逐帧补偿的话，
  // 欠得越久之后每一帧都要翻一次来追，那是一串真机上根本不该出现的快闪。
  Driver driver(TurboOnly(Source::kCircle, 10));
  const Mask circle = BitOf(Source::kCircle);

  EXPECT_NE(driver.At(0, Pressed(circle)).digits & circle, 0u);
  EXPECT_NE(driver.At(1000, Pressed(circle)).digits & circle, 0u);
  EXPECT_NE(driver.At(1010, Pressed(circle)).digits & circle, 0u);
  EXPECT_NE(driver.At(1049, Pressed(circle)).digits & circle, 0u);
  // 重新起算之后照旧按半周期开合。
  EXPECT_EQ(driver.At(1050, Pressed(circle)).digits & circle, 0u);
  EXPECT_NE(driver.At(1100, Pressed(circle)).digits & circle, 0u);
}

TEST(ButtonsSense, FastTurboTracksTheFrameRateInsteadOfAliasing) {
  // T-02：达成频率只会低于配置值。60 Hz 的半周期是 8 ms，比 10 ms 的帧间隔还短，
  // 于是每一帧都到翻的时候——它一帧只翻一次，不会像按理想相位取样那样停在某一
  // 半周期上不动。
  Driver driver(TurboOnly(Source::kCircle, 60));
  const Mask circle = BitOf(Source::kCircle);

  int flashes = 0;
  bool last = true;
  for (uint64_t t = 0; t <= 200; t += 10) {
    const bool on = (driver.At(t, Pressed(circle)).digits & circle) != 0;
    if (on != last) ++flashes;
    last = on;
  }
  EXPECT_GE(flashes, 10);
}

// ---------------------------------------------------------------------------
// A-01..A-05：宏脉冲
// ---------------------------------------------------------------------------

TEST(ButtonsSense, MacroPulseLastsTheDefaultWindow) {
  Driver driver(MacroOnly(Trig(Timing::kChord, Both(Source::kL3, Source::kR3), 0),
                          BitOf(Source::kCircle)));
  const Mask held = Both(Source::kL3, Source::kR3);
  const Mask circle = BitOf(Source::kCircle);

  EXPECT_NE(driver.At(0, Pressed(held)).inject & circle, 0u);
  EXPECT_NE(driver.At(40, Pressed(held)).inject & circle, 0u);
  EXPECT_NE(driver.At(79, Pressed(held)).inject & circle, 0u);
  // 到点就收（A-02），而那组键还按着也不会再来一次（A-05：脉冲是一次性的）。
  EXPECT_EQ(driver.At(80, Pressed(held)).inject & circle, 0u);
  EXPECT_EQ(driver.At(120, Pressed(held)).inject & circle, 0u);
  // 全部松开来才能再触发（K-08 同一条闸门）。
  EXPECT_EQ(driver.At(130, AtRest()).inject & circle, 0u);
  EXPECT_NE(driver.At(140, Pressed(held)).inject & circle, 0u);
}

TEST(ButtonsSense, MacroKeepsPulsingAfterTheKeysAreReleased) {
  Driver driver(MacroOnly(Trig(Timing::kChord, BitOf(Source::kSquare), 0),
                          BitOf(Source::kCross)));
  const Mask square = BitOf(Source::kSquare);
  const Mask cross = BitOf(Source::kCross);

  EXPECT_NE(driver.At(0, Pressed(square)).inject & cross, 0u);
  // 脉冲量的是时间，不是"还按着"：手收走了，游戏那几毫秒里照样看到 ✕。
  EXPECT_NE(driver.At(10, AtRest()).inject & cross, 0u);
  EXPECT_NE(driver.At(70, AtRest()).inject & cross, 0u);
  EXPECT_EQ(driver.At(80, AtRest()).inject & cross, 0u);
}

TEST(ButtonsSense, MacroDrivesATriggerWithFullTravel) {
  Driver driver(MacroOnly(Trig(Timing::kChord, BitOf(Source::kSquare), 0),
                          BitOf(Source::kR2)));
  const FrameDecision hit = driver.At(0, Pressed(BitOf(Source::kSquare)));

  // A-03：扳机的两半是同一个键，数字位在 inject 里，轴那一半按全按强制取值。
  EXPECT_NE(hit.inject & BitOf(Source::kR2), 0u);
  EXPECT_TRUE(Forced(hit, Axis::kRTrigger));
  EXPECT_EQ(ForcedValue(hit, Axis::kRTrigger), kTriggerPressed);
  // 没被宏碰到的轴一个都不强制。
  EXPECT_FALSE(Forced(hit, Axis::kLTrigger));
  EXPECT_FALSE(Forced(hit, Axis::kLstickX));
  // 脉冲结束之后两半一起收。
  const FrameDecision after = driver.At(80, Pressed(BitOf(Source::kSquare)));
  EXPECT_FALSE(Forced(after, Axis::kRTrigger));
  EXPECT_EQ(after.inject & BitOf(Source::kR2), 0u);
}

TEST(ButtonsSense, MacroTapFiresOnReleaseAndPulses) {
  Driver driver(MacroOnly(Trig(Timing::kTap, BitOf(Source::kCreate), 300),
                          BitOf(Source::kOptions)));
  const Mask options = BitOf(Source::kOptions);

  EXPECT_EQ(driver.At(0, Pressed(BitOf(Source::kCreate))).inject & options, 0u);
  // 单击在松手那一刻命中，脉冲从松手开始计（A-01 的时序 + K-02 的结算点）。
  EXPECT_NE(driver.At(100, AtRest()).inject & options, 0u);
  EXPECT_NE(driver.At(179, AtRest()).inject & options, 0u);
  EXPECT_EQ(driver.At(180, AtRest()).inject & options, 0u);
}

// ---------------------------------------------------------------------------
// I-01/I-02/M-17：摇杆方向
// ---------------------------------------------------------------------------

TEST(ButtonsSense, DeflectionBecomesADirectionBit) {
  Driver driver(RemapOnly(Source::kCircle, BitOf(Source::kCross)));
  // 阈值 48 个刻度：正好压到线上算命中。
  EXPECT_NE(driver.At(0, LeftStick(176, kStickCenter)).digits &
                BitOf(Source::kLstickRight),
            0u);
  // 退到迟滞的松松开线（128+38）以内才算放开。
  EXPECT_EQ(driver.At(8, LeftStick(140, kStickCenter)).digits &
                BitOf(Source::kLstickRight),
            0u);
  // 线上 y 向下增长，所以"上"是负方向。
  EXPECT_NE(driver.At(16, LeftStick(kStickCenter, 80)).digits &
                BitOf(Source::kLstickUp),
            0u);
  // 同一根摇杆的两个垂直方向同时越过阈值就是对角。
  const FrameDecision diagonal = driver.At(24, LeftStick(200, 200));
  EXPECT_NE(diagonal.digits & BitOf(Source::kLstickRight), 0u);
  EXPECT_NE(diagonal.digits & BitOf(Source::kLstickDown), 0u);
}

TEST(ButtonsSense, DirectionLatchHasHysteresis) {
  // I-01：停在阈值上的摇杆不能一帧判有、一帧判无，所以松开线在触发线里面 10 个
  // 刻度处。
  Driver driver(RemapOnly(Source::kLstickRight, BitOf(Source::kCross)));
  const Mask right = BitOf(Source::kLstickRight);

  EXPECT_NE(driver.At(0, LeftStick(176, kStickCenter)).digits & right, 0u);
  EXPECT_NE(driver.At(8, LeftStick(170, kStickCenter)).digits & right, 0u);
  EXPECT_NE(driver.At(16, LeftStick(166, kStickCenter)).digits & right, 0u);
  // 128 + 38 = 166 是回迟滞的松松开线：再退一刻就放开了。
  EXPECT_EQ(driver.At(24, LeftStick(165, kStickCenter)).digits & right, 0u);
  EXPECT_EQ(driver.At(32, LeftStick(kStickCenter, kStickCenter)).digits & right, 0u);
}

TEST(ButtonsSense, ABoundDirectionCentersItsOwnStick) {
  // I-02 的缺省那一半：方向被规则绑过、而且这一帧正命中，整根摇杆强制居中。
  Driver driver(ShortcutOnly(Trig(Timing::kChord, BitOf(Source::kLstickUp), 0)));
  const FrameDecision hit = driver.At(0, LeftStick(200, 60));

  EXPECT_TRUE(hit.power_off);
  EXPECT_TRUE(Forced(hit, Axis::kLstickX));
  EXPECT_TRUE(Forced(hit, Axis::kLstickY));
  EXPECT_EQ(ForcedValue(hit, Axis::kLstickX), kStickCenter);
  EXPECT_EQ(ForcedValue(hit, Axis::kLstickY), kStickCenter);
  // 另一根摇杆虽然也推着，但没人绑它的方向，所以一个字都不动。
  EXPECT_FALSE(Forced(hit, Axis::kRstickX));
  EXPECT_FALSE(Forced(hit, Axis::kRstickY));
}

TEST(ButtonsSense, AnUnboundStickIsLeftAlone) {
  // 没有任何规则用过摇杆方向时，推到满也不该有轴被强制取值。
  Driver driver(RemapOnly(Source::kCircle, BitOf(Source::kCross)));
  const FrameDecision pushed = driver.At(0, LeftStick(255, 0));

  EXPECT_NE(pushed.digits & BitOf(Source::kLstickRight), 0u);
  EXPECT_NE(pushed.digits & BitOf(Source::kLstickUp), 0u);
  EXPECT_FALSE(Forced(pushed, Axis::kLstickX));
  EXPECT_FALSE(Forced(pushed, Axis::kLstickY));
}

TEST(ButtonsSense, DirectionsAreJudgedFromTheRawAxes) {
  // M-17：方向的判据读的是快照里那四根轴的原始值，所以轴反向与轴互换改不到它。
  Set rules = ShortcutOnly(Trig(Timing::kChord, BitOf(Source::kLstickUp), 0));
  rules.axis[IndexOf(Axis::kLstickX)].invert = true;
  rules.axis[IndexOf(Axis::kLstickY)].invert = true;
  Driver driver(std::move(rules));

  // 反向之后的"上"在游戏那边是下，但命中判据仍然是物理的那一侧。
  const FrameDecision hit = driver.At(0, LeftStick(kStickCenter, 60));
  EXPECT_TRUE(hit.power_off);
  EXPECT_NE(hit.digits & BitOf(Source::kLstickUp), 0u);
  EXPECT_EQ(hit.digits & BitOf(Source::kLstickDown), 0u);
}

TEST(ButtonsSense, AChordCanAskForAStickDirection) {
  // 组合键的另一半是方向位也成立：摇杆推到右 + L1 按下才算命中。
  Driver driver(ShortcutOnly(
      Trig(Timing::kChord, Both(Source::kL1, Source::kLstickRight), 0)));

  EXPECT_FALSE(driver.At(0, LeftStick(200, kStickCenter)).power_off);
  EXPECT_FALSE(driver.At(8, Pressed(BitOf(Source::kL1))).power_off);
  Frame chord = LeftStick(200, kStickCenter);
  chord.digits = BitOf(Source::kL1);
  EXPECT_TRUE(driver.At(16, chord).power_off);
}

TEST(ButtonsSense, ATriggerHasNoDirection) {
  // 扳机那两根轴不参与方向判定：压满行程不是"往某个方向推"。
  Driver driver(RemapOnly(Source::kL2, BitOf(Source::kR2)));
  Frame frame = AtRest();
  frame.axis[IndexOf(Axis::kLTrigger)] = kTriggerPressed;
  const FrameDecision decision = driver.At(0, frame);

  EXPECT_EQ(decision.digits, 0u);
  EXPECT_FALSE(Forced(decision, Axis::kLTrigger));
}

// ---------------------------------------------------------------------------
// I-03/I-04：触摸板的半区、触点数与划动
// ---------------------------------------------------------------------------

TEST(ButtonsSense, AContactIsJudgedButAHalfClickNeedsThePress) {
  // I-03 的三种互斥：手指落在左半而触摸板没被按下时，只有半区那个电平成立，
  // pad_left 不成立；线上那一位此时仍然只代表"任意半区"。
  Driver driver(RemapOnly(Source::kCircle, BitOf(Source::kCross)));

  const FrameDecision lifted = driver.At(0, Touch(0, kLeftX, kSwipeFromY));
  EXPECT_NE(lifted.digits & BitOf(Source::kTouchLeft), 0u);
  EXPECT_EQ(lifted.digits & BitOf(Source::kPadLeft), 0u);
  EXPECT_EQ(lifted.digits & BitOf(Source::kTouchRight), 0u);

  const FrameDecision clicked =
      driver.At(8, Touch(BitOf(Source::kPad), kLeftX, kSwipeFromY));
  EXPECT_NE(clicked.digits & BitOf(Source::kPadLeft), 0u);
}

TEST(ButtonsSense, TheHalfIsTheOneTheFingerLandedIn) {
  // I-03 的"按下那一刻定下、保持到松开"：同一根手指从左半划到右半，归属跟着起点
  // 而不是当前坐标，否则一次划动会中途改判成另一个键。
  Driver driver(RemapOnly(Source::kCircle, BitOf(Source::kCross)));

  EXPECT_NE(driver.At(0, Touch(0, kLeftX, kSwipeFromY)).digits &
                BitOf(Source::kTouchLeft),
            0u);
  const FrameDecision crossed = driver.At(8, Touch(0, kRightX, kSwipeFromY));
  EXPECT_NE(crossed.digits & BitOf(Source::kTouchLeft), 0u);
  EXPECT_EQ(crossed.digits & BitOf(Source::kTouchRight), 0u);
}

TEST(ButtonsSense, TwoContactsAreTheTwoFingerLevel) {
  // I-04 的"一到两指"：这一档看的是同一帧里有几个触点，与它们在哪个半区无关。
  Driver driver(RemapOnly(Source::kCircle, BitOf(Source::kCross)));

  EXPECT_EQ(driver.At(0, Touch(0, kLeftX, kSwipeFromY)).digits &
                BitOf(Source::kTouchTwo),
            0u);
  const FrameDecision both =
      driver.At(8, TwoTouch(0, kLeftX, kSwipeFromY, kRightX, kSwipeFromY));
  EXPECT_NE(both.digits & BitOf(Source::kTouchTwo), 0u);
  EXPECT_NE(both.digits & BitOf(Source::kTouchLeft), 0u);
  EXPECT_NE(both.digits & BitOf(Source::kTouchRight), 0u);
  // 一根抬手之后这一档就没了：它是电平，不是"这一轮摸过两指"。
  EXPECT_EQ(driver.At(16, Touch(0, kLeftX, kSwipeFromY)).digits &
                BitOf(Source::kTouchTwo),
            0u);
}

TEST(ButtonsSense, ASwipeFiresOncePerGestureAndReArmsOnLift) {
  // 划动位保持到所有触点离开，一次手势只发一个事件：K-08 那个"全松开才复位"的
  // 闸门因此看得见一次完整的按下—松开，而不是每帧都读到"没按下"。
  Driver driver(ShortcutOnly(Trig(Timing::kChord, BitOf(Source::kSwipeUp), 0)));

  // 落指的那一帧只记下起点，位移还没有。
  EXPECT_FALSE(driver.At(0, Touch(0, kLeftX, kSwipeFromY)).power_off);
  EXPECT_TRUE(driver.At(8, Touch(0, kLeftX, kSwipeToY)).power_off);
  // 手指还在面板上继续往上擦：不再出第二次。
  EXPECT_FALSE(driver.At(16, Touch(0, kLeftX, 500)).power_off);
  // 抬手，闸门重新打开；下一句手势从落指重新起算。
  EXPECT_FALSE(driver.At(24, AtRest()).power_off);
  EXPECT_FALSE(driver.At(32, Touch(0, kLeftX, kSwipeFromY)).power_off);
  EXPECT_TRUE(driver.At(40, Touch(0, kLeftX, kSwipeToY)).power_off);
}

TEST(ButtonsSense, ASwipeCarriesTheHalfItStartedIn) {
  // I-04 的"区分起始半区"就是拿半区那个锁存位当组合键的另一半：右半起手的左划
  // 与左半起手的左划是两条规则。
  Driver driver(ShortcutOnly(
      Trig(Timing::kChord, Both(Source::kSwipeLeft, Source::kTouchRight), 0)));

  EXPECT_FALSE(driver.At(0, Touch(0, kRightX, kSwipeFromY)).power_off);
  EXPECT_TRUE(driver.At(8, Touch(0, 2800, kSwipeFromY)).power_off);
  // 同样一句左划，起手在左半就不命中：1500 与 2800 都在各自半区的带外。
  EXPECT_FALSE(driver.At(16, AtRest()).power_off);
  EXPECT_FALSE(driver.At(24, Touch(0, 1500, kSwipeFromY)).power_off);
  EXPECT_FALSE(driver.At(32, Touch(0, 500, kSwipeFromY)).power_off);
}

TEST(ButtonsSense, ATouchpadBindingCentersNoAxis) {
  // 触摸板那族跟摇杆方向一样是判出来的位，但它没有"自己的那根摇杆"：绑了它的
  // 时候一根轴都不许被强制取值——sense 里那圈居中只走那八个方向。
  Driver driver(RemapOnly(Source::kPadLeft, BitOf(Source::kCross)));

  EXPECT_FALSE(driver.At(0, Touch(BitOf(Source::kPad), kLeftX, kSwipeFromY))
                    .power_off);
  const FrameDecision swiped =
      driver.At(8, Touch(BitOf(Source::kPad), kLeftX, kSwipeToY));
  EXPECT_NE(swiped.digits & BitOf(Source::kSwipeUp), 0u);
  EXPECT_NE(swiped.digits & BitOf(Source::kPadLeft), 0u);
  for (const Axis axis : {Axis::kLstickX, Axis::kLstickY, Axis::kRstickX,
                          Axis::kRstickY, Axis::kLTrigger, Axis::kRTrigger}) {
    EXPECT_FALSE(Forced(swiped, axis)) << buttons::ids::TokenOf(axis);
  }
}

}  // namespace
