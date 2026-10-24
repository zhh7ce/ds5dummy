/**
 * @file
 * @brief bridge 内部：0x36 音频输出报告的字节布局（触觉段 + 喇叭段 + SetStateData）
 *
 * 手柄要的是整份 398 字节输出报告，内部按"子报告头 + 长度 + 数据块"分成
 * 几块：63 字节 SetStateData 状态、64 字节触觉包、200 字节喇叭包，每块前
 * 面是 `id | 1<<7` 的头和 1 字节长度。
 *
 * 布局对齐上游 DS5Client/DS5Dongle 实现的实测包形（出处为上游代码与实机
 * 行为，非本地文档推断），本项目只保留手柄侧的构造。
 *
 * @warning 子报告的两段长度（core::protocol::kHapticsSize /
 *          core::protocol::kSpeakerSize）与 SetStateData 长度归 core：产出
 *          方 audio 和打包方 bridge 都要引用。本头文件的常量只描述 0x36
 *          这一份报告自己的布局。
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/protocol/audio_format.h"
#include "core/protocol/report.h"

namespace bridge {
namespace detail {

/**
 * @brief 音频缓冲长度字段值，写进报告的 byte[5..9]。
 * @details 上游写 5 份相同的拷贝（SAMPLE_SIZE 同为 64），实测只有 byte[9]
 *          真正 gating 手柄侧的音频缓冲，其余 4 字节改动无效果。
 */
constexpr uint8_t kAudioBufferLength = 64;

/**
 * @brief 喇叭路由子报告 ID：`0x13 | 1<<7`。
 * @details 上游按 jack sensor 在 0x93（手柄扬声器）与 0x96（耳机）之间二
 *          选一；本模块不跟踪插孔状态，恒发手柄那一份。
 */
constexpr uint8_t kSpeakerSubReport = 0x93;

/**
 * @brief 把触觉段、喇叭段与 SetStateData 拼成整份 398 字节 0x36 报告体。
 *
 * 纯填充函数：不发送、不动全局状态；偏移全部由各段长度推出来（见本文件
 * 实现处的推导链与偏移表），static_assert 保证布局放得下。
 *
 * @param out            目标缓冲区，长度必须为 core::protocol::kBtAudioReportSize
 *                       （398），先整体清零，布局未列出的偏移保持 0。
 * @param packet_counter 音频段计数器，落 byte[10]；递增与丢包策略归调用方。
 * @param haptics        core::protocol::kHapticsSize（64）字节触觉包。
 * @param speaker        core::protocol::kSpeakerSize（200）字节 Opus 喇叭包。
 * @param state_data     63 字节 SetStateData 线格式状态（bridge::detail 的产物）。
 * @param buffer_length  写进 byte[5..9] 五份拷贝的音频缓冲长度，默认
 *                       kAudioBufferLength；参数化是为单测可覆盖该字段。
 * @param mic_enabled    主机是否在录音：置 byte[4] 的 bit0（0xFF），否则保持
 *                       0xFE。上游用这一位向手柄报告麦克风状态，它不是开关
 *                       本身——开关在 OnMicOpen 的 0x32 状态报告里。
 * @warning 两处刻意留白：out[1] 的 4-bit report seq 与尾部 4 字节 CRC 都
 *          由传输层在入队点（bt::PostOutput）盖——这里填任何值都会和传输层
 *          的赋值打架。
 * @note 三个数据指针都只读、按段长取用；触觉与喇叭在编码器里本来就不相邻，
 *       这里不再为拼布局多拷一次。
 */
void BuildAudioReport(uint8_t* out, uint8_t packet_counter,
                      const uint8_t* haptics, const uint8_t* speaker,
                      const uint8_t* state_data,
                      uint8_t buffer_length = kAudioBufferLength,
                      bool mic_enabled = false);

}  // namespace detail
}  // namespace bridge
