/**
 * @file
 * @brief 伪装 MAC 实现：0x09 报告的合成与真回复的末位改写
 *
 * 语义与翻位理由见 fake_mac.h。两条来源各翻一次末位：合成的那份在
 * FillFakeMac 里翻，交出去就是最终线上字节；手柄真回复的那份由
 * FlipReplyMac 翻。"该不该兜底"（只有 0x09 有兜底内容）是调用方的判断，
 * FillFakeMac 因此不看 report id。
 */

#include "bridge/fake_mac.h"

#include <cstdio>
#include <cstring>

#include "core/debug.h"
#include "core/protocol/report.h"
#include "loguru.hpp"

namespace bridge {
namespace detail {
namespace {

// 伪装成哪只手柄：bridge::Init() 经 SetFakeMac() 灌一次，之后只读。
// 初值全零只是静态初始化的结果；Manager 在 fork 前就要求 bt_addr 非空，
// 运行期不会再走到"没灌过"的状态。
uint8_t fake_mac[6] = {};

}  // namespace

void SetFakeMac(const std::string& mac) {
  unsigned char values[6] = {0, 0, 0, 0, 0, 0};
  // 要求 6 段全部匹配；%hhx 能吞任意宽度十六进制段，段数不足即整体放弃。
  if (std::sscanf(mac.c_str(), "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                  &values[0], &values[1], &values[2], &values[3], &values[4],
                  &values[5]) != 6) {
    return;  // 解析失败保持原值不动
  }
  for (int i = 0; i < 6; ++i) {
    fake_mac[i] = values[i];
  }
}

int FillFakeMac(uint8_t* payload, size_t capacity) {
  if (payload == nullptr || capacity == 0) return 0;

  // 0x09 的报告 payload 固定 19 字节（wLength 20 减 report id）：写再多
  // 也没人读，超出容量更要不得。
  const size_t n = capacity < 19 ? capacity : 19;
  std::memset(payload, 0, n);  // MAC 之外的字节（n>6 时）保持全零
  const size_t mac_bytes = n < 6 ? n : 6;
  for (size_t i = 0; i < mac_bytes; ++i) {
    payload[i] = fake_mac[5 - i];  // 逆序：payload[0] 对应正常序的末位
  }
  // 交出去前翻末位：与 FlipReplyMac 同一位、同一个理由（躲 -EEXIST）。
  payload[0] ^= 0x01;
  return static_cast<int>(n);
}

void FlipReplyMac(uint8_t* report, size_t len) {
  // 三个守卫合起来即"这是一份装得下 id + 6 字节 MAC 的 0x09 报告"；
  // 不满足就原样交出，不做任何改写。
  if (report == nullptr || report[0] != core::protocol::kFeatureReportIdMac ||
      len < 7) {
    return;
  }

  DS5DUMMY_DLOG(1,
                "[ep0] feature 0x09 MAC (reversed) before: {:02x} {:02x} "
                "{:02x} {:02x} {:02x} {:02x}",
                report[1], report[2], report[3], report[4], report[5],
                report[6]);
  DS5DUMMY_DLOG(1,
                "[ep0] feature 0x09 MAC (normal) before:  "
                "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                report[6], report[5], report[4], report[3], report[2],
                report[1]);
  // 0x09 payload 是反序 BD_ADDR，report[1] 正是正常字节序的末位 octet。
  report[1] ^= 0x01;
  DS5DUMMY_DLOG(1,
                "[ep0] feature 0x09 MAC (normal) after:   "
                "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                report[6], report[5], report[4], report[3], report[2],
                report[1]);
}

}  // namespace detail
}  // namespace bridge
