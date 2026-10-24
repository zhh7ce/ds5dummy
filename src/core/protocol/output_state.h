/**
 * @file output_state.h
 * @brief DualSense 输出状态（SetStateData）的语义模型与显式编解码声明
 *
 * OutputState 是主机→设备方向控制报告的字段化表示：震动/触觉模式与强度、
 * 各通道音量与静音、扳机力反馈波形、LED 与玩家灯、电源管理类开关。线上
 * 结构分两层——主机经 USB 0x02 只发送 47 字节 payload；设备内部（以及
 * BT 0x36 音频报告）使用 63 字节 SetStateData，[47..62] 是主机从不发送、
 * 由 bridge 影子缓冲补足的 reserved padding。
 *
 * 与 input_state.h 相同的纪律：不依赖 packed bitfield，位级拆装只发生在
 * 两个编解码函数里。
 *
 * @note Decode 只覆盖主机发来的 47 字节；reserved 段不经解码，Decode 后
 *       恒为全零（见实现处的说明）。
 */

#pragma once

#include <array>
#include <cstdint>

#include "core/protocol/report.h"

namespace core::protocol {

/// 静音指示灯模式，占 data[8] 低 3 位；3..7 为真机未定义动作位。
enum class MuteLightState : uint8_t {
  kOff = 0,        ///< 熄灭。
  kOn = 1,         ///< 常亮。
  kBreathing = 2,  ///< 呼吸闪烁。
  kDoNothing = 3,  ///< 不改变当前行为。
  kNoAction4 = 4,  ///< 未定义动作位，原样保留。
  kNoAction5 = 5,  ///< 未定义动作位，原样保留。
  kNoAction6 = 6,  ///< 未定义动作位，原样保留。
  kNoAction7 = 7,  ///< 未定义动作位，原样保留。
};

/// 玩家灯亮度档位，占低 3 位；0 最亮，3..7 为未定义动作位。
enum class LightBrightnessLevel : uint8_t {
  kBright = 0,      ///< 最亮。
  kMid = 1,         ///< 中档。
  kDim = 2,         ///< 暗档。
  kNoAction3 = 3,   ///< 未定义动作位，原样保留。
  kNoAction4 = 4,   ///< 未定义动作位，原样保留。
  kNoAction5 = 5,   ///< 未定义动作位，原样保留。
  kNoAction6 = 6,   ///< 未定义动作位，原样保留。
  kNoAction7 = 7,   ///< 未定义动作位，原样保留。
};

/// 彩色灯光淡入淡出模式，data[41] 整字节。
enum class LightFadeAnimationMode : uint8_t {
  kNothing = 0,  ///< 不做动画。
  kIn = 1,       ///< 淡入。
  kOut = 2,      ///< 淡出。
};

/**
 * @brief 63 字节 SetStateData 的语义表示；成员顺序是逻辑分组，线上偏移
 *        只由编解码函数决定。
 *
 * allow_* 位是主机的"本字段生效"许可开关：置位时后续对应字段才应被设备
 * 采纳，bridge 按位合并进影子缓冲时正是以它们为准。数值成员均为线上
 * 原始字节/位段，未做单位换算。
 */
struct OutputState {
  bool enable_rumble_emulation = false;  ///< data[0].b0 启用震动模拟。
  bool use_rumble_not_haptics = false;   ///< data[0].b1 用震动马达替代触觉。
  bool allow_right_trigger_ffb = false;  ///< data[0].b2 右扳机 FFB 生效许可。
  bool allow_left_trigger_ffb = false;   ///< data[0].b3 左扳机 FFB 生效许可。
  bool allow_headphone_volume = false;   ///< data[0].b4 耳机音量生效许可。
  bool allow_speaker_volume = false;     ///< data[0].b5 扬声器音量生效许可。
  bool allow_mic_volume = false;         ///< data[0].b6 麦克风音量生效许可。
  bool allow_audio_control = false;      ///< data[0].b7 音频控制组生效许可。

  bool allow_mute_light = false;              ///< data[1].b0 静音灯生效许可。
  bool allow_audio_mute = false;              ///< data[1].b1 音频静音组生效许可。
  bool allow_led_color = false;               ///< data[1].b2 LED 颜色生效许可。
  bool reset_lights = false;                  ///< data[1].b3 重置全部灯光。
  bool allow_player_indicators = false;       ///< data[1].b4 玩家灯生效许可。
  bool allow_haptic_low_pass_filter = false;  ///< data[1].b5 低通滤波生效许可。
  bool allow_motor_power_level = false;       ///< data[1].b6 电机功率生效许可。
  bool allow_audio_control_2 = false;         ///< data[1].b7 第二音频控制组许可。

  uint8_t rumble_emulation_right = 0;  ///< data[2] 右震动强度，0..255。
  uint8_t rumble_emulation_left = 0;   ///< data[3] 左震动强度，0..255。
  uint8_t volume_headphones = 0;       ///< data[4] 耳机音量。
  uint8_t volume_speaker = 0;          ///< data[5] 扬声器音量。
  uint8_t volume_mic = 0;              ///< data[6] 麦克风音量。

