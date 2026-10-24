/**
 * @file audio_sink.cpp
 * @brief audio::sink 实现：PipeWire 4 通道 f32 sink、RT process 回调、
 *        main loop 线程与断线退避重连
 *
 * 线程构成：一条自建的 PipeWire main loop 线程驱动事件，一条 monitor 线程
 * 只做"等断线 → 退避重建"的循环；process 回调由 PipeWire 的 RT data-loop
 * 线程执行，那里不允许日志与分配。断线后先销毁整条 pipeline 再按指数退避
 * 重建，连续 kMaxReconnectAttempts 次失败经 on_lost 通知调用方降级。
 */

#include "audio/audio_sink.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "audio/audio_encoder.h"
#include "audio/audio_identity.h"
#include "core/debug.h"
#include "loguru.hpp"

namespace audio {
namespace sink {
namespace {

/// 连续重建多少次失败就放弃（判定断链，通知 on_lost）。
constexpr int kMaxReconnectAttempts = 5;
/// 指数退避的首档延迟：500ms × 2^attempt。
constexpr int kReconnectBaseDelayMs = 500;
/// 指数退避的延迟上限：5 秒封顶，五轮总预算约 11.5s。
constexpr int kReconnectMaxDelayMs = 5000;

/**
 * @brief sink 建流用的配置，跨多次 start/重连存活。
 *
 * 采样率与通道数直接引 encoder 的常量：sink 端格式和编码端批几何必须同源，
 * 否则段长推导整体错位。节点名三元组在 Start() 里拼好，重连时不改。
 */
struct Config {
  std::string media_class = "Audio/Sink";          ///< 声明为播放型 sink
  uint32_t sample_rate = core::protocol::kOpusSampleRate; ///< 48kHz，与编码段几何一致
  uint32_t channels = encoder::kInputChannels;     ///< 4 通道：ch0/1 喇叭，ch2/3 触觉

  std::string node_description;
  std::string node_name;
  std::string node_nick;
  /// device.sysfs.path 的内容，相对 /sys；空串表示这一站没解析出来，属性
  /// 索性不写——空值会让按 sysfs 反查设备的消费者算出空结果，比缺键更糟。
  /// 同一段路径里的 dummy_hcd 序号还会被 audio::identity 推成 alsa.card。
  std::string host_sysfs_path;
};

// 节点显示名（昵称/描述）的前缀，后接 audio::identity 的 device_id。这两条
// 不受任何名字判据约束，保留人读的形态；节点名那条走 MakeSinkNodeName，因为
// 它的形状是消费方的判断量。
constexpr char kNodeDescriptionBase[] = "DualSense Wireless Controller (PS5) ";
constexpr char kNodeNickBase[] = "DualSense Wireless Controller ";

/**
 * @brief 进程内单实例状态。
 *
 * PipeWire 对象（loop/context/core/stream）只在建/毁 pipeline 的线程上动；
 * mutex+cv 是三方会合点：PipeWire 状态回调（报断线）、monitor 线程（等
  断线/退避）、外部 Stop()（要求停机）。
 */
struct State {
  pw_main_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;

  Config config;

  std::thread loop_thread;    ///< pw_main_loop_run 的载体
  std::thread monitor_thread; ///< MonitorLoop 的载体

  std::mutex mutex;
  std::condition_variable cv;
  bool stop_requested = false;      ///< 外部 Stop() 的停机位
  bool disconnected = false;        ///< 已上报、待重连的断线事件（去重用）
  bool running = false;             ///< 本 sink 处于服务状态（状态回调据此决定报不报断线）
  std::function<void()> on_lost;    ///< 重连彻底失败的通知，Start 时装填
} st;

/**
 * @brief stream 状态迁移回调：只把终态（UNCONNECTED/ERROR）记成断线事件。
 *
 * 判给 monitor 处理后本回调立刻返回——不在 PipeWire 事件线程里做任何重连
 * 动作。disconnected 标志兼作去重：一次断线只记一次、只唤醒一次。
 *
 * @param userdata 注册时传入的 &st。
 * @param old      迁移前的状态，仅用于日志。
 * @param state    迁移后的状态。
 * @param error    ERROR 状态时的错误描述，可为 null。
 * @note 可能跑在 PipeWire main loop 线程上：允许 LOG，禁止阻塞。
 */
void OnStreamStateChanged(void* userdata, enum pw_stream_state old,
                          enum pw_stream_state state, const char* error) {
  State* self = static_cast<State*>(userdata);
  if (!self) return;

