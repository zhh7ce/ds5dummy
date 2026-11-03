/**
 * @file audio_source.h
 * @brief PipeWire 单声道 f32 虚拟麦克风（Audio/Source）与"主机开始录音"通知
 *
 * 单实例模块。RT process 回调从 PCM 环取样本填进输出缓冲；state_changed
 * 把 STREAMING ↔ PAUSED 的迁移翻成一次 on_mic_open(true/false)，并把
 * UNCONNECTED/ERROR 交给 audio::port 的 monitor 退避重建——与下行 sink
 * 同一套恢复策略。重建前 PCM 环会被清空，旧会话的样本不会跨到新流上。
 *
 * @note 线程构成：一条 PipeWire main loop 线程 + 一条恢复 monitor 线程，
 *       加上 PipeWire 自己的 RT data-loop 线程执行 process 回调；前两条由
 *       audio::port 建与拆。PCM 环由解码线程写、RT 回调读。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "audio/audio.h"

namespace audio {
namespace source {

/**
 * @brief 建虚拟麦克风流、拉起事件线程与恢复 monitor。
 *
 * @param station_id 本站标识：12 位小写 MAC 十六进制，与同名 sink 同一个值。
 *                   节点名由 audio::identity 的 MakeSourceNodeName 拼，昵称与
 *                   描述直接把它接在显示名后面，用于在同一台机器上区分多个
 *                   dongle。
 * @param host_sysfs_path 本站在主机侧的 USB 接口目录，相对 /sys，形如
 *                   `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`；交给
 *                   audio::identity 写成节点的 device.sysfs.path 与从同一段
 *                   路径推出的 alsa.card；空串则两个属性都不出现。与同名 sink
 *                   传同一个值——主机上这是一台设备的两条流，硬件身份必须一致。
 *                   重连每次都会带上同一个值。
 * @param on_mic_open 主机开始/停止录音的通知；空 std::function 会被拒绝
 *                    ——没人能把"该开麦了"告诉手柄，建这个 Source 没有意义。
 *                    回调在 PipeWire main loop 线程上执行，必须非阻塞。
 * @param on_lost   连续重连失败后的通知，与 sink 收同一个 handler（两条流
 *                  任一彻底恢复失败都意味着整条音频链该交给 worker 重建）。
 *                  回调在恢复 monitor 线程上执行，只能设置标志或投递事件，
 *                  不得直接调用 audio::Stop()——Stop 要 join 的正是这条线程。
 *                  可为空。
 * @return 成功 true；失败 false 且已回收本次创建的资源，可安全再次 Start。
 * @note 本函数不调 pw_init/pw_deinit：PipeWire 的进程级初始化和 sink 共用，
 *       由 audio::Start 统一负责。
 * @note 流句柄、main loop 线程与恢复状态机都放在 audio::port 里；本函数只
 *       负责 source 的节点身份、格式、process 回调与重连前的 PCM 环清理。
 * @see Stop() —— 逆操作。
 */
bool Start(const std::string& station_id, const std::string& host_sysfs_path,
           MicOpenHandler on_mic_open, std::function<void()> on_lost = {});

/**
 * @brief 停恢复 monitor、事件线程并拆掉整条 Source pipeline。
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
 *          优先：宁丢旧样本也要让主机尽快听到当前声音）。未 Start / 已 Stop /
 *          重建期间时静默丢弃。
 */
void PushSamples(const float* samples, uint32_t frames);

}  // namespace source
}  // namespace audio
