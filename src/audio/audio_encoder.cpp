/**
 * @file audio_encoder.cpp
 * @brief audio::encoder 实现：输入环、重采样、Opus 编码、触觉量化与编码线程
 *
 * 输入环由 PipeWire RT 线程写入、编码线程按段读取，两者之间只有两个原子索引
 * 加一个门铃——RT 写方不碰任何锁。每段固定 512 输入帧，拆成喇叭(ch0/1)与
 * 触觉(ch2/3)两路，分别重采样后做 Opus 编码或 int8 量化。段内布局固定为
 * `[64B 触觉][200B 喇叭]`，两路产出都由编译期几何精确填满（见 static_assert），
 * 交付路径上没有补零和 carry。
 */

#include "audio/audio_encoder.h"

#include <opus.h>
#include <samplerate.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "audio/audio_ring.h"
#include "core/debug.h"
#include "loguru.hpp"

namespace audio {
namespace encoder {
namespace {

/**
 * @brief 一段 = 一批 = 一个 Opus 包：输入侧只有这一种批尺寸。
 *
 * 48000→3000 是精确的 1/16，48000→48000 段内喇叭路只差 512→480 一次重采样，
 * 所以按"一段一批"切分后两路都整除：触觉 512 帧 → 32 帧 × 2 声道 = 64B =
 * core::protocol::kHapticsSize；喇叭 512 帧 → 480 帧 → CBR 200B =
 * core::protocol::kSpeakerSize。段内既不需要补零，也不需要在段间留余料。
 */
constexpr size_t kSegmentInputFrames =
    512;  ///< 一段的输入帧数（48kHz，≈10.67ms）

constexpr size_t kAudioOutputFrames =
    core::protocol::kOpusFrameSamples;  ///< 喇叭路每段产出帧数 = 一包
constexpr size_t kHapticsOutputFrames =
    kSegmentInputFrames * core::protocol::kHapticsSampleRate /
    core::protocol::kOpusSampleRate;  ///< 触觉路每段产出帧数：512/16 = 32

constexpr float kAudioResampleRatio =
    static_cast<float>(core::protocol::kOpusFrameSamples) /
    kSegmentInputFrames;  ///< 喇叭路：480/512
constexpr float kHapticsResampleRatio =
    static_cast<float>(core::protocol::kHapticsSampleRate) /
    core::protocol::kOpusSampleRate;  ///< 触觉路：3000/48000 = 1/16

// 段几何必须是精确整除，否则"无补零、无 carry"的整套推理不成立。
static_assert(kHapticsOutputFrames * kEncodedChannels ==
                  core::protocol::kHapticsSize,
              "触觉一段的产出必须正好填满 HAPTICS_SIZE，否则又需要补零/carry");
static_assert(kAudioOutputFrames == 480,
              "喇叭一段的产出必须正好是一个 Opus 10ms 包");

/**
 * @brief 输入侧与码率的常量（输出几何与输入通道数见 audio_encoder.h）。
 *
 * 批尺寸和两个 ratio 全是编译期常量，采样率/通道数必须与它们一致，所以
 * 刻意不做成 Init 的参数——传进别的采样率只会让批边界整体错位，参数化是
 * 假灵活性。
 */
constexpr uint32_t kOpusBitrateBps =
    200 * 8 * 100;  ///< CBR 码率：200B/包 × 8bit × 100包/秒，恰让每包写满 kSpeakerSize

// 一段需要的输入量，和拆声道后每路的输入/输出量：门限、拷贝长度、重采样入参都从这里取。
constexpr size_t kSegmentInputFloats = kSegmentInputFrames * kInputChannels;
constexpr size_t kSegmentInputBytes = kSegmentInputFloats * sizeof(float);
constexpr size_t kPathInputFloats = kSegmentInputFrames * kEncodedChannels;
constexpr size_t kAudioOutputFloats = kAudioOutputFrames * kEncodedChannels;
constexpr size_t kHapticsOutputFloats = kHapticsOutputFrames * kEncodedChannels;

/**
 * @brief 输入环容量：32 段 ≈ 341ms。
 *
 * 下限由单次写入量决定：`ProcessFrame` 一次收到的帧数 = 图的一次 quantum，系统
 * 硬顶 `quantum-limit` 是 8192 帧 = 16 段 = 131072 B。容量等于它只能保证"一次
 * 最大回调不自覆盖"，连来两次或读方晚一拍就会丢，所以取 2 倍 = 32 段。
 *
 * 满了丢最旧——延迟优先：宁可丢旧样本也不让新样本等着。
 *
 * 环元素是"一帧"（4 声道 × 4 B = 16 B），容量与一切推进量都必须是它的整数倍，
 * 否则丢旧之后读起点会落在半个帧上，四个声道整体轮转。
 */
constexpr size_t kMaxInputSegments = 32;
constexpr size_t kMaxInputFloats = kMaxInputSegments * kSegmentInputFloats;
constexpr size_t kRingBytes = kMaxInputFloats * sizeof(float);
constexpr size_t kFrameBytes = kInputChannels * sizeof(float);

static_assert(kRingBytes % kFrameBytes == 0,
              "ring capacity must hold whole 4-channel frames");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "doorbell must be lock-free");

/**
 * @brief 进程内单实例状态。
 *
 * `ring` 由 PipeWire RT 线程写、编码线程读（严格单生产者单消费者）。`seq` 是
 * 唤醒用的门铃，用 alignas 独占一条缓存行，避免与环内的索引互相打掉。
 * `output` 在起线程前安装、join 之后清空，运行期只读。
 */
struct State {
  OpusEncoder* opus_encoder = nullptr;     ///< Opus 立体声编码器
  SRC_STATE* audio_src_state = nullptr;    ///< 喇叭路重采样 (512 → 480 帧)
  SRC_STATE* haptics_src_state = nullptr;  ///< 触觉路重采样 (512 → 32 帧)

