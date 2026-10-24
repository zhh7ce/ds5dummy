/**
 * @file input_state.cpp
 * @brief InputState 63 字节线格式的显式编解码实现
 *
 * 偏移表只存在于本文件的两个函数里：DecodeInputState 按协议布局读，
 * EncodeInputState 以完全相同的偏移写。两处改动必须成对，字段与位段
 * 语义见 input_state.h。
 */

#include "core/protocol/input_state.h"

#include <algorithm>
#include <cstring>

namespace core::protocol {
namespace {

/**
 * @brief 读小端 uint16。
 * @param data 至少 2 字节可读。
 * @return [data[0]] | [data[1]]<<8。
 */
uint16_t ReadU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

/**
 * @brief 读小端 int16（按位取 ReadU16 后重解释符号）。
 * @param data 至少 2 字节可读。
 * @return 有符号 16 位原始值。
 */
int16_t ReadI16(const uint8_t* data) {
  return static_cast<int16_t>(ReadU16(data));
}

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
 * @brief 写小端 uint16。
 * @param data 至少 2 字节可写。
 * @param value 待写入值，高位截断。
 */
void WriteU16(uint8_t* data, uint16_t value) {
  data[0] = value & 0xffu;
  data[1] = (value >> 8) & 0xffu;
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

/**
 * @brief 拆解一个触摸点 32 位打包字。
 *
 * 线上打包：bit0..6 序号、bit7 未接触、bit8..19 X、bit20..31 Y。
 *
 * @param raw 触摸点的 32 位原始字。
 * @return 拆出的 TouchFingerData；x/y 已截断到 12 位。
 */
TouchFingerData DecodeFinger(uint32_t raw) {
  TouchFingerData finger;
  finger.index = raw & 0x7fu;                  // 低 7 位 = 触摸点序号
  finger.not_touching = ((raw >> 7) & 0x1u) != 0;  // bit7 = 接触标志（注意语义是"未接触"）
  finger.x = (raw >> 8) & 0xfffu;              // bit8..19 = X，12 位
  finger.y = (raw >> 20) & 0xfffu;             // bit20..31 = Y，12 位
  return finger;
}

/**
 * @brief 把一个触摸点打包回 32 位线上字（DecodeFinger 的逆）。
 * @param finger 语义字段；x/y 超出 12 位的部分被掩码丢弃。
 * @return 打包后的 32 位字。
 */
uint32_t EncodeFinger(const TouchFingerData& finger) {
  return (finger.index & 0x7fu) | (finger.not_touching ? (1u << 7) : 0u) |
         ((static_cast<uint32_t>(finger.x) & 0xfffu) << 8) |
         ((static_cast<uint32_t>(finger.y) & 0xfffu) << 20);
}

}  // namespace

InputState DecodeInputState(const uint8_t* data) {
  InputState state;

  // [0..6] 模拟量与报告序号：四轴摇杆、两扳机、序号，各 1 字节。
  state.left_stick_x = data[0];
  state.left_stick_y = data[1];
  state.right_stick_x = data[2];
  state.right_stick_y = data[3];
  state.trigger_left = data[4];
  state.trigger_right = data[5];
  state.sequence_number = data[6];

  // [7] 低 nibble 是十字键 hat 值，高 nibble 依次是 方块/叉/圆/三角。
  const uint8_t buttons_0 = data[7];
  state.dpad = static_cast<Direction>(buttons_0 & 0x0fu);
  state.button_square = (buttons_0 & 0x10u) != 0;
  state.button_cross = (buttons_0 & 0x20u) != 0;
  state.button_circle = (buttons_0 & 0x40u) != 0;
  state.button_triangle = (buttons_0 & 0x80u) != 0;

  // [8] 肩键与小摇杆按下位，bit0 起：L1 R1 L2 R2 Create Options L3 R3。
  const uint8_t buttons_1 = data[8];
  state.button_l1 = (buttons_1 & 0x01u) != 0;
  state.button_r1 = (buttons_1 & 0x02u) != 0;
  state.button_l2 = (buttons_1 & 0x04u) != 0;
  state.button_r2 = (buttons_1 & 0x08u) != 0;
  state.button_create = (buttons_1 & 0x10u) != 0;
  state.button_options = (buttons_1 & 0x20u) != 0;
  state.button_l3 = (buttons_1 & 0x40u) != 0;
  state.button_r3 = (buttons_1 & 0x80u) != 0;

  // [9] 第二组按键位，bit0 起：Home Pad Mute 未定义 左功能 右功能 左背 右背。
  const uint8_t buttons_2 = data[9];
  state.button_home = (buttons_2 & 0x01u) != 0;
  state.button_pad = (buttons_2 & 0x02u) != 0;
  state.button_mute = (buttons_2 & 0x04u) != 0;
  state.unknown_button = (buttons_2 & 0x08u) != 0;
  state.button_left_function = (buttons_2 & 0x10u) != 0;
  state.button_right_function = (buttons_2 & 0x20u) != 0;
  state.button_left_paddle = (buttons_2 & 0x40u) != 0;
  state.button_right_paddle = (buttons_2 & 0x80u) != 0;

  // [10..31] 传感器组。陀螺仪三轴在线上按 x,z,y 顺序存放（[15..20]），
  // 加速度计按 x,y,z（[21..26]）；随后 4 字节时间戳与 1 字节温度。
  state.unknown_2 = data[10];
  state.unknown_counter = ReadU32(data + 11);
  state.angular_velocity_x = ReadI16(data + 15);
  state.angular_velocity_z = ReadI16(data + 17);
  state.angular_velocity_y = ReadI16(data + 19);
  state.accelerometer_x = ReadI16(data + 21);
  state.accelerometer_y = ReadI16(data + 23);
  state.accelerometer_z = ReadI16(data + 25);
  state.sensor_timestamp = ReadU32(data + 27);
  state.temperature = static_cast<int8_t>(data[31]);

  // [32..40] 触摸板：两个 32 位打包点加 1 字节时间戳。
  state.touch.fingers[0] = DecodeFinger(ReadU32(data + 32));
  state.touch.fingers[1] = DecodeFinger(ReadU32(data + 36));
  state.touch.timestamp = data[40];

  // [41..51] 扳机反馈状态：[41]/[42] 各装一个扳机的 止点|状态 两个 nibble，
  // [47] 打包左右效果码（右在低 nibble，与 [41]/[42] 的左右顺序相反），
  // 中间与之后是两个 32 位时间戳。
  state.trigger_right_stop_location = data[41] & 0x0fu;
  state.trigger_right_status = (data[41] >> 4) & 0x0fu;
  state.trigger_left_stop_location = data[42] & 0x0fu;
  state.trigger_left_status = (data[42] >> 4) & 0x0fu;
  state.host_timestamp = ReadU32(data + 43);
  state.trigger_right_effect = data[47] & 0x0fu;
  state.trigger_left_effect = (data[47] >> 4) & 0x0fu;
  state.device_timestamp = ReadU32(data + 48);

  // [52] 电量档位与电源状态各占一个 nibble。
  state.power_percent = data[52] & 0x0fu;
  state.power = static_cast<PowerState>((data[52] >> 4) & 0x0fu);

  // [53] 插入/供电状态位，bit0 起：耳机 麦克风 静音 USB数据 USB供电
  // USB供BT 底座 未定义。
  const uint8_t status = data[53];
  state.plugged_headphones = (status & 0x01u) != 0;
  state.plugged_mic = (status & 0x02u) != 0;
  state.mic_muted = (status & 0x04u) != 0;
  state.plugged_usb_data = (status & 0x08u) != 0;
  state.plugged_usb_power = (status & 0x10u) != 0;
  state.usb_power_on_bt = (status & 0x20u) != 0;
  state.dock_detect = (status & 0x40u) != 0;
  state.plugged_unknown = (status & 0x80u) != 0;

  // [54] bit0 外接麦、bit1 触觉低通、bit2..7 未定义 6 位。
  state.plugged_external_mic = (data[54] & 0x01u) != 0;
  state.haptic_low_pass_filter = (data[54] & 0x02u) != 0;
  state.unknown_54 = (data[54] >> 2) & 0x3fu;

  // [55..62] 尾部 8 字节 AES-CMAC，原样搬运。
  std::copy_n(data + 55, state.aes_cmac.size(), state.aes_cmac.begin());
  return state;
}

void EncodeInputState(const InputState& state, uint8_t* data) {
  // 先整体清零：未显式写入的间隙字节（掩码截断、保留位）保证为 0。
  std::memset(data, 0, kUsbInputStateSize);

  // [0..6] 模拟量与报告序号。
  data[0] = state.left_stick_x;
  data[1] = state.left_stick_y;
  data[2] = state.right_stick_x;
  data[3] = state.right_stick_y;
  data[4] = state.trigger_left;
  data[5] = state.trigger_right;
  data[6] = state.sequence_number;

  // [7..9] 三组按键位打包，方向与 DecodeInputState 的位映射逐位对应。
  data[7] =
      static_cast<uint8_t>(state.dpad) | (state.button_square ? 0x10u : 0u) |
      (state.button_cross ? 0x20u : 0u) | (state.button_circle ? 0x40u : 0u) |
      (state.button_triangle ? 0x80u : 0u);

  data[8] = (state.button_l1 ? 0x01u : 0u) | (state.button_r1 ? 0x02u : 0u) |
            (state.button_l2 ? 0x04u : 0u) | (state.button_r2 ? 0x08u : 0u) |
            (state.button_create ? 0x10u : 0u) |
            (state.button_options ? 0x20u : 0u) |
            (state.button_l3 ? 0x40u : 0u) | (state.button_r3 ? 0x80u : 0u);

  data[9] = (state.button_home ? 0x01u : 0u) | (state.button_pad ? 0x02u : 0u) |
            (state.button_mute ? 0x04u : 0u) |
            (state.unknown_button ? 0x08u : 0u) |
            (state.button_left_function ? 0x10u : 0u) |
            (state.button_right_function ? 0x20u : 0u) |
            (state.button_left_paddle ? 0x40u : 0u) |
            (state.button_right_paddle ? 0x80u : 0u);

  // [10..31] 传感器组：写入顺序与线上 x,z,y 一致，int16 经 uint16 中转
  // 只做位重解释，不做数值加工。
  data[10] = state.unknown_2;
  WriteU32(data + 11, state.unknown_counter);
  WriteU16(data + 15, static_cast<uint16_t>(state.angular_velocity_x));
  WriteU16(data + 17, static_cast<uint16_t>(state.angular_velocity_z));
  WriteU16(data + 19, static_cast<uint16_t>(state.angular_velocity_y));
  WriteU16(data + 21, static_cast<uint16_t>(state.accelerometer_x));
  WriteU16(data + 23, static_cast<uint16_t>(state.accelerometer_y));
  WriteU16(data + 25, static_cast<uint16_t>(state.accelerometer_z));
  WriteU32(data + 27, state.sensor_timestamp);
  data[31] = static_cast<uint8_t>(state.temperature);

  // [32..40] 触摸板两点打包。
  WriteU32(data + 32, EncodeFinger(state.touch.fingers[0]));
  WriteU32(data + 36, EncodeFinger(state.touch.fingers[1]));
  data[40] = state.touch.timestamp;

  // [41..51] 扳机反馈与时间戳；nibble 组合顺序与解码严格互逆。
  data[41] = (state.trigger_right_stop_location & 0x0fu) |
             ((state.trigger_right_status & 0x0fu) << 4);
  data[42] = (state.trigger_left_stop_location & 0x0fu) |
             ((state.trigger_left_status & 0x0fu) << 4);
  WriteU32(data + 43, state.host_timestamp);
  data[47] = (state.trigger_right_effect & 0x0fu) |
             ((state.trigger_left_effect & 0x0fu) << 4);
  WriteU32(data + 48, state.device_timestamp);

  // [52] 电量|电源状态两个 nibble。
  data[52] = (state.power_percent & 0x0fu) |
             ((static_cast<uint8_t>(state.power) & 0x0fu) << 4);

  // [53] 插入/供电状态位打包。
  data[53] = (state.plugged_headphones ? 0x01u : 0u) |
             (state.plugged_mic ? 0x02u : 0u) | (state.mic_muted ? 0x04u : 0u) |
             (state.plugged_usb_data ? 0x08u : 0u) |
             (state.plugged_usb_power ? 0x10u : 0u) |
             (state.usb_power_on_bt ? 0x20u : 0u) |
             (state.dock_detect ? 0x40u : 0u) |
             (state.plugged_unknown ? 0x80u : 0u);

  // [54] 外接麦、触觉低通与 6 位未定义段拼一字节。
  data[54] = ((state.unknown_54 & 0x3fu) << 2) |
             (state.haptic_low_pass_filter ? 0x02u : 0u) |
             (state.plugged_external_mic ? 0x01u : 0u);

  // [55..62] AES-CMAC 原样搬运。
  std::copy_n(state.aes_cmac.begin(), state.aes_cmac.size(), data + 55);
}

}  // namespace core::protocol
