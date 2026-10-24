/**
 * @file audio_format.h
 * @brief 音频链路两端的段尺寸契约：下行 0x36 的音频段与上行麦克风帧
 *
 * 下行：audio 模块产出一段 264 字节的编码数据（64 字节 int8 触觉 + 200 字节
 * Opus），bridge 把它原样嵌进 0x36 报告的固定偏移。上行：手柄每 10ms 发一帧
 * 71 字节单声道 Opus，bridge 按固定偏移切开交给 audio 解码。两条链路都是
 * 生产者/消费者关系，尺寸契约放在双方共用的本文件里，避免各写一遍后悄悄
 * 漂移。
 *
 * @note 本文件只定义段尺寸；0x36 报告的整体布局与子报告头语义在
 *       bridge/audio_report 组帧处，上行帧在 0x31 报告里的偏移在
 *       bridge/mic_report.h，report ID 在 report.h。
 */

#pragma once

#include <cstddef>

#include "core/protocol/report.h"

namespace core::protocol {

/// 触觉段字节数：32 帧 × 2 声道 × int8，即 audio_encoder 把 512 帧触觉
/// 输入线性重采样到 32 帧后的定长输出。
inline constexpr size_t kHapticsSize = 64;
/// 扬声器段字节数：Opus CBR 160000bps 定长包，恰好 10ms 的量化值。
inline constexpr size_t kSpeakerSize = 200;
/// audio 模块交给 bridge 的完整一段：触觉在前、Opus 在后。
inline constexpr size_t kAudioSegmentSize = kHapticsSize + kSpeakerSize;

// ---- Opus 帧几何（上下行共用）----
// 喇叭编码与麦克风解码是同一个 48kHz / 10ms 的 Opus 帧几何，两路各写一份
// 迟早会漂移，所以只在这里定义一次。

/// Opus 采样率；也是 PipeWire 两条流的名义采样率。
inline constexpr uint32_t kOpusSampleRate = 48000;
/// 一包 Opus 的帧数：48kHz 下恰好 10ms；喇叭每 10ms 一段，麦克风每 10ms
/// 上行一帧。
inline constexpr uint32_t kOpusFrameSamples = 480;
/// 触觉路的目标采样率：手柄内部的 HD 振动采样率，不是可调参数。
inline constexpr uint32_t kHapticsSampleRate = 3000;

// ---- 上行（手柄麦克风）----
// 手柄每 10ms 发一帧定长 Opus：单声道、kOpusFrameSamples 帧、71 字节。七十
// 一不是编码器参数的回推结果，而是上游固件实测的线上帧长（DS5Dongle 的
// MIC_OPUS_SIZE），bridge 按同样长度把包体转给 audio，解码端也按整帧喂给
// Opus——三处必须是同一个数，所以放在共享头里。

/// 手柄上行单帧 Opus 包的字节数。
inline constexpr size_t kMicOpusSize = 71;
/// 上行 Opus 的声道数：手柄内置麦克风是单声道。
inline constexpr uint32_t kMicChannels = 1;

// 编译期锁死三处跨模块契约：段尺寸改动会破坏 bridge 的偏移表，
// 报告体尺寸改动会破坏 CRC 尾部下标与 MTU 校验，帧长改动会同时破坏
// bridge 的帧偏移与解码端的定长假设。
static_assert(kAudioSegmentSize == 264,
              "0x36 audio segment must stay 264 bytes");
static_assert(kBtAudioReportSize == 398,
              "0x36 report body must stay 398 bytes");
static_assert(kOpusSampleRate / 100 == kOpusFrameSamples,
              "one Opus packet must stay 10ms");

}  // namespace core::protocol
