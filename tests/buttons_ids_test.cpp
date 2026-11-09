/**
 * @file
 * @brief buttons 插件 id 层的单测：token 表、与宿主解码器的对账、字节保留
 *
 * 这一层是"按 ○ 出 ●"这类静默错误的唯一防线：插件自己抄了一遍 63 字节的布局，
 * 所以每个用例都拿 core::protocol 的解码器当参照物，而不是拿插件自己的常量当
 * 参照物。缓冲区一律由 EncodeInputState 造出来——测试因此读到的就是宿主的
 * 编码器认得的那些字节。
 *
 * 无 root、无手柄。
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/protocol/input_state.h"
#include "core/protocol/report.h"
#include "gtest/gtest.h"
#include "plugin/buttons/ids.h"

namespace {

using buttons::ids::Axis;
using buttons::ids::Frame;
using buttons::ids::Mask;
using buttons::ids::Source;
using core::protocol::Direction;
using core::protocol::InputState;
using core::protocol::kUsbInputStateSize;

/**
 * @brief 一个键位在宿主解码器里对应的那个字段。
 * @note 十字键的四个方向不在这里：宿主把它们表达成一个 hat 值，由单独的用例
 *       逐个方向对账。
 */
struct DigitField {
  Source src;
  bool InputState::*field;
};

constexpr DigitField kDigitFields[] = {
    {Source::kTriangle, &InputState::button_triangle},
    {Source::kCircle, &InputState::button_circle},
    {Source::kCross, &InputState::button_cross},
    {Source::kSquare, &InputState::button_square},
    {Source::kL1, &InputState::button_l1},
    {Source::kR1, &InputState::button_r1},
    {Source::kL2, &InputState::button_l2},
    {Source::kR2, &InputState::button_r2},
    {Source::kCreate, &InputState::button_create},
    {Source::kOptions, &InputState::button_options},
    {Source::kL3, &InputState::button_l3},
    {Source::kR3, &InputState::button_r3},
    {Source::kHome, &InputState::button_home},
    {Source::kPad, &InputState::button_pad},
    {Source::kMute, &InputState::button_mute},
    {Source::kLFn, &InputState::button_left_function},
    {Source::kRFn, &InputState::button_right_function},
    {Source::kLPaddle, &InputState::button_left_paddle},
    {Source::kRPaddle, &InputState::button_right_paddle},
};

/// 八个方向的 hat 值，按 Source 的四个方向位依次展开成期望的位集合。
struct HatCase {
  Direction hat;
  Mask expected;
};

constexpr HatCase kHatCases[] = {
    {Direction::kNorth, buttons::ids::BitOf(Source::kDpadUp)},
    {Direction::kNorthEast,
     buttons::ids::BitOf(Source::kDpadUp) | buttons::ids::BitOf(Source::kDpadRight)},
    {Direction::kEast, buttons::ids::BitOf(Source::kDpadRight)},
    {Direction::kSouthEast,
     buttons::ids::BitOf(Source::kDpadRight) | buttons::ids::BitOf(Source::kDpadDown)},
    {Direction::kSouth, buttons::ids::BitOf(Source::kDpadDown)},
    {Direction::kSouthWest,
     buttons::ids::BitOf(Source::kDpadDown) | buttons::ids::BitOf(Source::kDpadLeft)},
    {Direction::kWest, buttons::ids::BitOf(Source::kDpadLeft)},
    {Direction::kNorthWest,
     buttons::ids::BitOf(Source::kDpadUp) | buttons::ids::BitOf(Source::kDpadLeft)},
    {Direction::kNone, 0},
};

/**
 * @brief 用宿主的编码器造一份状态体。
 * @param state 语义模型。
 * @return 63 字节的线格式。
 */
std::vector<uint8_t> Wire(const InputState& state) {
  std::vector<uint8_t> body(kUsbInputStateSize);
  core::protocol::EncodeInputState(state, body.data());
  return body;
}

/**
 * @brief token 的字符集：只允许小写字母、数字与 '_'。
 * @param token 待检查的文本。
 * @return 全部字符合法且非空时 true。
 */
bool IsLowerToken(const char* token) {
  if (token == nullptr || *token == '\0') return false;
  for (const char* p = token; *p != '\0'; ++p) {
    const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                    *p == '_';
    if (!ok) return false;
  }
  return true;
}

/// 一份"什么都按下来、什么都推到极值"的状态体，用作全位对账的输入。
InputState Everything() {
  InputState state;
  state.left_stick_x = 1;
  state.left_stick_y = 2;
  state.right_stick_x = 3;
  state.right_stick_y = 4;
  state.trigger_left = 5;
  state.trigger_right = 6;
  for (const DigitField& entry : kDigitFields) state.*(entry.field) = true;
  return state;
}

// ---------------------------------------------------------------------------
// token 表（C-03：一张表、大小写敏感、无别名）
// ---------------------------------------------------------------------------

