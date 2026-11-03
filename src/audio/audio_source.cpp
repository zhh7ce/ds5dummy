/**
 * @file audio_source.cpp
 * @brief audio::source 实现：PipeWire 单声道 f32 虚拟麦克风与录音状态通知
 *
 * 建流、main loop 线程与断线退避重连的骨架在 audio::port，与下行 sink 同一
 * 套恢复策略；本文件只保留 source 特有的部分：PCM 环、RT 回调的欠载补静音、
 * 以及把 STREAMING/PAUSED 的迁移翻成 on_mic_open 通知。
 *
 * 线程构成：PipeWire main loop 线程驱动事件，process 回调由 PipeWire 的 RT
 * data-loop 线程执行；port 另起一条 monitor 线程，在断线后按退避重建。PCM
 * 环由解码线程写、RT 回调读。每次重建前 monitor 先清空 PCM 环，旧会话的
 * 样本不会跨到新流上。
 */

#include "audio/audio_source.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <boost/circular_buffer.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "audio/audio_identity.h"
#include "audio/audio_port.h"
#include "core/debug.h"
#include "core/protocol/audio_format.h"
#include "loguru.hpp"

namespace audio {
namespace source {
namespace {

/**
 * @brief PCM 环容量：2048 帧 @48kHz ≈ 43ms。
 *
 * 解码线程写、RT 回调读。满了覆盖最旧样本——上行实时链路里过期样本没有
 * 价值；RT 路径不打印日志、不分配内存。
 */
constexpr size_t kRingFrames = 2048;

// 节点显示名（昵称/描述）的前缀，后接本站标识（12 位 MAC 十六进制）。描述里带
// Microphone，与同名 sink 在扁平列表里仍可分辨。节点名那条走
// MakeSourceNodeName，与 sink 同一族形状。
constexpr char kNodeDescriptionBase[] =
    "DualSense Wireless Controller (PS5) Microphone ";
constexpr char kNodeNickBase[] = "DualSense Wireless Controller ";

// 与同名 sink 同一条长度门禁：Wine 只原样搬 62 字节以内的描述（它的
// MAX_DEVICE_NAME_LEN），超了就用 device.product.name 顶掉；尾巴是 12 位 MAC
// 十六进制。采集侧多出的 "Microphone" 正好吃掉了 sink 那侧剩下的全部余量。
static_assert(std::string_view(kNodeDescriptionBase).size() + 12 <= 62);

/**
 * @brief 进程内单实例状态。
 *
 * PipeWire 对象与恢复状态机归 audio::port；ring 由解码线程与 RT 回调共享，
 * 受 ring_mutex 保护。open 只在 PipeWire main loop 线程上翻转，Stop() 在
 * join 之后读它补 close 通知，因此不需要锁。
 */
struct State {
  port::Pipeline pipeline;
  port::Monitor monitor;

  boost::circular_buffer<float> ring;  ///< 待交付的单声道 PCM，受 ring_mutex 保护
  std::mutex ring_mutex;               ///< 护 ring

  bool open = false;       ///< 主机是否正在录音（只在 main loop 线程翻转）
  MicOpenHandler on_open;  ///< Start 装填，Stop join 之后清空
};

State st;

/**
 * @brief 翻转"主机在录音"状态并通知调用方一次。
 *
 * @param self    单实例状态。
 * @param open    目标状态。
 * @details 只在流处于服务状态时生效（重建期间闸门是落的，不把拆流抖动报成
 *          "停止录音"）；同值重复到达直接丢弃——PipeWire 的状态回调可以反复
 *          报告同一个终态，链路发送不能跟着重复。回调跑在 PipeWire main
 *          loop 线程上，实现方必须非阻塞。
 */
void SetOpen(State* self, bool open) {
  if (!self->pipeline.running.load() || self->open == open) return;
  self->open = open;
  if (self->on_open) self->on_open(open);
}

/**
 * @brief stream 状态迁移回调：把 STREAMING/PAUSED 翻成录音开关，终态报断线。
 *
 * STREAMING = 有客户端在录，PAUSED = 没有客户端；UNCONNECTED/ERROR 说明流
 * 已经没了，同样按"没人在录"处理，同时交给 port 的 monitor 退避重建。
 * CONNECTING 等中间态忽略。
 *
 * @param userdata 注册时传入的 &st。
 * @param old      迁移前的状态，仅用于日志。
 * @param state    迁移后的状态。
 * @param error    ERROR 状态时的错误描述，可为 null。
 * @note 跑在 PipeWire main loop 线程上：允许 LOG，禁止阻塞。
 */
void OnStreamStateChanged(void* userdata, enum pw_stream_state old,
                          enum pw_stream_state state, const char* error) {
  State* self = static_cast<State*>(userdata);
  if (!self) return;

