/**
 * @file
 * @brief buttons 插件执行的单测：M-01..M-16 的表算法，外加 sense 交来的那两档位
 *
 * 每个用例都从宿主的编码器造输入、经宿主的解码器读输出：断言说的是"主机看到的
 * 那个字段"，插件内部的位集合与 63 字节的编排都不出现在期望值里。规则表直接
 * 摆（不经 settings），判定结果也直接摆（不经 sense），所以这里测的是算法不是
 * 语法、也不是时序——后者在 buttons_sense_test.cpp。
 *
 * 无 root、无手柄。
 */

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "core/protocol/input_state.h"
#include "core/protocol/report.h"
#include "gtest/gtest.h"
#include "plugin/buttons/ids.h"
#include "plugin/buttons/pipeline.h"
#include "plugin/buttons/rules.h"
#include "plugin/buttons/sense.h"

namespace {

using buttons::ids::Axis;
using buttons::ids::BitOf;
using buttons::ids::DecodeFrame;
using buttons::ids::Frame;
using buttons::ids::IndexOf;
using buttons::ids::Mask;
using buttons::ids::Source;
using buttons::rules::AxisRewrite;
using buttons::rules::Remap;
using buttons::rules::Set;
using buttons::sense::FrameDecision;
using core::protocol::DecodeInputState;
using core::protocol::Direction;
using core::protocol::EncodeInputState;
using core::protocol::InputState;
using core::protocol::kUsbInputStateSize;

/**
 * @brief 用宿主的编码器造一份状态体。
 * @param state 语义模型。
 * @return 63 字节的线格式。
 */
std::vector<uint8_t> Wire(const InputState& state) {
  std::vector<uint8_t> body(kUsbInputStateSize);
  EncodeInputState(state, body.data());
  return body;
}

/**
 * @brief 一条映射。
 * @param src 源键。
 * @param targets 目标位集合，0 就是"抹掉"。
 * @param keep_source 保留源键（M-03）。
 * @return 填好的条目。
 */
Remap Rule(Source src, Mask targets, bool keep_source = false) {
  Remap rule;
  rule.src = src;
  rule.targets = targets;
  rule.keep_source = keep_source;
  return rule;
}

/**
 * @brief 只有映射的规则表：其余四张留空。
 * @param entries 那些映射。
 * @return 一份可以直接交给 Apply 的规则表。
 */
Set RemapOnly(std::initializer_list<Remap> entries) {
  Set set;
  for (const Remap& entry : entries) set.remaps.push_back(entry);
  return set;
}

/**
 * @brief "sense 这一帧什么都没判"的判定结果。
 * @param frame 这一帧的快照。
 * @return 映射层看到的就是物理位、没有任何注入与强制值的判定结果。
 */
FrameDecision Plain(const Frame& frame) {
  FrameDecision decision;
  decision.digits = frame.digits;
  return decision;
}

/**
 * @brief 在"什么都没判"之外再摆上判出来的那几位。
 * @param frame 这一帧的快照。
 * @param judged sense 那一层补齐的位：摇杆方向或触摸板形态，它们不在 frame.digits 里。
 * @return 映射层这一帧看到的位 = 线上的位 + 那几位。
 * @details 触摸板那个触点坐标在这一层不参与判定（判它是 ids::TouchpadStates 的活，
 *          已在 buttons_ids_test.cpp 逐条断言过），所以这里的用例直接把判好的位
 *          摆进来，测的只是 gate 该抹哪一位。
 */
FrameDecision Judged(const Frame& frame, Mask judged) {
  FrameDecision decision = Plain(frame);
  decision.digits |= judged;
  return decision;
}

/**
 * @brief 把规则作用到一份状态体上并用宿主的解码器读回来。
 * @param rules 规则表。
 * @param input 手柄这一帧报的状态。
 * @param decision 这一帧的判定结果。
 * @return 主机将会看到的状态。
 */
InputState Pass(const Set& rules, const InputState& input,
                const FrameDecision& decision) {
  std::vector<uint8_t> body = Wire(input);
  buttons::pipeline::Apply(rules, decision, DecodeFrame(body.data()), body.data());
  return DecodeInputState(body.data());
}

/// 上一份的常用形状：没有判定结果可摆的用例。
InputState Pass(const Set& rules, const InputState& input) {
  std::vector<uint8_t> body = Wire(input);
  const Frame frame = DecodeFrame(body.data());
  return Pass(rules, input, Plain(frame));
}

/// 一份静置的状态：摇杆在中心，其余全松。
InputState AtRest() {
  InputState state;
  state.left_stick_x = 128;
  state.left_stick_y = 128;
  state.right_stick_x = 128;
  state.right_stick_y = 128;
  return state;
}

// ---------------------------------------------------------------------------
// 数字位：换掉、保留、抹掉、多对一、不叠加
// ---------------------------------------------------------------------------

TEST(ButtonsPipeline, ReplacesTheSourceByKey) {
  // M-01 + M-02：默认是换掉，源键不再出现在主机面前。
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kCross))});
  InputState input = AtRest();
  input.button_circle = true;
  input.button_square = true;

  const InputState output = Pass(rules, input);
  EXPECT_TRUE(output.button_cross);
  EXPECT_FALSE(output.button_circle);
  EXPECT_TRUE(output.button_square);
}

