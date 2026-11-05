/**
 * @file
 * @brief bridge 模块门面：BT ↔ USB 之间的报告转发策略
 *
 * 四个数据方向都从本头文件进出：
 * - BT 输入 → USB 输入（手柄 → 主机，OnBtInput）；
 * - BT 输入里的麦克风帧 → 上行解码链（OnBtInput 的 mic 分支）；
 * - USB 输出 → BT 输出（主机 → 手柄，OnUsbOutput）；
 * - 编码音频段 → BT 输出（0x36 报告，OnAudioOutput）；
 * 外加两条控制方向：USB ep0 类请求的应答策略（OnClassRequest），以及主机
 * 开始/停止录音时开关手柄麦克风（OnMicOpen）。
 *
 * bt:: 传输层对调用方隐藏了 HIDP framing，本模块所有方向看到的都是裸报告
 * 体：首字节是 report id，没有 0xA1/0xA2。
 *
 * 边界：本模块没有线程、没有队列。每个入口都在调用方的线程上把报告组好、
 * 交出去就结束（交给 bt::PostOutput、usb::PostInputReport 或 MicSink）；
 * 发送队列、byte[1] 的 4-bit seq 和输出报告 CRC 都归传输层在入队点处理。
 * 建链末尾的 0x32 唤醒报告不经过本模块：它是 L2CAP 后端在 bt::Open() 内部
 * 自己补发的一份固定字节。
 *
 * @warning 进程级单实例：多手柄靠多对 fork 出来的 Manager + worker 进程，
 *          门面因此是 namespace 自由函数，进程内不存在第二份 bridge 状态。
 */

#pragma once

#include <linux/usb/ch9.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "usb/usb.h"

