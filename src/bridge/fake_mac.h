/**
 * @file
 * @brief bridge 内部的伪装 MAC：0x09 feature 报告里那只 BD_ADDR 的两个来源
 *
 * 只有一份内容（0x09 报告的 MAC 字段），两个来源，所以拆成两个动作：
 * - 手柄没回 0x09 时，按用户配置里的 bt_addr 合成一份（FillFakeMac）；
 * - 手柄回了真 0x09，就把真 MAC 的末位翻掉再交出（FlipReplyMac）。
 *
 * 翻位是这份报告离开本进程的前提：hid_playstation 驱动从 0x09 反序取手柄
 * MAC，而真 MAC 已被内核的蓝牙 HID 会话登记过，原样返回会让驱动 probe
 * 撞 -EEXIST、设备起不来。
 *
 * 实现留在本模块自己的 TU（fake_mac.cpp），唯一调用方是门面 bridge.cpp 的
 * OnClassRequest；命名空间用 detail，对外只露 bridge 门面。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bridge {
namespace detail {

/**
 * @brief 记下要伪装成哪只手柄；之后每次合成的 MAC 都是它。
 *
 * 解析 "AA:BB:CC:DD:EE:FF" 形式的 6 段十六进制。
 *
 * @param mac BD_ADDR 字符串，来自用户配置里的 bt_addr；段数不对时
 *            原值不动、不报错（保持上一次或全零）。
 * @warning 只应在起线程前由 bridge::Init() 调用一次，之后本模块只读这份值。
 */
void SetFakeMac(const std::string& mac);

/**
 * @brief 手柄没回复时，把伪装 MAC 合成为一份 0x09 报告的 payload。
 *
 * payload[0..5] 是 SetFakeMac() 那份 MAC 的逆序（0x09 报告的线上格式就是
 * 反序 BD_ADDR），其中 payload[0]（正常字节序的末位）在这里已经翻过一位
 * ——翻位是这份报告能安全交出的前提，所以写进合成里，调用方不需要再动它。
 * payload[6..18] 补零。
 *
 * @param payload  目标缓冲区，只写 capacity 以内。
 * @param capacity payload 的容量；0x09 的 wLength 恒为 20，去掉 report id
 *                 剩 19 字节，容量更大也按 19 截。
 * @return 实际写入的字节数（min(capacity, 19)）；payload 为 null 或
 *         capacity 为 0 时 0。
 * @note 不看 report id："该不该用这份兜底"是调用方（只给 0x09 兜底）的判断。
 */
int FillFakeMac(uint8_t* payload, size_t capacity);

/**
 * @brief 把手柄真回复的 0x09 报告里 MAC 的末位翻掉，躲开内核的 -EEXIST。
 *
 * 只认含 report id 的完整报告：report[0] 不是 0x09 或长度装不下
 * id + 6 字节 MAC（len < 7）时原样不动。FillFakeMac 交出的报告不经过
 * 这里——那份在合成时已经翻过。
 *
 * @param report 含 report id 首字节的完整 0x09 报告，原地修改。
 * @param len    report 的有效长度。
 */
void FlipReplyMac(uint8_t* report, size_t len);

}  // namespace detail
}  // namespace bridge
