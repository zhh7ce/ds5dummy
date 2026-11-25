/**
 * @file audio_decoder.cpp
 * @brief audio::decoder 实现：有界包环、解码线程与整包 PCM 交付
 *
 * bt 读线程往环里写整包，解码线程按到达顺序读；两者之间只有两个原子索引加
 * 一个门铃，没有互斥量。每包恒定 480 帧（10ms）单声道，解出即整包交给出口，
 * 不做重采样、不攒批——上行麦克风的端到端延迟就是环里积压的包数。
 */

#include "audio/audio_decoder.h"

#include <opus.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

#include "audio/audio_ring.h"
#include "core/debug.h"
#include "core/protocol/audio_format.h"
#include "loguru.hpp"

namespace audio {
namespace decoder {
namespace {

/**
 * @brief 环容量：8 包 ≈ 80ms。
 *
 * 环里存整包字节（71 B 定长），所以容量必须是 71 的整数倍——71 是质数，取不了
 * 2 的幂；取模由编译器按常量展开，不需要手写掩码。深度 8 包与解码线程的唤醒
 * 节奏同量级：读方被饿住 7 个包周期（70ms）之后才开始丢旧。
 *
 * 远端对齐：解码线程把队列抽干时，8 包 × 480 帧正好填满 source 的 PCM 环
 * （`kRingFrames` = 3840 帧）。那个常量在 audio_source.cpp 里，跨 TU 无法
 * 编译期断言，改这里必须同步改那边。
 */
constexpr size_t kQueueMax = 8;
constexpr size_t kMicOpusSize = core::protocol::kMicOpusSize;
constexpr size_t kRingBytes = kQueueMax * kMicOpusSize;

// 环元素 = 一整包：容量与一切推进量都必须是它的整数倍，否则丢旧之后的读起点
// 会落在包的中间，Opus 解出来就是噪声。
static_assert(kRingBytes % kMicOpusSize == 0,
              "ring capacity must hold whole packets");

// 门铃必须是无锁原子：它挂在等待路径上，若退化成内部加锁，消费者"阻塞零 CPU"
// 与生产者"永不阻塞"两条前提同时失效。
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "doorbell must be lock-free");

/**
 * @brief 进程内单实例状态。
 *
 * `ring` 由 bt 读线程写、解码线程读（严格单生产者单消费者）。`seq` 是唤醒用的
 * 门铃，用 alignas 独占一条缓存行，避免与环内的索引互相打掉。
 */
struct State {
  OpusDecoder* opus_decoder = nullptr;  ///< 单声道 48kHz 解码器
  std::thread decode_thread;            ///< DecodeLoop 的载体

  AudioRing ring{kRingBytes};  ///< 整包字节环，容量 = 8 个 71 字节包
  alignas(64) std::atomic<uint64_t> seq{0};  ///< 写入门铃，解码线程在它上面 wait

  std::atomic<bool> running{false};  ///< 解码线程运行标志