TEST(ButtonsPipeline, KeepAddsTheTargetBesideTheSource) {
  // M-03：+keep 是叠加不是替换。
  const Set rules =
      RemapOnly({Rule(Source::kCircle, BitOf(Source::kCross), true)});
  InputState input = AtRest();
  input.button_circle = true;

  const InputState output = Pass(rules, input);
  EXPECT_TRUE(output.button_circle);
  EXPECT_TRUE(output.button_cross);
}

TEST(ButtonsPipeline, DisableRemovesTheKeyAndAddsNothing) {
  // M-04：抹掉 = 目标位集合为空；别的键照常通过。
  const Set rules = RemapOnly({Rule(Source::kMute, 0)});
  InputState input = AtRest();
  input.button_mute = true;
  input.button_home = true;

  const InputState output = Pass(rules, input);
  EXPECT_FALSE(output.button_mute);
  EXPECT_TRUE(output.button_home);
}

TEST(ButtonsPipeline, SeveralSourcesOnOneTargetIsAnOr) {
  // M-05：两条规则指向同一个目标，任一按下即目标按下。
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kCross)),
                               Rule(Source::kSquare, BitOf(Source::kCross))});

  InputState one = AtRest();
  one.button_circle = true;
  EXPECT_TRUE(Pass(rules, one).button_cross);

  InputState other = AtRest();
  other.button_square = true;
  EXPECT_TRUE(Pass(rules, other).button_cross);

  InputState both = AtRest();
  both.button_circle = true;
  both.button_square = true;
  const InputState output = Pass(rules, both);
  EXPECT_TRUE(output.button_cross);
  EXPECT_FALSE(output.button_circle);
  EXPECT_FALSE(output.button_square);
}

TEST(ButtonsPipeline, ChainsDoNotCompound) {
  // M-06：每条规则只读物理位，一帧只搬一次。□→✕ 且 ✕→○ 时，按 □ 出 ✕、
  // 按 ✕ 出 ○——一条链不会在两格里各走一步。
  const Set rules = RemapOnly({Rule(Source::kSquare, BitOf(Source::kCross)),
                               Rule(Source::kCross, BitOf(Source::kCircle))});

  InputState square = AtRest();
  square.button_square = true;
  InputState out = Pass(rules, square);
  EXPECT_TRUE(out.button_cross);
  EXPECT_FALSE(out.button_circle);
  EXPECT_FALSE(out.button_square);

  InputState cross = AtRest();
  cross.button_cross = true;
  out = Pass(rules, cross);
  EXPECT_TRUE(out.button_circle);
  EXPECT_FALSE(out.button_cross);

  // 两个键同时按下：□ 的结果是 ✕、✕ 的结果是 ○，各走各的一跳。
  InputState both = AtRest();
  both.button_square = true;
  both.button_cross = true;
  out = Pass(rules, both);
  EXPECT_TRUE(out.button_circle);
  EXPECT_TRUE(out.button_cross);
  EXPECT_FALSE(out.button_square);
}

TEST(ButtonsPipeline, AnUnpressedSourceLeavesItsTargetAlone) {
  // 一条没被触发的规则什么都不做：○→✕ 不该让没人按的 ✕ 自己亮起来。
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kCross))});
  EXPECT_FALSE(Pass(rules, AtRest()).button_cross);
}

