/**
 * @file audio_decoder.h
 * @brief 手柄上行麦克风帧：71 字节单声道 Opus 包 → 48kHz PCM 帧
 *
 * 单实例模块，与下行编码器正好反向：bridge 在 bt 的读线程上把包体推进来
 * （PushPacket），解码线程按到达顺序逐包 opus_decode_float，再把 480 帧
 * PCM 交给 Init() 装好的出口（生产实现是 source::PushSamples，写进虚拟
 * 麦克风的 PCM 环）。
 *
 * 队列有界（kQueueMax 包 ≈ 80ms）：麦克风是实时链路，积压比丢包更糟，满了
 * 丢最旧的一包。解码不做重采样——48kHz、480 帧、10ms 与来包节奏一致；时钟
 * 漂移由 source 的 PCM 环兜底（满则覆盖、空则静音）。
 *
 * @note 不碰报告、不碰 PipeWire：包在 0x31 报告里的偏移与长度契约分别在
 *       bridge/mic_report.h 与 core/protocol/audio_format.h。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace audio {
namespace decoder {

/**
 * @brief 一包解码产出的去处。
 *
 * @param samples 单声道 float PCM，长度 frames；指针只在调用期间有效。
 * @param frames  解出的帧数，正常恒为 core::protocol::kOpusFrameSamples
 *                （480，即 10ms）。
 * @details 由解码线程调用：实现必须非阻塞。生产实现 source::PushSamples 只
 *          往 PCM 环里拷一份就返回，真正的 PipeWire 交付在 RT 回调里。
 */
using PcmSink = std::function<void(const float* samples, uint32_t frames)>;

/**
 * @brief 建 Opus 单声道解码器并拉起解码线程。
 *
 * @param output 每包解码产出的去处；空 std::function 会被拒绝（链跑起来
 *               没人收 PCM，比不起更难查）。
 * @return 成功 true；失败 false 且不留已起的线程。可重复调用，先 Stop 再
 *         Init。
 * @note 起线程前装好出口：线程一起来就可能交付。
 * @see Stop() —— 逆操作。
 */
bool Init(PcmSink output);

/**
 * @brief 推进一帧待解码的 Opus 包。
 *
 * @param packet core::protocol::kMicOpusSize 字节的单声道 Opus 包体。
 * @param len    包体长度；短于 kMicOpusSize 的整帧丢弃。
 * @details 由 bridge 在 bt 的读线程上调用：一次锁 + 一次 71 字节拷贝就返回，
 *          不阻塞、不打日志。未 Init / 已 Stop 时静默丢弃；队列满时覆盖最旧
 *          的一包（丢旧保新）。
 */
void PushPacket(const uint8_t* packet, size_t len);

/**
 * @brief 停解码线程并释放 Opus 解码器；可重复调用，未起来时调用无害。
 *
 * @note join 之后才清出口与队列：线程最后一次交付仍要用它。
 */
void Stop();

}  // namespace decoder
}  // namespace audio
