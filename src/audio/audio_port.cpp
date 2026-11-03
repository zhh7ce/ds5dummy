/**
 * @file audio_port.cpp
 * @brief audio::port 实现：单流建拆骨架、main loop 线程与断线退避重建
 *
 * 线程构成与 audio_sink/audio_source 同形：一条自建的 PipeWire main loop
 * 线程驱动事件，一条 monitor 线程只做"等断线 → 退避重建"的循环；process
 * 回调由 PipeWire 的 RT data-loop 线程执行，那里不允许日志与分配。
 *
 * 闸门与断线位的读写都在 Monitor::mutex 里成对完成，拆除期 PipeWire 自己
 * 发出的状态迁移因此不会被误记成"又断了一次"——否则重建成功后 monitor 会
 * 立刻看见一条陈旧事件，陷入无休止的重连。
 */

#include "audio/audio_port.h"

#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "audio/audio_identity.h"
#include "core/debug.h"
#include "loguru.hpp"

namespace audio {
namespace port {
namespace {

/// 连续重建多少次失败就放弃（通知 on_lost）。
constexpr int kMaxReconnectAttempts = 5;
/// 指数退避的首档延迟：500ms × 2^attempt。
constexpr int kReconnectBaseDelayMs = 500;
/// 指数退避的延迟上限：5 秒封顶，五轮总预算约 11.5s。
constexpr int kReconnectMaxDelayMs = 5000;

/**
 * @brief 落闸：此后的状态迁移只属于拆除，不再算断线事件。
 *
 * @details running 与 disconnected 必须在同一把锁里改：状态回调先读闸门再
 *          置断线位，拆流线程先落闸再清位，两边串行化之后不存在"回调读到
 *          running=true、随后在重建成功后才把断线位留下的窗口"。
 */
void CloseGate(Monitor& monitor, Pipeline& pipeline) {
  std::lock_guard<std::mutex> lock(monitor.mutex);
  pipeline.running = false;
  monitor.disconnected = false;
}

/// 开闸：新 pipeline 已 connect，状态回调重新参与断线判定。
void OpenGate(Monitor& monitor, Pipeline& pipeline) {
  std::lock_guard<std::mutex> lock(monitor.mutex);
  pipeline.running = true;
  monitor.disconnected = false;
}

/// 停机位读取；monitor 线程在锁外检查时用。
bool StopRequested(Monitor& monitor) {
  std::lock_guard<std::mutex> lock(monitor.mutex);
  return monitor.stop_requested;
}

/**
 * @brief 销毁一条 pipeline，未建全时也能安全调用（幂等）。
 *
 * @details 顺序承重：先 quit 并 join main loop 线程，再销毁 stream/core/
 *          context/loop——PipeWire 对象只允许在驱动它们的事件线程停下来之后
 *          销毁。闸门由调用方在本函数之前落好（CloseGate）。
 */
void Destroy(Pipeline& pipeline) {
  if (pipeline.loop != nullptr) pw_main_loop_quit(pipeline.loop);
  if (pipeline.loop_thread.joinable()) pipeline.loop_thread.join();

  // 与建立反序逐项释放并置空：半途失败的 pipeline 也能拆干净。
  if (pipeline.stream != nullptr) {
    pw_stream_destroy(pipeline.stream);
    pipeline.stream = nullptr;
  }
  if (pipeline.core != nullptr) {
    pw_core_disconnect(pipeline.core);
    pipeline.core = nullptr;
  }
  if (pipeline.context != nullptr) {
    pw_context_destroy(pipeline.context);
    pipeline.context = nullptr;
  }
  if (pipeline.loop != nullptr) {
    pw_main_loop_destroy(pipeline.loop);
    pipeline.loop = nullptr;
  }
}

/**
 * @brief 建一整条 pipeline：main loop → context → core → stream → 事件线程。
 *
 * @details 每个失败分支都调 Destroy() 回收已建部分。节点参数按伪装身份
 *          填写：1ms 量子、F32 交织、空闲挂起的 ALSA 风格节点；媒体类型、
 *          方向、通道布局与硬件身份全部取自 spec，两条流只在 spec 上不同。
 * @return 全部建好并 connect 成功 true；任一环节失败 false（此时无残留）。
 */
bool Build(Pipeline& pipeline, const StreamSpec& spec) {
  pw_properties* props = nullptr;

  pipeline.loop = pw_main_loop_new(nullptr);
  if (!pipeline.loop) {
    DS5DUMMY_LOG(ERROR, "[{}] Failed to create main loop", spec.tag);
    Destroy(pipeline);
    return false;
  }

  pipeline.context = pw_context_new(pw_main_loop_get_loop(pipeline.loop),
                                    nullptr, 0);
  if (!pipeline.context) {
    DS5DUMMY_LOG(ERROR, "[{}] Failed to create context", spec.tag);
    Destroy(pipeline);
    return false;
  }

  pipeline.core = pw_context_connect(pipeline.context, nullptr, 0);
  if (!pipeline.core) {
    DS5DUMMY_LOG(ERROR, "[{}] Failed to connect to PipeWire", spec.tag);
    Destroy(pipeline);
    return false;
  }

  // 节点身份四件套 + 时序/空闲策略。node.latency "48/48000" = 标称延迟 1ms；
  // clock.quantum 48（最小 48、最大 1024 帧）把节点标成能跑 1ms 小量子的
  // ALSA 硬件风格——与伪装节点名同一套人设。
  //
  // node.pause-on-idle 显式写成 true（`keys.h` 里这个键的释义就是 "pause the
  // node when idle"），与真 ALSA 手柄节点上的 false 相反：那个 false 是给硬件
  // 节点保持设备唤醒用的。我们背后没有设备要唤醒，而空闲时让节点挂着正好把
  // 数据流停下来——没有流连着却继续被驱动，下行会往蓝牙上持续灌静音的 0x36，
  // 上行则会让 source 的 STREAMING↔PAUSED 判据失效。想反过来无条件跑起来用的
  // 键是相邻的 `node.always-process`，不要拿这个当它用。
  props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, spec.media_class, PW_KEY_NODE_NAME,
      spec.node_name.c_str(), PW_KEY_NODE_NICK, spec.node_nick.c_str(),
      PW_KEY_NODE_DESCRIPTION, spec.node_description.c_str(),
      PW_KEY_MEDIA_ROLE, spec.media_role, "node.latency", "48/48000",
      "clock.quantum", "48", "clock.min-quantum", "48", "clock.max-quantum",
      "1024", PW_KEY_NODE_PAUSE_ON_IDLE, "true", nullptr);
  if (!props) {
    DS5DUMMY_LOG(ERROR, "[{}] Failed to create properties", spec.tag);
    Destroy(pipeline);
    return false;
  }

