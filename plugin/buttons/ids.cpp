/**
 * @file
 * @brief id 空间的表与状态体编解码
 *
 * 布局只在本文件的几张表里出现：单比特键位表、十字键 hat 表、轴偏移、触摸点
 * 打包字的偏移。配置 token 表是这份实现里唯一的键名真值来源，settings 与报错
 * 文本都从这里取。
 */

#include "ids.h"

#include <algorithm>
#include <cstddef>

namespace buttons {
namespace ids {
namespace {

/// 状态体里的轴起点：[0..5] 依次是 lx ly rx ry l2 r2，与 Axis 的枚举顺序同序。
constexpr std::size_t kAxisOffset = 0;

/// [7]：低 nibble 是十字键的 hat 值，高 nibble 依次是 □ ✕ ○ △。
constexpr std::size_t kButtons0 = 7;
constexpr uint8_t kDpadMask = 0x0f;
constexpr uint8_t kSquareMask = 0x10;
constexpr uint8_t kCrossMask = 0x20;
constexpr uint8_t kCircleMask = 0x40;
constexpr uint8_t kTriangleMask = 0x80;

/// [8]：bit0 起依次是 L1 R1 L2 R2 Create Options L3 R3。
constexpr std::size_t kButtons1 = 8;

/// [9]：bit0 起依次是 Home Pad Mute 未解释 左Fn 右Fn 左背 右背。
constexpr std::size_t kButtons2 = 9;

/// [9] bit3 这个项目不解释（core 里叫 unknown_button），既不读也不写。
constexpr uint8_t kUnexplainedButtonMask = 0x08;

/// [32..35] 与 [36..39]：两个触摸点各一个 32 位小端打包字。
constexpr std::size_t kTouchOffset = 32;

/// 打包字里的字段位置，与 core 的 DecodeFinger 逐位相同（ids 单测对账的就是这条）。
constexpr uint32_t kContactNotTouchingMask = 1u << 7;
constexpr uint32_t kContactAxisMask = 0xfff;

/// hat 的"未按下"值，与 core 的 Direction::kNone 同值。
constexpr uint8_t kHatNone = 8;

/**
 * @brief 一个键位在状态体里的位置。
 * @note mask 为 0 表示它不在单比特表里（只有十字键的四个方向是那种情形）。
 */
struct Slot {
  std::size_t offset;
  uint8_t mask;
};

/// 按 Source 的枚举顺序排列的比特表；mask 为 0 的那几项不在单比特表里：十字键
/// 的四个方向由 kHatDirs/kHatOf 管，八个摇杆方向与触摸板那一族压根没有对应的位。
constexpr Slot kSlots[] = {
    {0, 0}, {0, 0}, {0, 0}, {0, 0},                         // dpad 上 右 下 左
    {kButtons0, kTriangleMask}, {kButtons0, kCircleMask},   // △ ○
    {kButtons0, kCrossMask}, {kButtons0, kSquareMask},      // ✕ □
    {kButtons1, 0x01}, {kButtons1, 0x02},                   // L1 R1
    {kButtons1, 0x04}, {kButtons1, 0x08},                   // L2 R2
    {kButtons1, 0x10}, {kButtons1, 0x20},                   // Create Options
    {kButtons1, 0x40}, {kButtons1, 0x80},                   // L3 R3
    {kButtons2, 0x01}, {kButtons2, 0x02}, {kButtons2, 0x04},  // Home Pad Mute
    {kButtons2, 0x10}, {kButtons2, 0x20},                   // 左Fn 右Fn
    {kButtons2, 0x40}, {kButtons2, 0x80},                   // 左背 右背
    {0, 0}, {0, 0}, {0, 0}, {0, 0},                         // 左摇杆 上右下左
    {0, 0}, {0, 0}, {0, 0}, {0, 0},                         // 右摇杆 上右下左
    {0, 0}, {0, 0}, {0, 0},                                 // 触摸板 左半 右半 两指
    {0, 0}, {0, 0},                                         // 左半点击 右半点击
    {0, 0}, {0, 0}, {0, 0}, {0, 0},                         // 划动 上右下左
};
static_assert(std::size(kSlots) == static_cast<std::size_t>(Source::kCount),
              "the bit table must cover every bindable source");

/**
 * @brief hat 值 → 四个方向位，下标是线上那个 nibble。
 * @details 0..7 是八个方向（顺时针从正北起），8 是未按下；9..15 手柄不报，
 *          这里与"未按"同列，归一的后果由 ids 单测逐值断言。
 */
constexpr Mask kHatDirs[] = {
    BitOf(Source::kDpadUp),
    BitOf(Source::kDpadUp) | BitOf(Source::kDpadRight),
    BitOf(Source::kDpadRight),
    BitOf(Source::kDpadRight) | BitOf(Source::kDpadDown),
    BitOf(Source::kDpadDown),
    BitOf(Source::kDpadDown) | BitOf(Source::kDpadLeft),
    BitOf(Source::kDpadLeft),
    BitOf(Source::kDpadUp) | BitOf(Source::kDpadLeft),
    0, 0, 0, 0, 0, 0, 0, 0,
};

/**
 * @brief 两个方向的符号 → hat 值，下标是 (纵向+1)*3 + (横向+1)。
 * @details 表里有 8 个方向加"未按"，占 9 格中的 8 格：同一轴上两个相反方向
 *          都在时那一轴算居中（M-10 的"两个相反方向同时被注入则回到居中"），
 *          所以 (0,0) 这格是 kHatNone 而不是某个方向。
 */
constexpr uint8_t kHatOf[] = {
    5, 4, 3,   // 下：左下 下 右下
    6, kHatNone, 2,  // 横向：左 居中 右
    7, 0, 1,   // 上：左上 上 右上
};

/// 配置 token 表，下标即 Source 的枚举值（C-03：一张表、大小写敏感、无别名）。
constexpr std::string_view kSourceTokens[] = {
    "dpad_up",    "dpad_right", "dpad_down",  "dpad_left",
    "triangle",   "circle",     "cross",      "square",
    "l1",         "r1",         "l2",         "r2",
    "create",     "options",    "l3",         "r3",
    "home",       "pad",        "mute",       "lfn",
    "rfn",        "lpaddle",    "rpaddle",
    "lstick_up",   "lstick_right", "lstick_down", "lstick_left",
    "rstick_up",   "rstick_right", "rstick_down", "rstick_left",
    "touch_left",  "touch_right",  "touch_two",
    "pad_left",    "pad_right",
    "swipe_up",    "swipe_right",  "swipe_down",   "swipe_left",
};
static_assert(std::size(kSourceTokens) == static_cast<std::size_t>(Source::kCount),
              "the token table must cover every bindable source");

/// 轴名表，下标即 Axis 的枚举值。
constexpr std::string_view kAxisTokens[] = {
    "lstick_x", "lstick_y", "rstick_x", "rstick_y", "ltrigger", "rtrigger",
};
static_assert(std::size(kAxisTokens) == kAxisCount,
              "the axis token table must cover every axis");

/// 整根摇杆的 token 表，只服务配置语法（`center_lstick` 那一档粒度）。
constexpr std::string_view kStickTokens[] = {"lstick", "rstick"};

/// 与 kStickTokens 同序的轴表：每项是那一根摇杆的 X 轴。
constexpr Axis kStickAxes[] = {Axis::kLstickX, Axis::kRstickX};
static_assert(std::size(kStickTokens) == std::size(kStickAxes),
              "the stick token table must cover every stick");

/**
 * @brief 四个方向位 → hat 值。
 * @param digits 快照里的位集合。
 * @return 0..7 之一，或 kHatNone。
 */
uint8_t HatOf(Mask digits) {
  const bool up = (digits & BitOf(Source::kDpadUp)) != 0;
  const bool down = (digits & BitOf(Source::kDpadDown)) != 0;
  const bool right = (digits & BitOf(Source::kDpadRight)) != 0;
  const bool left = (digits & BitOf(Source::kDpadLeft)) != 0;
  // 相反的两个方向同时置位时那一轴取 0：这一轴没有可表达的倾斜。
  const int vertical = up == down ? 0 : (up ? 1 : -1);
  const int horizontal = right == left ? 0 : (right ? 1 : -1);
  return kHatOf[(vertical + 1) * 3 + (horizontal + 1)];
}

/**
 * @brief 一个摇杆方向的判定表：读哪根轴、正方向是哪一边。
 * @details 顺序跟着 Source 的枚举走（每根摇杆都是上、右、下、左）。"上"配的是
 *          负的 y 偏移：线上这个字节向下增长，所以把摇杆推向上方时 y 变小——
 *          与上游 Studio 的 stick_axis()/MACRO_STICK_UP 同一口径。
 */
constexpr struct {
  Axis axis;
  bool positive;
} kStickDirs[] = {
    {Axis::kLstickY, false}, {Axis::kLstickX, true},
    {Axis::kLstickY, true},  {Axis::kLstickX, false},
    {Axis::kRstickY, false}, {Axis::kRstickX, true},
    {Axis::kRstickY, true},  {Axis::kRstickX, false},
};

/// 第一个摇杆方向源在枚举里的位置，kStickDirs 的下标由它偏移出来。
constexpr std::size_t kFirstStickDir = static_cast<std::size_t>(Source::kLstickUp);

/**
 * @brief 读状态体里的一个 32 位小端字。
 * @param data 指向那四个字。
 * @return 拼好的字。
 * @details core 里同一个原语叫 ReadU32；插件拿不到那份头，所以这里是第二处写法，
 *          与触摸点字段的位置一起由 ids 单测和 core 的解码器逐字段对账。
 */
uint32_t ReadU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

/// 一个位移的绝对值：两端都是 12 位坐标，差值远在 int32 之内。
uint32_t AbsDelta(int32_t delta) {
  return delta < 0 ? static_cast<uint32_t>(-delta) : static_cast<uint32_t>(delta);
}

/**
 * @brief 横坐标落在哪个半区。
 * @param x 12 位 raw 横坐标。
 * @return kTouchLeft 或 kTouchRight；中线那条带里返回 0，也就是"任意半区"。
 */
Mask HalfOf(uint32_t x) {
  const uint32_t distance = x > kPadMidX ? x - kPadMidX : kPadMidX - x;
  if (distance <= kPadNeutralBand) return 0;
  return x > kPadMidX ? BitOf(Source::kTouchRight) : BitOf(Source::kTouchLeft);
}

/// 划动位移的门限，单位与 kSwipeScaled 同一把刻度（横向 raw 刻度的 16 倍）。
constexpr uint32_t kSwipeMinScaled = kSwipeMinSpan * kPadAspectX;

/**
 * @brief 一个触点从起点算起有没有划出一次划动。
 * @param contact 这一帧的位置。
 * @param track 这一轮的起点（TouchpadStates 在触点出现那一刻写进去的）。
 * @return 划出的方向位；还没到门限返回 0。
 * @details 两路位移先折算到同一把刻度再比：横向乘 16、纵向乘 9，比的就是实际距离
 *          而不是 raw 刻度（面板是 16:9，而两个轴都跑满 12 位）。方向取占主导的
 *          那一轴，两轴恰好等距离时算横向——那是一条真实的斜向笔画，横向那一档
 *          更常用，不需要为它再留一个"说不清"的状态。
 */
Mask SwipeOf(const Contact& contact, const TouchTrack& track) {
  const int32_t dx = static_cast<int32_t>(contact.x) - track.start_x;
  const int32_t dy = static_cast<int32_t>(contact.y) - track.start_y;
  const uint32_t scaled_x = AbsDelta(dx) * kPadAspectX;
  const uint32_t scaled_y = AbsDelta(dy) * kPadAspectY;
  if (std::max(scaled_x, scaled_y) < kSwipeMinScaled) return 0;
  if (scaled_x >= scaled_y) {
    return dx > 0 ? BitOf(Source::kSwipeRight) : BitOf(Source::kSwipeLeft);
  }
  // 线上这个坐标向下增长，所以正的 dy 是"向下划"——与摇杆方向同一口径。
  return dy > 0 ? BitOf(Source::kSwipeDown) : BitOf(Source::kSwipeUp);
}

}  // namespace

bool ParseToken(std::string_view token, Source& out) {
  for (std::size_t i = 0; i < std::size(kSourceTokens); ++i) {
    if (kSourceTokens[i] == token) {
      out = static_cast<Source>(i);
      return true;
    }
  }
  return false;
}

bool ParseAxisToken(std::string_view token, Axis& out) {
  for (std::size_t i = 0; i < std::size(kAxisTokens); ++i) {
    if (kAxisTokens[i] == token) {
      out = static_cast<Axis>(i);
      return true;
    }
  }
  return false;
}

bool ParseStickToken(std::string_view token, Axis& out) {
  for (std::size_t i = 0; i < std::size(kStickTokens); ++i) {
    if (kStickTokens[i] == token) {
      out = kStickAxes[i];
      return true;
    }
  }
  return false;
}

const char* TokenOf(Source src) {
  const std::size_t index = static_cast<std::size_t>(src);
  return index < std::size(kSourceTokens) ? kSourceTokens[index].data() : "";
}

const char* TokenOf(Axis axis) {
  const std::size_t index = static_cast<std::size_t>(axis);
  return index < std::size(kAxisTokens) ? kAxisTokens[index].data() : "";
}

Frame DecodeFrame(const uint8_t* state) {
  Frame frame;
  for (uint8_t a = 0; a < kAxisCount; ++a) {
    frame.axis[a] = state[kAxisOffset + a];
  }
  frame.digits = kHatDirs[state[kButtons0] & kDpadMask];
  for (std::size_t i = 0; i < std::size(kSlots); ++i) {
    const Slot& slot = kSlots[i];
    if (slot.mask != 0 && (state[slot.offset] & slot.mask) != 0) {
      frame.digits |= BitOf(static_cast<Source>(i));
    }
  }
  for (std::size_t i = 0; i < kTouchPoints; ++i) {
    const uint32_t word = ReadU32(state + kTouchOffset + i * 4);
    Contact& contact = frame.contact[i];
    // bit7 的语义是"未接触"，与 core 一样在这里取反成"接触"。
    contact.touching = (word & kContactNotTouchingMask) == 0;
    contact.x = static_cast<uint16_t>((word >> 8) & kContactAxisMask);
    contact.y = static_cast<uint16_t>((word >> 20) & kContactAxisMask);
  }
  return frame;
}

void EncodeFrame(const Frame& frame, uint8_t* state) {
  for (uint8_t a = 0; a < kAxisCount; ++a) {
    state[kAxisOffset + a] = frame.axis[a];
  }

  state[kButtons0] = static_cast<uint8_t>(
      HatOf(frame.digits) |
      ((frame.digits & BitOf(Source::kSquare)) ? kSquareMask : 0) |
      ((frame.digits & BitOf(Source::kCross)) ? kCrossMask : 0) |
      ((frame.digits & BitOf(Source::kCircle)) ? kCircleMask : 0) |
      ((frame.digits & BitOf(Source::kTriangle)) ? kTriangleMask : 0));

  uint8_t buttons_1 = 0;
  for (std::size_t i = static_cast<std::size_t>(Source::kL1);
       i <= static_cast<std::size_t>(Source::kR3); ++i) {
    if ((frame.digits & BitOf(static_cast<Source>(i))) != 0) {
      buttons_1 = static_cast<uint8_t>(buttons_1 | kSlots[i].mask);
    }
  }
  state[kButtons1] = buttons_1;

  // 未解释的那一位从原缓冲里带来：本插件不生产它，所以也不生产它的 0。
  uint8_t buttons_2 = state[kButtons2] & kUnexplainedButtonMask;
  for (std::size_t i = static_cast<std::size_t>(Source::kHome);
       i <= static_cast<std::size_t>(Source::kRPaddle); ++i) {
    if ((frame.digits & BitOf(static_cast<Source>(i))) != 0) {
      buttons_2 = static_cast<uint8_t>(buttons_2 | kSlots[i].mask);
    }
  }
  state[kButtons2] = buttons_2;
}

Axis TriggerAxisOf(Source src) {
  switch (src) {
    case Source::kL2: return Axis::kLTrigger;
    case Source::kR2: return Axis::kRTrigger;
    default: return Axis::kCount;
  }
}

Axis StickAxisOf(Source src) {
  const std::size_t index = static_cast<std::size_t>(src);
  return index >= kFirstStickDir && index < kFirstStickDir + std::size(kStickDirs)
             ? kStickDirs[index - kFirstStickDir].axis
             : Axis::kCount;
}

Mask StickDirections(const Frame& frame, uint8_t step, uint8_t hysteresis,
                     Mask latched) {
  const int16_t on = step;
  // 迟滞吃满阈值时松开线就没了（甚至成负数），那种情形下退到 1：仍然要求摇杆
  // 真的离开中心一点才算松开。
  const int16_t off = step > hysteresis ? static_cast<int16_t>(step - hysteresis) : 1;
  Mask engaged = 0;
  for (std::size_t i = 0; i < std::size(kStickDirs); ++i) {
    const int16_t deflection = static_cast<int16_t>(
        frame.axis[IndexOf(kStickDirs[i].axis)]) - kStickCenter;
    const int16_t projected = kStickDirs[i].positive ? deflection : -deflection;
    const Mask bit = BitOf(static_cast<Source>(kFirstStickDir + i));
    if (projected >= ((latched & bit) != 0 ? off : on)) engaged |= bit;
  }
  return engaged;
}

Mask TouchpadStates(const Frame& frame, TouchState& io) {
  Mask levels = 0;
  std::size_t present = 0;
  for (std::size_t i = 0; i < kTouchPoints; ++i) {
    const Contact& contact = frame.contact[i];
    TouchTrack& track = io.contact[i];
    if (!contact.touching) {
      // 触点离开就是把这一轮作废：下一次上手指重新定半区、重新起算划动。
      track.present = false;
      track.side = 0;
      continue;
    }
    if (!track.present) {
      track.present = true;
      track.side = HalfOf(contact.x);
      track.start_x = contact.x;
      track.start_y = contact.y;
      track.swiped = false;
    }
    levels |= track.side;
    ++present;
    if (!track.swiped) {
      const Mask swipe = SwipeOf(contact, track);
      if (swipe != 0) {
        track.swiped = true;
        io.swipe_latch |= swipe;
      }
    }
  }

  // 划动位保持到所有触点离开，为的是让 K-08 那个"全松开才复位"的闸门看得见一次
  // 完整的按下—松开：事件只出现一帧的话，触发判定每帧读到的都是"没有按下"。
  if (present == 0) io.swipe_latch = 0;

  Mask out = levels | io.swipe_latch;
  if (present >= 2) out |= BitOf(Source::kTouchTwo);
  // 半区点击 = 按下位 + 那一侧有起始触点。面板上没有任何触点时两个半区点击都不
  // 成立，线上那个 pad 位因此仍然只代表"任意半区"（I-03 的三种绑定互斥）。
  if ((frame.digits & BitOf(Source::kPad)) != 0) {
    if ((levels & BitOf(Source::kTouchLeft)) != 0) out |= BitOf(Source::kPadLeft);
    if ((levels & BitOf(Source::kTouchRight)) != 0) out |= BitOf(Source::kPadRight);
  }
  return out;
}

uint8_t Invert(uint8_t value) {
  // 0 那一端算出 256，落在界外，夹回 255：头文件说的那个刻度就丢在这里。
  return value == kStickMin ? kStickMax
                            : static_cast<uint8_t>(kStickMax + 1 - value);
}

}  // namespace ids
}  // namespace buttons