TEST(ButtonsIds, EverySourceHasOneDistinctToken) {
  Mask seen_tokens = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(Source::kCount); ++i) {
    const Source src = static_cast<Source>(i);
    const char* token = buttons::ids::TokenOf(src);
    ASSERT_TRUE(IsLowerToken(token)) << "source " << i;

    Source parsed;
    ASSERT_TRUE(buttons::ids::ParseToken(token, parsed)) << token;
    EXPECT_EQ(parsed, src);
    EXPECT_EQ(seen_tokens & buttons::ids::BitOf(src), 0u) << token << " is twice";
    seen_tokens |= buttons::ids::BitOf(src);
  }
  EXPECT_EQ(seen_tokens, (buttons::ids::Mask{1}
                          << static_cast<uint8_t>(Source::kCount)) - 1);
}

TEST(ButtonsIds, EveryAxisHasOneDistinctToken) {
  for (uint8_t i = 0; i < buttons::ids::kAxisCount; ++i) {
    const Axis axis = static_cast<Axis>(i);
    const char* token = buttons::ids::TokenOf(axis);
    ASSERT_TRUE(IsLowerToken(token)) << "axis " << i;

    Axis parsed;
    ASSERT_TRUE(buttons::ids::ParseAxisToken(token, parsed)) << token;
    EXPECT_EQ(parsed, axis);
  }
  EXPECT_EQ(buttons::ids::kAxisCount, 6u);
}

TEST(ButtonsIds, TokensAreCaseSensitiveAndHaveNoAliases) {
  Source src;
  EXPECT_FALSE(buttons::ids::ParseToken("", src));
  EXPECT_FALSE(buttons::ids::ParseToken("Circle", src));
  EXPECT_FALSE(buttons::ids::ParseToken("CIRCLE", src));
  EXPECT_FALSE(buttons::ids::ParseToken("circle ", src));
  EXPECT_FALSE(buttons::ids::ParseToken("cross_o", src));
  // 别名一律不认：同一个键的第二种写法会让"这张表就是真值"失效。
  EXPECT_FALSE(buttons::ids::ParseToken("x", src));
  EXPECT_FALSE(buttons::ids::ParseToken("triangle_button", src));

  Axis axis;
  EXPECT_FALSE(buttons::ids::ParseAxisToken("lx", axis));
  EXPECT_FALSE(buttons::ids::ParseAxisToken("Lstick_x", axis));
  EXPECT_FALSE(buttons::ids::ParseAxisToken("left_trigger", axis));
}

TEST(ButtonsIds, OnlyTheTwoTriggerSourcesOwnAnAxis) {
  EXPECT_EQ(buttons::ids::TriggerAxisOf(Source::kL2), Axis::kLTrigger);
  EXPECT_EQ(buttons::ids::TriggerAxisOf(Source::kR2), Axis::kRTrigger);
  for (uint8_t i = 0; i < static_cast<uint8_t>(Source::kCount); ++i) {
    const Source src = static_cast<Source>(i);
    if (src == Source::kL2 || src == Source::kR2) continue;
    EXPECT_EQ(buttons::ids::TriggerAxisOf(src), Axis::kCount)
        << buttons::ids::TokenOf(src);
  }
}

// ---------------------------------------------------------------------------
// 与宿主解码器的对账
// ---------------------------------------------------------------------------

TEST(ButtonsIds, DecodeAgreesWithTheCoreDecoderFieldByField) {
  const InputState state = Everything();
  const std::vector<uint8_t> body = Wire(state);
  const Frame frame = buttons::ids::DecodeFrame(body.data());

  EXPECT_EQ(frame.axis[static_cast<uint8_t>(Axis::kLstickX)],
            state.left_stick_x);
  EXPECT_EQ(frame.axis[static_cast<uint8_t>(Axis::kLstickY)],
            state.left_stick_y);
  EXPECT_EQ(frame.axis[static_cast<uint8_t>(Axis::kRstickX)],
            state.right_stick_x);
  EXPECT_EQ(frame.axis[static_cast<uint8_t>(Axis::kRstickY)],
            state.right_stick_y);
  EXPECT_EQ(frame.axis[static_cast<uint8_t>(Axis::kLTrigger)],
            state.trigger_left);
  EXPECT_EQ(frame.axis[static_cast<uint8_t>(Axis::kRTrigger)],
            state.trigger_right);

  Mask expected = 0;
  for (const DigitField& entry : kDigitFields) {
    if (state.*(entry.field)) expected |= buttons::ids::BitOf(entry.src);
  }
  EXPECT_EQ(frame.digits, expected);
}