  uint8_t mic_select = 0;            ///< data[7] 低 2 位麦克风选择。
  bool echo_cancel_enable = false;   ///< data[7].b2 回声消除。
  bool noise_cancel_enable = false;  ///< data[7].b3 降噪。
  uint8_t output_path_select = 0;    ///< data[7] bit4..5 输出通路选择。
  uint8_t input_path_select = 0;     ///< data[7] bit6..7 输入通路选择。

  MuteLightState mute_light_mode = MuteLightState::kOff;  ///< data[8] 整字节。

  bool touch_power_save = false;   ///< data[9].b0 触摸板省电。
  bool motion_power_save = false;  ///< data[9].b1 运动传感器省电。
  bool haptic_power_save = false;  ///< data[9].b2 触觉省电。
  bool audio_power_save = false;   ///< data[9].b3 音频省电。
  bool mic_mute = false;           ///< data[9].b4 麦克风静音。
  bool speaker_mute = false;       ///< data[9].b5 扬声器静音。
  bool headphone_mute = false;     ///< data[9].b6 耳机静音。
  bool haptic_mute = false;        ///< data[9].b7 触觉静音。

  std::array<uint8_t, 11> right_trigger_ffb{};  ///< data[10..20] 右扳机波形。
  std::array<uint8_t, 11> left_trigger_ffb{};   ///< data[21..31] 左扳机波形。
  uint32_t host_timestamp = 0;                  ///< data[32..35] 主机时间戳。

  uint8_t rumble_motor_power_reduction = 0;   ///< data[36] 低 nibble。
  uint8_t trigger_motor_power_reduction = 0;  ///< data[36] 高 nibble。
  uint8_t speaker_comp_pre_gain = 0;          ///< data[37] 低 3 位前置增益。
  bool beamforming_enable = false;            ///< data[37].b3 波束成形。
  uint8_t unknown_audio_control_2 = 0;        ///< data[37] 高 nibble，未解释。

  bool allow_light_brightness_change = false;     ///< data[38].b0 亮度许可。
  bool allow_color_light_fade_animation = false;  ///< data[38].b1 淡入淡出许可。
  bool enable_improved_rumble_emulation = false;  ///< data[38].b2 增强震动。
  bool use_rumble_not_haptics_2 = false;          ///< data[38].b3 第二震动选择位。
  uint8_t unknown_38 = 0;                         ///< data[38] 高 nibble，未解释。

  bool haptic_low_pass_filter = false;  ///< data[39].b0 触觉低通滤波值。
  uint8_t unknown_39 = 0;               ///< data[39] bit1..7，未解释。
  uint8_t unknown_byte_40 = 0;          ///< data[40] 整字节，未解释。

  LightFadeAnimationMode light_fade_animation =
      LightFadeAnimationMode::kNothing;    ///< data[41] 整字节。
  LightBrightnessLevel light_brightness =
      LightBrightnessLevel::kBright;       ///< data[42] 整字节。

  bool player_light_1 = false;         ///< data[43].b0 玩家灯 1。
  bool player_light_2 = false;         ///< data[43].b1 玩家灯 2。
  bool player_light_3 = false;         ///< data[43].b2 玩家灯 3。
  bool player_light_4 = false;         ///< data[43].b3 玩家灯 4。
  bool player_light_5 = false;         ///< data[43].b4 玩家灯 5。
  bool player_light_fade = false;      ///< data[43].b5 玩家灯淡入淡出。
  uint8_t unknown_player_lights = 0;   ///< data[43] bit6..7，未解释。

  uint8_t led_red = 0;    ///< data[44] LED 红分量，0..255。
  uint8_t led_green = 0;  ///< data[45] LED 绿分量，0..255。
  uint8_t led_blue = 0;   ///< data[46] LED 蓝分量，0..255。

  std::array<uint8_t, 16> reserved{};  ///< data[47..62] padding；主机从不发送。
};

/**
 * @brief 从主机发来的 47 字节 SetStateData payload 解码语义模型。
 * @param wire 指向 payload 首字节；本函数恰好读 kUsbOutputPayloadSize
 *             （47）字节，指针不得为空。
 * @return 解码后的 OutputState；reserved 恒为全零——47 字节里没有它。
 * @note 纯读，无副作用，任意线程可调。
 * @see EncodeOutputState —— 编码回 63 字节完整段。
 */
OutputState DecodeOutputState(const uint8_t* wire);

/**
 * @brief 把语义模型编码为 63 字节完整 SetStateData（含 reserved 尾部）。
 *
 * 先整体清零再逐字段写入；位段按线上宽度掩码。reserved 段来自 state
 * 自身而非解码结果——调用方若要求 [47..62] 保持影子值，需先把影子缓冲
 * 的这段填进 state.reserved 再编码。
 *
 * @param state 待编码的语义模型。
 * @param out 目标缓冲区，本函数恰好写 kSetStateShadowSize（63）字节。
 * @note 纯写，无副作用，任意线程可调。
 * @see DecodeOutputState —— 47 字节方向的逆操作（不含 reserved）。
 */
void EncodeOutputState(const OutputState& state, uint8_t* out);

}  // namespace core::protocol
