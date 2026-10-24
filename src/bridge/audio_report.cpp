/**
 * @file
 * @brief 0x36 报告体的构造：段计数器、SetStateData 与两段编码数据
 *
 * 完整字节布局（偏移/长度均为字节，未列出的偏移为 0）：
 *
 *          | 偏移      | 长度 | 内容 |
 *          |-----------|------|------|
 *          | 0         | 1    | `0x36` 报告 ID |
 *          | 1         | 1    | `seq << 4`，**由传输层在入队点盖章**，本函数不写 |
 *          | 2         | 1    | `0x91` 未知子报告头，固定值 |
 *          | 3         | 1    | `0x07` 上述子报告的长度 |
 *          | 4         | 1    | `0xFE`/`0xFF` 上述子报告的数据，bit0 = 麦克风开 |
 *          | 5..9      | 5    | `buffer_length` 的五份拷贝，只有 byte[9] 起作用 |
 *          | 10        | 1    | `packet_counter`（音频段计数，非 bridge 的 seq） |
 *          | 11        | 1    | `0x90` SetStateData 子报告头 |
 *          | 12        | 1    | `63` SetStateData 长度 |
 *          | 13..75    | 63   | SetStateData：rumble/LED/volume |
 *          | 76        | 1    | `0x92` 触觉子报告头 |
 *          | 77        | 1    | `64` 触觉长度 |
 *          | 78..141   | 64   | 触觉 int8 包 |
 *          | 142       | 1    | `0x93`（kSpeakerSubReport）喇叭子报告头 |
 *          | 143       | 1    | `200` 喇叭长度 |
 *          | 144..343  | 200  | Opus 喇叭包 |
 *          | 344..397  | 54   | 填充 0，其中 [394..397] 是传输层写 CRC 的位置 |
 *
 *          布局出处见 audio_report.h；表里的偏移不是写死的下标，而是由各段
 *          长度推出来的：每段子报告占 `2 字节块头 + 数据`，下一段从上一段的
 *          数据末尾接着排。
 */

#include "bridge/audio_report.h"

#include <cstring>

namespace bridge {
namespace detail {
namespace {

// 以下偏移常量共享同一条推导链：每段子报告都是 `[头][长度][数据]`，下一段
// 从上一段数据末尾 +2 开始。上游正是靠这个性质换配置的——DS5Dongle 的单包
// 0x36 不嵌 0x90 状态块，触觉块因此从 11 起；双包 0x39（一包两帧）少一份
// buffer_length 拷贝，触觉块从 10 起。本项目只发带状态块的这一种。
constexpr size_t kConfigHeader = 2;  // 0x91 配置子报告的起点
constexpr size_t kBufferLengthFirst = kConfigHeader + 3;  // 跳过 [头][长度][0xFE]
constexpr size_t kBufferLengthCopies = 5;  // 上游写 5 份相同拷贝，仅末份起作用
constexpr size_t kPacketCounter = kBufferLengthFirst + kBufferLengthCopies;
constexpr size_t kStateHeader = kPacketCounter + 1;
constexpr size_t kStateBody = kStateHeader + 2;  // 头 + 长度之后是数据
constexpr size_t kHapticsHeader =
    kStateBody + core::protocol::kSetStateShadowSize;
constexpr size_t kHapticsBody = kHapticsHeader + 2;
constexpr size_t kSpeakerHeader = kHapticsBody + core::protocol::kHapticsSize;
constexpr size_t kSpeakerBody = kSpeakerHeader + 2;

// 段几何必须精确放得下，否则"偏移由长度推导"的前提和 398B 定长同时破产。
static_assert(kSpeakerBody + core::protocol::kSpeakerSize <=
                  core::protocol::kBtAudioReportSize,
              "0x36 的分段布局放不下这份报告体");

/// 配置子报告的数据字节：bit0 是手柄麦克风状态（上游 audio_send_single 的
/// mic_enabled 同一位置），其余 7 位在两个形态里都是 1。
constexpr uint8_t kConfigDataMicOff = 0xFE;
constexpr uint8_t kConfigDataMicOn = 0xFF;

}  // namespace

void BuildAudioReport(uint8_t* out, uint8_t packet_counter,
                      const uint8_t* haptics, const uint8_t* speaker,
                      const uint8_t* state_data, uint8_t buffer_length,
                      bool mic_enabled) {
  // 先全零：布局里不写的偏移（含 [344..397] 填充段）由此保证为 0，
  // 尾部 4 字节留空给传输层写 CRC。
  std::memset(out, 0, core::protocol::kBtAudioReportSize);

  out[0] = core::protocol::kBtAudioReportId;
  // out[1] = seq << 4：由传输层在入队点（bt::PostOutput）盖，理由见头文件。
  out[kConfigHeader] = 0x91;  // 0x11 | 1<<7：配置子报告头，语义未拆解，固定照抄
  out[kConfigHeader + 1] = 0x07;   // 该子报告的数据长度
  // 该子报告唯一的 1 字节数据：只有 bit0 随麦克风开关变化。
  out[kConfigHeader + 2] = mic_enabled ? kConfigDataMicOn : kConfigDataMicOff;
  for (size_t i = 0; i < kBufferLengthCopies; ++i) {
    out[kBufferLengthFirst + i] =
        buffer_length;  // 5 份拷贝全写：手柄只读 byte[9]，照抄上游形态
  }
  out[kPacketCounter] = packet_counter;

  // SetStateData 子报告：rumble/LED/volume，63 字节。
  out[kStateHeader] = 0x90;  // 0x10 | 1<<7
  out[kStateHeader + 1] = core::protocol::kSetStateShadowSize;
  std::memcpy(out + kStateBody, state_data,
              core::protocol::kSetStateShadowSize);

  // 触觉子报告：int8 包。
  out[kHapticsHeader] = 0x92;  // 0x12 | 1<<7
  out[kHapticsHeader + 1] = core::protocol::kHapticsSize;
  std::memcpy(out + kHapticsBody, haptics, core::protocol::kHapticsSize);

  // 喇叭子报告：Opus 包。
  out[kSpeakerHeader] = kSpeakerSubReport;
  out[kSpeakerHeader + 1] = core::protocol::kSpeakerSize;
  std::memcpy(out + kSpeakerBody, speaker, core::protocol::kSpeakerSize);
}

}  // namespace detail
}  // namespace bridge
