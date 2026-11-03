/**
 * @file audio_sink.cpp
 * @brief audio::sink 实现：PipeWire 4 通道 f32 sink 与 RT process 回调
 *
 * 建流、main loop 线程与断线退避重连的骨架在 audio::port；本文件只保留
 * sink 特有的三件事：节点身份怎么拼、process 回调把样本交给谁、重建前清什么
 * 残料。断线判定与重连预算由 port 的 monitor 统一负责，与 source 同一套。
 */

#include "audio/audio_sink.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "audio/audio_encoder.h"
#include "audio/audio_identity.h"
#include "audio/audio_port.h"
#include "core/debug.h"
#include "core/protocol/audio_format.h"
#include "loguru.hpp"

namespace audio {
namespace sink {
namespace {

// 节点显示名（昵称/描述）的前缀，后接本站标识（12 位 MAC 十六进制）。这两条
// 不受任何名字判据约束，保留人读的形态；节点名那条走 MakeSinkNodeName，因为
// 它的形状是消费方的判断量。
constexpr char kNodeDescriptionBase[] = "DualSense Wireless Controller (PS5) ";
constexpr char kNodeNickBase[] = "DualSense Wireless Controller ";

// 长度门禁：主机侧的 Wine 只原样搬 62 字节以内的节点描述（它的
// MAX_DEVICE_NAME_LEN），超了就把整条丢掉、换成 device.product.name。尾巴接的是
// 12 位 MAC 十六进制，所以这里把"前缀 + 标识"钉在门槛之内：改文案先撞编译，
// 而不是撞在游戏上。
static_assert(std::string_view(kNodeDescriptionBase).size() + 12 <= 62);

/// 进程内单实例状态：流句柄与恢复状态机都归 audio::port 所有。
struct State {
  port::Pipeline pipeline;
  port::Monitor monitor;
};

State st;

/**
 * @brief stream 状态迁移回调：只把终态（UNCONNECTED/ERROR）记成断线事件。
 *
 * 判给 monitor 处理后本回调立刻返回——不在 PipeWire 事件线程里做任何重连
 * 动作。中间态（CONNECTING/激活等）不参与断线判定。
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

  DS5DUMMY_LOG(INFO, "[sink] Stream state changed: {} -> {}",
               pw_stream_state_as_string(old),
               pw_stream_state_as_string(state));

  if (state != PW_STREAM_STATE_UNCONNECTED &&
      state != PW_STREAM_STATE_ERROR) {
    return;
  }
  // 停机流程里的状态抖动与重复上报由 port 过滤；只有被受理的那一条才记 ERROR。
  if (port::ReportDisconnected(self->monitor, self->pipeline)) {
    DS5DUMMY_LOG(ERROR, "[sink] Stream disconnected or error: {}",
                 error ? error : "unknown");
  }
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
  if (!self) return;

  pw_buffer* buffer = port::DequeueBuffer(self->pipeline);
  if (!buffer) return;

  spa_buffer* spa_buf = buffer->buffer;

  // 单 plane 交织布局：每帧 = 1 个 float × 通道数。通道数必须与 Start 里
  // 宣告的通道位置个数一致（都是 encoder::kInputChannels）。
  constexpr size_t kBytesPerFrame = sizeof(float) * encoder::kInputChannels;
  // chunk->size 是本帧有效字节数（可小于缓冲分配量），向下取整成帧数：
  // 半帧没有意义，丢掉尾部。
  const spa_chunk* chunk = spa_buf->datas[0].chunk;
  const uint32_t n_frames = static_cast<uint32_t>(chunk->size / kBytesPerFrame);

  float* data = static_cast<float*>(spa_buf->datas[0].data);
  encoder::ProcessFrame(data, n_frames);
  pw_stream_queue_buffer(self->pipeline.stream, buffer);  // 消费完毕，归还
}

}  // namespace

bool Start(const std::string& station_id, const std::string& host_sysfs_path,
           std::function<void()> on_lost) {
  // 节点名与昵称/描述三条都从同一个 station_id 出发：节点名的形状是消费方的
  // strstr 判断量，必须走 audio::identity，不在这里手拼；显示名没有判据，直接
  // 把那 12 位接在本地前缀后面。
  port::StreamSpec spec;
  spec.tag = "sink";
  spec.media_class = "Audio/Sink";
  spec.media_role = "Game";
  spec.icon_name = identity::kSinkIconName;
  spec.direction = PW_DIRECTION_INPUT;  // 本端读样本：播放 sink
  spec.sample_rate = core::protocol::kOpusSampleRate;
  // 通道位置就是与 encoder 的声道约定：ch0/1 喇叭、ch2/3 触觉。
  spec.positions = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
                    SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR};
  spec.node_name = identity::MakeSinkNodeName(station_id);
  spec.node_nick = std::string(kNodeNickBase) + station_id;
  spec.node_description = std::string(kNodeDescriptionBase) + station_id;
  // 接口目录与节点三元组同级存进 spec：重连每次重建 stream，属性必须每次
  // 都带上，所以它属于 spec 而不是一次性入参。
  spec.host_sysfs_path = host_sysfs_path;
  spec.state_changed = OnStreamStateChanged;
  spec.process = OnStreamProcess;
  spec.userdata = &st;

  // 重连前清掉旧会话攒下的半批样本：跨到新流上只会播一段过期音频并错开
  // 时序，整块丢掉最干净。回调跑在 port 的 monitor 线程上。
  return port::Start(st.pipeline, st.monitor, std::move(spec),
                     [] { encoder::ResetInputBuffer(); }, std::move(on_lost));
}

void Stop() {
  port::Stop(st.pipeline, st.monitor);
}

}  // namespace sink
}  // namespace audio
