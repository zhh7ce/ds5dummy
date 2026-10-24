/**
 * @file output_state.cpp
 * @brief OutputState 线格式（47 字节解码 / 63 字节编码）的显式实现
 *
 * 解码读 47 字节（主机发送的全部），编码写 63 字节（0x36 需要的完整
 * SetStateData）——这个不对称是协议本身决定的：[47..62] reserved 段主机
 * 从不发送。字段与位段语义见 output_state.h。
 */

#include "core/protocol/output_state.h"

#include <algorithm>
#include <cstring>

namespace core::protocol {
namespace {

/**
 * @brief 读小端 uint32。
 * @param data 至少 4 字节可读。
 * @return 4 字节按位拼合的无符号值。
 */
uint32_t ReadU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

/**
 * @brief 写小端 uint32。
 * @param data 至少 4 字节可写。
 * @param value 待写入值。
 */
void WriteU32(uint8_t* data, uint32_t value) {
  data[0] = value & 0xffu;
  data[1] = (value >> 8) & 0xffu;
  data[2] = (value >> 16) & 0xffu;
  data[3] = (value >> 24) & 0xffu;
}

}  // namespace

OutputState DecodeOutputState(const uint8_t* data) {
  OutputState state;

  // [0] 第一组生效许可位，bit0 起：震动模拟 震动代触觉 右扳机FFB
  // 左扳机FFB 耳机音量 扬声器音量 麦克风音量 音频控制。
  const uint8_t flags_0 = data[0];
  state.enable_rumble_emulation = (flags_0 & 0x01u) != 0;
  state.use_rumble_not_haptics = (flags_0 & 0x02u) != 0;
  state.allow_right_trigger_ffb = (flags_0 & 0x04u) != 0;
  state.allow_left_trigger_ffb = (flags_0 & 0x08u) != 0;
  state.allow_headphone_volume = (flags_0 & 0x10u) != 0;
  state.allow_speaker_volume = (flags_0 & 0x20u) != 0;
  state.allow_mic_volume = (flags_0 & 0x40u) != 0;
  state.allow_audio_control = (flags_0 & 0x80u) != 0;

  // [1] 第二组许可位，bit0 起：静音灯 音频静音 LED颜色 重置灯光
  // 玩家灯 触觉低通 电机功率 音频控制2。
  const uint8_t flags_1 = data[1];
  state.allow_mute_light = (flags_1 & 0x01u) != 0;
  state.allow_audio_mute = (flags_1 & 0x02u) != 0;
  state.allow_led_color = (flags_1 & 0x04u) != 0;
  state.reset_lights = (flags_1 & 0x08u) != 0;
  state.allow_player_indicators = (flags_1 & 0x10u) != 0;
  state.allow_haptic_low_pass_filter = (flags_1 & 0x20u) != 0;
  state.allow_motor_power_level = (flags_1 & 0x40u) != 0;
  state.allow_audio_control_2 = (flags_1 & 0x80u) != 0;

  // [2..6] 强度与音量：两组震动强度、三路音量，各 1 字节原始值。
  state.rumble_emulation_right = data[2];
  state.rumble_emulation_left = data[3];
  state.volume_headphones = data[4];
  state.volume_speaker = data[5];
  state.volume_mic = data[6];

  // [7] 音频路由杂项：低 2 位麦选择、b2/b3 回声与降噪开关、
  // bit4..5 输出通路、bit6..7 输入通路。
  state.mic_select = data[7] & 0x03u;
  state.echo_cancel_enable = (data[7] & 0x04u) != 0;
  state.noise_cancel_enable = (data[7] & 0x08u) != 0;
  state.output_path_select = (data[7] >> 4) & 0x03u;
  state.input_path_select = (data[7] >> 6) & 0x03u;

  // [8] 静音灯模式整字节入枚举。
  state.mute_light_mode = static_cast<MuteLightState>(data[8]);

  // [9] 省电与静音位，bit0 起：触摸 运动 触觉 音频 麦 扬声器 耳机 触觉静音。
  state.touch_power_save = (data[9] & 0x01u) != 0;
  state.motion_power_save = (data[9] & 0x02u) != 0;
  state.haptic_power_save = (data[9] & 0x04u) != 0;
  state.audio_power_save = (data[9] & 0x08u) != 0;
  state.mic_mute = (data[9] & 0x10u) != 0;
  state.speaker_mute = (data[9] & 0x20u) != 0;
  state.headphone_mute = (data[9] & 0x40u) != 0;
  state.haptic_mute = (data[9] & 0x80u) != 0;

  // [10..35] 扳机 FFB 波形两段各 11 字节（不解释内容，整段搬运），
  // 随后 4 字节主机时间戳。
  std::copy_n(data + 10, state.right_trigger_ffb.size(),
              state.right_trigger_ffb.begin());
  std::copy_n(data + 21, state.left_trigger_ffb.size(),
              state.left_trigger_ffb.begin());
  state.host_timestamp = ReadU32(data + 32);

  // [36..37] 电机功率衰减与音频控制 2：nibble/窄位段拆分。
  state.rumble_motor_power_reduction = data[36] & 0x0fu;
  state.trigger_motor_power_reduction = (data[36] >> 4) & 0x0fu;
  state.speaker_comp_pre_gain = data[37] & 0x07u;
  state.beamforming_enable = (data[37] & 0x08u) != 0;
  state.unknown_audio_control_2 = (data[37] >> 4) & 0x0fu;

  // [38..40] 亮度/淡入淡出/增强震动许可与各自的不解释位段。
  state.allow_light_brightness_change = (data[38] & 0x01u) != 0;
  state.allow_color_light_fade_animation = (data[38] & 0x02u) != 0;
  state.enable_improved_rumble_emulation = (data[38] & 0x04u) != 0;
  state.use_rumble_not_haptics_2 = (data[38] & 0x08u) != 0;
  state.unknown_38 = (data[38] >> 4) & 0x0fu;

  state.haptic_low_pass_filter = (data[39] & 0x01u) != 0;
  state.unknown_39 = (data[39] >> 1) & 0x7fu;
  state.unknown_byte_40 = data[40];

  // [41..42] 灯光动画模式与亮度档位，整字节入枚举。
  state.light_fade_animation = static_cast<LightFadeAnimationMode>(data[41]);
  state.light_brightness = static_cast<LightBrightnessLevel>(data[42]);

  // [43] 玩家灯 5 位加淡入淡出位，高 2 位不解释。
  state.player_light_1 = (data[43] & 0x01u) != 0;
  state.player_light_2 = (data[43] & 0x02u) != 0;
  state.player_light_3 = (data[43] & 0x04u) != 0;
  state.player_light_4 = (data[43] & 0x08u) != 0;
  state.player_light_5 = (data[43] & 0x10u) != 0;
  state.player_light_fade = (data[43] & 0x20u) != 0;
  state.unknown_player_lights = (data[43] >> 6) & 0x03u;

  // [44..46] LED  RGB 三分量，到此 47 字节读完；[47..62] reserved 不属于
  // 主机发送范围，state.reserved 保持默认全零。
  state.led_red = data[44];
  state.led_green = data[45];
  state.led_blue = data[46];
  return state;
}

void EncodeOutputState(const OutputState& state, uint8_t* data) {
  // 先整体清零：解码方向没有对应字段的间隙（如 reserved）在未显式写入
  // 时保证为 0。
  std::memset(data, 0, kSetStateShadowSize);

  // [0..1] 两组生效许可位打包，位映射与解码逐位对应。
  data[0] = (state.enable_rumble_emulation ? 0x01u : 0u) |
            (state.use_rumble_not_haptics ? 0x02u : 0u) |
            (state.allow_right_trigger_ffb ? 0x04u : 0u) |
            (state.allow_left_trigger_ffb ? 0x08u : 0u) |
            (state.allow_headphone_volume ? 0x10u : 0u) |
            (state.allow_speaker_volume ? 0x20u : 0u) |
            (state.allow_mic_volume ? 0x40u : 0u) |
            (state.allow_audio_control ? 0x80u : 0u);

  data[1] = (state.allow_mute_light ? 0x01u : 0u) |
            (state.allow_audio_mute ? 0x02u : 0u) |
            (state.allow_led_color ? 0x04u : 0u) |
            (state.reset_lights ? 0x08u : 0u) |
            (state.allow_player_indicators ? 0x10u : 0u) |
            (state.allow_haptic_low_pass_filter ? 0x20u : 0u) |
            (state.allow_motor_power_level ? 0x40u : 0u) |
            (state.allow_audio_control_2 ? 0x80u : 0u);

  // [2..6] 震动强度与三路音量。
  data[2] = state.rumble_emulation_right;
  data[3] = state.rumble_emulation_left;
  data[4] = state.volume_headphones;
  data[5] = state.volume_speaker;
  data[6] = state.volume_mic;

  // [7..9] 音频路由、静音灯模式、省电/静音位。
  data[7] = (state.mic_select & 0x03u) |
            (state.echo_cancel_enable ? 0x04u : 0u) |
            (state.noise_cancel_enable ? 0x08u : 0u) |
            ((state.output_path_select & 0x03u) << 4) |
            ((state.input_path_select & 0x03u) << 6);
  data[8] = static_cast<uint8_t>(state.mute_light_mode);

  data[9] = (state.touch_power_save ? 0x01u : 0u) |
            (state.motion_power_save ? 0x02u : 0u) |
            (state.haptic_power_save ? 0x04u : 0u) |
            (state.audio_power_save ? 0x08u : 0u) |
            (state.mic_mute ? 0x10u : 0u) | (state.speaker_mute ? 0x20u : 0u) |
            (state.headphone_mute ? 0x40u : 0u) |
            (state.haptic_mute ? 0x80u : 0u);

  // [10..35] FFB 波形整段搬运加主机时间戳。
  std::copy_n(state.right_trigger_ffb.begin(), state.right_trigger_ffb.size(),
              data + 10);
  std::copy_n(state.left_trigger_ffb.begin(), state.left_trigger_ffb.size(),
              data + 21);
  WriteU32(data + 32, state.host_timestamp);

  // [36..40] 功率衰减、音频控制 2、亮度许可组、低通与未解释位段。
  data[36] = (state.rumble_motor_power_reduction & 0x0fu) |
             ((state.trigger_motor_power_reduction & 0x0fu) << 4);
  data[37] = (state.speaker_comp_pre_gain & 0x07u) |
             (state.beamforming_enable ? 0x08u : 0u) |
             ((state.unknown_audio_control_2 & 0x0fu) << 4);

  data[38] = (state.allow_light_brightness_change ? 0x01u : 0u) |
             (state.allow_color_light_fade_animation ? 0x02u : 0u) |
             (state.enable_improved_rumble_emulation ? 0x04u : 0u) |
             (state.use_rumble_not_haptics_2 ? 0x08u : 0u) |
             ((state.unknown_38 & 0x0fu) << 4);

  data[39] = (state.haptic_low_pass_filter ? 0x01u : 0u) |
             ((state.unknown_39 & 0x7fu) << 1);
  data[40] = state.unknown_byte_40;

  // [41..43] 灯光动画、亮度档位、玩家灯位组。
  data[41] = static_cast<uint8_t>(state.light_fade_animation);
  data[42] = static_cast<uint8_t>(state.light_brightness);

  data[43] = (state.player_light_1 ? 0x01u : 0u) |
             (state.player_light_2 ? 0x02u : 0u) |
             (state.player_light_3 ? 0x04u : 0u) |
             (state.player_light_4 ? 0x08u : 0u) |
             (state.player_light_5 ? 0x10u : 0u) |
             (state.player_light_fade ? 0x20u : 0u) |
             ((state.unknown_player_lights & 0x03u) << 6);

  // [44..46] LED 三分量。
  data[44] = state.led_red;
  data[45] = state.led_green;
  data[46] = state.led_blue;

  // [47..62] reserved：解码方向没有来源，只能取自调用方事先放进
  // state.reserved 的影子值；未填则保持清零结果。
  std::copy_n(state.reserved.begin(), state.reserved.size(),
              data + kUsbOutputPayloadSize);
}

}  // namespace core::protocol