  DS5DUMMY_LOG(INFO, "[sink] Stream state changed: {} -> {}",
        pw_stream_state_as_string(old), pw_stream_state_as_string(state));

  // 中间态（CONNECTING/R/stream 激活等）不参与断线判定。
  if (state != PW_STREAM_STATE_UNCONNECTED && state != PW_STREAM_STATE_ERROR) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(self->mutex);
    // 停机流程里的状态抖动不算断线；已记过一次的不再重复唤醒。
    if (self->stop_requested || !self->running) return;
    if (self->disconnected) return;
    self->disconnected = true;
  }

  DS5DUMMY_LOG(ERROR, "[sink] Stream disconnected or error: {}",
        error ? error : "unknown");
  self->cv.notify_one();  // 叫 monitor 进重连分支
}

/**
 * @brief RT process 回调：出队一块 buffer，把有效样本交给编码环。
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

  // 单 plane 交织布局：每帧 = 1 个 float × 通道数。
  const size_t bytes_per_frame = sizeof(float) * self->config.channels;
  const spa_chunk* chunk = spa_buf->datas[0].chunk;
  if (bytes_per_frame == 0) {
    pw_stream_queue_buffer(self->stream, buffer);
    return;  // 通道数被配坏，除零防线
  }
  // chunk->size 是本帧有效字节数（可小于缓冲分配量），向下取整成帧数：
  // 半帧没有意义，丢掉尾部。
  const uint32_t n_frames =
      static_cast<uint32_t>(chunk->size / bytes_per_frame);

  float* data = static_cast<float*>(spa_buf->datas[0].data);
  encoder::ProcessFrame(data, n_frames);
  pw_stream_queue_buffer(self->stream, buffer);  // 消费完毕，归还
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
 * 顺序承重：先 quit 并 join main loop 线程，再销毁 stream/core/context/
 * loop——PipeWire 对象只允许在驱动它们的事件线程停下来之后销毁。
 * running 落闸放在最前，让随后的拆除期状态回调不再报断线。
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
 * 填写：一个 1ms 量子、F32 4 通道的 ALSA 风格输出节点。
 *
 * @return 全部建好并 connect 成功 true；任一环节失败 false（此时无残留）。
 */
bool BuildPipeline() {
  pw_properties* props = nullptr;

  st.loop = pw_main_loop_new(nullptr);
  if (!st.loop) {
    DS5DUMMY_LOG(ERROR, "[sink] Failed to create main loop");
    DestroyPipeline();
    return false;
  }

  st.context = pw_context_new(pw_main_loop_get_loop(st.loop), nullptr, 0);
  if (!st.context) {
    DS5DUMMY_LOG(ERROR, "[sink] Failed to create context");
    DestroyPipeline();
    return false;
  }

  st.core = pw_context_connect(st.context, nullptr, 0);
  if (!st.core) {
    DS5DUMMY_LOG(ERROR, "[sink] Failed to connect to PipeWire");
    DestroyPipeline();
    return false;
  }

  // 节点身份四件套 + 时序/图标提示。node.latency "48/48000" = 标称延迟
  // 1ms；clock.quantum 48（最小 48、最大 1024 帧）把节点标成能跑 1ms 小
  // 量子的 ALSA 硬件风格——与伪装节点名同一套人设。
  //
  // node.pause-on-idle 显式写成 true（`keys.h` 里这个键的释义就是 "pause the
  // node when idle"），与真 ALSA 手柄节点上的 false 相反：那个 false 是给硬件
  // 节点保持设备唤醒用的。我们背后没有设备要唤醒，而空闲时让节点挂着正好把编码
  // 环停下来——没有流连着却继续被驱动，就会往蓝牙上持续灌静音的 0x36。想反过来
  // 无条件跑起来用的键是相邻的 `node.always-process`，不要拿这个当它用。
  props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, st.config.media_class.c_str(), PW_KEY_NODE_NAME,
      st.config.node_name.c_str(), PW_KEY_NODE_NICK,
      st.config.node_nick.c_str(), PW_KEY_NODE_DESCRIPTION,
      st.config.node_description.c_str(), PW_KEY_MEDIA_ROLE, "Game",
      "node.latency", "48/48000", "clock.quantum", "48", "clock.min-quantum",
      "48", "clock.max-quantum", "1024", PW_KEY_NODE_PAUSE_ON_IDLE, "true",
      nullptr);
  if (!props) {
    DS5DUMMY_LOG(ERROR, "[sink] Failed to create properties");
    DestroyPipeline();
    return false;
  }

