/**
 * @file audio_sink.h
 * @brief PipeWire 4 通道 f32 sink 与断线恢复控制
 *
 * 单实例模块。RT process 回调把交织 float 样本交给
 * audio::encoder::ProcessFrame()；state_changed 只记录断线事件，由内部
 * monitor 线程负责退避重连。stream 节点由 PipeWire graph 被动激活，本
 * 模块不调用 pw_stream_set_active()。
 *
 * @note 线程构成：一条 PipeWire main loop 线程 + 一条恢复 monitor 线程，
 *       加上 PipeWire 自己的 RT data-loop 线程执行 process 回调。
 */

#pragma once

#include <functional>
#include <string>

namespace audio {
namespace sink {

/**
 * @brief 创建 PipeWire sink 流、后台 main loop 线程和恢复 monitor 线程。
 *
 * @param station_id 本站标识：12 位小写 MAC 十六进制，由 audio::Start 从手柄
 *                  MAC 得出。节点名与昵称/描述各取它一次（形状分别由
 *                  audio::identity 的 MakeSinkNodeName 与 MakeDeviceId 决定），
 *                  用于在同一台机器上区分多个 dongle。
 * @param host_sysfs_path 本站在主机侧的 USB 接口目录，相对 /sys，形如
 *                  `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`；交给
 *                  audio::identity 写成节点的 device.sysfs.path 与从同一段
 *                  路径推出的 alsa.card；空串则两个属性都不出现。重连每次都会
 *                  带上同一个值。
 * @param on_lost   连续重连失败后的通知。回调在恢复 monitor 线程上执行，
 *                  只能设置标志或投递事件，不得直接调用 audio::Stop()
 *                  ——Stop 要 join 的正是这条线程。可为空。
 * @return 成功 true；失败时已回收本次创建的资源，可安全再次 Start。
 * @note 本函数不调 pw_init/pw_deinit：PipeWire 的进程级初始化和 source
 *       共用，由 audio::Start 统一负责。
 */
bool Start(const std::string& station_id, const std::string& host_sysfs_path,
           std::function<void()> on_lost = {});

/**
 * @brief 停止恢复 monitor、PipeWire main loop 和 stream。
 *
 * @details 未 Start 或已 Stop 时安全，可重复调用。
 */
void Stop();

}  // namespace sink
}  // namespace audio