TEST(ButtonsIds, DpadDirectionsMatchTheHatsTheCoreDecoderReads) {
  for (const HatCase& test : kHatCases) {
    InputState state;
    state.dpad = test.hat;
    const std::vector<uint8_t> body = Wire(state);
    EXPECT_EQ(buttons::ids::DecodeFrame(body.data()).digits, test.expected)
        << "hat " << static_cast<int>(test.hat);
  }
}

TEST(ButtonsIds, IdleFrameDecodesToNothingAtAll) {
  const InputState state;  // 结构体默认值就是"全放松、无触摸"。
  const std::vector<uint8_t> body = Wire(state);
  const Frame frame = buttons::ids::DecodeFrame(body.data());
  EXPECT_EQ(frame.digits, 0u);
  for (uint8_t a = 0; a < buttons::ids::kAxisCount; ++a) {
    EXPECT_EQ(frame.axis[a], state.left_stick_x) << "axis " << a;
  }
}

// ---------------------------------------------------------------------------
// 写回的边界：不生产的字节一律不碰
// ---------------------------------------------------------------------------

TEST(ButtonsIds, DecodingThenEncodingLeavesAllSixtyThreeBytesIdentical) {
  InputState state = Everything();
  state.dpad = Direction::kSouthWest;
  state.sequence_number = 0x5a;
  state.unknown_button = true;      // core 里那个未解释的位。
  state.unknown_2 = 0x77;
  state.sensor_timestamp = 0x01020304;
  state.power_percent = 6;
  state.touch.fingers[0].x = 1234;
  state.touch.fingers[1].not_touching = false;
  for (std::size_t i = 0; i < state.aes_cmac.size(); ++i) {
    state.aes_cmac[i] = static_cast<uint8_t>(i * 31u);
  }

  std::vector<uint8_t> body = Wire(state);
  const std::vector<uint8_t> original = body;

  buttons::ids::EncodeFrame(buttons::ids::DecodeFrame(body.data()), body.data());
  EXPECT_EQ(std::memcmp(body.data(), original.data(), body.size()), 0);
}

TEST(ButtonsIds, EncodingNeverProducesTheUnexplainedBitItDidNotRead) {
  InputState state;
  state.unknown_button = true;
  std::vector<uint8_t> body = Wire(state);

  // 快照里没有任何位对应 [9] bit3，写回去时它必须还是 1。
  buttons::ids::EncodeFrame(buttons::ids::DecodeFrame(body.data()), body.data());
  EXPECT_NE(body[9] & 0x08, 0);
}

TEST(ButtonsIds, UndefinedHatNibblesReadAsReleased) {
  // 线上还可能出现 9..15（core 把底层值原样保留）。手柄不报它们，插件也不
  // 生产它们：读成"未按"，写回时归一成 8。
  for (uint8_t nibble = 9; nibble <= 15; ++nibble) {
    InputState state;
    state.dpad = static_cast<Direction>(nibble);
    std::vector<uint8_t> body = Wire(state);
    const std::vector<uint8_t> original = body;

    const Frame frame = buttons::ids::DecodeFrame(body.data());
    EXPECT_EQ(frame.digits & (buttons::ids::BitOf(Source::kDpadUp) |
                              buttons::ids::BitOf(Source::kDpadRight) |
                              buttons::ids::BitOf(Source::kDpadDown) |
                              buttons::ids::BitOf(Source::kDpadLeft)),
              0u)
        << "nibble " << nibble;

    buttons::ids::EncodeFrame(frame, body.data());
    EXPECT_EQ(body[7] & 0x0f, 8) << "nibble " << nibble;
    // 那一个 nibble 之外，整份缓冲逐字节不变。
    for (std::size_t i = 0; i < body.size(); ++i) {
      if (i == 7) continue;
      EXPECT_EQ(body[i], original[i]) << "byte " << i;
    }
  }
}

TEST(ButtonsIds, OppositeDirectionsOnOneAxisEncodeBackToCentered) {
  // 编码这一侧只可能从 4 个方向位拼回 hat：两个相反方向同时置位时那一轴没有
  // 可表达的倾斜，回到"未按"（M-10 的相反方向回到居中）。
  InputState state;
  std::vector<uint8_t> body = Wire(state);

  Frame frame;
  frame.digits = buttons::ids::BitOf(Source::kDpadUp) |
                 buttons::ids::BitOf(Source::kDpadDown) |
                 buttons::ids::BitOf(Source::kDpadLeft);
  buttons::ids::EncodeFrame(frame, body.data());
  EXPECT_EQ(body[7] & 0x0f, static_cast<int>(Direction::kWest));

  frame.digits = buttons::ids::BitOf(Source::kDpadUp) |
                 buttons::ids::BitOf(Source::kDpadRight);
  buttons::ids::EncodeFrame(frame, body.data());
  EXPECT_EQ(body[7] & 0x0f, static_cast<int>(Direction::kNorthEast));
}