TEST(ButtonsPipeline, AHalfClickErasesTheTouchpadPress) {
  // I-03 + M-02：换成 ✕ 之后主机不能再看到"触摸板被按下"。那一位不在 kPadLeft
  // 自己身上——它在线上就是 pad 那一位，所以 gate 靠 GateBitsOf 把它展开进来。
  const Set rules = RemapOnly({Rule(Source::kPadLeft, BitOf(Source::kCross))});
  InputState input = AtRest();
  input.button_pad = true;

  std::vector<uint8_t> body = Wire(input);
  const Frame frame = DecodeFrame(body.data());
  const InputState output =
      Pass(rules, input, Judged(frame, BitOf(Source::kTouchLeft) |
                                           BitOf(Source::kPadLeft)));
  EXPECT_TRUE(output.button_cross);
  EXPECT_FALSE(output.button_pad);
}

TEST(ButtonsPipeline, AClickOnTheOtherHalfIsNotTheBoundOne) {
  // 同一份线上字节，触点在右半：这一帧判出的位里没有 pad_left，那条规则因此不算
  // 被触发，触摸板照原样交出去。
  const Set rules = RemapOnly({Rule(Source::kPadLeft, BitOf(Source::kCross))});
  InputState input = AtRest();
  input.button_pad = true;

  std::vector<uint8_t> body = Wire(input);
  const Frame frame = DecodeFrame(body.data());
  const InputState output =
      Pass(rules, input, Judged(frame, BitOf(Source::kTouchRight) |
                                           BitOf(Source::kPadRight)));
  EXPECT_FALSE(output.button_cross);
  EXPECT_TRUE(output.button_pad);
}

TEST(ButtonsPipeline, AJudgedSourceErasesNoByteOfItsOwn) {
  // 绑"左半有手指"而不是"左半点击"时，源没有自己的线上位，gate 也就是空的：目标
  // 加了，触摸板那一位仍然在。要抹掉那一下得绑 pad_left。
  const Set rules = RemapOnly({Rule(Source::kTouchLeft, BitOf(Source::kCircle))});
  InputState input = AtRest();
  input.button_pad = true;

  std::vector<uint8_t> body = Wire(input);
  const Frame frame = DecodeFrame(body.data());
  const InputState output =
      Pass(rules, input, Judged(frame, BitOf(Source::kTouchLeft)));
  EXPECT_TRUE(output.button_circle);
  EXPECT_TRUE(output.button_pad);
}

// ---------------------------------------------------------------------------
// 十字键：按轴合并、只抹一个方向
// ---------------------------------------------------------------------------

TEST(ButtonsPipeline, ErasingOneDirectionKeepsTheOther) {
  // M-11：按住"右上"再抹掉"右"，主机看到"上"。
  const Set rules = RemapOnly({Rule(Source::kDpadRight, 0)});
  InputState input = AtRest();
  input.dpad = Direction::kNorthEast;
  EXPECT_EQ(Pass(rules, input).dpad, Direction::kNorth);
}

TEST(ButtonsPipeline, InjectedDirectionMergesWithTheHeldOne) {
  // M-10：注入的方向在自己那一轴胜出，玩家另一轴的倾向保留。
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kDpadRight))});
  InputState input = AtRest();
  input.dpad = Direction::kNorth;
  input.button_circle = true;
  EXPECT_EQ(Pass(rules, input).dpad, Direction::kNorthEast);
}

TEST(ButtonsPipeline, OppositeDirectionsOnOneAxisFallBackToCentered) {
  // M-10：两个相反方向同时被注入，那一轴回到居中；十字键只有一个方向字段，
  // 这个形状是唯一能表达的结果。
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kDpadUp)),
                               Rule(Source::kSquare, BitOf(Source::kDpadDown))});
  InputState input = AtRest();
  input.button_circle = true;
  input.button_square = true;
  EXPECT_EQ(Pass(rules, input).dpad, Direction::kNone);
}

TEST(ButtonsPipeline, PerpendicularDirectionsMakeADiagonal) {
  // M-07：两个源映到十字键的两个不同方向时，四个方向位的模型能表达对角，
  // 不必像单个 hat 字段那样丢一个。
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kDpadUp)),
                               Rule(Source::kSquare, BitOf(Source::kDpadRight))});
  InputState input = AtRest();
  input.button_circle = true;
  input.button_square = true;
  EXPECT_EQ(Pass(rules, input).dpad, Direction::kNorthEast);
}

