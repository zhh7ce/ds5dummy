// audio::encoder 的出包边界测试。
//
// 不需要 PipeWire、不需要手柄：直接从 process_frame 灌 4 通道交织 float，检查
// "攒够一段才交付、只取整数段"的节奏，和段内 [64B 触觉 | 200B 喇叭] 的几何。
#include "audio/audio_encoder.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/protocol/audio_format.h"
#include "loguru.hpp"

namespace {

namespace encoder = audio::encoder;

// encoder 内部的段几何：一段 = 512 输入帧 @48kHz ≈ 10.7ms。
constexpr uint32_t kSegmentFrames = 512;

// 触觉两路都是常量 1.0f，量化后必然是 127 = 0x7f：段头 64B 全填满才说明
// 512 帧 → 32 帧 × 2 声道 = core::protocol::kHapticsSize 这条整除关系还成立。
constexpr uint8_t kHapticsFull = 127;
static_assert(static_cast<size_t>(kSegmentFrames) *
                      core::protocol::kHapticsSampleRate / core::protocol::kOpusSampleRate *
                      encoder::kEncodedChannels ==
                  core::protocol::kHapticsSize,
              "一段触觉输入不再正好填满 HAPTICS_SIZE，段内几何需要补零/carry");

/// 一次交付的副本：回调期间指针有效，测试线程要留就自己拷（契约如此）。
struct CapturedSegment {
  std::array<uint8_t, core::protocol::kHapticsSize> haptics{};
  std::array<uint8_t, core::protocol::kSpeakerSize> speaker{};
};

/// 编码线程写入、测试线程读取的收集器。
class Collector {
 public:
  void operator()(const uint8_t* haptics, const uint8_t* speaker) {
    CapturedSegment seg;
    std::memcpy(seg.haptics.data(), haptics, seg.haptics.size());
    std::memcpy(seg.speaker.data(), speaker, seg.speaker.size());
    {
      std::lock_guard<std::mutex> lk(mu_);
      segments_.push_back(seg);
    }
    cv_.notify_all();
  }

  bool wait_for(size_t count, int timeout_ms = 1000) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&] { return segments_.size() >= count; });
  }

  size_t count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return segments_.size();
  }

  std::vector<CapturedSegment> snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return segments_;
  }

  /// 用 collector 作 SegmentSink 交给 init()（同形 std::function，直接构造）。
  encoder::SegmentSink sink() {
    return [this](const uint8_t* h, const uint8_t* s) { (*this)(h, s); };
  }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<CapturedSegment> segments_;
};

/// 造一批 4
/// 通道交织样本：喇叭侧给个非静音的交替值，触觉侧默认满幅（量化后必然非零）。
/// haptics_value
/// 可指定，用来造"段与段之间触觉跳变"，看新段的触觉半有没有被上段的样本污染。
std::vector<float> make_frames(uint32_t frames, float haptics_value = 1.0f) {
  std::vector<float> buf(frames * 4);
  for (uint32_t f = 0; f < frames; ++f) {
    buf[f * 4 + 0] = (f % 2 == 0) ? 0.5f : -0.5f;  // ch0 FL → Opus
    buf[f * 4 + 1] = (f % 2 == 0) ? -0.5f : 0.5f;  // ch1 FR → Opus
    buf[f * 4 + 2] = haptics_value;                // ch2 RL → 触觉
    buf[f * 4 + 3] = haptics_value;                // ch3 RR → 触觉
  }
  return buf;
}

/// 灌 n 段所需的输入，每次一批 kSegmentFrames
/// 帧。喂得比编码线程快时，环里会攒好几段，
/// 编码线程一轮取一段连着处理——只关心总段数的用例可以这么喂。
void feed(size_t segments) {
  const std::vector<float> frames = make_frames(kSegmentFrames);
  for (size_t i = 0; i < segments; ++i) {
    encoder::ProcessFrame(frames.data(), kSegmentFrames);
  }
}

/// 喂一批并等它交付完：一段输入 = 一段产出，段内边界才是可复现的。
void feed_one_batch(Collector& collector, uint32_t frames = kSegmentFrames,
                    float haptics_value = 1.0f) {
  const std::vector<float> data = make_frames(frames, haptics_value);
  const size_t expected = collector.count() + 1;
  encoder::ProcessFrame(data.data(), frames);
  ASSERT_TRUE(collector.wait_for(expected, 1000))
      << "只收到 " << collector.count() << " 段，期望 " << expected;
}