  std::thread encode_thread;  ///< EncodeLoop 的载体

  AudioRing ring{kRingBytes};  ///< 4 通道交织字节环，32 段
  alignas(64) std::atomic<uint64_t> seq{0};       ///< 写入门铃，编码线程在它上面 wait

  std::atomic<bool> running{false};            ///< 编码线程运行标志
  std::atomic<bool> discard_requested{false};  ///< 清环请求，只由编码线程消费

  SegmentSink output;  ///< 攒够一段时的去处（Init 装填，Stop join 后清空）

  float haptics_gain = 1.0f;  ///< 触觉振幅倍率（Init 装填，运行期只读）
} st;

/**
 * @brief 一批定长重采样：input_frames 帧进，写满 output 的 output_frames 帧。
 *
 * 喇叭路 (512→480) 与触觉路 (512→32) 只差 ratio 和 SRC_STATE，共用本函数。
 * 批边界由段几何决定、调用方一次交出正好一批，所以这里没有"实际用了多少
 * 帧"的消费计数，也不erase残留。
 *
 * @param input         交织的输入样本，长度恰好 input_frames × kEncodedChannels。
 * @param output        输出落点，容量恰好 output_frames × kEncodedChannels。
 * @param src_state     本路专用的 libsamplerate 状态（两路不可共用：内部
 *                      滤波器历史互不相干）。
 * @param input_frames  本批输入帧数。
 * @param output_frames 本批输出帧数。
 * @param ratio         输出/输入帧率比。
 * @param label         出错日志里的路名。
 * @note src_process 出错只记日志、不返回状态：产出区已预清零，坏一批最多
 *       哑一批，不会把上一批的样本混进这一批。
 */
void Resample(const float* input, float* output, SRC_STATE* src_state,
              size_t input_frames, size_t output_frames, float ratio,
              const char* label) {
  // 产出区先清零：src_process 出错时不会写满，留着上一次的样本就是让
  // 一段音频混进这一段。
  std::fill_n(output, output_frames * kEncodedChannels, 0.0f);

  // 全字段零初始化：SRC_DATA 还有 latency/se_ptr 等成员，未初始化的值会
  // 作为参数传给 libsamplerate。
  SRC_DATA src_data = {};
  src_data.data_in = input;
  src_data.data_out = output;
  src_data.input_frames = static_cast<long>(
      input_frames);  // NOLINT: libsamplerate 用 long 而非 size_t
  src_data.output_frames = static_cast<long>(output_frames);  // NOLINT: 同上
  src_data.src_ratio = ratio;
  src_data.end_of_input = 0;  // 后面还有更多数据：保持流式连续性

  const int error = src_process(src_state, &src_data);
  if (error != 0) {
    DS5DUMMY_LOG(ERROR, "[encoder] {} SRC process failed: {}", label,
          src_strerror(error));
  }
}

/**
 * @brief float [-1,1] → int8 [-128,127]（乘 127 × 增益后夹取、截断），
 *        逐字节写进段缓冲。
 *
 * 截断而非四舍五入：保持与手柄可接受的触觉包量化行为一致，精度损失对
 * 触觉波形无感。输出写的是 int8 的位型（负数补码）按字节存进 uint8_t
 * ——0x36 触觉半段就是这个布局。
 *
 * @param input  量化源，交织后的浮点样本。
 * @param count  要量化的样本个数（元素数，非字节数的两倍之类——就是 float 个数）。
 * @param output 落点，容量至少 count 字节。
 * @param gain   振幅倍率；0 静音、1 原样、>1 放大。放大后超幅的部分由
 *               下面的 clamp 削平，不会回卷。
 */
void ConvertToInt8(const float* input, size_t count, uint8_t* output,
                   float gain) {
  for (size_t i = 0; i < count; ++i) {
    const float val = input[i] * 127.0f * gain;  // 满幅 float → int8 满幅
    // clamp 兜住超出 [-1,1] 的输入（PipeWire 不保证客户端守幅）；
    // 先转 int8_t 再按 uint8_t 存，保住的正是补码位型。
    output[i] = static_cast<uint8_t>(
        static_cast<int8_t>(std::clamp(val, -128.0f, 127.0f)));
  }
}

/**
 * @brief 从环里取一整段交织样本；不够一段就什么都不动。
 *
 * 先看可读量再读，是因为 Read 会读走"能读的部分"：零头读出来既凑不成一段，
 * 还会让下一段的起点偏离段边界。溢出与索引环过头两种异常由 Read 内部纠正。
 *
 * @param out 目标缓冲，容量恰好 kSegmentInputFloats 个 float。
 * @return 取到一整段 true；不足一段 false（零头仍留在环里，等下一批接上）。
 * @note 只由编码线程调用——这里会写读索引。
 */
bool TakeSegment(float* out) {
  if (st.ring.Readable() < kSegmentInputBytes) return false;
  return st.ring.Read(out, kSegmentInputBytes) == kSegmentInputBytes;
}

/**
 * @brief 整段输入是否数字静音：4 个通道全部正好是零。
 *
 * 判 4 个通道而不是只看喇叭两路：ch2/3 是触觉，游戏完全可能只震动不出声，
 * 只看喇叭会把纯触觉段当成静音丢掉。
 *
 * 用 `== 0.0f` 而不是按字节比较：`-0.0f` 的位模式是 0x80000000，memcmp 会把它
 * 判成非静音；IEEE 下 `-0.0f == 0.0f` 为真，比较天然覆盖。
 *
 * @param interleaved 刚读出的一段交织样本，长度恰为 kSegmentInputFloats。
 * @return 全零 true。
 * @note 遇到第一个非零就退出，有声段几乎零开销；只有真静音才走满 2048 次比较。
 */
bool SegmentIsSilent(const float* interleaved) {
  return std::all_of(interleaved, interleaved + kSegmentInputFloats,
                     [](float value) { return value == 0.0f; });
}

/**
 * @brief 把两路重采样器与 Opus 编码器的状态清干净。
 *
 * 三者都是有状态的：SRC 带着滤波历史，Opus 带着上一帧的预测器。丢掉一段静音
 * 之后若不复位，恢复时的第一帧会拿静音之前的样本做插值和预测，带一次可闻的
 * 瞬态。复位就在判定静音、丢弃这一段的当时做——状态直接归零，之后恢复时从
 * 干净状态开始。成本是微秒级。
 */
void ResetCodecState() {
  if (st.audio_src_state != nullptr) (void)src_reset(st.audio_src_state);
  if (st.haptics_src_state != nullptr) (void)src_reset(st.haptics_src_state);
  if (st.opus_encoder != nullptr) {
    (void)opus_encoder_ctl(st.opus_encoder, OPUS_RESET_STATE);
  }
}

/**
 * @brief 编码线程主体：一次取满一段 → 拆声道 → 重采样 → Opus 编码 /
 *        int8 量化 → 交付。
 *
 * 唤醒与消费的门限都是"够不够出一段"（512 输入帧 ≈ 10.7ms），且只取整数
 * 段：凑不满下一段的输入留在环里，等下一次跨门槛一并带走——环本身携带
 * 零头，就不需要本地 staging 缓冲和跨批 carry。批尺寸 = 一段一批后
 * （见 kSegmentInputFrames），两路都精确整除，交付就是循环体本身。
 *
 * @note 线程全程只用进入时分配的固定缓冲（std::array），循环内零分配。
 */
void EncodeLoop() {
  DS5DUMMY_LOG(INFO, "[encoder] Encode thread started");

  // 一段的全部工作缓冲，线程生命周期内复用到固定容量，循环里只有读写没有分配。
  std::array<float, kSegmentInputFloats>
      segment_input{};                                // 512 帧 × 4 通道交织
  std::array<float, kPathInputFloats> audio_in{};     // ch0/1
  std::array<float, kPathInputFloats> haptics_in{};   // ch2/3
  std::array<float, kAudioOutputFloats> audio_out{};  // 480 帧 × 2
  std::array<float, kHapticsOutputFloats>
      haptics_out{};                            // 32 帧 × 2 = 64 个样本
  std::array<uint8_t, kSegmentSize> segment{};  // [64B 触觉 | 200B 喇叭]

  while (st.running.load(std::memory_order_acquire)) {
    // 先记门铃、再取数据：顺序反过来会丢唤醒——生产者在"发现环空"与"记录
    // 门铃"之间写入时，新值已被算进 seen，wait 会认为没人动过而睡死。
    const uint64_t seen = st.seq.load(std::memory_order_acquire);

    // 清环请求（重建 / 复位）只由本线程执行——读索引归本线程所有。
    if (st.discard_requested.exchange(false, std::memory_order_acquire)) {
      st.ring.Clear();
    }

    // 只取一段的量：零头留在环里，不进任何本地缓冲。
    if (!TakeSegment(segment_input.data())) {
      // 不够一段就等门铃；Stop() 会加一并发 notify_all 把它叫醒。
      st.seq.wait(seen, std::memory_order_acquire);
      continue;
    }

    // 整段静音：主机静音时数据照来（实测），内容全零，编出来就是静音包——
    // 直接丢，不拆声道、不重采样、不编码、不交付。丢完之后两路 SRC 与 Opus
    // 的历史停在静音之前，顺手复位，免得恢复时第一帧拿旧历史做插值。
    if (SegmentIsSilent(segment_input.data())) {
      ResetCodecState();
      continue;
    }

    // 诊断：每真的编一段打一行。静音段在上面已经丢掉了，所以这行出现的频率
    // 就是实际编码频率。只在 DS5DUMMY_DEBUG_LOG 构建里编译。
    DS5DUMMY_DLOG(1, "[encoder] encode request: frames={} floats={}",
                  kSegmentInputFrames, kSegmentInputFloats);

    // 拆声道：一段 512 帧，两路各 512 帧立体声。
    for (size_t i = 0; i < kSegmentInputFrames; ++i) {
      const float* frame = segment_input.data() + i * kInputChannels;
      audio_in[i * kEncodedChannels] = frame[0];        // Left (ch0)
      audio_in[i * kEncodedChannels + 1] = frame[1];    // Right (ch1)
      haptics_in[i * kEncodedChannels] = frame[2];      // Haptics Left (ch2)
      haptics_in[i * kEncodedChannels + 1] = frame[3];  // Haptics Right (ch3)
    }

    // 段缓冲先清零：正常路径两路都正好填满自己的半段，一个字节都不剩；
    // 万一 libsamplerate 少产或 Opus 包短于 200B，兜底的也是零而不是
    // 上一段的残留。
    segment.fill(0);

    // 触觉：512 → 32 帧，量化后正好铺满段头 64B。
    Resample(haptics_in.data(), haptics_out.data(), st.haptics_src_state,
             kSegmentInputFrames, kHapticsOutputFrames, kHapticsResampleRatio,
             "Haptics");
    ConvertToInt8(haptics_out.data(), kHapticsOutputFloats, segment.data(),
                  st.haptics_gain);

    // 喇叭：重采样到 480 帧 → 一个 CBR Opus 包，直接写进段的 [64..264)。
    Resample(audio_in.data(), audio_out.data(), st.audio_src_state,
             kSegmentInputFrames, kAudioOutputFrames, kAudioResampleRatio,
             "Audio");
    const int encoded_bytes = opus_encode_float(
        st.opus_encoder, audio_out.data(),
        static_cast<opus_int32>(kAudioOutputFrames),
        segment.data() + core::protocol::kHapticsSize,
        static_cast<opus_int32>(core::protocol::kSpeakerSize));
    if (encoded_bytes <= 0) {
      // 编码失败不丢整段：喇叭半段保持 fill(0) 的静音继续交付，节奏不断，
      // 手柄侧最多听到一帧静音。
      DS5DUMMY_LOG(ERROR, "[encoder] Opus encode failed: {}",
            opus_strerror(encoded_bytes));
    }

    // 一段一次交付。两个指针都指 segment，回调返回即失效；st.output 由
    // Init 在起线程前装好、Stop join 后才清空，此处必非空。
    st.output(segment.data(), segment.data() + core::protocol::kHapticsSize);
  }

  DS5DUMMY_LOG(INFO, "[encoder] Encode thread exited");
}

/**
 * @brief 释放 Opus 编码器与两路 SRC state，逐项判空并置空。
 *
 * Init() 的失败分支和 Stop() 共用这一份：init 半途失败时不回收，前一步
 * 已经建好的对象就永久泄漏——本模块是单实例 namespace 风格，没有对象
 * 析构来兜底。
 */
void ReleaseCodec() {
  if (st.opus_encoder) {
    opus_encoder_destroy(st.opus_encoder);
    st.opus_encoder = nullptr;
  }
  if (st.audio_src_state) {
    src_delete(st.audio_src_state);
    st.audio_src_state = nullptr;
  }
  if (st.haptics_src_state) {
    src_delete(st.haptics_src_state);
    st.haptics_src_state = nullptr;
  }
}

}  // namespace

bool Init(SegmentSink output, float haptics_gain) {
  if (!output) {
    DS5DUMMY_LOG(ERROR,
          "[encoder] No segment sink — encoded data would have nowhere to go");
    return false;
  }
  // 起线程前装好：编码线程一跑起来就要能用它交付，没有"先跑几拍再补接线"
  // 的窗口。增益同样只在这里写一次，之后编码线程只读。
  st.output = std::move(output);
  st.haptics_gain = haptics_gain;

  // 丢掉上一会话的残料：此刻没有任何线程能碰它——编码线程还没创建，sink 的
  // RT 回调还没注册（sink::Start 在 encoder::Init 之后）。worker 会在同一进程里
  // Stop→Start 重试音频，残料因此不会跨到新会话。
  st.ring.Clear();
  // 运行期那次清理由 ResetInputBuffer() 置标志、由编码线程执行；这里把可能
  // 残留的请求一并清掉，免得新会话第一段被上一次的请求误吞。
  st.discard_requested.store(false, std::memory_order_relaxed);

  // Opus 编码器：48kHz 立体声；RESTRICTED_LOWDELAY 不引入任何预听延迟，
  // 本链路每一帧都要尽快上线。
  int error = OPUS_OK;
  st.opus_encoder = opus_encoder_create(
      static_cast<opus_int32>(core::protocol::kOpusSampleRate),
      static_cast<int>(kEncodedChannels), OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      &error);

  if (error != OPUS_OK || !st.opus_encoder) {
    DS5DUMMY_LOG(ERROR, "[encoder] Failed to create Opus encoder: {}",
          opus_strerror(error));
    return false;
  }

  // 编码参数：固定 10ms 帧（匹配段几何）、纯 CBR 200B/包（匹配
  // kSpeakerSize 定长槽）、复杂度 0（编码线程只换最低延迟档的算力）。
  opus_encoder_ctl(st.opus_encoder,
                   OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
  opus_encoder_ctl(st.opus_encoder,
                   OPUS_SET_BITRATE(static_cast<int>(kOpusBitrateBps)));
  opus_encoder_ctl(st.opus_encoder, OPUS_SET_VBR(false));
  opus_encoder_ctl(st.opus_encoder, OPUS_SET_COMPLEXITY(0));  // 最低复杂度

  // 喇叭路 SRC：512 → 480 帧。SRC_LINEAR（线性插值）是最便宜的挡位，
  // 通道数按立体声两路。
  int audio_src_error = 0;
  st.audio_src_state =
      src_new(SRC_LINEAR, static_cast<int>(kEncodedChannels), &audio_src_error);
  if (audio_src_error != 0) {
    DS5DUMMY_LOG(ERROR, "[encoder] Failed to create audio SRC state: {}",
          src_strerror(audio_src_error));
    ReleaseCodec();
    return false;
  }

  // 触觉路 SRC：一段一批，512 帧 @48kHz → 32 帧 @3kHz；触觉同样是两路
  // 立体声，与喇叭共用 kEncodedChannels。
  int haptics_src_error = 0;
  st.haptics_src_state = src_new(SRC_LINEAR, static_cast<int>(kEncodedChannels),
                                 &haptics_src_error);
  if (haptics_src_error != 0) {
    DS5DUMMY_LOG(ERROR, "[encoder] Failed to create haptics SRC state: {}",
          src_strerror(haptics_src_error));
    ReleaseCodec();
    return false;
  }

  // 全部对象就绪才放线程：running 先置 true，线程首行检查时闸门已开。
  st.running = true;
  st.encode_thread = std::thread(EncodeLoop);

  DS5DUMMY_LOG(INFO,
        "[encoder] Initialized: audio {} Hz x {}, haptics {} Hz, {} bps, "
        "haptics gain {}",
        core::protocol::kOpusSampleRate, kEncodedChannels,
        core::protocol::kHapticsSampleRate, kOpusBitrateBps,
        st.haptics_gain);

  return true;
}

void ProcessFrame(const float* input, uint32_t frames) {
  // 跑在 PipeWire RT 线程上：静默返回，不记日志——loguru 的格式化、内存
  // 分配和文件写入都不适合出现在实时回调里。running 未置位（未 Init 或
  // 已 Stop）与空指针/零帧一律拒绝。
  if (!st.running.load(std::memory_order_acquire) || input == nullptr ||
      frames == 0) {
    return;
  }

  // 不看剩余空间：写索引前进多少就覆盖多少最旧样本，丢旧由编码线程在下次取
  // 一段时发现。整批写入保证环里不会出现半帧。
  st.ring.Write(input, frames * kFrameBytes);

  // 门限只按"写完之后够不够一整段"判断，不看"之前够不够"。
  //
  // 跨越式判断（before < 段 && after >= 段）在这里有一个会永久卡住的竞态：
  // before 里的读索引可能是旧值——消费者刚把读索引推上去、编码线程还没看见，
  // 于是 before 被抬到门限之上，条件不成立、不按铃，而消费者恰好也在这一刻
  // 判断"不够一段"睡下了。此后读索引不再前进，每次写算出的 before 都 >= 门限，
  // 再也不会按铃，音频就此停住，直到 Stop 或重建。
  //
  // 电平式判断只会多按不会漏按：没攒够一段时不按（稳态下就是每秒约 94 次，
  // 和跨越式一样），攒够了就按——此时消费者本来就在跑，多一次唤醒无害。
  if (st.ring.Readable() >= kSegmentInputBytes) {
    st.seq.fetch_add(1, std::memory_order_release);
    st.seq.notify_one();
  }
}

void ResetInputBuffer() {
  // 只置标志 + 唤醒：发起方（port 的 monitor 线程）不是读索引的所有者，
  // 真正清空由编码线程在自己的循环里执行。
  st.discard_requested.store(true, std::memory_order_release);
  st.seq.fetch_add(1, std::memory_order_release);
  st.seq.notify_one();
}

void Stop() {
  // 门铃值必须变一次：睡在 wait 上的编码线程只在"值变了"时才会醒，光置
  // running 会让 join 永久阻塞。
  if (st.running.exchange(false, std::memory_order_acq_rel)) {
    st.seq.fetch_add(1, std::memory_order_release);
    st.seq.notify_all();
  }

  if (st.encode_thread.joinable()) {
    st.encode_thread.join();
    DS5DUMMY_LOG(INFO, "[encoder] Stopped");
  }

  // 不在上面早退：Init 半途失败时线程根本没起来，但出口已经装好、编解码
  // 对象可能建了一部分。收尾一律走这里，停着的模块不该继续持有调用方的
  // lambda。join 之后才动 output，最后一次交付还用它。
  ReleaseCodec();
  // 环不在这里清：下一次 Init 会把索引归零，两步之间没有任何线程读写它
  // （读方已 join、写方被 running 闸门挡住）。
  st.output = nullptr;
}

}  // namespace encoder
}  // namespace audio