  // device.* 这一组把这份人设从"名字像 ALSA 设备"升级到"身份可被按硬件查
  // 到"，与录音侧共用同一个生成器：同一只手柄在主机上就是一台 USB 声卡，两
  // 条流报出的硬件身份必须一致。
  identity::ApplyHardwareIdentity(props, identity::kSinkIconName,
                                  st.config.host_sysfs_path);

  st.stream = pw_stream_new_simple(pw_main_loop_get_loop(st.loop),
                                   st.config.node_name.c_str(), props,
                                   &kStreamEvents, &st);
  if (!st.stream) {
    // props 的所有权只在 stream 建成时才转移给 PipeWire；失败要自己释放。
    DS5DUMMY_LOG(ERROR, "[sink] Failed to create stream");
    pw_properties_free(props);
    DestroyPipeline();
    return false;
  }

  // 1KB 栈缓冲装 pod：这里只构建一个 raw audio 格式 pod，绰绰有余。
  uint8_t buffer[1024];
  spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  // 对外只宣告一种格式：F32 48kHz 4 通道交织。通道位置 FL/FR/RL/RR 就是
  // 与 encoder 的声道约定——ch0/1 喇叭、ch2/3 触觉。
  spa_audio_info_raw audio_info = {};
  audio_info.format = SPA_AUDIO_FORMAT_F32_LE;
  audio_info.rate = st.config.sample_rate;
  audio_info.channels = st.config.channels;
  audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
  audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
  audio_info.position[2] = SPA_AUDIO_CHANNEL_RL;
  audio_info.position[3] = SPA_AUDIO_CHANNEL_RR;

  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat,
                                         &audio_info);

  // DIRECTION_INPUT：从 PipeWire 视角是"输入流"，即播放 sink（我们读、
  // 别人写）。AUTOCONNECT 不等显式激活、MAP_BUFFERS 由 PipeWire 映射好
  // 样本内存、RT_PROCESS 声明回调跑实时上下文。
  const int result = pw_stream_connect(
      st.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                   PW_STREAM_FLAG_RT_PROCESS),
      params, 1);
  if (result < 0) {
    DS5DUMMY_LOG(ERROR, "[sink] Failed to connect stream: {}", strerror(-result));
    DestroyPipeline();
    return false;
  }

  // 事件线程最后起：此时 stream 已 connect，回调挂上去就是完整状态。
  try {
    st.loop_thread = std::thread([] { pw_main_loop_run(st.loop); });
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[sink] Cannot start PipeWire loop thread: {}", error.what());
    DestroyPipeline();
    return false;
  }

  DS5DUMMY_LOG(INFO, "[sink] started: node={} rate={} ch={}", st.config.node_name,
        st.config.sample_rate, st.config.channels);
  return true;
}

/**
 * @brief monitor 线程的睡眠：等 stop_requested 或超时，二者取其一。
 *
 * @param timeout_ms 退避睡眠上限（毫秒）。
 * @return true 表示等来了停机请求（调用方应放弃后续重连）；false 是单纯
 *         超时，继续重建。
 */
bool WaitForStopOrTimeout(int timeout_ms) {
  std::unique_lock<std::mutex> lock(st.mutex);
  return st.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [] { return st.stop_requested; });
}

/**
 * @brief 恢复 monitor 主循环：等断线 → 清残料 → 销毁 → 指数退避重建。
 *
 * 退避序列 delay = min(500ms × 2^attempt, 5000ms)：500/1000/2000/4000/
 * 5000ms，先睡后建，五轮全败则回调 on_lost 并退出（调用方据此降级
 * HID-only）。重连前先 encoder::ResetInputBuffer()：旧会话攒下的半批样本
 * 跨到新流上只会播一段过期音频并错开时序，整块丢掉最干净。
 *
 * @note 退出前无条件 DestroyPipeline()：无论成功恢复后被 Stop、还是判定
 *       失败，都保证拆干净。
 */