/// 八个方向源的位集合，方向判定的用例反复用它。
Mask AllStickDirections() {
  Mask mask = 0;
  for (uint8_t i = static_cast<uint8_t>(Source::kLstickUp);
       i <= static_cast<uint8_t>(Source::kRstickLeft); ++i) {
    mask |= buttons::ids::BitOf(static_cast<Source>(i));
  }
  return mask;
}

/**
 * @brief 一份只有左摇杆 y 摆到 value 的快照，其余都在中心。
 * @param y 要写的线上值。
 * @return 快照。
 */
Frame FrameWithLeftStickY(uint8_t y) {
  Frame frame;
  frame.axis[static_cast<uint8_t>(Axis::kLstickX)] = buttons::ids::kStickCenter;
  frame.axis[static_cast<uint8_t>(Axis::kLstickY)] = y;
  frame.axis[static_cast<uint8_t>(Axis::kRstickX)] = buttons::ids::kStickCenter;
  frame.axis[static_cast<uint8_t>(Axis::kRstickY)] = buttons::ids::kStickCenter;
  return frame;
}

TEST(ButtonsIds, OnlyTheEightDirectionsOwnAStickAxis) {
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kLstickUp), Axis::kLstickY);
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kLstickRight), Axis::kLstickX);
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kLstickDown), Axis::kLstickY);
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kLstickLeft), Axis::kLstickX);
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kRstickUp), Axis::kRstickY);
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kRstickRight), Axis::kRstickX);
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kRstickDown), Axis::kRstickY);
  EXPECT_EQ(buttons::ids::StickAxisOf(Source::kRstickLeft), Axis::kRstickX);
  for (uint8_t i = 0; i < static_cast<uint8_t>(Source::kCount); ++i) {
    const Source src = static_cast<Source>(i);
    if (src >= Source::kLstickUp && src <= Source::kRstickLeft) continue;
    EXPECT_EQ(buttons::ids::StickAxisOf(src), Axis::kCount)
        << buttons::ids::TokenOf(src);
  }
}

TEST(ButtonsIds, UpIsTheNegativeEndOfTheYAxis) {
  // 线上这个字节向下增长：0 那一端是"上"，255 那一端是"下"（与 Studio 同口径）。
  const Mask up = buttons::ids::BitOf(Source::kLstickUp);
  const Mask down = buttons::ids::BitOf(Source::kLstickDown);
  EXPECT_EQ(buttons::ids::StickDirections(FrameWithLeftStickY(buttons::ids::kStickMin),
                                          buttons::ids::kStickStep,
                                          buttons::ids::kStickHysteresis, 0) &
                (up | down),
            up);
  EXPECT_EQ(buttons::ids::StickDirections(FrameWithLeftStickY(buttons::ids::kStickMax),
                                          buttons::ids::kStickStep,
                                          buttons::ids::kStickHysteresis, 0) &
                (up | down),
            down);

  Frame right;
  right.axis[static_cast<uint8_t>(Axis::kLstickX)] = buttons::ids::kStickMax;
  right.axis[static_cast<uint8_t>(Axis::kLstickY)] = buttons::ids::kStickCenter;
  const Mask horizontal = buttons::ids::BitOf(Source::kLstickRight) |
                          buttons::ids::BitOf(Source::kLstickLeft);
  EXPECT_EQ(buttons::ids::StickDirections(right, buttons::ids::kStickStep,
                                          buttons::ids::kStickHysteresis,
                                          0) & horizontal,
            buttons::ids::BitOf(Source::kLstickRight));
}

TEST(ButtonsIds, DirectionEngagesAtTheStepAndReleasesAtTheStepMinusHysteresis) {
  const Mask up = buttons::ids::BitOf(Source::kLstickUp);
  constexpr uint8_t kStep = buttons::ids::kStickStep;         // 48
  constexpr uint8_t kOff = kStep - buttons::ids::kStickHysteresis;  // 38
  const uint8_t engage_at = buttons::ids::kStickCenter - kStep;     // 80
  const uint8_t hold_at = buttons::ids::kStickCenter - kOff;        // 90
  const uint8_t release_at = hold_at + 1;                           // 91

  // 未到阈值：没有锁存就不算按下。
  EXPECT_EQ(buttons::ids::StickDirections(FrameWithLeftStickY(engage_at + 1), kStep,
                                          buttons::ids::kStickHysteresis, 0) &
                up,
            0u);
  // 恰好到阈值。
  EXPECT_NE(buttons::ids::StickDirections(FrameWithLeftStickY(engage_at), kStep,
                                          buttons::ids::kStickHysteresis, 0) &
                up,
            0u);
  // 退回阈值以内：锁存着的仍然算按下（停在阈值上的摇杆不能一帧开一帧关）。
  EXPECT_NE(buttons::ids::StickDirections(FrameWithLeftStickY(hold_at), kStep,
                                          buttons::ids::kStickHysteresis, up) &
                up,
            0u);
  // 退过迟滞带才真的松开。
  EXPECT_EQ(buttons::ids::StickDirections(FrameWithLeftStickY(release_at), kStep,
                                          buttons::ids::kStickHysteresis, up) &
                up,
            0u);
}