  // device.* 这一组把这份人设从"名字像 ALSA 设备"升级到"身份可被按硬件查
  // 到"，两条流共用同一个生成器：同一只手柄在主机上就是一台 USB 声卡，两条
  // 流报出的硬件身份必须一致。
  identity::ApplyHardwareIdentity(props, spec.icon_name,
                                  spec.host_sysfs_path);

  // 事件表按值存进 Pipeline：PipeWire 的 listener 持有的是它的指针，函数内
  // 临时对象在 connect 返回后就悬空了。
  pipeline.events = {};
  pipeline.events.version = PW_VERSION_STREAM_EVENTS;
  pipeline.events.state_changed = spec.state_changed;
  pipeline.events.process = spec.process;

  pipeline.stream = pw_stream_new_simple(pw_main_loop_get_loop(pipeline.loop),
                                         spec.node_name.c_str(), props,
                                         &pipeline.events, spec.userdata);
  if (!pipeline.stream) {
    // props 的所有权只在 stream 建成时才转移给 PipeWire；失败要自己释放。
    DS5DUMMY_LOG(ERROR, "[{}] Failed to create stream", spec.tag);
    pw_properties_free(props);
    Destroy(pipeline);
    return false;
  }

  // 1KB 栈缓冲装 pod：这里只构建一个 raw audio 格式 pod，绰绰有余。
  const uint32_t channels = static_cast<uint32_t>(spec.positions.size());
  uint8_t buffer[1024];
  spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  // 对外只宣告一种格式：F32 交织，通道位置与顺序由 spec 给定——下行
  // FL/FR/RL/RR 对应 encoder 的"ch0/1 喇叭、ch2/3 触觉"，上行只有 MONO。
  spa_audio_info_raw audio_info = {};
  audio_info.format = SPA_AUDIO_FORMAT_F32_LE;
  audio_info.rate = spec.sample_rate;
  audio_info.channels = channels;
  for (size_t i = 0; i < spec.positions.size(); ++i) {
    audio_info.position[i] = spec.positions[i];
  }

  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat,
                                         &audio_info);

