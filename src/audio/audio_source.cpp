/**
 * @file audio_source.cpp
 * @brief audio::source 实现：PipeWire 单声道 f32 虚拟麦克风与录音状态通知
 *
 * 线程构成：一条自建的 PipeWire main loop 线程驱动事件，process 回调由
 * PipeWire 的 RT data-loop 线程执行。PCM 环由解码线程写入、RT 回调消费；
 * 环里没有样本时补静音。state_changed 把 STREAMING/PAUSED 的迁移翻成一次
 * on_mic_open 通知，其余状态不参与。
 */

#include "audio/audio_source.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <atomic>
#include <boost/circular_buffer.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "audio/audio_identity.h"
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

/**
 * @brief 虚拟麦克风建流用的配置，跨 Start/Stop 复用。
 *
 * 采样率与声道数直接引 core 的上行契约：解码端按同一组几何产出 PCM，环里
 * 的样本无需任何转换就能交给 PipeWire。
 */
struct Config {
  std::string media_class = "Audio/Source";  ///< 声明为采集型 source
  uint32_t sample_rate = core::protocol::kOpusSampleRate;  ///< 48kHz
  uint32_t channels = core::protocol::kMicChannels;       ///< 单声道

  std::string node_description;
  std::string node_name;
  std::string node_nick;
  /// device.sysfs.path 的内容：本站的 USB 接口目录，相对 /sys；空串表示这一站没
  /// 解析出来，属性索性不写——空值会让按 sysfs 反查设备容器的消费者算出空结果，
  /// 比缺键更糟。
  std::string host_sysfs_path;
};

// 节点显示名（昵称/描述）的前缀，后接 audio::identity 的 device_id。描述里带
// Microphone，与同名 sink 在扁平列表里仍可分辨。节点名那条走
// MakeSourceNodeName，与 sink 同一族形状。
constexpr char kNodeDescriptionBase[] =
    "DualSense Wireless Controller (PS5) Microphone ";
constexpr char kNodeNickBase[] = "DualSense Wireless Controller ";

/**
 * @brief 进程内单实例状态。
 *
 * PipeWire 对象只在建/毁 pipeline 的线程上动；ring 由解码线程与 RT 回调
 * 共享，受 ring_mutex 保护。open 只在 PipeWire main loop 线程上翻转，
 * Stop() 在 join 之后读它补 close 通知，因此不需要锁。
 */
struct State {
  pw_main_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;

  Config config;

  std::thread loop_thread;  ///< pw_main_loop_run 的载体

  boost::circular_buffer<float> ring;  ///< 待交付的单声道 PCM，受 ring_mutex 保护
  std::mutex ring_mutex;               ///< 护 ring

  bool open = false;           ///< 主机是否正在录音（只在 main loop 线程翻转）
  std::atomic<bool> running{false};  ///< 闸门：Stop 落闸后状态回调不再通知
  MicOpenHandler on_open;      ///< Start 装填，Stop join 之后清空
} st;

/**
 * @brief 翻转"主机在录音"状态并通知调用方一次。
 *
 * @param self    单实例状态。
 * @param open    目标状态。
 * @details 只在 running 时生效；同值重复到达直接丢弃——PipeWire 的状态回调
 *          可以反复报告同一个终态，链路发送不能跟着重复。回调跑在 PipeWire
 *          main loop 线程上，实现方必须非阻塞。
 */
void SetOpen(State* self, bool open) {
  if (!self->running || self->open == open) return;
  self->open = open;
  if (self->on_open) self->on_open(open);
}

/**
 * @brief stream 状态迁移回调：只把 STREAMING/PAUSED 翻成录音开关。
 *
 * STREAMING = 有客户端在录，PAUSED = 没有客户端；UNCONNECTED/ERROR 说明流
 * 已经没了，同样按"没人在录"处理（本模块不重连）。CONNECTING 等中间态
 * 忽略。
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
        pw_stream_state_as_string(old), pw_stream_state_as_string(state));

  if (state == PW_STREAM_STATE_ERROR && self->running) {
    DS5DUMMY_LOG(ERROR, "[source] Stream error: {}", error ? error : "unknown");
  }

  if (state == PW_STREAM_STATE_STREAMING) {
    SetOpen(self, true);
  } else if (state == PW_STREAM_STATE_PAUSED ||
             state == PW_STREAM_STATE_UNCONNECTED ||
             state == PW_STREAM_STATE_ERROR) {
    SetOpen(self, false);
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
  if (!self || !self->stream) return;

  pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
  if (!buffer) return;  // 没缓冲可取：本轮空转，下轮再来

  spa_buffer* spa_buf = buffer->buffer;
  if (!spa_buf || !spa_buf->datas[0].data || !spa_buf->datas[0].chunk) {
    pw_stream_queue_buffer(self->stream, buffer);  // 归还义务优先于数据有效性
    return;
  }

  // 单 plane 交织布局：每帧 = 1 个 float × 声道数（单声道即 1 个 float）。
  const size_t bytes_per_frame = sizeof(float) * self->config.channels;
  const size_t max_frames = spa_buf->datas[0].maxsize / bytes_per_frame;
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
  chunk->stride = static_cast<int32_t>(bytes_per_frame);
  chunk->size = static_cast<uint32_t>(frames * bytes_per_frame);
  buffer->size = frames;  // 音频流按帧报告已排入量，服务端据此估算耗时
  pw_stream_queue_buffer(self->stream, buffer);
}

/// 只订阅 state_changed 与 process 两个事件；lambda 仅为聚合初始化把
/// 其余字段清零（PipeWire 事件结构按版本前缀生效）。
const pw_stream_events kStreamEvents = [] {
  pw_stream_events events = {};
  events.version = PW_VERSION_STREAM_EVENTS;
  events.state_changed = OnStreamStateChanged;
  events.process = OnStreamProcess;
  return events;
}();

/**
 * @brief 销毁整条 PipeWire pipeline，未建全时也能安全调用（幂等）。
 *
 * 顺序承重：先落 running 闸门，再 quit 并 join main loop 线程，最后销毁
 * stream/core/context/loop——PipeWire 对象只允许在驱动它们的事件线程停下来
 * 之后销毁。
 */