class AudioEncoder : public ::testing::Test {
 protected:
  void SetUp() override {
    // 单实例状态：上一例留下的线程必须先收干净，否则交付会串到本例的
    // collector。
    encoder::Stop();
  }
  void TearDown() override { encoder::Stop(); }
};

TEST_F(AudioEncoder, InitRejectsEmptySink) {
  EXPECT_FALSE(encoder::Init(nullptr, 1.0f));

  // 没 init 成功就灌样本：不该产出，也不该崩。
  const std::vector<float> frames = make_frames(kSegmentFrames);
  encoder::ProcessFrame(frames.data(), kSegmentFrames);
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));
  EXPECT_EQ(0u, collector.count()) << "空出口那次 init 留了东西在环里";
}

TEST_F(AudioEncoder, DeliversOneSegmentPerInputBatch) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));

  feed(3);
  ASSERT_TRUE(collector.wait_for(3)) << "只收到 " << collector.count() << " 段";

  // 攒够才交：3 批输入 = 3 段，不该有第 4 段凭空出现。
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(3u, collector.count());
}

TEST_F(AudioEncoder, BelowOneBatchNothingIsDelivered) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));

  const std::vector<float> frames = make_frames(kSegmentFrames / 2);
  encoder::ProcessFrame(frames.data(), kSegmentFrames / 2);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(0u, collector.count()) << "不够一段的输入被交付了";

  // 补齐后必须能出来：说明那半批留在输入环里攒着，而不是被丢了。
  const std::vector<float> rest = make_frames(kSegmentFrames / 2);
  encoder::ProcessFrame(rest.data(), kSegmentFrames / 2);
  EXPECT_TRUE(collector.wait_for(1));
}

TEST_F(AudioEncoder, EverySegmentFillsHapticsExactly) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));

  for (int i = 0; i < 3; ++i) {
    feed_one_batch(collector);
  }

  // 一段 = 512 输入帧 = 32 触觉帧 × 2 声道 = 64B，正好填满：不该有补零，
  // 也不该有留给下一段的余量。第 1 段和后面几段的形状完全一致。
  for (const auto& seg : collector.snapshot()) {
    for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
      EXPECT_EQ(kHapticsFull, seg.haptics[i])
          << "haptics[" << i << "] 没被真实数据填满";
    }
  }
}

TEST_F(AudioEncoder, HapticsGainScalesBeforeQuantisation) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 0.5f));

  // 满幅常量输入 × 0.5 = 127 → 63.5，截断后 63。三处都必须在同一条链上
  // 对上：增益乘在量化前、量化是截断、常量输入经 SRC 后仍是常量。
  feed_one_batch(collector, kSegmentFrames, 1.0f);

  const auto segments = collector.snapshot();
  ASSERT_EQ(1u, segments.size());
  for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
    EXPECT_EQ(63, segments[0].haptics[i]) << "haptics[" << i << "] 未按增益缩放";
  }
}

TEST_F(AudioEncoder, ZeroHapticsGainSilencesTheSegment) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 0.0f));

  feed_one_batch(collector, kSegmentFrames, 1.0f);

  const auto segments = collector.snapshot();
  ASSERT_EQ(1u, segments.size());
  // 增益 0 是用户显式关触觉：整半段必须是静音，不能靠 clamp 削出来。
  for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
    EXPECT_EQ(0, segments[0].haptics[i]) << "haptics[" << i << "] 没被增益静音";
  }
}