  PcmSink output;  ///< 每包 PCM 的去处（Init 装填，Stop join 后清空）
} st;

/**
 * @brief 解码线程主体：等门铃 → 把能解的包全解掉 → 没活了才睡。
 *
 * 外层循环的**顺序是承重的**：先记下门铃当前值，再去取数据。反过来（先发现环空、
 * 再记门铃、再 wait）会丢唤醒——生产者在两步之间写入时，门铃的新值已经被算进
 * seen，wait 会认为"没人动过"而睡死到下一次写入。
 *
 * 解码失败丢这一包继续下一包，不重同步——Opus 每包自包含，最坏哑 10ms。
 *
 * @note 全程只用进入时分配的固定缓冲，循环内零分配。
 */
void DecodeLoop() {
  DS5DUMMY_LOG(INFO, "[decoder] Decode thread started");

  // 480 帧单声道，线程生命周期内复用。
  std::array<float, core::protocol::kOpusFrameSamples> pcm{};
  std::array<uint8_t, kMicOpusSize> packet{};

  for (;;) {
    const uint64_t seen = st.seq.load(std::memory_order_acquire);
    if (!st.running.load(std::memory_order_acquire)) break;

    bool worked = false;
    for (;;) {
      // 整包才是一个消费单位：不足一包就等下一包。先看可读量再读，是因为
      // Read 会读走"能读的部分"——半包读出来既解不出东西，还会让之后的
      // 读起点落在包中间。
      if (st.ring.Readable() < kMicOpusSize) break;
      const size_t got = st.ring.Read(packet.data(), kMicOpusSize);
      if (got != kMicOpusSize) break;  // 写方只写整包，实际不会发生

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
      worked = true;
    }

    if (!st.running.load(std::memory_order_acquire)) break;
    // 干过活就直接回到顶部再看一眼门铃，别在一批积压上多睡一轮。
    if (!worked) st.seq.wait(seen, std::memory_order_acquire);
  }

  DS5DUMMY_LOG(INFO, "[decoder] Decode thread exited");
}

}  // namespace

bool Init(PcmSink output) {
  if (!output) {
    DS5DUMMY_LOG(ERROR,
                 "[decoder] No PCM sink — decoded microphone data would have "
                 "nowhere to go");
    return false;
  }
  // 起线程前装好：解码线程一跑起来就要用它交付。
  st.output = std::move(output);

  // 丢掉上一会话的残料：worker 会在同一进程里 Stop→Start 重试音频，不解码线程
  // 早已 join、写方被 running 闸门挡着，此刻既没有读者也没有写者，可以直接清。
  st.ring.Clear();

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
  st.running.store(true, std::memory_order_release);
  st.decode_thread = std::thread(DecodeLoop);

  DS5DUMMY_LOG(INFO,
               "[decoder] Initialized: {} Hz x {} channel, {} frames/packet, "
               "{} packets buffered",
               core::protocol::kOpusSampleRate, core::protocol::kMicChannels,
               core::protocol::kOpusFrameSamples, kQueueMax);
  return true;
}

void PushPacket(const uint8_t* packet, size_t len) {
  // 跑在 bt 读线程上：未 Init / 已 Stop、空指针、残缺帧都静默丢弃——这里每
  // 10ms 来一次，记日志会淹没传输线程的输出。
  if (!st.running.load(std::memory_order_acquire) || packet == nullptr ||
      len < kMicOpusSize) {
    return;
  }

  // 不看剩余空间：环满了由读方跳过最旧的一包，写方永不等待、永不丢本批。
  st.ring.Write(packet, kMicOpusSize);

  // 一包就是一个完整的消费单位，不存在"攒不够"的中间态，所以不必节流：
  // 每次写入都可能是消费方正在等的那一包。
  st.seq.fetch_add(1, std::memory_order_release);
  st.seq.notify_one();
}

void Stop() {
  // 门铃值必须变一次：睡在 wait 上的解码线程只在"值变了"时才会醒，光置
  // running 会让 join 永久阻塞。
  if (st.running.exchange(false, std::memory_order_acq_rel)) {
    st.seq.fetch_add(1, std::memory_order_release);
    st.seq.notify_all();
  }

  if (st.decode_thread.joinable()) {
    st.decode_thread.join();
    DS5DUMMY_LOG(INFO, "[decoder] Stopped");
  }

  // 不在上面早退：Init 半途失败时线程没起来，但解码器可能建了一半。join 之后
  // 才动 output，最后一次交付还用它。
  if (st.opus_decoder) {
    opus_decoder_destroy(st.opus_decoder);
    st.opus_decoder = nullptr;
  }
  // 环不在这里清：下一次 Init 会把两个索引归零，而两步之间没有任何线程读写
  // 它（读方已 join、写方被 running 挡住）。
  st.output = nullptr;
}

}  // namespace decoder
}  // namespace audio