void DestroyPipeline() {
  st.running = false;

  if (st.loop != nullptr) pw_main_loop_quit(st.loop);
  if (st.loop_thread.joinable()) st.loop_thread.join();

  // 与建立反序逐项释放并置空：半途失败的 pipeline 也能拆干净。
  if (st.stream != nullptr) {
    pw_stream_destroy(st.stream);
    st.stream = nullptr;
  }
  if (st.core != nullptr) {
    pw_core_disconnect(st.core);
    st.core = nullptr;
  }
  if (st.context != nullptr) {
    pw_context_destroy(st.context);
    st.context = nullptr;
  }
  if (st.loop != nullptr) {
    pw_main_loop_destroy(st.loop);
    st.loop = nullptr;
  }
}

/**
 * @brief 建一整条 pipeline：main loop → context → core → stream → 事件线程。
 *
 * 每个失败分支都调 DestroyPipeline() 回收已建部分。节点参数按伪装身份
 * 填写：单声道 F32 48kHz 的 ALSA 风格采集节点。
 *
 * @return 全部建好并 connect 成功 true；任一环节失败 false（此时无残留）。
 */
bool BuildPipeline() {
  pw_properties* props = nullptr;

  st.loop = pw_main_loop_new(nullptr);
  if (!st.loop) {
    DS5DUMMY_LOG(ERROR, "[source] Failed to create main loop");
    DestroyPipeline();
    return false;
  }

  st.context = pw_context_new(pw_main_loop_get_loop(st.loop), nullptr, 0);
  if (!st.context) {
    DS5DUMMY_LOG(ERROR, "[source] Failed to create context");
    DestroyPipeline();
    return false;
  }

  st.core = pw_context_connect(st.context, nullptr, 0);
  if (!st.core) {
    DS5DUMMY_LOG(ERROR, "[source] Failed to connect to PipeWire");
    DestroyPipeline();
    return false;
  }

  // 节点身份四件套 + 时序提示，与 sink 同一套伪装。录音节点的角色用
  // Communication（会话管理器会把它路由到通话类默认设备）。
  //
  // node.pause-on-idle 写成 true 和 sink 同一取值，同时把本模块的开关前提写在
  // 节点字典上：`STREAMING ↔ PAUSED` 就是"主机在不在录音"的信号（见
  // OnStreamStateChanged），而这个迁移只有在允许空闲挂起时才会发生。
  props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, st.config.media_class.c_str(), PW_KEY_NODE_NAME,
      st.config.node_name.c_str(), PW_KEY_NODE_NICK,
      st.config.node_nick.c_str(), PW_KEY_NODE_DESCRIPTION,
      st.config.node_description.c_str(), PW_KEY_MEDIA_ROLE, "Communication",
      "node.latency", "48/48000", "clock.quantum", "48", "clock.min-quantum",
      "48", "clock.max-quantum", "1024", PW_KEY_NODE_PAUSE_ON_IDLE, "true",
      nullptr);
  if (!props) {
    DS5DUMMY_LOG(ERROR, "[source] Failed to create properties");
    DestroyPipeline();
    return false;
  }

  // 硬件身份与 sink 同一份：主机上这是一台 USB 声卡的两条流，报出两个身份
  // 会让按硬件查端点的消费者只认其中一条。图标换成麦克风。
  identity::ApplyHardwareIdentity(props, identity::kSourceIconName,
                                  st.config.host_sysfs_path);

  st.stream = pw_stream_new_simple(pw_main_loop_get_loop(st.loop),
                                   st.config.node_name.c_str(), props,
                                   &kStreamEvents, &st);
  if (!st.stream) {
    // props 的所有权只在 stream 建成时才转移给 PipeWire；失败要自己释放。
    DS5DUMMY_LOG(ERROR, "[source] Failed to create stream");
    pw_properties_free(props);
    DestroyPipeline();
    return false;
  }

  // 1KB 栈缓冲装 pod：这里只构建一个 raw audio 格式 pod，绰绰有余。
  uint8_t buffer[1024];
  spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  // 对外只宣告一种格式：F32 48kHz 单声道，与解码端的产出几何一致。
  spa_audio_info_raw audio_info = {};
  audio_info.format = SPA_AUDIO_FORMAT_F32_LE;
  audio_info.rate = st.config.sample_rate;
  audio_info.channels = st.config.channels;
  audio_info.position[0] = SPA_AUDIO_CHANNEL_MONO;

  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat,
                                         &audio_info);

  // DIRECTION_OUTPUT：从 PipeWire 视角是"输出流"——本端是生产者，也就是
  // 一个 Audio/Source。AUTOCONNECT 等客户端来连、MAP_BUFFERS 由 PipeWire
  // 映射样本内存、RT_PROCESS 声明回调跑实时上下文。
  const int result = pw_stream_connect(
      st.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                   PW_STREAM_FLAG_RT_PROCESS),
      params, 1);
  if (result < 0) {
    DS5DUMMY_LOG(ERROR, "[source] Failed to connect stream: {}",
          strerror(-result));
    DestroyPipeline();
    return false;
  }

  // 事件线程最后起：此时 stream 已 connect，回调挂上去就是完整状态。
  try {
    st.loop_thread = std::thread([] { pw_main_loop_run(st.loop); });
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[source] Cannot start PipeWire loop thread: {}",
          error.what());
    DestroyPipeline();
    return false;
  }

  DS5DUMMY_LOG(INFO, "[source] started: node={} rate={} ch={}",
        st.config.node_name, st.config.sample_rate, st.config.channels);
  return true;
}

}  // namespace