void MonitorLoop() {
  DS5DUMMY_LOG(INFO, "[sink] recovery monitor started");
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(st.mutex);
      // 两个唤醒源：外部 Stop()（stop_requested）或断线事件（disconnected）。
      st.cv.wait(lock, [] { return st.stop_requested || st.disconnected; });
      if (st.stop_requested) break;
      st.disconnected = false;  // 事件已受理，允许下一次断线再报
    }

    DS5DUMMY_LOG(WARNING, "[sink] PipeWire stream lost; reconnecting");
    encoder::ResetInputBuffer();
    DestroyPipeline();

    bool recovered = false;
    for (int attempt = 0; attempt < kMaxReconnectAttempts; ++attempt) {
      // 指数退避给 PipeWire 侧（daemon 重启、节点冲突留白)留恢复窗口。
      const int delay_ms =
          std::min(kReconnectBaseDelayMs << attempt, kReconnectMaxDelayMs);
      if (WaitForStopOrTimeout(delay_ms)) break;  // 睡眠间被叫停

      // running 先复位再建：新 stream 上的状态回调据此判断本 sink 仍在服务。
      st.running = true;
      if (BuildPipeline()) {
        recovered = true;
        break;
      }
      DS5DUMMY_LOG(WARNING, "[sink] reconnect attempt {}/{} failed", attempt + 1,
            kMaxReconnectAttempts);
    }

    if (st.stop_requested) break;  // 退避或重建期间收到停机，不再报 lost
    if (!recovered) {
      // 五轮全败：把降级决定交回调用方，monitor 的职责到此为止。
      DS5DUMMY_LOG(ERROR, "[sink] PipeWire reconnect failed after {} attempts",
            kMaxReconnectAttempts);
      auto callback = st.on_lost;
      if (callback) callback();
      break;
    }
    DS5DUMMY_LOG(INFO, "[sink] PipeWire stream recovered");
    // 恢复成功：回到 wait，盯下一次断线。
  }

  DestroyPipeline();
  DS5DUMMY_LOG(INFO, "[sink] recovery monitor stopped");
}

}  // namespace

bool Start(const std::string& station_id, const std::string& host_sysfs_path,
           std::function<void()> on_lost) {
  // 双 Start 拒绝：running 或 monitor 线程还在就说明上一份还没拆干净。
  if (st.running || st.monitor_thread.joinable()) {
    DS5DUMMY_LOG(ERROR, "[sink] sink already running");
    return false;
  }

  // 事件标志复位 + 节点身份拼装：三个串都从同一个 station_id 出发，形状全在
  // audio::identity——节点名那一条是要被消费方 strstr 的，不能在这里手拼。
  st.stop_requested = false;
  st.disconnected = false;
  st.on_lost = std::move(on_lost);
  const std::string device_id = identity::MakeDeviceId(station_id);
  st.config.node_name = identity::MakeSinkNodeName(station_id);
  st.config.node_nick = std::string(kNodeNickBase) + device_id;
  st.config.node_description = std::string(kNodeDescriptionBase) + device_id;
  // 接口目录跟着 sink 存进 config：重连每次重建 stream，属性必须每次都带上，
  // 所以它和节点三元组同级，而不是 BuildPipeline 的一次性入参。
  st.config.host_sysfs_path = host_sysfs_path;

  // pw_init 是进程级的，由 audio::Start 统一负责；本子模块只建自己的流。
  st.running = true;
  if (!BuildPipeline()) {
    st.running = false;
    return false;
  }

  // monitor 最后起：断线事件必须等 pipeline 完整存在才有意义。
  try {
    st.monitor_thread = std::thread(MonitorLoop);
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[sink] Cannot start recovery monitor: {}", error.what());
    DestroyPipeline();
    return false;
  }

  return true;
}

void Stop() {
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.stop_requested = true;
    // 清断线位：monitor 谓词里 stop_requested 优先，等待断线时收到停机
    // 直接走 break，不再进重连分支。
    st.disconnected = false;
  }
  st.cv.notify_all();  // 叫醒可能在 wait/wait_for 里的 monitor

  if (st.monitor_thread.joinable()) {
    // monitor 的退出路径自带 DestroyPipeline()，这里不重复拆。
    st.monitor_thread.join();
  } else {
    // monitor 从没起来（Start 失败路径）：pipeline 由本线程直接拆。
    DestroyPipeline();
  }

  st.running = false;
  DS5DUMMY_LOG(INFO, "[sink] stopped");
}

}  // namespace sink
}  // namespace audio
