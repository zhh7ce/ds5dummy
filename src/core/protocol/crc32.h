/**
 * @file crc32.h
 * @brief DualSense 报告尾部的反射 CRC-32 计算与写入
 *
 * DualSense 的输出报告与 feature 报告尾部各带 4 字节小端 CRC-32：反射
 * 多项式 0xEDB88320，但两类报告使用不同的初始 seed。校验失败时手柄直接
 * 丢弃报告——发送侧的系统调用照常成功，错误只表现为"设备无响应"，所以
 * 任何改动报告字节的代码都必须保证发送前重算 CRC。
 *
 * 算法为标准查表法反射 CRC-32：CRC = ~update(~seed, data)，seed 即通过
 * 初值注入变体的种子；空数据返回 seed 本身。seed 的具体数值是 DualSense
 * 固件的规定值（上游 DS5Dongle 项目实测可用），不是本仓库推导出来的。
 *
 * @note 全部入口为 inline，供 bt/bridge 在发送路径上直接内联；查表在
 *       constexpr 期生成，无运行时初始化。
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace core::protocol {

/// 反射 CRC-32 多项式（0xEDB88320 = 0x04C11DB7 的位反转）。
inline constexpr uint32_t kCrc32Polynomial = 0xEDB88320;
/// 输出报告（0x31/0x36 等）的 CRC 初值 seed，DualSense 固件规定值。
inline constexpr uint32_t kOutputReportCrcSeed = 0xEADA2D49;
/// feature 报告的 CRC 初值 seed，与输出报告不同，DualSense 固件规定值。
inline constexpr uint32_t kFeatureReportCrcSeed = 0x2060EFC3;
/// 报告尾部 CRC 字段自身的字节数，计算范围 = 总长 - 本值。
inline constexpr size_t kReportCrcTailBytes = 4;

namespace internal {

/**
 * @brief constexpr 生成单个表项：对字节值做 8 轮反射移位。
 * @param index 表项对应的输入字节，0..255。
 * @return 该字节一次查表应异或进 CRC 的值。
 */
inline constexpr uint32_t Crc32TableEntry(uint32_t index) {
  for (unsigned bit = 0; bit < 8; ++bit) {
    // 低位为 1 时右移后异或多项式：-（index&1）是 0/全 1 掩码的分支消除写法。
    index = (index >> 1) ^ (kCrc32Polynomial & -(index & 1));
  }
  return index;
}

/**
 * @brief constexpr 构建 256 项查表。
 * @return 反射 CRC-32 全表，下标为输入字节。
 */
inline constexpr std::array<uint32_t, 256> MakeCrc32Table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < table.size(); ++i) {
    table[i] = Crc32TableEntry(i);
  }
  return table;
}

/// 编译期生成的查表，进程内零初始化成本。
inline constexpr std::array<uint32_t, 256> kCrc32LookupTable = MakeCrc32Table();

}  // namespace internal

/**
 * @brief 反射 CRC-32 主循环，seed 由调用方指定。
 *
 * 以 ~seed 作初始寄存器值逐字节查表，返回前再取反；空数据（size==0）
 * 时结果恰为 seed。
 *
 * @param data 待计算缓冲区，可为空指针当且仅当 size==0。
 * @param size 字节数，计算覆盖全部 size 字节。
 * @param seed 报告类型对应的初值（输出或 feature）。
 * @return 最终 CRC；调用方负责按小端写入报告尾部。
 * @note 纯函数，无副作用，任意线程可调。
 * @see FillOutputReportChecksum / FillFeatureReportChecksum —— 计算并落位的一体化入口。
 */
inline uint32_t Crc32Seeded(const uint8_t* data, size_t size, uint32_t seed) {
  uint32_t crc = ~seed;
  while (size-- > 0) {
    crc = (crc >> 8) ^ internal::kCrc32LookupTable[(crc ^ *data++) & 0xff];
  }
  return ~crc;
}

/**
 * @brief 输出报告 seed（kOutputReportCrcSeed）的 Crc32Seeded 快捷入口。
 * @param data 报告缓冲区首字节。
 * @param size 覆盖字节数（不含尾部 CRC 字段本身，调用方负责裁剪）。
 * @return 输出报告 CRC。
 */
inline uint32_t Crc32(const uint8_t* data, size_t size) {
  return Crc32Seeded(data, size, kOutputReportCrcSeed);
}

/**
 * @brief feature 报告 seed（kFeatureReportCrcSeed）的 Crc32Seeded 快捷入口。
 * @param data 报告缓冲区首字节。
 * @param size 覆盖字节数（不含尾部 CRC 字段本身）。
 * @return feature 报告 CRC。
 */
inline uint32_t Crc32Feature(const uint8_t* data, size_t size) {
  return Crc32Seeded(data, size, kFeatureReportCrcSeed);
}

/**
 * @brief 计算输出报告 CRC 并写入缓冲区尾部 4 字节（小端）。
 *
 * CRC 覆盖 [data, data+len-4)，写入紧随其后的 4 字节——即整段 len 恰好
 * 是"报告体 + CRC 尾巴"的完整线尺寸。
 *
 * @param data 报告缓冲区，原地写入尾部。
 * @param len 报告总字节数，必须 >= 4，否则尾部下标回绕（未做防御检查）。
 * @note 假设 len 已含 CRC 尾巴：传入不含尾的长度会把 CRC 写进有效载荷。
 * @see FillFeatureReportChecksum —— 只差 seed 的孪生入口。
 */
inline void FillOutputReportChecksum(uint8_t* data, size_t len) {
  const size_t tail = len - kReportCrcTailBytes;
  const uint32_t crc = Crc32(data, tail);
  data[tail + 0] = (crc >> 0) & 0xFF;
  data[tail + 1] = (crc >> 8) & 0xFF;
  data[tail + 2] = (crc >> 16) & 0xFF;
  data[tail + 3] = (crc >> 24) & 0xFF;
}

/**
 * @brief 计算 feature 报告 CRC 并写入缓冲区尾部 4 字节（小端）。
 * @param data 报告缓冲区，原地写入尾部。
 * @param len 报告总字节数（含 CRC 尾巴），必须 >= 4。
 * @note 覆盖范围从 data 起算：组帧时 0x53 事务头等外层字节须留在指针之外，
 *       手柄只校验 [report id][payload] 段（见 bt_l2cap 的 SET_REPORT）。
 */
inline void FillFeatureReportChecksum(uint8_t* data, size_t len) {
  const size_t tail = len - kReportCrcTailBytes;
  const uint32_t crc = Crc32Feature(data, tail);
  data[tail + 0] = (crc >> 0) & 0xFF;
  data[tail + 1] = (crc >> 8) & 0xFF;
  data[tail + 2] = (crc >> 16) & 0xFF;
  data[tail + 3] = (crc >> 24) & 0xFF;
}

}  // namespace core::protocol