  // 方向由 spec 决定：DIRECTION_INPUT 从 PipeWire 视角是"输入流"（播放
  // sink，我们读、别人写）；DIRECTION_OUTPUT 是"输出流"（录音 source，本端
  // 产样本）。AUTOCONNECT 不等显式激活、MAP_BUFFERS 由 PipeWire 映射好样本
  // 内存、RT_PROCESS 声明回调跑实时上下文。
  const int result = pw_stream_connect(
      pipeline.stream, spec.direction, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                   PW_STREAM_FLAG_RT_PROCESS),
      params, 1);
  if (result < 0) {
    DS5DUMMY_LOG(ERROR, "[{}] Failed to connect stream: {}", spec.tag,
                 strerror(-result));
    Destroy(pipeline);
    return false;
  }

  // 事件线程最后起：此时 stream 已 connect，回调挂上去就是完整状态。
  try {
    pipeline.loop_thread =
        std::thread([loop = pipeline.loop] { pw_main_loop_run(loop); });
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[{}] Cannot start PipeWire loop thread: {}", spec.tag,
                 error.what());
    Destroy(pipeline);
    return false;
  }

  DS5DUMMY_LOG(INFO, "[{}] started: node={} rate={} ch={}", spec.tag,
               spec.node_name, spec.sample_rate, channels);
  return true;
}

/**
 * @brief monitor 线程的睡眠：等 stop_requested 或超时，二者取其一。
 *
 * @return true 表示等来了停机请求（调用方应放弃后续重连）；false 是单纯
 *         超时，继续重建。
 */
bool WaitForStopOrTimeout(Monitor& monitor, int timeout_ms) {
  std::unique_lock<std::mutex> lock(monitor.mutex);
  return monitor.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [&monitor] { return monitor.stop_requested; });
}

/**
 * @brief 恢复 monitor 主循环：等断线 → 清残料 → 销毁 → 指数退避重建。
 *
 * 退避序列 delay = min(500ms × 2^attempt, 5000ms)：500/1000/2000/4000/
 * 5000ms，先睡后建，五轮全败则回调 on_lost 并退出（调用方据此降级）。落闸
 * 之后、拆流之前先调 on_reset()：旧会话攒下的半批样本跨到新流上只会播一段
 * 过期音频并错开时序，整块丢掉最干净。
 *
 * @note 退出前无条件拆干净：无论成功恢复后被 Stop、还是判定失败，返回时
 *       本流都不再持有线程与 PipeWire 对象。
 */
void MonitorLoop(Monitor* monitor, Pipeline* pipeline) {
  DS5DUMMY_LOG(INFO, "[{}] recovery monitor started", monitor->spec.tag);
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(monitor->mutex);
      // 两个唤醒源：外部 Stop()（stop_requested）或断线事件（disconnected）。
      monitor->cv.wait(lock, [monitor] {
        return monitor->stop_requested || monitor->disconnected;
      });
      if (monitor->stop_requested) break;
      monitor->disconnected = false;  // 事件已受理，允许下一次断线再报
    }

    DS5DUMMY_LOG(WARNING, "[{}] PipeWire stream lost; reconnecting",
                 monitor->spec.tag);
    // 先落闸再清残料：落闸之后生产者（解码线程、encoder 的输入环）的写入
    // 要么被拒、要么已经可见，清场不会被一条迟到的 PushSamples 打穿。
    CloseGate(*monitor, *pipeline);
    if (monitor->on_reset) monitor->on_reset();
    Destroy(*pipeline);

    bool recovered = false;
    for (int attempt = 0; attempt < kMaxReconnectAttempts; ++attempt) {
      // 指数退避给 PipeWire 侧（daemon 重启、节点冲突留白）留恢复窗口。
      // 开闸放在每次尝试之前：新 stream 一 connect 就参与断线判定。
      const int delay_ms =
          std::min(kReconnectBaseDelayMs << attempt, kReconnectMaxDelayMs);
      if (WaitForStopOrTimeout(*monitor, delay_ms)) break;  // 睡眠间被叫停

      OpenGate(*monitor, *pipeline);
      if (Build(*pipeline, monitor->spec)) {
        recovered = true;
        break;
      }
      DS5DUMMY_LOG(WARNING, "[{}] reconnect attempt {}/{} failed",
                   monitor->spec.tag, attempt + 1, kMaxReconnectAttempts);
    }

    if (StopRequested(*monitor)) break;  // 退避或重建期间收到停机，不再报 lost
    if (!recovered) {
      // 五轮全败：把降级决定交回调用方，monitor 的职责到此为止。
      DS5DUMMY_LOG(ERROR, "[{}] PipeWire reconnect failed after {} attempts",
                   monitor->spec.tag, kMaxReconnectAttempts);
      auto callback = monitor->on_lost;
      if (callback) callback();
      break;
    }
    DS5DUMMY_LOG(INFO, "[{}] PipeWire stream recovered", monitor->spec.tag);
    // 恢复成功：回到 wait，盯下一次断线。
  }

  CloseGate(*monitor, *pipeline);
  Destroy(*pipeline);
  DS5DUMMY_LOG(INFO, "[{}] recovery monitor stopped", monitor->spec.tag);
}

}  // namespace