TEST(ButtonsIds, DirectionsOnTheTwoAxesDoNotCompete) {
  // 一个方向各占自己那一轴：斜推同时出两个方向，边界上的抖动因此不存在（Studio
  // 的 per-axis 判据，I-01）。
  Frame frame;
  frame.axis[static_cast<uint8_t>(Axis::kLstickX)] = 200;
  frame.axis[static_cast<uint8_t>(Axis::kLstickY)] = 40;
  const Mask engaged = buttons::ids::StickDirections(
      frame, buttons::ids::kStickStep, buttons::ids::kStickHysteresis, 0);
  EXPECT_EQ(engaged & (buttons::ids::BitOf(Source::kLstickUp) |
                       buttons::ids::BitOf(Source::kLstickRight)),
            buttons::ids::BitOf(Source::kLstickUp) |
                buttons::ids::BitOf(Source::kLstickRight));
}

TEST(ButtonsIds, CrossingToTheOtherSideReleasesTheLatchedDirection) {
  const Mask up = buttons::ids::BitOf(Source::kLstickUp);
  EXPECT_EQ(buttons::ids::StickDirections(FrameWithLeftStickY(buttons::ids::kStickMax),
                                          buttons::ids::kStickStep,
                                          buttons::ids::kStickHysteresis,
                                          up) &
                up,
            0u);
}

TEST(ButtonsIds, DirectionBitsNeitherComeFromTheWireNorGoBackToIt) {
  // 摇杆推到极值，解码出来的快照里不该有任何方向位：它们不是线上的位。
  InputState state;
  state.left_stick_x = buttons::ids::kStickMax;
  state.left_stick_y = buttons::ids::kStickMin;
  state.right_stick_x = buttons::ids::kStickMin;
  state.right_stick_y = buttons::ids::kStickMax;
  std::vector<uint8_t> body = Wire(state);

  Frame frame = buttons::ids::DecodeFrame(body.data());
  EXPECT_EQ(frame.digits & AllStickDirections(), 0u);

  // 即使有人把方向位塞进快照，写回去也不该动任何一个字节。
  frame.digits |= AllStickDirections();
  const std::vector<uint8_t> original = body;
  buttons::ids::EncodeFrame(frame, body.data());
  EXPECT_EQ(std::memcmp(body.data(), original.data(), body.size()), 0);
}

// ---------------------------------------------------------------------------
// 触摸板：两个触摸点的解码、半区锁存、划动（I-03/I-04）
// ---------------------------------------------------------------------------

/// "任意"带的两个边界（含这两个值本身）：带里不算左也不算右。
constexpr uint16_t kBandLeft =
    static_cast<uint16_t>(buttons::ids::kPadMidX - buttons::ids::kPadNeutralBand);  // 1721
constexpr uint16_t kBandRight =
    static_cast<uint16_t>(buttons::ids::kPadMidX + buttons::ids::kPadNeutralBand);  // 2375

/// 恰好到门限的那一个横向位移（门限本身算"划到了"）。
constexpr uint16_t kSwipeReach =
    static_cast<uint16_t>(buttons::ids::kSwipeMinSpan);   // 491

/**
 * @brief 把快照里指定那个触摸点摆成接触中。
 * @param frame 要改的快照。
 * @param index 第几个触摸点，线上那两个打包字。
 * @param x 12 位横坐标。
 * @param y 12 位纵坐标，向下增长。
 */
void PutContact(Frame& frame, std::size_t index, uint16_t x, uint16_t y) {
  frame.contact[index].touching = true;
  frame.contact[index].x = x;
  frame.contact[index].y = y;
}

/**
 * @brief 拿一份刚复位过的跨帧状态判一帧。
 * @param frame 快照。
 * @return 这一帧的触摸板位集合。
 * @details 只看"某一帧本身成立成什么样"的用例用它：半区与起点是锁存的，共用一份
 *          io 就把后一帧的判断绑在前一帧上了，那类用例该自己拿 io 逐帧喂。
 */
Mask JudgeOnce(const Frame& frame) {
  buttons::ids::TouchState io;
  return buttons::ids::TouchpadStates(frame, io);
}

TEST(ButtonsIds, TouchContactsDecodeLikeTheCoreDecoder) {
  InputState state;
  state.touch.fingers[0] = {3, false, 1000, 200};
  state.touch.fingers[1] = {7, true, 4095, 4095};
  const std::vector<uint8_t> body = Wire(state);

  const InputState decoded = core::protocol::DecodeInputState(body.data());
  const Frame frame = buttons::ids::DecodeFrame(body.data());
  for (std::size_t i = 0; i < buttons::ids::kTouchPoints; ++i) {
    // 线上那一位的语义是"未接触"，快照里存的是"接触"，所以对账要取反。
    EXPECT_EQ(frame.contact[i].touching, !decoded.touch.fingers[i].not_touching)
        << "contact " << i;
    EXPECT_EQ(frame.contact[i].x, decoded.touch.fingers[i].x) << "contact " << i;
    EXPECT_EQ(frame.contact[i].y, decoded.touch.fingers[i].y) << "contact " << i;
  }
}

