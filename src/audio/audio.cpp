/**
 * @file audio.cpp
 * @brief audio 链的装配点：下行 encoder+sink、上行 decoder+source 的建与拆
 *
 * 下行：sink 的 RT 样本交给 encoder::ProcessFrame()，编码线程攒够一段
 * `[触觉][喇叭]` 后调用 OutputSink。上行：bridge 推来的 Opus 包进 decoder
 * 的队列，解码线程把 PCM 交给 source 的环，虚拟麦克风的 RT 回调再把它交给
 * PipeWire。0x36 报告组包与 BT 发送、麦克风开关报告都由 bridge/bt 负责，
 * 本文件不认识报告。
 *
 * PipeWire 的 pw_init/pw_deinit 是进程级的（两处都不引用计数），sink 与
 * source 共用同一套全局状态，所以只在这里成对调用——子模块各自 init/deinit
 * 会在拆任意一条流时把另一条一起拆死。
 */

#include "audio/audio.h"

#include <pipewire/pipewire.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "audio/audio_decoder.h"
#include "audio/audio_encoder.h"
#include "audio/audio_sink.h"
#include "audio/audio_source.h"
#include "core/debug.h"
#include "loguru.hpp"

namespace audio {
namespace {

/// pw_init/pw_deinit 是否成对；进程级一次，sink 与 source 共享。
bool pipewire_initialized = false;

/// 进程级初始化，幂等。
void EnsurePipeWire() {
  if (pipewire_initialized) return;
  pw_init(nullptr, nullptr);
  pipewire_initialized = true;
}

/// 与 EnsurePipeWire() 配对；没初始化过时调用无害。
void ShutdownPipeWire() {
  if (!pipewire_initialized) return;
  pw_deinit();
  pipewire_initialized = false;
}

}  // namespace

bool Start(const std::string& mac, float haptics_gain, OutputSink output,
           const std::string& host_sysfs_path, LostHandler on_lost,
           MicOpenHandler on_mic_open) {
  // 空 sink 在这里拒掉：链路跑起来却没人收数据，比不起更难查。
  if (!output) {
    DS5DUMMY_LOG(ERROR,
          "[audio] No output sink — the encoded data would have nowhere to go");
    return false;
  }

  // 本站标识取手柄 MAC（由 worker 传入，不读任何全局），让同一台机器上两只
  // dongle 的节点名与显示名都可分辨；解析失败退化成全零标识。
  unsigned v[6] = {0, 0, 0, 0, 0, 0};
  if (std::sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1],
                  &v[2], &v[3], &v[4], &v[5]) != 6) {
    memset(v, 0, sizeof(v));
  }
  // 12 位小写十六进制 + 结尾 NUL；节点名与显示名各自在 audio::identity 里
  // 按自己的形状拼这一段。
  char station_id[13] = {0};
  snprintf(station_id, sizeof(station_id), "%02x%02x%02x%02x%02x%02x", v[0],
           v[1], v[2], v[3], v[4], v[5]);

  EnsurePipeWire();

  // 顺序是承重的：sink 的 RT 线程一起来就会直接调 encoder::ProcessFrame，
  // 编码器必须先建好——出口也随线程一起装好，没有"先跑几拍再补接线"的窗口。
  if (!encoder::Init(std::move(output), haptics_gain)) {
    DS5DUMMY_LOG(ERROR, "[audio] Encoder init failed — HID only");
    ShutdownPipeWire();
    return false;
  }

  // sink 内部负责断线退避重连；连续恢复失败后经 on_lost 通知 worker 降级
  // HID-only。失败路径把已建好的编码器一起收回，不留半开的链。
  if (!sink::Start(station_id, host_sysfs_path, std::move(on_lost))) {
    DS5DUMMY_LOG(ERROR, "[audio] Sink start failed — HID only");
    sink::Stop();
    encoder::Stop();
    ShutdownPipeWire();
    return false;
  }

  // 上行是可降级的一路：没给 on_mic_open 就不建 Source（没人能把手柄麦克风
  // 打开，节点存在也没用）；建失败只丢麦克风，不拖累已经就绪的下行。
  bool mic_up = false;
  if (on_mic_open) {
    if (!decoder::Init(source::PushSamples)) {
      DS5DUMMY_LOG(WARNING,
            "[audio] Mic decoder unavailable — the uplink stays off");
    } else if (!source::Start(station_id, host_sysfs_path,
                              std::move(on_mic_open))) {
      DS5DUMMY_LOG(WARNING,
            "[audio] Microphone source unavailable — the uplink stays off");
      decoder::Stop();
    } else {
      mic_up = true;
    }
  }

  DS5DUMMY_LOG(INFO, "[audio] Downlink up: station={} encoder ready",
               station_id);
  if (mic_up) {
    DS5DUMMY_LOG(INFO, "[audio] Uplink up: station={} decoder ready",
                 station_id);
  }
  return true;
}

void Stop() {
  // 先停上行（Source → 解码线程），再停下行（sink → encoder）。Source 的
  // Stop 会在真的录音过时补一次 on_mic_open(false)——audio::Stop 早于
  // bt::StopThreads()，那份关麦报告还发得出去。两边都可重复调用。
  source::Stop();
  decoder::Stop();
  sink::Stop();
  encoder::Stop();
  ShutdownPipeWire();
}

void OnMicPacket(const uint8_t* packet, size_t len) {
  // 纯转发：队列、丢弃与解码都在 decoder 里，这一步只发生在 bt 读线程上。
  decoder::PushPacket(packet, len);
}

}  // namespace audio