// ---------------------------------------------------------------------------
// 扳机：两半一起搬
// ---------------------------------------------------------------------------

TEST(ButtonsPipeline, TriggerToTriggerCarriesTheTravel) {
  // M-08 + M-16：右扳机的模拟量等于左扳机的实际深度，不是一个开关。
  const Set rules = RemapOnly({Rule(Source::kL2, BitOf(Source::kR2))});
  InputState input = AtRest();
  input.trigger_left = 90;
  input.button_l2 = true;

  const InputState output = Pass(rules, input);
  EXPECT_EQ(output.trigger_right, 90);
  EXPECT_EQ(output.trigger_left, 0);
  EXPECT_FALSE(output.button_l2);
  EXPECT_TRUE(output.button_r2);
}

TEST(ButtonsPipeline, AnalogHalfMovesWithoutTheDigitalHalf) {
  // M-16 的两半各自对应：浅压左扳机（数字位还没过阈值）只搬行程，不搬按下位。
  const Set rules = RemapOnly({Rule(Source::kL2, BitOf(Source::kR2))});
  InputState input = AtRest();
  input.trigger_left = 90;

  const InputState output = Pass(rules, input);
  EXPECT_EQ(output.trigger_right, 90);
  EXPECT_FALSE(output.button_r2);
  EXPECT_FALSE(output.button_l2);
}

TEST(ButtonsPipeline, ErasingATriggerZeroesItsAnalogValue) {
  // M-09：游戏读扳机读的是轴值，只抹数字位等于什么都没抹掉。
  const Set rules = RemapOnly({Rule(Source::kL2, 0)});
  InputState input = AtRest();
  input.trigger_left = 200;
  input.button_l2 = true;

  const InputState output = Pass(rules, input);
  EXPECT_EQ(output.trigger_left, 0);
  EXPECT_FALSE(output.button_l2);
}

TEST(ButtonsPipeline, AKeyDrivingATriggerIsAFullPress) {
  // M-08 末句：非扳机的键驱动扳机时是全按。
  const Set rules = RemapOnly({Rule(Source::kCross, BitOf(Source::kR2))});
  InputState pressed = AtRest();
  pressed.button_cross = true;
  const InputState output = Pass(rules, pressed);
  EXPECT_EQ(output.trigger_right, 255);
  EXPECT_TRUE(output.button_r2);
  EXPECT_FALSE(output.button_cross);

  // 没按下的时候目标扳机自己该是什么就是什么。
  EXPECT_EQ(Pass(rules, AtRest()).trigger_right, 0);
}

TEST(ButtonsPipeline, DeepestSourceWinsOnOneTrigger) {
  // 数字位那侧的多对一是 OR，模拟量这侧取最大值：两条规则投同一根扳机时，
  // 主机看到的是最深的那一个。
  const Set rules = RemapOnly({Rule(Source::kL2, BitOf(Source::kR2)),
                               Rule(Source::kCross, BitOf(Source::kR2))});
  InputState input = AtRest();
  input.trigger_left = 90;
  input.button_cross = true;
  EXPECT_EQ(Pass(rules, input).trigger_right, 255);

  InputState shallow = AtRest();
  shallow.trigger_left = 90;
  shallow.trigger_right = 120;
  // 左扳机只贡献行程，右扳机自己的轴没被任何规则抹掉，所以取两者里更深的。
  EXPECT_EQ(Pass(rules, shallow).trigger_right, 120);
}

TEST(ButtonsPipeline, BothHalvesReadTheSameFrame) {
  // M-15：互换按同一帧的原始值一次算完。l2↔r2 是两条规则，各拿到对方的深度，
  // 而不是自己刚写出去的那一份。
  const Set rules = RemapOnly({Rule(Source::kL2, BitOf(Source::kR2)),
                               Rule(Source::kR2, BitOf(Source::kL2))});
  InputState input = AtRest();
  input.trigger_left = 100;
  input.trigger_right = 200;
  input.button_r2 = true;

  const InputState output = Pass(rules, input);
  EXPECT_EQ(output.trigger_left, 200);
  EXPECT_EQ(output.trigger_right, 100);
  EXPECT_TRUE(output.button_l2);
  EXPECT_FALSE(output.button_r2);
}