TEST(ButtonsIds, AnEmptyTouchpadJudgesNoBitsAtAll) {
  const Frame frame;   // 两个触摸点默认都不接触。
  buttons::ids::TouchState io;
  EXPECT_EQ(buttons::ids::TouchpadStates(frame, io), 0u);
}

TEST(ButtonsIds, AContactInsideTheMidlineBandBelongsToNeitherHalf) {
  const struct {
    uint16_t x;
    Mask expected;
  } cases[] = {
      {0, buttons::ids::BitOf(Source::kTouchLeft)},
      {kBandLeft, 0},
      {static_cast<uint16_t>(kBandLeft - 1), buttons::ids::BitOf(Source::kTouchLeft)},
      {static_cast<uint16_t>(buttons::ids::kPadMidX), 0},
      {kBandRight, 0},
      {static_cast<uint16_t>(kBandRight + 1), buttons::ids::BitOf(Source::kTouchRight)},
      {4095, buttons::ids::BitOf(Source::kTouchRight)},
  };
  for (const auto& test : cases) {
    Frame frame;
    PutContact(frame, 0, test.x, 500);
    buttons::ids::TouchState io;
    EXPECT_EQ(buttons::ids::TouchpadStates(frame, io), test.expected) << "x " << test.x;
  }
}

TEST(ButtonsIds, TheHalfIsSetWhenTheContactAppearsAndHeldUntilItLeaves) {
  buttons::ids::TouchState io;
  Frame arrival;
  PutContact(arrival, 0, 300, 500);
  EXPECT_EQ(buttons::ids::TouchpadStates(arrival, io),
            buttons::ids::BitOf(Source::kTouchLeft));

  // 手指越过中线：半区归按下那一刻，不跟着改判（I-03 的"保持到松开"）。够大的
  // 位移同时判出一次划动，那是另一件事。
  Frame crossed;
  PutContact(crossed, 0, 3800, 500);
  const Mask moved = buttons::ids::TouchpadStates(crossed, io);
  EXPECT_EQ(moved & (buttons::ids::BitOf(Source::kTouchLeft) |
                     buttons::ids::BitOf(Source::kTouchRight)),
            buttons::ids::BitOf(Source::kTouchLeft));
  EXPECT_EQ(moved & buttons::ids::BitOf(Source::kSwipeRight),
            buttons::ids::BitOf(Source::kSwipeRight));

  // 抬手之后一切归零，重新上手重新定半区。
  Frame lifted;
  EXPECT_EQ(buttons::ids::TouchpadStates(lifted, io), 0u);
  PutContact(lifted, 0, 3800, 500);
  EXPECT_EQ(buttons::ids::TouchpadStates(lifted, io),
            buttons::ids::BitOf(Source::kTouchRight));
}

TEST(ButtonsIds, AShorterMoveIsNotASwipe) {
  buttons::ids::TouchState io;
  Frame start;
  PutContact(start, 0, 300, 2000);
  buttons::ids::TouchpadStates(start, io);

  // 差一个刻度不到门限（门限那一站本身算到，就在下一帧）。
  Frame short_move;
  PutContact(short_move, 0, static_cast<uint16_t>(300 + kSwipeReach - 1), 2000);
  EXPECT_EQ(buttons::ids::TouchpadStates(short_move, io),
            buttons::ids::BitOf(Source::kTouchLeft));

  Frame reached;
  PutContact(reached, 0, static_cast<uint16_t>(300 + kSwipeReach), 2000);
  EXPECT_EQ(buttons::ids::TouchpadStates(reached, io),
            buttons::ids::BitOf(Source::kTouchLeft) |
                buttons::ids::BitOf(Source::kSwipeRight));
}