bool Start(Pipeline& pipeline, Monitor& monitor, StreamSpec spec,
           std::function<void()> on_reset,
           std::function<void()> on_lost) {
  // 双 Start 拒绝：闸门还开着或 monitor 线程还在（含 on_lost 后尚未 Stop
  // 回收的那种）就说明上一份没拆干净。
  if (pipeline.running.load() || monitor.thread.joinable()) {
    DS5DUMMY_LOG(ERROR, "[{}] stream or recovery monitor still alive",
                 spec.tag);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(monitor.mutex);
    monitor.stop_requested = false;
    monitor.disconnected = false;
  }
  // spec 归 monitor 所有：每次重建都要原样复用节点名与硬件身份。
  monitor.spec = std::move(spec);
  monitor.on_reset = std::move(on_reset);
  monitor.on_lost = std::move(on_lost);

  // pw_init 是进程级的，由 audio::Start 统一负责；本模块只建自己的流。
  OpenGate(monitor, pipeline);
  if (!Build(pipeline, monitor.spec)) {
    CloseGate(monitor, pipeline);
    return false;
  }

  // monitor 最后起：断线事件必须等 pipeline 完整存在才有意义。
  try {
    monitor.thread = std::thread(MonitorLoop, &monitor, &pipeline);
  } catch (const std::system_error& error) {
    DS5DUMMY_LOG(ERROR, "[{}] Cannot start recovery monitor: {}",
                 monitor.spec.tag, error.what());
    CloseGate(monitor, pipeline);
    Destroy(pipeline);
    return false;
  }

  return true;
}

void Stop(Pipeline& pipeline, Monitor& monitor) {
  const bool was_running = pipeline.running.load() || monitor.thread.joinable() ||
                           pipeline.loop != nullptr;
  {
    std::lock_guard<std::mutex> lock(monitor.mutex);
    monitor.stop_requested = true;
    // 清断线位：monitor 谓词里 stop_requested 优先，等待断线时收到停机
    // 直接走 break，不再进重连分支。
    monitor.disconnected = false;
  }
  monitor.cv.notify_all();  // 叫醒可能在 wait/wait_for 里的 monitor

  if (monitor.thread.joinable()) {
    // monitor 的退出路径自带落闸与拆除，这里不重复拆。
    monitor.thread.join();
    CloseGate(monitor, pipeline);
  } else {
    // monitor 从没起来（Start 失败路径）：pipeline 由本线程直接拆。
    CloseGate(monitor, pipeline);
    Destroy(pipeline);
  }

  if (was_running) {
    DS5DUMMY_LOG(INFO, "[{}] stopped", monitor.spec.tag);
  }
}

bool ReportDisconnected(Monitor& monitor, const Pipeline& pipeline) {
  // 快路径：闸门已落（停机/拆除中）时连锁都不用取。
  if (!pipeline.running.load()) return false;

  {
    std::lock_guard<std::mutex> lock(monitor.mutex);
    // 锁内重查：落闸与清位都在同一把锁里，读到 false 就是拆除期抖动。
    if (monitor.stop_requested || !pipeline.running.load() ||
        monitor.disconnected) {
      return false;
    }
    monitor.disconnected = true;
  }
  monitor.cv.notify_one();  // 叫 monitor 进重连分支
  return true;
}

pw_buffer* DequeueBuffer(Pipeline& pipeline) {
  if (pipeline.stream == nullptr) return nullptr;

  pw_buffer* buffer = pw_stream_dequeue_buffer(pipeline.stream);
  if (buffer == nullptr) return nullptr;  // 没缓冲可取：本轮空转，下轮再来

  spa_buffer* spa_buf = buffer->buffer;
  if (!spa_buf || !spa_buf->datas[0].data || !spa_buf->datas[0].chunk) {
    // 归还义务优先于数据有效性：不还，缓冲池很快枯竭、流卡死。
    pw_stream_queue_buffer(pipeline.stream, buffer);
    return nullptr;
  }
  return buffer;
}

}  // namespace port
}  // namespace audio