bool Start(const std::string& station_id, const std::string& host_sysfs_path,
           MicOpenHandler on_mic_open) {
  // 空回调在这里拒掉：Source 建出来也永远没人能告诉手柄"该开麦了"，等于
  // 一个录不到声音的节点，比不起更难查。
  if (!on_mic_open) {
    DS5DUMMY_LOG(ERROR,
          "[source] No mic-open handler — the controller microphone could "
          "never be enabled");
    return false;
  }
  // 双 Start 拒绝：上一次的 pipeline 还没拆干净。
  if (st.running || st.loop_thread.joinable()) {
    DS5DUMMY_LOG(ERROR, "[source] source already running");
    return false;
  }

  // 状态复位 + 节点身份拼装：三个串都从同一个 station_id 出发，形状全在
  // audio::identity，与 sink 侧同一族（这是一台声卡的播放与录音两半边）。
  st.open = false;
  st.on_open = std::move(on_mic_open);
  st.ring.set_capacity(kRingFrames);
  st.ring.clear();
  const std::string device_id = identity::MakeDeviceId(station_id);
  st.config.node_name = identity::MakeSourceNodeName(station_id);
  st.config.node_nick = std::string(kNodeNickBase) + device_id;
  st.config.node_description = std::string(kNodeDescriptionBase) + device_id;
  // 接口目录存进 config 而不是透传给 BuildPipeline：建流用的字段全在那一份
  // 配置里，sink 侧同一形态（那边重连时每次都要重新带上同一个值）。
  st.config.host_sysfs_path = host_sysfs_path;

  // 闸门先开：事件线程一起来，状态回调就据此决定要不要通知。
  st.running = true;
  if (!BuildPipeline()) {
    st.running = false;
    st.on_open = nullptr;
    return false;
  }
  return true;
}

void Stop() {
  // 先落闸再拆：拆除期间的状态迁移（UNCONNECTED 等）不再回调调用方。
  st.running = false;
  DestroyPipeline();

  // main loop 线程已 join，open 不会再变。拆之前真的在录音，就在这里补一次
  // close——调用方（audio::Stop）保证 bt 还活着，这份报告发得出去。
  const bool was_open = st.open;
  st.open = false;
  auto callback = std::move(st.on_open);
  st.on_open = nullptr;
  {
    // 与在途的 PushSamples 互斥：它可能在 running 检查之后已经拿到锁。
    std::lock_guard<std::mutex> lock(st.ring_mutex);
    st.ring.clear();
  }
  if (was_open && callback) callback(false);

  DS5DUMMY_LOG(INFO, "[source] stopped");
}

void PushSamples(const float* samples, uint32_t frames) {
  // 跑在解码线程上：未 Start / 已 Stop、空指针、零帧一律拒绝。
  if (!st.running || samples == nullptr || frames == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(st.ring_mutex);
  // 整批插入：环满时 circular_buffer 覆盖最旧样本（丢旧保新，见
  // kRingFrames），不做背压反馈。
  st.ring.insert(st.ring.end(), samples, samples + frames);
}

}  // namespace source
}  // namespace audio