TEST(ButtonsPipeline, KeepLeavesTheSourceTriggerReporting) {
  // M-03 在扳机上同样成立：+keep 时源键的两半都照常发出。
  const Set rules =
      RemapOnly({Rule(Source::kL2, BitOf(Source::kR2), true)});
  InputState input = AtRest();
  input.trigger_left = 90;
  input.button_l2 = true;

  const InputState output = Pass(rules, input);
  EXPECT_EQ(output.trigger_left, 90);
  EXPECT_EQ(output.trigger_right, 90);
  EXPECT_TRUE(output.button_l2);
  EXPECT_TRUE(output.button_r2);
}

// ---------------------------------------------------------------------------
// 轴改写：互换、反向、居中（M-12～M-15）
// ---------------------------------------------------------------------------

/// 只有一格轴改写的规则表；其余五格保持"不改写"。
Set AxisRules(Axis axis, const AxisRewrite& rewrite) {
  Set set;
  set.axis[IndexOf(axis)] = rewrite;
  return set;
}

TEST(ButtonsPipeline, SwappedSticksExchangeTheirPositions) {
  // M-12 + M-15：整根对调是四格"读对方"，每格读的都是快照里对方那一帧的值。
  Set set;
  for (const Axis axis : {Axis::kLstickX, Axis::kLstickY, Axis::kRstickX,
                          Axis::kRstickY}) {
    const Axis partner = buttons::ids::SwapPartner(axis);
    set.axis[IndexOf(axis)].read_from = partner;
  }
  InputState input;
  input.left_stick_x = 10;
  input.left_stick_y = 20;
  input.right_stick_x = 30;
  input.right_stick_y = 40;

  const InputState output = Pass(set, input);
  EXPECT_EQ(output.left_stick_x, 30);
  EXPECT_EQ(output.left_stick_y, 40);
  EXPECT_EQ(output.right_stick_x, 10);
  EXPECT_EQ(output.right_stick_y, 20);
}

TEST(ButtonsPipeline, InvertingAnAxisKeepsTheRestPoint) {
  // M-13 + M-14：反向是绕静置点翻，静置的摇杆反向之后仍然静置。
  const Set rules = AxisRules(Axis::kRstickX, AxisRewrite{.invert = true});
  InputState rest = AtRest();
  EXPECT_EQ(Pass(rules, rest).right_stick_x, 128);

  InputState pushed = AtRest();
  pushed.right_stick_x = 200;
  EXPECT_EQ(Pass(rules, pushed).right_stick_x, 56);

  InputState other = AtRest();
  other.left_stick_x = 200;
  // 反向只动配了的那一根。
  EXPECT_EQ(Pass(rules, other).left_stick_x, 200);
}

TEST(ButtonsPipeline, InvertAndSwapCompose) {
  // 互换与反向写在同一条记录的两个开关上，所以"换过来再反向"不需要判冲突。
  Set set;
  set.axis[IndexOf(Axis::kLstickX)].read_from = Axis::kRstickX;
  set.axis[IndexOf(Axis::kLstickX)].invert = true;
  InputState input = AtRest();
  input.right_stick_x = 200;
  EXPECT_EQ(Pass(set, input).left_stick_x, 56);
}

TEST(ButtonsPipeline, AlwaysCenteredStickReportsNeutral) {
  // I-02 的 always：静置抖动与推到底都不再漏给游戏。
  Set set;
  set.axis[IndexOf(Axis::kLstickX)].always_center = true;
  set.axis[IndexOf(Axis::kLstickY)].always_center = true;
  InputState input;
  input.left_stick_x = 250;
  input.left_stick_y = 3;
  input.right_stick_x = 20;

  const InputState output = Pass(set, input);
  EXPECT_EQ(output.left_stick_x, 128);
  EXPECT_EQ(output.left_stick_y, 128);
  EXPECT_EQ(output.right_stick_x, 20);
}

TEST(ButtonsPipeline, CenteringBeatsASwappedInValue) {
  // 优先级是 强制 > 映射行程 > 抹掉 > 互换 > 快照：居中的那格不因为换了来源就
  // 又要按对方的值来。
  Set set;
  set.axis[IndexOf(Axis::kLstickX)].read_from = Axis::kRstickX;
  set.axis[IndexOf(Axis::kLstickX)].always_center = true;
  InputState input = AtRest();
  input.right_stick_x = 200;
  EXPECT_EQ(Pass(set, input).left_stick_x, 128);
}

