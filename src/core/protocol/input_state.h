/**
 * @file input_state.h
 * @brief DualSense 输入状态（63 字节线格式）的语义模型与显式编解码声明
 *
 * InputState 是设备→主机方向一份完整输入报告的字段化表示：摇杆、扳机、
 * 按键、六轴传感器、触摸板、扳机反馈状态、电源与插入状态、末尾 8 字节
 * AES-CMAC。线格式固定 63 字节，嵌在 USB 0x01 输入报告（去掉 report ID
 * 后整体）或 BT 0x31 输入报告的状态区里；同一份布局两条链路共用。
 *
 * 本模块刻意不使用 packed bitfield，也不允许调用方 reinterpret_cast 原始
 * 字节——所有位级拆装必须经过 DecodeInputState/EncodeInputState，字段与
 * 字节偏移的对应关系只在这两个函数里出现一次。
 *
 * @note 结构体默认值即"全放松、无触摸"的空闲状态，可直接作为初始值使用。
 */

#pragma once

#include <array>
#include <cstdint>

#include "core/protocol/report.h"

namespace core::protocol {

/// 电池/充电状态，线格式取低 4 位；0x0A/0x0B/0x0F 为异常分支。
enum class PowerState : uint8_t {
  kDischarging = 0x00,       ///< 未接电源放电中。
  kCharging = 0x01,          ///< 充电中。
  kComplete = 0x02,          ///< 充电完成。
  kAbnormalVoltage = 0x0A,   ///< 电压异常。
  kAbnormalTemperature = 0x0B,  ///< 温度异常。
  kChargingError = 0x0F,     ///< 充电错误。
};

/// 十字键 4 位 hat 值：0..7 为八个方向，顺时针从正北起；8 表示未按下。
/// 线上还可能出现 9..15，解码后原样保留在枚举底层值里，无对应命名项。
enum class Direction : uint8_t {
  kNorth = 0,      ///< 上。
  kNorthEast = 1,  ///< 右上。
  kEast = 2,       ///< 右。
  kSouthEast = 3,  ///< 右下。
  kSouth = 4,      ///< 下。
  kSouthWest = 5,  ///< 左下。
  kWest = 6,       ///< 左。
  kNorthWest = 7,  ///< 左上。
  kNone = 8,       ///< 未按下（hat null 态）。
};

/**
 * @brief 触摸板单个触摸点的解码后字段。
 *
 * 线上是一个 32 位小端字：bit0..6 序号、bit7 未接触标志、bit8..19 X、
 * bit20..31 Y，见 DecodeFinger/EncodeFinger。
 */
struct TouchFingerData {
  uint8_t index = 0;         ///< 触摸点序号，0..127。
  bool not_touching = true;  ///< true=该点当前未接触触摸板。
  uint16_t x = 0;            ///< X 坐标，线上截断为 12 位（0..4095）。
  uint16_t y = 0;            ///< Y 坐标，线上截断为 12 位（0..4095）。
};

/** @brief 两个触摸点加一个 8 位循环时间戳，对应线上 [32..40]。 */
struct TouchData {
  std::array<TouchFingerData, 2> fingers{};  ///< 下标 0/1 对应线上先后两字。
  uint8_t timestamp = 0;                     ///< 触摸时间戳，设备侧循环计数。
};

/**
 * @brief 63 字节输入状态的语义表示；成员声明顺序即逻辑分组，不等于线上
 *        偏移——偏移只由编解码函数决定。
 *
 * 值语义：整数成员都是线上原始值（摇杆/扳机为 0..255 的 8 位模拟量，
 * 传感器为 int16 原始量，未做单位换算）；unknown_* 是协议里已知存在但
 * 本项目不解释的位段，编解码保持原值往返。
 */
struct InputState {
  uint8_t left_stick_x = 0;     ///< 左摇杆 X，0..255。
  uint8_t left_stick_y = 0;     ///< 左摇杆 Y，0..255。
  uint8_t right_stick_x = 0;    ///< 右摇杆 X，0..255。
  uint8_t right_stick_y = 0;    ///< 右摇杆 Y，0..255。
  uint8_t trigger_left = 0;     ///< 左扳机模拟量，0..255。
  uint8_t trigger_right = 0;    ///< 右扳机模拟量，0..255。
  uint8_t sequence_number = 0;  ///< 报告序号（与 BT 帧头 nibble 同源）。

  Direction dpad = Direction::kNone;  ///< 十字键方向（低 nibble）。
  bool button_square = false;         ///< 方块键。
  bool button_cross = false;          ///< 叉键。
  bool button_circle = false;         ///< 圆键。
  bool button_triangle = false;       ///< 三角键。

  bool button_l1 = false;       ///< L1 肩键。
  bool button_r1 = false;       ///< R1 肩键。
  bool button_l2 = false;       ///< L2 键位标志（模拟量见 trigger_left）。
  bool button_r2 = false;       ///< R2 键位标志（模拟量见 trigger_right）。
  bool button_create = false;   ///< Create 键。
  bool button_options = false;  ///< Options 键。
  bool button_l3 = false;       ///< 左摇杆按下。
  bool button_r3 = false;       ///< 右摇杆按下。

