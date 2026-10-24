/**
 * @file
 * @brief SetStateData 状态实现：语义模型合并 + 线格式缓冲
 *
 * 逐字段合并规则移植自上游 DS5Dongle 固件，与原始实现有三处差异：合并操
 * 作 core::protocol::OutputState 语义模型，不 reinterpret 线格式；FunctionFS
 * 上 0x02 payload 只有 47 字节有效，按 kUsbOutputPayloadSize 解码；位域显式
 * 写成 bool/bit 操作，不做隐式窄化。
 *
 * 对外只暴露线格式：state_data 编译期就是固件初值，UpdateOutputState()
 * 合并完重新编码它，CopyOutputState() 就是一次 memcpy——0x36 每包都要取
 * 这份状态，编码开销放在低频的合并侧而不是高频的取用侧。
 *
 * 锁的分工（UpdateOutputState 不加锁）是有意的：更新只发生在 usb OUT 线程，
 * 且合并是对模型字段的整值写；CopyOutputState 在锁内取快照。要严格化就
 * 给更新侧也上同一把锁，别拆掉读取侧现有的。
 */

#include "bridge/state_report.h"

#include <cstring>
#include <mutex>

#include "core/protocol/output_state.h"

namespace bridge {
namespace detail {
namespace {

// 0x32 / 0x36 报告里嵌的就是这 63 字节 SetStateData。字面值是固件的初始
// 状态，只给到 47 字节 payload，[47..62] 的 reserved 由聚合初始化隐式补零；
// byte[4]/byte[5] 是耳机/扬声器音量，非零是控制器接受 0x36 报告的前提。
// 之后只由 UpdateOutputState() 重新编码，没人直接写它。
uint8_t state_data[core::protocol::kSetStateShadowSize] = {
    0xfd, 0xf7, 0x0, 0x0,  0x7f, 0x64,  // Headphone, Speaker volume
    0xff, 0x9,  0x0, 0x0F, 0x0,  0x0,  0x0,  0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0,  0x0,  0x0,  0x0,  0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0,  0x0,  0x0,  0x0,  0x0, 0x0, 0xa, 0x7,
    0x0,  0x0,  0x2, 0x1,  0x00, 0xff, 0xd7, 0x00  // RGB LED: R, G, B
};

// 合并用的语义模型：allow-flag 规则作用在它上面，只在本模块内可见。
// InitOutputState() 从 state_data 解码出初值，之后是各字段的权威载体。
core::protocol::OutputState output_state;

// 保护线格式缓冲的读取快照（CopyOutputState）与模型初建（Init）；
// UpdateOutputState 不加锁的分工见文件头。
std::mutex state_mutex;

}  // namespace

void InitOutputState(uint8_t speaker_pre_gain) {
  std::lock_guard<std::mutex> lock(state_mutex);
  output_state = core::protocol::DecodeOutputState(state_data);
  // 扬声器前置增益是用户配置，不来自主机报告（UpdateOutputState 有意跳过
  // 音频控制类字段），所以只在这里写进模型，再编码回线格式供 0x36 取用。
  output_state.speaker_comp_pre_gain = speaker_pre_gain;
  core::protocol::EncodeOutputState(output_state, state_data);
}

void CopyOutputState(uint8_t* out) {
  std::lock_guard<std::mutex> lock(state_mutex);
  std::memcpy(out, state_data, sizeof(state_data));
}

void UpdateOutputState(const uint8_t* data) {
  // 解码：47 字节 payload → 语义模型；只读到 kUsbOutputPayloadSize 为止，
  // 容量由调用方保证。
  const core::protocol::OutputState payload =
      core::protocol::DecodeOutputState(data);

  // ---- 无条件合并：三个 rumble 开关没有 allow-flag，总跟着主机走 ----
  output_state.enable_rumble_emulation = payload.enable_rumble_emulation;
  output_state.use_rumble_not_haptics = payload.use_rumble_not_haptics;
  output_state.enable_improved_rumble_emulation =
      payload.enable_improved_rumble_emulation;

  // 左右 rumble 强度：任一 rumble 开关开着才透传——两个开关全关时主机写
  // 的强度不生效，也没必要存。
  if (payload.use_rumble_not_haptics || payload.enable_rumble_emulation) {
    output_state.rumble_emulation_right = payload.rumble_emulation_right;
    output_state.rumble_emulation_left = payload.rumble_emulation_left;
  }

  // ---- 以下每组的合并都以各自的 allow-flag 为门：主机没开这个位，
  // 对应字段就保持上一份的值（位段分组即主机的字段许可粒度）----
  if (payload.allow_mute_light) {
    output_state.mute_light_mode = payload.mute_light_mode;
  }
  if (payload.allow_right_trigger_ffb) {
    output_state.right_trigger_ffb = payload.right_trigger_ffb;
  }
  if (payload.allow_left_trigger_ffb) {
    output_state.left_trigger_ffb = payload.left_trigger_ffb;
  }
  if (payload.allow_color_light_fade_animation) {
    output_state.light_fade_animation = payload.light_fade_animation;
  }
  if (payload.allow_light_brightness_change) {
    output_state.light_brightness = payload.light_brightness;
  }
  // 玩家指示灯是一个整体：7 个灯位字段同进同退，不做逐位挑选。
  if (payload.allow_player_indicators) {
    output_state.player_light_1 = payload.player_light_1;
    output_state.player_light_2 = payload.player_light_2;
    output_state.player_light_3 = payload.player_light_3;
    output_state.player_light_4 = payload.player_light_4;
    output_state.player_light_5 = payload.player_light_5;
    output_state.player_light_fade = payload.player_light_fade;
    output_state.unknown_player_lights = payload.unknown_player_lights;
  }
  if (payload.allow_led_color) {
    output_state.led_red = payload.led_red;
    output_state.led_green = payload.led_green;
    output_state.led_blue = payload.led_blue;
  }

  // ---- 有意不合并的字段：耳/扬/麦音量与音频控制类。DS5Dummy 的音频控制
  // 由 PipeWire 侧处理，这几个字段保留 Init 时的初值，不随主机报告漂移。----

  // 模型 → 线格式：读侧（CopyOutputState）因此永远只需一次 memcpy。
  core::protocol::EncodeOutputState(output_state, state_data);
}

}  // namespace detail
}  // namespace bridge
