/**
 * @file dump_sink.h
 * @brief 离线回听的出口：把一段编码产出（触觉 + 喇叭两个指针）写成两个音频文件。
 * @details 是 `audio::OutputSink` 的一个**测试侧实现**：喇叭侧封成 Ogg Opus
 *          （`<prefix>.opus`，ffplay/VLC 直接播），触觉侧写成 3kHz 立体声 8-bit
 *          PCM WAV（`<prefix>.haptics.wav`）。生产链路里没有它——daemon 只有一个
 *          出口，就是 bridge；要落盘就拿 `tools/audio_dump.cpp` 这个离线入口跑，
 *          不碰 USB/BT，也不用手柄连上。
 *
 *          @warning 线程约定：`write()` 只由 audio 的编码线程调；`open()`/`close()`
 *          由调用方在 `audio::Start()` 之前、`audio::Stop()` 之后调——open 时编码
 *          线程还不存在，close 时它已经 join，所以两个文件句柄没有并发访问者。
 *          本模块会分配内存和写文件，绝不能出现在 PipeWire RT 回调里。
 */

#pragma once

#include <cstdint>
#include <string>

namespace tools {
namespace dump {

/**
 * @brief 打开两个落盘文件并写头（OpusHead/OpusTags、WAV 44 字节头）。
 * @param path_prefix 输出路径前缀；空串表示不启用，直接返回 false。
 * @return 两个文件都打开并写好头时 true。
 * @details 已打开时先关掉再重开（页序号/granule/字节计数全部归零）。
 */
bool open(const std::string& path_prefix);

/**
 * @brief 追加一段编码产出：触觉进 WAV，喇叭作为一个 Opus 包进 Ogg。
 * @param haptics `core::protocol::kHapticsSize` 字节的触觉包。
 * @param speaker `core::protocol::kSpeakerSize` 字节的喇叭包。
 * @details 与 `audio::OutputSink` 同形，可直接当回调交给 `audio::Start()`。
 *          未 open 时是空操作。写失败（磁盘满、管道断）记一条 WARNING 后自动
 *          close，不影响在途数据。
 */
void write(const uint8_t* haptics, const uint8_t* speaker);

/**
 * @brief 收尾：Ogg 补一页 end-of-stream，WAV 回填 RIFF/data 两个长度字段后关闭。
 * @details 可重复调用。
 */
void close();

}  // namespace dump
}  // namespace tools
