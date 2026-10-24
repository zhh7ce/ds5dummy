/**
 * @file
 * @brief bridge 内部状态：SetStateData 的 63 字节线格式缓冲
 *
 * SetStateData 是主机 0x02 输出报告里的设备状态镜像，规则是"按 allow-flag
 * 逐字段合并"：主机报告里只有 allow-flag 允许的字段进这份状态，状态随后
 * 被整块嵌进 0x32 唤醒报告和每一份 0x36 音频报告。语义模型
 * （core::protocol::OutputState）只是合并用的中间表示，只活在本模块的
 * .cpp 里；对外一律交换线格式字节。
 *
 * 宽度的单一来源：47 字节的入向 payload 和 63 字节的线格式缓冲都由本模块
 * 按 core::protocol 的常量自己界定，调用方只给指针。
 *
 * @warning 线程模型（从代码可证）：InitOutputState() 与 CopyOutputState()
 *          在 state_mutex 锁内访问；UpdateOutputState() 不加锁，它跑在 usb
 *          的 OUT 线程上，与 CopyOutputState() 的唯一调用方（audio 编码线程
 *          经 OnAudioOutput）之间存在未同步的读写。动这块锁语义前先读
 *          state_report.cpp 头注释里的分工理由。
 */

#pragma once

#include <cstdint>

namespace bridge {
namespace detail {

/**
 * @brief 从当前线格式缓冲重建语义模型，覆盖扬声器前置增益后重新编码。
 *
 * 线格式缓冲在编译期就是固件初值，本函数把它解码一次：之后的合并都
 * 在这个模型上做。唯一在这里覆盖的字段是 byte[37] 低 3 位的
 * SpeakerCompPreGain——它是用户配置项，不属于主机报告的合并范围。
 *
 * @param speaker_pre_gain 控制器侧扬声器前置增益，[0, 7]；只在低 3 位有效。
 * @note 初值里 byte[4]/byte[5]（耳机/扬声器音量）必须非零，否则控制器会
 *       静音每一份 0x36 报告——这是"必须先 Init 再用状态"的真实原因。
 * @warning 使用这份状态之前必须调用一次，且只能由 bridge::Init() 在起线程
 *          之前调用。
 */
void InitOutputState(uint8_t speaker_pre_gain);

/**
 * @brief 拷出整份 63 字节 SetStateData 线格式，供构建 0x32 / 0x36 报告使用。
 *
 * 高频路径（每份 0x36 都要一次），所以只做一次 memcpy：模型→线格式的
 * 编码开销放在低频的 UpdateOutputState() 侧。
 *
 * @param out 目标缓冲区，容量由调用方保证 63 字节——宽度即
 *            core::protocol::kSetStateShadowSize，与本模块缓冲一致。
 */
void CopyOutputState(uint8_t* out);

/**
 * @brief 用主机 output report 的有效 payload 更新 SetStateData 状态。
 *
 * 解码 payload 后按 allow-flag 逐字段合并进语义模型，最后重新编码回线
 * 格式缓冲。耳/扬/麦音量与音频控制类字段有意不合并：音频控制归 PipeWire
 * 侧，这份状态保留 Init 时的初值，不随主机报告漂移。
 *
 * @param data 47 字节 SetStateData payload（不含 report id），容量由调用方
 *             保证；本模块只读到 core::protocol::kUsbOutputPayloadSize。
 * @note 跑在 usb 的 OUT 线程上，不加锁（见文件头的线程模型警告）。
 * @see CopyOutputState() —— 消费侧。
 */
void UpdateOutputState(const uint8_t* data);

}  // namespace detail
}  // namespace bridge