TEST(ButtonsIds, ASwipeFiresOncePerGestureAndIsHeldUntilEveryContactLeaves) {
  buttons::ids::TouchState io;
  Frame start;
  PutContact(start, 0, 2000, 2000);
  buttons::ids::TouchpadStates(start, io);

  const Mask up = buttons::ids::BitOf(Source::kSwipeUp);
  const Mask down = buttons::ids::BitOf(Source::kSwipeDown);
  Frame out;
  PutContact(out, 0, 2000, static_cast<uint16_t>(2000 - 4 * kSwipeReach));
  EXPECT_EQ(buttons::ids::TouchpadStates(out, io) & (up | down), up);

  // 往回划不再发第二次：一次手势一个事件，否则一次划过去再划回来就是两下。
  Frame back;
  PutContact(back, 0, 2000, 2000);
  EXPECT_EQ(buttons::ids::TouchpadStates(back, io) & (up | down), up);

  // 事件位保持到**所有**触点离开：这里第二根手指还按着，第一根先抬不算松开。
  Frame second;
  PutContact(second, 0, 2000, static_cast<uint16_t>(2000 - 4 * kSwipeReach));
  PutContact(second, 1, 1000, 1000);
  EXPECT_EQ(buttons::ids::TouchpadStates(second, io) & (up | down), up);
  Frame first_lifted;
  PutContact(first_lifted, 1, 1000, 1000);
  EXPECT_EQ(buttons::ids::TouchpadStates(first_lifted, io) & (up | down), up);

  // 两根都抬起来，这一轮才结束；下一次上手算新一轮，可以再发一次。
  Frame released;
  EXPECT_EQ(buttons::ids::TouchpadStates(released, io), 0u);
  Frame again;
  PutContact(again, 0, 2000, 300);
  buttons::ids::TouchpadStates(again, io);   // 新一轮的第一帧只定起点
  Frame downward;
  PutContact(downward, 0, 2000, static_cast<uint16_t>(300 + 4 * kSwipeReach));
  EXPECT_EQ(buttons::ids::TouchpadStates(downward, io) & (up | down), down);
}

TEST(ButtonsIds, SwipeDirectionWeighsActualDistanceNotRawTicks) {
  // 两个坐标轴都是 12 位而面板是 16:9：raw 位移相同的一条斜线其实更偏横，实际
  // 等距的那条算横竖平手，而平手归横（ids 里 SwipeOf 说的就是这两档）。
  const Mask left = buttons::ids::BitOf(Source::kSwipeLeft);
  const Mask right = buttons::ids::BitOf(Source::kSwipeRight);
  const Mask up = buttons::ids::BitOf(Source::kSwipeUp);
  const Mask down = buttons::ids::BitOf(Source::kSwipeDown);

  struct Case {
    uint16_t from_x;
    uint16_t from_y;
    uint16_t to_x;
    uint16_t to_y;
    Mask expected;
  };
  const uint16_t reach = static_cast<uint16_t>(4 * kSwipeReach);   // 1964
  const Case cases[] = {
      // 横竖各 1964 个 raw 刻度：横向才是实际更长的那一路。
      {1000, 1000, static_cast<uint16_t>(1000 + reach),
       static_cast<uint16_t>(1000 + reach), right},
      // 横 1440、纵 2560：1440*16 == 2560*9，实际距离正好相等 → 平手算横。
      {1000, 3000, 2440, 440, right},
      // 同一笔把纵向再拉长一点，就归纵。
      {1000, 3000, 2440, 100, up},
      // 纵向拉长、横向不到门限：向下。
      {1000, 200, 1400, static_cast<uint16_t>(200 + reach), down},
  };
  for (const Case& test : cases) {
    buttons::ids::TouchState io;
    Frame start;
    PutContact(start, 0, test.from_x, test.from_y);
    buttons::ids::TouchpadStates(start, io);
    Frame moved;
    PutContact(moved, 0, test.to_x, test.to_y);
    EXPECT_EQ(buttons::ids::TouchpadStates(moved, io) & (left | right | up | down),
              test.expected)
        << "from (" << test.from_x << ", " << test.from_y << ") to (" << test.to_x
        << ", " << test.to_y << ")";
  }
}

TEST(ButtonsIds, AHalfClickIsTheClickBitPlusAContactOnThatSide) {
  const Mask click = buttons::ids::BitOf(Source::kPad);
  const Mask halves = buttons::ids::BitOf(Source::kPadLeft) |
                      buttons::ids::BitOf(Source::kPadRight);
  Frame left_touch;
  PutContact(left_touch, 0, 300, 500);
  left_touch.digits = click;
  EXPECT_EQ(JudgeOnce(left_touch) & halves, buttons::ids::BitOf(Source::kPadLeft));

  Frame right_touch;
  PutContact(right_touch, 0, 3800, 500);
  right_touch.digits = click;
  EXPECT_EQ(JudgeOnce(right_touch) & halves, buttons::ids::BitOf(Source::kPadRight));

  // 触点在中线带里、面板上根本没有触点，两种情形都不属于任何一半：那时线上那个
  // pad 位就是"任意半区"那第三种绑定（I-03 的三种互斥）。
  Frame middle;
  PutContact(middle, 0, static_cast<uint16_t>(buttons::ids::kPadMidX), 500);
  middle.digits = click;
  EXPECT_EQ(JudgeOnce(middle) & halves, 0u);
  Frame bare;
  bare.digits = click;
  EXPECT_EQ(JudgeOnce(bare), 0u);
}

