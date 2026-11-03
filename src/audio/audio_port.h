/**
 * @file audio_port.h
 * @brief sink 与 source 共用的 PipeWire 单流骨架与断线退避重建
 *
 * 两条流的结构完全同形：一条自建的 main loop 线程驱动 stream，一条 monitor
 * 线程只做"等断线 → 清残料 → 销毁 → 指数退避重建"。差别只在装进 StreamSpec
 * 的那几项——媒体类型、方向、格式、节点名与两个事件回调。把这一份骨架放在
 * 这里，是为了让"恢复策略"只有一处实现：重连预算、退避曲线、拆建顺序改一次
 * 两条流同时生效，不会一条已经会自愈、另一条还停在原地。
 *
 * @note 本模块不调 pw_init/pw_deinit：PipeWire 的进程级初始化由 audio::Start
 *       统一负责，sink 与 source 共享同一套全局状态。
 * @note Pipeline/Monitor 是**嵌入调用方静态状态**的值类型，本模块不持有任何
 *       进程级单例；一个进程里起几条流就有几组实例。
 */

#pragma once

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audio {
namespace port {

/**
 * @brief 一条 stream 的 PipeWire 句柄与它的 main loop 线程。
 *
 * 属于建/拆流的线程，PipeWire 对象（loop/context/core/stream）只在 Start 与
 * monitor 线程上被创建和销毁；RT 回调只经 stream 指针取缓冲。
 *
 * @note running 是断线通知的闸门：每次建流前开闸、拆除前落闸，因此拆除期
 *       PipeWire 自己发出的 UNCONNECTED 不会被记成一次新的断线。
 */
struct Pipeline {
  pw_main_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_stream* stream = nullptr;

  /// 交给 pw_stream_new_simple 的事件表。必须活到 stream 销毁——PipeWire
  /// 存的是它的指针而不是副本，所以它归 Pipeline 所有，不用函数内临时值。
  pw_stream_events events{};

  std::thread loop_thread;           ///< pw_main_loop_run 的载体
  std::atomic<bool> running{false};  ///< 断线通知闸门，见 ReportDisconnected
};

/**
 * @brief 建一条流所需的全部输入；monitor 保留一份用于重连。
 *
 * 三个名字与 sysfs 路径是 std::string：它们在 Start 时拼好，之后每次重建都
 * 原样复用。tag/媒体类型/角色/图标是静态字面量或编译期常量，指针必须指向
 * 静态存储期的字符串（两个调用点传的都是这种）。
 */
struct StreamSpec {
  const char* tag = "";           ///< 日志前缀，形如 "sink"/"source"
  const char* media_class = "";   ///< Audio/Sink 或 Audio/Source
  const char* media_role = "";    ///< Game / Communication
  const char* icon_name = "";     ///< audio::identity 的图标常量

  /// PW_DIRECTION_INPUT = 本端读样本（播放 sink）；PW_DIRECTION_OUTPUT = 本端
  /// 产样本（录音 source）。方向决定 process 回调里缓冲的用法，建流侧只透传。
  enum pw_direction direction = PW_DIRECTION_INPUT;

  uint32_t sample_rate = 0;                        ///< 线性 PCM 采样率
  std::vector<spa_audio_channel> positions;        ///< 通道位置与顺序，元素数即通道数

  std::string node_name;
  std::string node_nick;
  std::string node_description;
  std::string host_sysfs_path;  ///< 空串 = 不写 device.sysfs.path/alsa.card

  /// PipeWire 事件回调原样装进 Pipeline::events。两者都由 PipeWire 调用：
  /// state_changed 在 main loop 线程上，process 在 RT data-loop 线程上，
  /// 都不得阻塞；userdata 在重建前后是同一个指针。
  void (*state_changed)(void*, enum pw_stream_state, enum pw_stream_state,
                        const char*) = nullptr;
  void (*process)(void*) = nullptr;
  void* userdata = nullptr;
};

/**
 * @brief 一条流的恢复状态机：一条 monitor 线程 + 与状态回调的会合点。
 *
 * mutex+cv 是三方会合点：PipeWire 状态回调（报断线）、monitor 线程（等断线/
 * 退避）、调用方 Stop()（要求停机）。on_reset 与 on_lost 在 Start 时装填，
 * 之后只读，因此不需要额外加锁。
 */
struct Monitor {
  std::thread thread;                    ///< MonitorLoop 的载体
  std::mutex mutex;
  std::condition_variable cv;
  bool stop_requested = false;           ///< Stop() 的停机位
  bool disconnected = false;             ///< 已受理、待重连的断线事件（去重用）
  std::function<void()> on_reset;        ///< 重建前丢弃跨会话残料
  std::function<void()> on_lost;         ///< 连续重建失败的通知，可为空
  StreamSpec spec;                       ///< 重建用的完整快照
};

/**
 * @brief 建第一条 pipeline 并拉起 monitor 线程。
 *
 * @param pipeline 调用方持有的流句柄与闸门。
 * @param monitor  调用方持有的恢复状态；本函数写入 spec/回调并清事件位。
 * @param spec     建流参数；本函数接管所有权，重连时从 Monitor::spec 复用。
 * @param on_reset 每次销毁旧流之前调用一次，跑在 monitor 线程上；用来丢掉
 *                 跨会话残留的输入样本（sink 清编码环，source 清 PCM 环）。
 *                 可为空。
 * @param on_lost  连续 kMaxReconnectAttempts 次重建失败后的通知，跑在
 *                 monitor 线程上；可为空（不降级，只丢这条流）。实现方不得在
 *                 回调里调用 Stop()——要 join 的正是这条线程。
 * @return 成功 true；失败 false 且已拆掉半建的 pipeline，可安全再次 Start。
 * @note 双 Start（running 为真或 monitor 线程还活着）会被拒绝并返回 false。
 * @see Stop。
 */
bool Start(Pipeline& pipeline, Monitor& monitor, StreamSpec spec,
           std::function<void()> on_reset = {},
           std::function<void()> on_lost = {});

/**
 * @brief 停 monitor 线程并拆掉当前 pipeline；可重复调用。
 *
 * @details monitor 的退出路径自带拆除；线程从没起来过（Start 失败）时由本
 *          函数直接拆。返回后本流不再有任何活动线程或 PipeWire 对象。
 */
void Stop(Pipeline& pipeline, Monitor& monitor);

/**
 * @brief 把一次断线上报给 monitor；返回是否被受理。
 *
 * @details 由调用方的 state_changed 回调在 PipeWire main loop 线程上调用，
 *          只做状态位与一次唤醒，不在事件线程上做任何重建动作。
 * @return true = 这是本次服务期内第一条被受理的断线（调用方可据此打一条
 *         ERROR 日志）；false = 停机中、闸门已落或已有待处理事件（重复上报
 *         或拆除期抖动）。
 */
bool ReportDisconnected(Monitor& monitor, const Pipeline& pipeline);

/**
 * @brief 从 stream 取一块可用的缓冲；没有或布局非法时返回 null。
 *
 * @details 两个 RT process 回调共用的样板：无缓冲可取的返回 null 且不留下
 *          未归还的缓冲；缓冲存在但 data/chunk 缺失的**先归还再返回** null
 *          ——缓冲池枯竭比丢一帧更糟。返回非空时调用方必须在处理完后调用
 *          pw_stream_queue_buffer()。
 * @warning 由 PipeWire RT data-loop 线程调用：不分配、不打日志。
 */
pw_buffer* DequeueBuffer(Pipeline& pipeline);

}  // namespace port
}  // namespace audio