  bool button_home = false;            ///< Home/PS 键。
  bool button_pad = false;             ///< 触摸板按下。
  bool button_mute = false;            ///< Mute 键。
  bool unknown_button = false;         ///< 该位段真机含义未解释，往返保留。
  bool button_left_function = false;   ///< 左功能键。
  bool button_right_function = false;  ///< 右功能键。
  bool button_left_paddle = false;     ///< 左背键。
  bool button_right_paddle = false;    ///< 右背键。

  uint8_t unknown_2 = 0;         ///< [10] 未定义字节，往返保留。
  uint32_t unknown_counter = 0;  ///< [11..14] 未定义计数，往返保留。

  int16_t angular_velocity_x = 0;  ///< 陀螺仪 X 原始值（线上顺序 x,z,y）。
  int16_t angular_velocity_z = 0;  ///< 陀螺仪 Z 原始值。
  int16_t angular_velocity_y = 0;  ///< 陀螺仪 Y 原始值。
  int16_t accelerometer_x = 0;     ///< 加速度计 X 原始值。
  int16_t accelerometer_y = 0;     ///< 加速度计 Y 原始值。
  int16_t accelerometer_z = 0;     ///< 加速度计 Z 原始值。
  uint32_t sensor_timestamp = 0;   ///< [27..30] 传感器时间戳，设备侧计数。
  int8_t temperature = 0;          ///< [31] 机体温度原始值（未标定单位）。

  TouchData touch{};  ///< 触摸板两指与时间戳。

  uint8_t trigger_right_stop_location = 0;  ///< 右扳机止点，4 位。
  uint8_t trigger_right_status = 0;         ///< 右扳机状态码，4 位。
  uint8_t trigger_left_stop_location = 0;   ///< 左扳机止点，4 位。
  uint8_t trigger_left_status = 0;          ///< 左扳机状态码，4 位。
  uint32_t host_timestamp = 0;              ///< 主机时间戳回显。

  uint8_t trigger_right_effect = 0;  ///< 右扳机效果码，4 位。
  uint8_t trigger_left_effect = 0;   ///< 左扳机效果码，4 位。
  uint32_t device_timestamp = 0;     ///< 设备时间戳，循环计数。

  uint8_t power_percent = 0;                    ///< 电量档位，4 位（非百分比小数）。
  PowerState power = PowerState::kDischarging;  ///< 电源状态，4 位。

  bool plugged_headphones = false;  ///< 耳机插孔有插头。
  bool plugged_mic = false;         ///< 插孔麦克风。
  bool mic_muted = false;           ///< 麦克风硬件静音开关。
  bool plugged_usb_data = false;    ///< USB 数据连接。
  bool plugged_usb_power = false;   ///< USB 供电。
  bool usb_power_on_bt = false;     ///< 蓝牙链路下经 USB 供电。
  bool dock_detect = false;         ///< 底座接入。
  bool plugged_unknown = false;     ///< 该位含义未解释，往返保留。

  bool plugged_external_mic = false;    ///< 外接麦克风。
  bool haptic_low_pass_filter = false;  ///< 触觉低通滤波状态回显。
  uint8_t unknown_54 = 0;               ///< 6 位未定义位段，往返保留。

  std::array<uint8_t, 8> aes_cmac{};  ///< 报告尾部 8 字节 AES-CMAC。
};

/**
 * @brief 从 63 字节线格式解码出语义模型。
 *
 * 显式逐字段拆装：8/16/32 位小端读取、位段抽取与触摸点 32 位打包拆解，
 * 不做任何数值换算或符号扩展之外的"加工"，保证与 EncodeInputState 严格
 * 往返（unknown_* 位段同样原样保留）。
 *
 * @param wire 指向 63 字节状态体首字节（不含 USB report ID）；本函数恰好
 *             读 kUsbInputStateSize 字节，指针不得为空。
 * @return 解码后的 InputState，按值返回。
 * @note 纯读，无副作用，任意线程可调。
 * @see EncodeInputState —— 逆操作。
 */
InputState DecodeInputState(const uint8_t* wire);

/**
 * @brief 把语义模型编码回 63 字节线格式。
 *
 * 先把整个输出区清零再逐字段写入，因此调用方无需预清零缓冲区；所有
 * 位段写入前按线上宽度掩码，越界的高位被静默截断。
 *
 * @param state 待编码的语义模型。
 * @param out 目标缓冲区，本函数恰好写 kUsbInputStateSize 字节（63），指针
 *            不得为空且容量必须足够。
 * @note 纯写，无副作用，任意线程可调。
 * @see DecodeInputState —— 逆操作。
 */
void EncodeInputState(const InputState& state, uint8_t* out);

}  // namespace core::protocol