// ---------------------------------------------------------------------------
// sense 交来的那两档：注入的位与强制的轴（宏、方向命中期间）
// ---------------------------------------------------------------------------

TEST(ButtonsPipeline, InjectedKeysArePressed) {
  // A-01：宏脉冲期间的那组键由 sense 摆在 inject 里，pipeline 只管把它们按下。
  const Set rules = RemapOnly({});
  FrameDecision decision;
  decision.inject = BitOf(Source::kL3) | BitOf(Source::kDpadUp);
  const InputState output = Pass(rules, AtRest(), decision);
  EXPECT_TRUE(output.button_l3);
  EXPECT_EQ(output.dpad, Direction::kNorth);
}

TEST(ButtonsPipeline, InjectedKeysAreNotRemapped) {
  // 注入排在映射之后：宏要的是那组键本身，配了一条 l3→○ 也不该把它换走。
  const Set rules = RemapOnly({Rule(Source::kL3, BitOf(Source::kCircle))});
  FrameDecision decision;
  decision.inject = BitOf(Source::kL3);
  const InputState output = Pass(rules, AtRest(), decision);
  EXPECT_TRUE(output.button_l3);
  EXPECT_FALSE(output.button_circle);
}

TEST(ButtonsPipeline, ForcedAxisBeatsEverythingElse) {
  // 宏驱动扳机时按全按给出（A-03）：那一格的值由 sense 直接摆在这里，压过
  // 快照里的手柄值与任何改写。
  Set set = AxisRules(Axis::kRstickX, AxisRewrite{.invert = true});
  FrameDecision decision;
  decision.forces_axis[IndexOf(Axis::kRstickX)] = true;
  decision.forced_axis[IndexOf(Axis::kRstickX)] = 128;
  InputState input = AtRest();
  input.right_stick_x = 200;
  EXPECT_EQ(Pass(set, input, decision).right_stick_x, 128);
}

// ---------------------------------------------------------------------------
// 边界：不属于本插件的字节
// ---------------------------------------------------------------------------

TEST(ButtonsPipeline, SticksAndEveryOtherAxisStayWhereTheControllerPutThem) {
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kCross))});
  InputState input;
  input.left_stick_x = 10;
  input.left_stick_y = 20;
  input.right_stick_x = 30;
  input.right_stick_y = 40;
  input.button_circle = true;

  const InputState output = Pass(rules, input);
  EXPECT_EQ(output.left_stick_x, 10);
  EXPECT_EQ(output.left_stick_y, 20);
  EXPECT_EQ(output.right_stick_x, 30);
  EXPECT_EQ(output.right_stick_y, 40);
}

TEST(ButtonsPipeline, EverythingThePluginDoesNotProduceIsLeftAlone) {
  // 序号、未解释的那一位、传感器/触摸板/电源/CMAC：Apply 之后逐字节不变。
  const Set rules = RemapOnly({Rule(Source::kCircle, BitOf(Source::kCross))});
  InputState input = AtRest();
  input.sequence_number = 0x7b;
  input.unknown_button = true;
  input.unknown_2 = 0x33;
  input.sensor_timestamp = 0x11223344;
  input.device_timestamp = 0x55667788;
  input.power_percent = 7;
  input.touch.fingers[0].not_touching = false;
  input.touch.fingers[0].x = 2000;
  input.touch.fingers[1].y = 3000;
  input.button_circle = true;
  for (std::size_t i = 0; i < input.aes_cmac.size(); ++i) {
    input.aes_cmac[i] = static_cast<uint8_t>(i + 1u);
  }

  std::vector<uint8_t> body = Wire(input);
  const std::vector<uint8_t> original = body;
  const Frame frame = DecodeFrame(body.data());
  buttons::pipeline::Apply(rules, Plain(frame), frame, body.data());

  EXPECT_EQ(body[6], original[6]);
  EXPECT_EQ(body[9] & 0x08, original[9] & 0x08);
  for (std::size_t i = 10; i < body.size(); ++i) {
    EXPECT_EQ(body[i], original[i]) << "byte " << i;
  }
  // 唯一改过的是 [7]：○ 的位没了、✕ 的位多了，十字键仍是未按。
  EXPECT_EQ(body[7] & 0x40, 0);
  EXPECT_EQ(body[7] & 0x20, 0x20);
  EXPECT_EQ(body[7] & 0x0f, static_cast<int>(Direction::kNone));
}

}  // namespace