TEST(ButtonsIds, TwoContactsAreTheTwoFingerForm) {
  buttons::ids::TouchState io;
  Frame one;
  PutContact(one, 0, 300, 500);
  EXPECT_EQ(buttons::ids::TouchpadStates(one, io) &
                buttons::ids::BitOf(Source::kTouchTwo),
            0u);
  Frame two;
  PutContact(two, 0, 300, 500);
  PutContact(two, 1, 3800, 500);
  EXPECT_EQ(buttons::ids::TouchpadStates(two, io) &
                buttons::ids::BitOf(Source::kTouchTwo),
            buttons::ids::BitOf(Source::kTouchTwo));
}

TEST(ButtonsIds, EveryTouchpadFormIsJudgedAndNeverOwnsAWireBit) {
  for (uint8_t i = static_cast<uint8_t>(Source::kTouchLeft);
       i < static_cast<uint8_t>(Source::kCount); ++i) {
    const Source src = static_cast<Source>(i);
    EXPECT_FALSE(buttons::ids::IsWireBit(src)) << buttons::ids::TokenOf(src);
    EXPECT_FALSE(buttons::ids::IsStickDirection(src)) << buttons::ids::TokenOf(src);
    EXPECT_EQ(buttons::ids::StickAxisOf(src), Axis::kCount)
        << buttons::ids::TokenOf(src);
  }
  for (uint8_t i = static_cast<uint8_t>(Source::kSwipeUp);
       i < static_cast<uint8_t>(Source::kCount); ++i) {
    EXPECT_TRUE(buttons::ids::IsSwipe(static_cast<Source>(i)));
  }
  for (uint8_t i = 0; i < static_cast<uint8_t>(Source::kSwipeUp); ++i) {
    EXPECT_FALSE(buttons::ids::IsSwipe(static_cast<Source>(i)))
        << buttons::ids::TokenOf(static_cast<Source>(i));
  }
}

TEST(ButtonsIds, OnlyTheHalfClicksExpandToAWireBitTheyDoNotOwn) {
  for (uint8_t i = 0; i < static_cast<uint8_t>(Source::kCount); ++i) {
    const Source src = static_cast<Source>(i);
    if (buttons::ids::IsWireBit(src)) {
      EXPECT_EQ(buttons::ids::GateBitsOf(src), buttons::ids::BitOf(src))
          << buttons::ids::TokenOf(src);
    } else if (src == Source::kPadLeft || src == Source::kPadRight) {
      EXPECT_EQ(buttons::ids::GateBitsOf(src), buttons::ids::BitOf(Source::kPad))
          << buttons::ids::TokenOf(src);
    } else {
      EXPECT_EQ(buttons::ids::GateBitsOf(src), 0u) << buttons::ids::TokenOf(src);
    }
  }
}

TEST(ButtonsIds, InvertKeepsTheRestPointAndFlipsBothEnds) {
  // M-14：反向之后静置必须还在静置。写成 255 - value 会让 128 变成 127，主机
  // 看到一根永远在缓慢输入的摇杆。
  EXPECT_EQ(buttons::ids::Invert(buttons::ids::kStickCenter),
            buttons::ids::kStickCenter);
  EXPECT_EQ(buttons::ids::Invert(buttons::ids::kStickMin), buttons::ids::kStickMax);
  EXPECT_EQ(buttons::ids::Invert(buttons::ids::kStickMax),
            static_cast<uint8_t>(buttons::ids::kStickMin + 1));

  // 静置点是不动点，其余按单调不增翻转：两端各自翻到另一端，中间不产生回头。
  for (int value = 1; value <= buttons::ids::kStickMax; ++value) {
    EXPECT_LE(buttons::ids::Invert(static_cast<uint8_t>(value)),
              buttons::ids::Invert(static_cast<uint8_t>(value - 1)))
        << value;
  }

  // 1..255 这一段是回返函数；0 是唯一的例外。这不是实现没做干净，是这一族映射
  // 里根本不存在"既回返又以 128 为唯一不动点"的那种：一个有不动点的对合把剩下
  // 的点两两配对，剩下的必须是偶数个，而 255 是奇数。所以按 128 翻转必然在某一端
  // 丢一个刻度（丢在 0 那一端，0 与 1 都映到 255）。
  for (int value = 1; value <= buttons::ids::kStickMax; ++value) {
    EXPECT_EQ(buttons::ids::Invert(buttons::ids::Invert(
                  static_cast<uint8_t>(value))),
              static_cast<uint8_t>(value))
        << value;
  }
  // 丢的那一个刻度不超过 1：反向重新扫过的量程与原量程同一量级，静置与满偏都不跳。
  EXPECT_EQ(buttons::ids::Invert(buttons::ids::Invert(buttons::ids::kStickMin)),
            static_cast<uint8_t>(buttons::ids::kStickMin + 1));
}

}  // namespace
