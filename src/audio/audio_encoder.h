/**
 * @file audio_encoder.h
 * @brief 4 通道交织 float → Opus 喇叭包 + int8 触觉包
 *
 * 单实例模块。PipeWire RT 线程经 ProcessFrame() 写入输入环，编码线程按
 * 512 输入帧为一段取出并拆声道：ch0/1（喇叭）重采样到 480 帧送 Opus，
 * ch2/3（触觉）重采样到 32 帧量化为 int8。每段输出固定为
 * `[kHapticsSize 触觉][kSpeakerSize 喇叭]`，由编码线程直接调用
 * SegmentSink 交付。
 *
 * @note 全部几何（帧数、比率、段长）是编译期常量，与
 *       core/protocol/audio_format.h 里的 kOpusSampleRate /
 *       kHapticsSampleRate 绑死；改采样率必须同步重推 audio_encoder.cpp
 *       里的 static_assert。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "core/protocol/audio_format.h"

namespace audio {
namespace encoder {

/**
 * @brief 一段编码产出的交付出口；两个指针指向同一段缓冲的相邻区段，
 *        只在回调期间有效。由编码线程调用，契约见 audio::OutputSink。
 */
using SegmentSink =
    std::function<void(const uint8_t* haptics, const uint8_t* speaker)>;

constexpr uint32_t kInputChannels = 4;     ///< PipeWire 交织通道数：ch0/1 喇叭，ch2/3 触觉
constexpr uint32_t kEncodedChannels = 2;   ///< Opus 与两路 SRC 实际使用的立体声通道数
// 采样率与帧几何（kOpusSampleRate / kOpusFrameSamples / kHapticsSampleRate）
// 是上下行共用的线格式契约，定义在 core/protocol/audio_format.h。

/**
 * @brief 一段编码产出的总长度：触觉 64B + 喇叭 200B，即 0x36 里两段数据
 *        块的字节数之和。
 */
constexpr size_t kSegmentSize =
    core::protocol::kHapticsSize + core::protocol::kSpeakerSize;

/**
 * @brief 创建 Opus/SRC 状态并启动编码线程。
 *
 * @param output 每段编码产出的交付出口；空函数会被拒绝。起线程前装好，
 *               线程一起来就能交付。
 * @param haptics_gain 触觉振幅倍率，乘在 int8 量化之前；1.0 = 原样，
 *               0 = 静音。范围由调用方保证（配置层收在 [0, 2]），本函数
 *               不再夹取。
 * @return 成功 true；任一环节失败 false，失败时已释放本次创建的编解码对象
 *         （出口 lambda 由随后的 Stop() 统一清）。
 * @warning 单实例：已有编码线程在跑时再次调用不构成双实例——状态是全局
 *          一份，先 Stop 再 Init。
 * @see Stop() —— 逆操作。
 */
bool Init(SegmentSink output, float haptics_gain);

/**
 * @brief 向输入环追加一段 4 通道交织样本。
 *
 * @param input  4 通道交织 float 首地址，长度恰为 frames × kInputChannels。
 * @param frames 本批输入帧数，0 会被忽略。
 * @details 由 PipeWire RT 线程调用：一次锁操作 + 一次插入，环满覆盖最旧
 *          数据；不打印日志、不分配内存。
 */
void ProcessFrame(const float* input, uint32_t frames);

/**
 * @brief 停止编码线程并释放 Opus/SRC；可重复调用，未起来时调用无害。
 *
 * @note join 之后才清出口 lambda：线程最后一次交付仍要用它。
 */
void Stop();

/**
 * @brief 清空输入环，用于 sink 断线重连后丢弃残留样本。
 *
 * 残留样本跨会话播出只会播放旧音频并错开新流的时序，宁可丢。
 */
void ResetInputBuffer();

}  // namespace encoder
}  // namespace audio
