/**
 * @file audio.h
 * @brief PipeWire 音频链的门面：下行 sink→编码，上行解码→虚拟麦克风
 *
 * 依赖方向：audio → core（只有数据头）。本模块只搬运**数据**：
 * - 下行每段给调用方 core::protocol::kHapticsSize 字节触觉 +
 *   core::protocol::kSpeakerSize 字节喇叭，经 Start() 传入的 OutputSink；
 * - 上行每帧给虚拟麦克风 core::protocol::kOpusFrameSamples 帧 PCM，由
 *   OnMicPacket() 推进来的 71 字节 Opus 包解码而来。
 * 0x36 报告怎么拼、什么时候发，是 bridge 的事（bridge/audio_report.h +
 * bridge::OnAudioOutput）；麦克风开关报告同理（bridge::OnMicOpen）。本模块
 * 不认识报告格式，也不持有任何报告缓冲。
 *
 * 装配关系：worker 是调用方，也是唯一的接线处——它在 bt::StartThreads() 之后
 * Start、退出时最先 Stop，所以 audio 是 bridge 的兄弟模块而不是它的下属：
 * 编码线程产出的一切只经 sink 出去、上行 PCM 只经虚拟麦克风出去，消费方
 * （bt 的传输线程）必然已在跑。链路上任何一环失败只损音频，不损 HID。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace audio {

/**
 * @brief 一段编码产出的出口。
 *
 * @param haptics core::protocol::kHapticsSize 字节 int8 触觉包。
 * @param speaker core::protocol::kSpeakerSize 字节 Opus 喇叭包。
 * @details 由编码线程调用：攒够一整段才调一次，节奏 = 段节奏
 *          （512 输入帧 @48kHz ≈ 10.7ms 一段，约 93.75 次/秒）；指针指向的
 *          字节只在调用期间有效——要留就自己拷。
 *
 *          这是一次**直接调用**，不是投递到别的线程：实现方的活儿全部发生
 *          在编码线程的栈上。生产实现 bridge::OnAudioOutput 因此在这条线程上
 *          取状态快照（state_mutex）、拼 398B 报告、再进 bt::PostOutput
 *          （queue_mutex）。本模块把这当成设计事实承认下来：下行链的执行者
 *          就是编码线程，代价是契约硬性——实现必须非阻塞、不自起线程、不
 *          长时间持锁。再加一层"投给别人执行"只会多出一个队列，而背压点
 *          只能有一个（已经在 bt 的有界队列上，满 8 份丢最新）。
 */
using OutputSink =
    std::function<void(const uint8_t* haptics, const uint8_t* speaker)>;

/**
 * @brief 下行 sink 或上行 source 连续恢复失败后的通知。
 *
 * @details 两条流各有一条恢复 monitor，但收同一份 handler：任一方向彻底
 *          恢复失败都意味着整条音频链该交回调用方重建。回调跑在对应流的
 *          恢复线程上；实现方只应设置原子标志或投递事件，不得在回调里直接
 *          调用 audio::Stop()——Stop 要 join 的正是这条线程，回调里等它就是
 *          自己等自己。
 */
using LostHandler = std::function<void()>;

/**
 * @brief 主机侧录音接口开/关的通知。
 *
 * @param open true = 有客户端开始从虚拟麦克风录音（Source 流进入
 *             STREAMING），请把手柄麦克风打开；false = 没人录了（回到
 *             PAUSED），或 Source 正在拆除，请把它关掉。
 * @details 由 source 的 PipeWire 主循环线程调用；audio::Stop() 在拆除
 *          Source 之后也会在调用方线程上补一次 false（前提是拆除前为
 *          true）。实现方应把它转成非阻塞的链路动作——生产实现
 *          bridge::OnMicOpen 往 bt 的发送队列入队一份 0x32 状态报告。
 * @note 同一值可能被重复通知，实现必须自己按状态去重。
 */
using MicOpenHandler = std::function<void(bool open)>;

/**
 * @brief 建 PipeWire sink + Opus 编码器并拉起编码线程；可选地再建上行
 *        麦克风（解码线程 + 虚拟 Source）。
 *
 * 建链顺序是 encoder 先、sink 后：sink 的 RT 线程一起来就直接调
 * encoder::ProcessFrame，编码器没建好就是往空指针上灌。上行链在 sink
 * 之后建，失败只让麦克风不可用，不影响下行。
 *
 * @param mac         只用于本站标识（12 位 MAC 十六进制，进节点名与显示名），
 *                    让两只 dongle 在 wpctl/pavucontrol 里可分辨；解析失败时
 *                    用全零标识。
 * @param haptics_gain 触觉振幅倍率，0.0 = 静音、1.0 = 原样、2.0 = 两倍；
 *                    乘在 int8 量化之前，只影响下行触觉，不碰喇叭通路。
 *                    范围由配置层保证，本模块不再夹取。
 * @param output      每段编码产出的去处；空 std::function 会被拒绝（链路跑
 *                    起来没人收数据，比不起更难查）。
 * @param host_sysfs_path 本站在主机侧的 USB 接口目录，相对 /sys，形如
 *                    `/devices/platform/dummy_hcd.0/usb9/9-1/9-1:1.0`；由
 *                    Manager 建站时解析、经 worker 的配置快照传到这里，sink 与
 *                    source 共用同一个值，空串则两条流都不带 device.sysfs.path
 *                    与 alsa.card 属性。
 * @param on_lost     sink 或 source 连续恢复失败后的通知；两条流收同一个
 *                    handler，可为空（不降级，只丢音频）。
 * @param on_mic_open 主机开始/停止录音的通知；空表示**不建**上行麦克风
 *                    （虚拟 Source 不出现，手柄麦克风永远不会被打开）。
 * @return 成功 true；false 时线程与流没有半开的残留，调用方继续 HID-only
 *         即可——音频失败从不致命。上行链起不来只记 WARNING，不影响 true。
 */
bool Start(const std::string& mac, float haptics_gain, OutputSink output,
           const std::string& host_sysfs_path, LostHandler on_lost = {},
           MicOpenHandler on_mic_open = {});

/**
 * @brief 停整条音频链：先停上行 Source/解码，再停 sink、encoder。
 *
 * 与 worker 的拆机顺序同口径。真的拆过上行 Source 才会在调用方线程上补
 * 一次 on_mic_open(false)，让手柄在 bt 还在的时候把麦克风关掉。可重复调用。
 */
void Stop();

/**
 * @brief 把手柄上行的一帧 Opus 包推进解码链。
 *
 * @param packet core::protocol::kMicOpusSize 字节的单声道 Opus 包体，不含
 *               report id/flags。
 * @param len    包体长度；短于 kMicOpusSize 的整帧丢弃。
 * @warning 由 bridge 在 bt 的**读线程**上直接调用：只拷进有界队列就返回，
 *          不阻塞、不打日志。解码与 PCM 交付在解码线程上；上行链没起来
 *          （Start 没给 on_mic_open，或麦克风建失败）时静默丢弃。
 * @see bridge::MicSink
 */
void OnMicPacket(const uint8_t* packet, size_t len);

}  // namespace audio