namespace bridge {

/**
 * @brief 装填进程内状态：记下要伪装的 MAC、按固件初值备好输出状态，并发布
 *        会话里第一份 USB 输入报告。
 *
 * 输出状态初值里 byte[4]/byte[5]（耳机/扬声器音量）必须非零——缺了这一步，
 * 控制器会把每一份 0x36 音频报告静音。
 *
 * USB 侧发布的是 63 字节状态体全零的空闲报告：报告格式只在 bridge 里组，
 * usb 不认识它，所以持有的状态不由 usb 播种；这份空闲报告既是 IN 线程在
 * 蓝牙报告到达前的首份内容（重发档会重复写它），也供 ep0 的
 * GET_REPORT(INPUT) 应答。
 * usb::StartThreads() 不清空它。
 *
 * @param bt_addr 合成 0x09 feature 报告用的 BD_ADDR，形如
 *                "AA:BB:CC:DD:EE:FF"；解析失败时 MAC 保持全零。
 * @param speaker_pre_gain 控制器侧扬声器前置增益（SetStateData byte[37] 低
 *                3 位），[0, 7]；固件初值是 2。手柄用它放大 0x36 里的喇叭
 *                数据，主机侧不改 PCM。范围由配置层保证，本函数只做掩码。
 * @warning 必须是本模块的第一个调用，且早于 usb::StartThreads() 与
 *          bt::StartThreads()——三个 on_* 入口都无条件读这份状态。
 * @see detail::BuildUsbInputReport() —— 空闲报告的字节布局。
 */
void Init(const std::string& bt_addr, uint8_t speaker_pre_gain);

/**
 * @brief 决定 TYPE_CLASS SETUP 请求怎么回答。
 *
 * 覆盖 GET/SET_REPORT、GET/SET_IDLE、GET/SET_PROTOCOL 六个类请求：
 * IN 方向组好缓冲区后经 @p ep0 回，OUT 方向的
 * SET_REPORT(OUTPUT 且 id=0x02) 读完 status stage 数据后转给 OnUsbOutput()，
 * 其余不认识的请求 stall。
 *
 * @param request ep0 上收到的原始 setup 包，本函数只读它。
 * @param ep0 本轮回调的应答通道，只在本函数内有效、不得存下（见
 *            usb::Ep0Responder）。
 * @warning 由 usb 的 ep0 线程调用；GET_REPORT(FEATURE) 缓存未命中时会在
 *          这条线程上同步等手柄的链路回复。
 */
void OnClassRequest(const usb_ctrlrequest& request, usb::Ep0Responder& ep0);

/**
 * @brief 转发主机经 OUT 端点发来的输出报告。
 *
 * payload 够长（len ≥ 48）时先取 47 字节有效 payload 更新模块内的 63 字节
 * SetStateData，然后把同样的 47 字节搬进 78 字节的 BT 0x31 报告体
 * （body[0]=0x31、body[2]=0x10 置 rumble 使能）交给 bt::PostOutput——
 * byte[1] 的 seq 与尾部 CRC 由其入队时盖。
 *
 * @param data OUT 端点收到的字节，data[0] 是 USB report id。
 * @param len  data 的有效长度；端点 maxpacket 是 64，所以它可以大于 0x02 报告
 *             的 48 字节，超出 47 字节 payload 的尾部按填充处理、不转发。
 * @warning 由 usb 的 OUT 线程调用；len 为 0 或 report id 不是 0x02 时
 *          静默丢弃（不记日志——这是端点上的常态噪声，不是错误）。
 */
void OnUsbOutput(const uint8_t* data, size_t len);

/**
 * @brief 把 BT 侧的裸输入报告换成整份 USB 输入报告发布出去。
 *
 * 收到的布局是 [0]=0x31、[1]=flags、[2..64]=63 字节状态体；取状态体交给
 * detail::BuildUsbInputReport 摆成 [0x01][状态体]，再经 usb::PostInputReport
 * 覆盖 usb 持有的那份状态（不排队），由 usb 的 IN 写线程按配置的节奏写出
 * （重发档按主机轮询重发，real-time 档每份只写一次）。
 *
 * flags bit1 置位时这份 0x31 装的不是状态体，而是一帧麦克风 Opus：此时不再
 * 发布 USB 输入报告，改成把从 [3] 起的 core::protocol::kMicOpusSize 字节
 * 包体交给 MicSink（未装或麦克风未开时整份丢弃）。
 *
 * @param data 裸报告体，data[0] 是 BT report id。
 * @param len  data 的有效长度。
 * @warning 由 bt 的读线程调用；len < 2、report id 不是 0x31、状态报告
 *          len < 65、麦克风报告 len < detail::kMicReportSize 时丢弃这一份。
 *          这些丢弃只写 DS5DUMMY_DLOG：麦克风帧 100 Hz，WARNING 会刷屏。
 * @see OnUsbOutput() —— 反方向的转发。
 * @see detail::BuildUsbInputReport() —— 状态报告的字节布局。
 * @see detail::kMicPayloadOffset —— 麦克风帧在报告体里的起点。
 */
void OnBtInput(const uint8_t* data, size_t len);

/**
 * @brief 一份输入状态钩子：在 USB 输入报告发布之前改写那 63 字节。
 *
 * 插件是进程级单例，所以这里就是一个裸函数指针，没有 self、没有辅助结构。
 * @param state 63 字节状态体，可写；调用返回之后宿主才把它交给 usb。
 * @warning 在 bt 的读线程上调用：实现不许阻塞、不许分配，也不许碰链路。
 */
using StateHook = void (*)(uint8_t* state);

/**
 * @brief 注册一份输入状态钩子。
 * @param hook 要追加到表尾的钩子，按注册顺序调用。
 * @warning 只能在 bt/usb 传输线程起来之前调用；运行期这张表只读，不加锁。
 * @see ClearStateHooks() —— 拆机时的反向一步。
 */
void AddStateHook(StateHook hook);

/**
 * @brief 清空全部输入状态钩子。
 * @warning 只能在两条传输线程 join 之后调用：清表与钩子调用不能并发。
 * @see AddStateHook()。
 */
void ClearStateHooks();

/**
 * @brief 让手柄自己关机：发一份整包冻结的 0x08 feature 报告。
 *
 * 报告字节在 detail::kPowerOffReport（48 字节，payload[0]=0x02 关，尾部
 * CRC 已含）。它走控制通道的 SET_REPORT，不进发送队列，因此调用路径是
 * bt::SetFeature 而不是 bt::PostOutput；手柄不回应答，返回 true 只代表
 * 报告已交给链路。
 *
 * @return 报告已发出 true；链路没开或发送失败 false。
 * @warning 同步写控制通道，可能短暂阻塞调用线程（与 GetFeature 共用一把锁，
 *          对方可能正在等手柄的 feature 应答）；不要从 bt/usb 的传输线程上
 *          调用。
 * @see detail::kPowerOffReport —— 冻结的 48 字节报告。
 * @see bt::SetFeature() —— 控制通道上的 SET_REPORT。
 */
bool PowerOff();

/**
 * @brief 上行麦克风帧的去处：bridge 把纯 Opus 包体交给谁。
 *
 * @param packet core::protocol::kMicOpusSize（71）字节的单声道 Opus 包体，
 *               不含 report id / flags；指针只在调用期间有效。
 * @param len    恒为 core::protocol::kMicOpusSize，随参数一起给是为了让实现
 *               不必再依赖常量尺寸。
 * @details 由 bridge 在 bt 的**读线程**上直接调用，和 OnBtInput 是同一根
 *          栈：实现必须非阻塞、不自起线程。生产实现 audio::OnMicPacket 只把
 *          包体拷进有界队列就返回；真正的解码在 audio 自己的线程上。
 */
using MicSink = std::function<void(const uint8_t* packet, size_t len)>;

/**
 * @brief 装填上行麦克风帧的去处。
 *
 * 和 OutputSink 是数据反方向上的对偶：sink 把编码结果推出 bridge，
 * MicSink 把手柄送来的编码音频推出 bridge。bridge 不解析、不排队、不做
 * 背压——这些全在实现方（audio）手里。
 *
 * @param sink 收到纯 Opus 包体的实现；空 std::function 表示不接收上行帧
 *             （OnBtInput 的 mic 分支随后只丢弃）。
 * @note 必须在 bt::StartThreads() 之前调用：读线程一起来就会调它，运行期
 *       只读、没有同步。audio 起不来时可以不装，麦克风数据自然被丢掉。
 * @see audio::OnMicPacket() —— 生产实现。
 */
void SetMicSink(MicSink sink);

/**
 * @brief 主机开始或停止录音时，开关手柄的麦克风上行。
 *
 * 往手柄发一份 142 字节的 0x32 状态报告（detail::BuildMicStatusReport），
 * 与 L2CAP 建链时补发的唤醒报告同 id 不同子报告。只有状态真的翻转才发：
 * 同一值的重复调用被去重，PipeWire 的状态回调可以在 STREAMING/PAUSED 之间
 * 反复打转而不刷链路。
 *
 * @param open true 让手柄开始上行麦克风（主机打开了 UAC 录音接口），
 *             false 让它停下（接口关闭，或 audio 模块正在拆除）。
 * @warning 由 audio source 的 PipeWire 主循环线程与 audio::Stop() 调用；
 *          函数本身线程安全，但要求 bt 的发送队列还活着（audio 的 Stop 早
 *          于 bt::StopThreads()）。队列满时这份状态报告按背压规则丢掉最新，
 *          记 ERROR——手柄与主机的麦克风状态会一直不一致，直到下一次开关。
 * @see detail::BuildMicStatusReport() —— 报告的字节布局。
 */
void OnMicOpen(bool open);

/**
 * @brief 收一段已编码音频（触觉 + 喇叭），拼成整份 0x36 报告发出去。
 *
 * SetStateData 由模块内取出后整块嵌入；byte[10] 的段计数器在这里自增，且
 * 只有入队成功才递增——队列满丢包不会在线上留下计数器空洞。byte[4] 的 bit0
 * 是麦克风状态的镜像（0xFF = 开着），由 OnMicOpen 维护。byte[1] 的 seq 与
 * 尾部 CRC 由传输层在入队点盖。
 *
 * @param haptics core::protocol::kHapticsSize（64）字节触觉包。
 * @param speaker core::protocol::kSpeakerSize（200）字节 Opus 喇叭包。
 * @warning 由 audio 的编码线程调用，是唯一碰段计数器的线程——计数器因此
 *          不需要原子性；函数本身线程安全、非阻塞。任一指针为 null 时
 *          丢弃并记 ERROR；队列满丢的就是这一份。
 * @see bt::PostOutput() —— 实际的排队与盖章处。
 */
void OnAudioOutput(const uint8_t* haptics, const uint8_t* speaker);

}  // namespace bridge
