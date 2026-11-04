/**
 * @file
 * @brief bridge 内部：让手柄自己关机的 0x08 feature 报告
 *
 * 0x08（Set Bluetooth Control）是 feature 报告，不经中断通道，也不进
 * bt::PostOutput 的发送队列——它由 bt::SetFeature 走控制通道的 SET_REPORT
 * 原样发出。报告整包冻结：没有 seq 槽、没有逐次变化的字段，尾部 CRC 在
 * 编排时就写死，运行时不重算、不拷贝。
 *
 * 布局（48 字节，未列出的字节为 0）：
 *
 * | 偏移    | 值       | 含义 |
 * |---------|----------|------|
 * | 0       | 0x08     | report id |
 * | 1       | 0x02     | 蓝牙控制：0x02 = 关（0x01 = 开） |
 * | 2..43   | 0        | 其余 payload |
 * | 44..47  | CRC32 LE | 覆盖 [0..43]，feature seed，小端 |
 *
 * 长度出处：HID report descriptor 里 Report ID 8 的 Report Count 是 47，
 * Report Count 不含 report id，所以线上是 48 字节。CRC 覆盖"报告总长减尾部
 * 4 字节"，起点是 report id，因此覆盖 44 字节——与 0x31/0x36/0x32 由传输层
 * 在入队点盖章不同，这份是冻结整包，0x23A2EFE0 直接写进字节。
 *
 * @note 手柄收到后自行断电，蓝牙链路随之断开；后续的看门狗判死、worker
 *       退出与 Manager 拆站重建都由既有路径处理，本报告不参与那些决定。
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/protocol/report.h"

namespace bridge::detail {

/// 0x08 报告的线上总长：1 字节 report id + descriptor 的 47 字节 Report Count。
inline constexpr size_t kPowerOffReportSize = 48;

/// 关机报告整包冻结：[1]=0x02 关，[44..47] 是覆盖前 44 字节的 feature CRC。
inline constexpr uint8_t kPowerOffReport[kPowerOffReportSize] = {
    core::protocol::kFeatureReportIdBluetooth, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0xE0, 0xEF, 0xA2, 0x23,
};

}  // namespace bridge::detail
