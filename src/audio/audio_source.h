/**
 * @file audio_source.h
 * @brief PipeWire 单声道 f32 虚拟麦克风（Audio/Source）与"主机开始录音"通知
 *
 * 单实例模块。RT process 回调从 PCM 环取样本填进输出缓冲；state_changed
 * 只做一件事：STREAMING ↔ PAUSED 的迁移翻成一次 on_mic_open(true/false)。
 * 断线不重连：Source 只是上行的端点，PipeWire 重启后麦克风静默到 worker
 * 下一轮重启为止；下行 sink 的恢复 monitor 与本模块无关。
 *
 * @note 线程构成：一条 PipeWire main loop 线程，加上 PipeWire 自己的 RT
 *       data-loop 线程执行 process 回调。PCM 环由解码线程写、RT 回调读。
 */

#pragma once

#include <cstdint>
#include <string>

#include "audio/audio.h"

namespace audio {
namespace source {

/**
 * @brief 建虚拟麦克风流并拉起事件线程。
 *
 * @param station_id 本站标识：12 位小写 MAC 十六进制，与同名 sink 同一个值。
 *                   节点名与昵称/描述各取它一次（形状分别由 audio::identity
 *                   的 MakeSourceNodeName 与 MakeDeviceId 决定），用于在同一台
 *                   机器上区分多个 dongle。
 * @param host_sysfs_path 本站在主机侧的 USB 接口目录，相对 /sys，形如
 *                   `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`；交给
 *                   audio::identity 写成节点的 device.sysfs.path 与从同一段
 *                   路径推出的 alsa.card；空串则两个属性都不出现。与同名 sink
 *                   传同一个值——主机上这是一台设备的两条流，硬件身份必须一致。
 * @param on_mic_open 主机开始/停止录音的通知；空 std::function 会被拒绝
 *                    ——没人能把"该开麦了"告诉手柄，建这个 Source 没有意义。
 *                    回调在 PipeWire main loop 线程上执行，必须非阻塞。
 * @return 成功 true；失败 false 且已回收本次创建的资源，可安全再次 Start。
 * @note 本函数不调 pw_init/pw_deinit：PipeWire 的进程级初始化和 sink 共用，
 *       由 audio::Start 统一负责。
 * @see Stop() —— 逆操作。
 */
bool Start(const std::string& station_id, const std::string& host_sysfs_path,
           MicOpenHandler on_mic_open);

/**
 * @brief 停事件线程并拆掉整条 Source pipeline。
 *
 * 若拆之前主机正在录音（流处于 STREAMING），会在本线程上补一次
 * on_mic_open(false)：手柄麦克风必须关掉，而这件事只能在 bt 还活着的时候
 * 做——audio::Stop 早于 bt::StopThreads()。
 *
 * @details 未 Start 或已 Stop 时安全，可重复调用。
 */
void Stop();

/**
 * @brief 把解码出的 PCM 写进播放环。
 *
 * @param samples 单声道 float 样本，长度 frames。
 * @param frames  帧数，0 会被忽略。
 * @details 由解码线程调用：一次锁 + 一次插入；环满时覆盖最旧样本（延迟
 *          优先：宁丢旧样本也要让主机尽快听到当前声音）。未 Start / 已 Stop
 *          时静默丢弃。
 */
void PushSamples(const float* samples, uint32_t frames);

}  // namespace source
}  // namespace audio