  DS5DUMMY_LOG(INFO, "[source] Stream state changed: {} -> {}",
               pw_stream_state_as_string(old),
               pw_stream_state_as_string(state));

  if (state == PW_STREAM_STATE_STREAMING) {
    SetOpen(self, true);
  } else if (state == PW_STREAM_STATE_PAUSED ||
             state == PW_STREAM_STATE_UNCONNECTED ||
             state == PW_STREAM_STATE_ERROR) {
    SetOpen(self, false);
  }

  if (state != PW_STREAM_STATE_UNCONNECTED &&
      state != PW_STREAM_STATE_ERROR) {
    return;
  }
  // 停机流程里的状态抖动与重复上报由 port 过滤；只有被受理的那一条才记 ERROR。
  if (port::ReportDisconnected(self->monitor, self->pipeline)) {
    DS5DUMMY_LOG(ERROR, "[source] Stream disconnected or error: {}",
                 error ? error : "unknown");
  }
}

/**
 * @brief RT process 回调：从 PCM 环取样本填输出缓冲，欠载补静音。
 *
 * 跑在 PipeWire RT 线程上：不分配、不打日志，且无论走哪条路都必须把
 * buffer 归还（pw_stream_queue_buffer），否则缓冲池很快枯竭、流卡死。
 *
 * @param userdata 注册时传入的 &st。
 */
void OnStreamProcess(void* userdata) {
  State* self = static_cast<State*>(userdata);
  if (!self) return;

  pw_buffer* buffer = port::DequeueBuffer(self->pipeline);
  if (!buffer) return;

  spa_buffer* spa_buf = buffer->buffer;

  // 单 plane 交织布局：每帧 = 1 个 float × 声道数（单声道即 1 个 float）。
  constexpr size_t kBytesPerFrame =
      sizeof(float) * core::protocol::kMicChannels;
  const size_t max_frames = spa_buf->datas[0].maxsize / kBytesPerFrame;
  // requested 是服务端建议的帧数（重采样需要多少）；没有建议值时整块交付。
  size_t frames = buffer->requested != 0 ? static_cast<size_t>(buffer->requested)
                                         : max_frames;
  frames = std::min(frames, max_frames);

  float* data = static_cast<float*>(spa_buf->datas[0].data);
  size_t copied = 0;
  {
    std::lock_guard<std::mutex> lock(self->ring_mutex);
    copied = std::min(frames, self->ring.size());
    std::copy_n(self->ring.begin(), copied, data);
    self->ring.erase_begin(copied);
  }
  // 欠载补静音：主机拿到的是连续时间轴，不会因为解码慢一拍听到拖尾。
  std::fill(data + copied, data + frames, 0.0f);

  // 输出流的块头由生产者填：偏移 0、步长一帧、长度实际产出的字节数。
  spa_chunk* chunk = spa_buf->datas[0].chunk;
  chunk->offset = 0;
  chunk->stride = static_cast<int32_t>(kBytesPerFrame);
  chunk->size = static_cast<uint32_t>(frames * kBytesPerFrame);
  buffer->size = frames;  // 音频流按帧报告已排入量，服务端据此估算耗时
  pw_stream_queue_buffer(self->pipeline.stream, buffer);
}

}  // namespace

bool Start(const std::string& station_id, const std::string& host_sysfs_path,
           MicOpenHandler on_mic_open, std::function<void()> on_lost) {
  // 空回调在这里拒掉：Source 建出来也永远没人能告诉手柄"该开麦了"，等于
  // 一个录不到声音的节点，比不起更难查。
  if (!on_mic_open) {
    DS5DUMMY_LOG(ERROR,
                 "[source] No mic-open handler — the controller microphone "
                 "could never be enabled");
    return false;
  }
  // 双 Start 在这里拒掉、而不是交给 port：下面的状态复位会覆盖在跑实例的
  // on_open 与 PCM 环，必须在动它们之前退出。
  if (st.pipeline.running.load() || st.monitor.thread.joinable()) {
    DS5DUMMY_LOG(ERROR, "[source] source already running");
    return false;
  }

  // 状态复位 + 节点身份拼装：三个串都从同一个 station_id 出发，形状全在
  // audio::identity，与 sink 侧同一族（这是一台声卡的播放与录音两半边）。
  st.open = false;
  st.on_open = std::move(on_mic_open);
  {
    std::lock_guard<std::mutex> lock(st.ring_mutex);
    st.ring.set_capacity(kRingFrames);
    st.ring.clear();
  }
  port::StreamSpec spec;
  spec.tag = "source";
  spec.media_class = "Audio/Source";
  spec.media_role = "Communication";
  spec.icon_name = identity::kSourceIconName;
  spec.direction = PW_DIRECTION_OUTPUT;  // 本端产样本：录音 source
  spec.sample_rate = core::protocol::kOpusSampleRate;
  spec.positions = {SPA_AUDIO_CHANNEL_MONO};
  spec.node_name = identity::MakeSourceNodeName(station_id);
  spec.node_nick = std::string(kNodeNickBase) + station_id;
  spec.node_description = std::string(kNodeDescriptionBase) + station_id;
  // 接口目录与节点三元组一样进 spec：重连每次重建 stream，属性必须每次带上。
  spec.host_sysfs_path = host_sysfs_path;
  spec.state_changed = OnStreamStateChanged;
  spec.process = OnStreamProcess;
  spec.userdata = &st;

  // 重建前清空 PCM 环：旧会话剩的样本比新流的第一帧更旧，跨过去只会让主机
  // 听到一段错位的回声。回调跑在 port 的 monitor 线程上。
  if (!port::Start(st.pipeline, st.monitor, std::move(spec),
                   [] {
                     std::lock_guard<std::mutex> lock(st.ring_mutex);
                     st.ring.clear();
                   },
                   std::move(on_lost))) {
    st.on_open = nullptr;
    return false;
  }
  return true;
}

void Stop() {
  // port 先落闸、join monitor 并拆流；main loop 线程就此停下，open 不再变。
  port::Stop(st.pipeline, st.monitor);

  // 拆之前真的在录音，就在这里补一次 close——调用方（audio::Stop）保证 bt
  // 还活着，这份报告发得出去。
  const bool was_open = st.open;
  st.open = false;
  auto callback = std::move(st.on_open);
  st.on_open = nullptr;
  {
    // 与在途的 PushSamples 互斥：它可能在闸门检查之后已经拿到锁。
    std::lock_guard<std::mutex> lock(st.ring_mutex);
    st.ring.clear();
  }
  if (was_open && callback) callback(false);
}

void PushSamples(const float* samples, uint32_t frames) {
  // 跑在解码线程上：未 Start / 已 Stop / 重建中、空指针、零帧一律拒绝。
  if (!st.pipeline.running.load() || samples == nullptr || frames == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(st.ring_mutex);
  // 整批插入：环满时 circular_buffer 覆盖最旧样本（丢旧保新，见
  // kRingFrames），不做背压反馈。
  st.ring.insert(st.ring.end(), samples, samples + frames);
}

}  // namespace source
}  // namespace audio