TEST_F(AudioEncoder, HapticsCarriesNothingIntoNextSegment) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));

  feed_one_batch(collector, kSegmentFrames, 1.0f);  // 第 1 段：触觉满幅
  for (int i = 0; i < 3; ++i) {
    feed_one_batch(collector, kSegmentFrames, 0.0f);  // 其后三段：触觉静音
  }

  const auto segments = collector.snapshot();
  ASSERT_EQ(4u, segments.size());
  for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
    EXPECT_EQ(kHapticsFull, segments[0].haptics[i]);
  }

  // 跳变处允许 SRC_LINEAR 固有的群延迟（输出帧 k 由输入 k*16-1 与 k*16
  // 插值而来，延迟为 2 个触觉帧 = 4
  // 字节），这是重采样器的相位，不是段间结转。
  for (size_t i = 4; i < core::protocol::kHapticsSize; ++i) {
    EXPECT_EQ(0, segments[1].haptics[i])
        << "haptics[" << i << "] 是上一段带过来的";
  }
  // 真·结转会一路往下滚：第 3、4 段必须一个字节都不剩。
  for (size_t s = 2; s < segments.size(); ++s) {
    for (size_t i = 0; i < core::protocol::kHapticsSize; ++i) {
      EXPECT_EQ(0, segments[s].haptics[i])
          << "第 " << s + 1 << " 段仍包含上一段样本";
    }
  }
}

TEST_F(AudioEncoder, PartialSegmentWaitsInTheRingNotInLocalStorage) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));

  // 1.5 段输入：只该出一段，多出的 256
  // 帧留在输入环里。旧实现一次取空整环、把零头
  // 攒在编码线程的本地缓冲，那段零头要等环里再次攒够 512 帧才有机会被处理——
  // 也就是整整慢一个周期。
  const std::vector<float> whole = make_frames(kSegmentFrames);
  const std::vector<float> half = make_frames(kSegmentFrames / 2);
  encoder::ProcessFrame(whole.data(), kSegmentFrames);
  encoder::ProcessFrame(half.data(), kSegmentFrames / 2);
  ASSERT_TRUE(collector.wait_for(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(1u, collector.count()) << "不够一段的输入被交付了";

  // 再补 256 帧：环里的零头立刻凑成第二段，不需要先攒满一整段新输入。
  encoder::ProcessFrame(half.data(), kSegmentFrames / 2);
  EXPECT_TRUE(collector.wait_for(2, 200)) << "环里的零头没被下一批接上";
}

TEST_F(AudioEncoder, SpeakerHalfIsAFullCbrPacket) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));

  feed(2);
  ASSERT_TRUE(collector.wait_for(2));

  // CBR 160kbps × 10ms = 整 200 字节入段，不按 opus 返回长度截断。
  for (const auto& seg : collector.snapshot()) {
    size_t non_zero = 0;
    for (const uint8_t b : seg.speaker) {
      if (b != 0) ++non_zero;
    }
    EXPECT_GT(non_zero, 100u) << "喇叭半段几乎全零，Opus 没产出";
  }
}

TEST_F(AudioEncoder, StopIsIdempotentAndSilent) {
  Collector collector;
  ASSERT_TRUE(encoder::Init(collector.sink(), 1.0f));
  feed(1);
  ASSERT_TRUE(collector.wait_for(1));

  encoder::Stop();
  encoder::Stop();  // 重复 stop 安全

  // 停线后灌样本不该产出，也不该崩（RT 侧靠 process_frame 的 running
  // 闸门挡住）。
  const std::vector<float> frames = make_frames(kSegmentFrames);
  encoder::ProcessFrame(frames.data(), kSegmentFrames);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(1u, collector.count()) << "停线后还在出段";

  // 重启要用新的出口：旧的 lambda 已在 join 后被清掉。
  Collector next;
  ASSERT_TRUE(encoder::Init(next.sink(), 1.0f));
  feed(1);
  EXPECT_TRUE(next.wait_for(1));
}

TEST_F(AudioEncoder, StaleHalfBatchIsNotCarriedIntoNewSession) {
  Collector first;
  ASSERT_TRUE(encoder::Init(first.sink(), 1.0f));

  // 半批输入出不了段，会留在输入环里；停线时没人排空它。
  const std::vector<float> half = make_frames(kSegmentFrames / 2);
  encoder::ProcessFrame(half.data(), kSegmentFrames / 2);
  encoder::Stop();

  Collector second;
  ASSERT_TRUE(encoder::Init(second.sink(), 1.0f));
  // 新会话再喂半批：若旧残留还连着，这里就会凑成一段交出去（不该发生）。
  encoder::ProcessFrame(half.data(), kSegmentFrames / 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(0u, second.count()) << "上一次停线前的半批音频被当成了新数据";

  // 补齐一批后照常出段。
  feed(1);
  EXPECT_TRUE(second.wait_for(1));
}

}  // namespace
