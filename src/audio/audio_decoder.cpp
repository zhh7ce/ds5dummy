/**
 * @file audio_decoder.cpp
 * @brief audio::decoder 实现：有界包队列、解码线程与 10ms PCM 交付
 *
 * bt 读线程写队列，解码线程按到达顺序读；两者之间只有 mutex + cv。每包恒定
 * 480 帧（10ms）单声道，解出即交给出口，不做重采样、不攒批——上行麦克风的
 * 端到端延迟就是队列里积压的包数。
 */

#include "audio/audio_decoder.h"

#include <opus.h>

#include <array>
#include <atomic>
#include <boost/circular_buffer.hpp>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "core/debug.h"
#include "core/protocol/audio_format.h"
#include "loguru.hpp"

namespace audio {
namespace decoder {
namespace {

/**
 * @brief 待解码队列深度：8 包 ≈ 80ms。
 *
 * 上行是实时链路，积压只会让主机听到更旧的声音；宁可丢最旧的一包。深度取
 * 下行发送队列同一个量级（bt 的 kQueueMax = 8），两侧背压手感一致。
 */
constexpr size_t kQueueMax = 8;

/// 一帧包体的定长槽：长度由 core 的共享契约钉死，槽里不会出现半包。
using Packet = std::array<uint8_t, core::protocol::kMicOpusSize>;

/**
 * @brief 进程内单实例状态。
 *
 * queue 由 bt 读线程（PushPacket）与解码线程共同访问，受 mutex 保护；
 * opus_decoder 与 output 只在起线程前装填、join 之后释放，运行期只读。
 */
struct State {
  OpusDecoder* opus_decoder = nullptr;  ///< 单声道 48kHz 解码器
  std::thread decode_thread;            ///< DecodeLoop 的载体

  boost::circular_buffer<Packet> queue;  ///< 待解码包，受 mutex 保护
  std::mutex mutex;                      ///< 护 queue
  std::condition_variable queue_cv;      ///< 通知"有包可解"或停机
  std::atomic<bool> running{false};      ///< 解码线程运行标志

  PcmSink output;  ///< 每包 PCM 的去处（Init 装填，Stop join 后清空）
} st;

/**
 * @brief 解码线程主体：等一包 → 拷出 → 解码 → 交付。
 *
 * 包在锁内拷到栈上再出锁解码，锁只覆盖队列操作。解码失败丢这一包继续下一
 * 包，不重同步——Opus 每包自包含，最坏哑 10ms。
 *
 * @note 全程只用进入时分配的固定缓冲，循环内零分配。
 */
void DecodeLoop() {
  DS5DUMMY_LOG(INFO, "[decoder] Decode thread started");

  // 480 帧单声道，线程生命周期内复用。
  std::array<float, core::protocol::kOpusFrameSamples> pcm{};

  while (st.running) {
    Packet packet{};
    {
      std::unique_lock<std::mutex> lock(st.mutex);
      // 谓词双出口：有包可解，或者 Stop() 要求停机。
      st.queue_cv.wait(lock, [] { return !st.queue.empty() || !st.running; });
      if (!st.running) break;
      packet = st.queue.front();
      st.queue.pop_front();
    }

    const int decoded = opus_decode_float(
        st.opus_decoder, packet.data(),
        static_cast<opus_int32>(packet.size()), pcm.data(),
        static_cast<int>(core::protocol::kOpusFrameSamples), 0);
    if (decoded < 0) {
      DS5DUMMY_LOG(WARNING, "[decoder] Opus decode failed: {}",
            opus_strerror(decoded));
      continue;
    }

    // 一包一次交付：正常 decoded == 480；出口拿到的是这 10ms 的 PCM。
    st.output(pcm.data(), static_cast<uint32_t>(decoded));
  }

  DS5DUMMY_LOG(INFO, "[decoder] Decode thread exited");
}

}  // namespace

bool Init(PcmSink output) {
  if (!output) {
    DS5DUMMY_LOG(ERROR,
          "[decoder] No PCM sink — decoded microphone data would have nowhere "
          "to go");
    return false;
  }
  // 起线程前装好：解码线程一跑起来就要用它交付。
  st.output = std::move(output);

  st.queue.set_capacity(kQueueMax);
  // 上一轮停线时剩下的包不带进新会话：解码器状态已经重建，旧包只能解出
  // 断档的声音。
  st.queue.clear();

  // 单声道 48kHz：手柄上行麦克风的固有格式，不随配置变化。
  int error = OPUS_OK;
  st.opus_decoder = opus_decoder_create(
      static_cast<opus_int32>(core::protocol::kOpusSampleRate),
      static_cast<int>(core::protocol::kMicChannels), &error);
  if (error != OPUS_OK || !st.opus_decoder) {
    DS5DUMMY_LOG(ERROR, "[decoder] Failed to create Opus decoder: {}",
          opus_strerror(error));
    return false;
  }

  // 全部就绪才放线程：running 先置 true，线程首行检查时闸门已开。
  st.running = true;
  st.decode_thread = std::thread(DecodeLoop);

  DS5DUMMY_LOG(INFO, "[decoder] Initialized: {} Hz x {} channel, {} frames/packet",
        core::protocol::kOpusSampleRate, core::protocol::kMicChannels,
        core::protocol::kOpusFrameSamples);
  return true;
}

void PushPacket(const uint8_t* packet, size_t len) {
  // 跑在 bt 读线程上：未 Init / 已 Stop、空指针、残缺帧都静默丢弃——这里
  // 每 10ms 来一次，记日志会淹没传输线程的输出。
  if (!st.running || packet == nullptr || len < core::protocol::kMicOpusSize) {
    return;
  }

  Packet frame{};
  std::memcpy(frame.data(), packet, frame.size());

  {
    std::lock_guard<std::mutex> lock(st.mutex);
    // 环满时 circular_buffer 覆盖最旧的一包（丢旧保新，见 kQueueMax）；
    // 整包插入保证队列里不会出现半包。
    st.queue.push_back(frame);
  }
  st.queue_cv.notify_one();
}

void Stop() {
  if (st.running) {
    st.running = false;
    st.queue_cv.notify_all();  // 叫醒可能卡在 wait 谓词上的线程走停机出口
  }

  if (st.decode_thread.joinable()) {
    st.decode_thread.join();
    DS5DUMMY_LOG(INFO, "[decoder] Stopped");
  }

  // 不在上面早退：Init 半途失败时线程没起来，但出口可能已经装好、解码器
  // 可能建了一半。join 之后才动 output，最后一次交付还用它。
  if (st.opus_decoder) {
    opus_decoder_destroy(st.opus_decoder);
    st.opus_decoder = nullptr;
  }
  {
    // 与在途的 PushPacket 互斥：running 检查之后它可能已经拿到锁。
    std::lock_guard<std::mutex> lock(st.mutex);
    st.queue.clear();
  }
  st.output = nullptr;
}

}  // namespace decoder
}  // namespace audio
