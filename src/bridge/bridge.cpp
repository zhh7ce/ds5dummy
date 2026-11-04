/**
 * @file
 * @brief bridge 门面的实现：生命周期、各方向的转发、ep0 类请求
 *
 * 门面函数都在这份 TU 里，顺序同 bridge.h。每个数据方向都是"取别人的裸
 * 报告体 → 换成另一边的布局 → 交给别人的传输原语"，不排队、不起线程，跑
 * 在调用方的线程上。会在这条线程上等的是 OnClassRequest：它阻塞读 ep0 的
 * status stage，也是本模块唯一决定 stall/ack 的地方。
 *
 * 伪装 MAC 的读写在 fake_mac.cpp，输出状态在 state_report.cpp，两种报告布局
 * 在 usb_report.cpp / audio_report.cpp / mic_report.cpp；它们都只被这里调用。
 */

#include "bridge/bridge.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <utility>

#if DS5DUMMY_DEBUG_LOG
#include <iomanip>
#include <sstream>
#endif

#include "bridge/audio_report.h"
#include "bridge/fake_mac.h"
#include "bridge/mic_report.h"
#include "bridge/power_report.h"
#include "bridge/state_report.h"
#include "bridge/usb_report.h"
#include "bt/bt.h"
#include "core/debug.h"
#include "core/device/hid.h"
#include "core/protocol/audio_format.h"
#include "core/protocol/report.h"
#include "loguru.hpp"
#include "usb/usb.h"

namespace bridge {
namespace {

// 0x36 的 byte[10] 段计数器；audio 编码线程是唯一写者，不需要原子。
// 只有入队成功才递增：队列满丢包时，线上留下的计数器序列始终连续。
uint8_t audio_packet_counter = 0;

// 主机侧录音接口是否开着。跨线程读写：OnMicOpen 由 audio 的 PipeWire 主循环
// 线程（以及 Stop 时的主线程）调用，OnBtInput / OnAudioOutput 在 bt 读线程与
// audio 编码线程上读，所以必须是原子。初值 false = 主机还没开始录音。
std::atomic<bool> mic_open{false};

// 上行 Opus 包体的去处。SetMicSink 在 bt::StartThreads() 之前装填，此后只读，
// 因此不需要锁（与 Init 装填的状态同一个线程相位约定）。
MicSink mic_sink;

}  // namespace

void Init(const std::string& bt_addr, uint8_t speaker_pre_gain) {
  // 以下每一步都只在此刻（起线程前）做一次，之后各入口函数无条件读其成果。
  detail::SetFakeMac(bt_addr);
  detail::InitOutputState(speaker_pre_gain);

  // usb 侧先持有这份空闲 0x01 报告：第一批 BT 输入到达之前，IN 线程写出的
  // （重发档会反复重发同一份）和主机打 GET_REPORT(INPUT) 拿到的都是它。
  // Init 在时序上早于 usb::StartThreads()，后者不清空持有的报告。
  std::array<uint8_t, core::protocol::kUsbInputStateSize> idle_state{};
  uint8_t report[core::protocol::kUsbInputReportSize];
  detail::BuildUsbInputReport(report, idle_state.data());
  usb::PostInputReport(report, sizeof(report));
}

/**
 * @brief ep0 类请求分派：应答 GET 侧、消费 SET 侧、其余 stall。
 *
 * 实现要点：OUT 方向的 SET_REPORT 会在这条线程上阻塞读完 status stage；
 * IN 方向的 feature 请求缓存未命中时同步等链路回复。两个方向最终都靠
 * 随回调传入的 Ep0Responder 落盘，本函数是 bridge 里唯一做这个决定的
 * 地方。
 *
 * @param request 见 bridge.h 声明；wValue 拆法为高字节报告类型、低字节 id。
 * @param ep0 本轮回调的应答通道，只在本函数内有效。
 */
void OnClassRequest(const usb_ctrlrequest& request, usb::Ep0Responder& ep0) {
  // HID 类请求的 wValue 约定：高 8 位 = report type，低 8 位 = report id。
  const uint8_t report_type = static_cast<uint8_t>(request.wValue >> 8);
  const uint8_t report_id = static_cast<uint8_t>(request.wValue & 0xFF);

  // ---- OUT 方向（主机 → 设备）：SET_REPORT / SET_IDLE / SET_PROTOCOL ----
  if (!(request.bRequestType & USB_DIR_IN)) {
    switch (request.bRequest) {
      case core::device::kHidSetReport: {
        // 128B 是本模块的读取上限；SET_REPORT(0x02) 的 payload 远小于它。
        uint8_t buffer[128];
        // wLength 超过缓冲时只读缓冲那么多：宁读前缀，不把堆栈读出界。
        const size_t want =
            request.wLength < sizeof(buffer) ? request.wLength : sizeof(buffer);
        const ssize_t read_size = ep0.ReadData(buffer, want);
        if (read_size <= 0) {
          // status stage 读不到数据，这份请求没有可转发的内容，就地放弃。
          DS5DUMMY_LOG(ERROR, "[ep0] SET_REPORT id=0x{:02X}: no data{}", report_id,
                read_size < 0 ? strerror(errno) : "");
          break;
        }
        // 只有"输出类型 + 0x02 号报告"这一种组合有去处，其余 ACK 后丢弃。
        if (report_type == core::device::kHidReportTypeOutput &&
            buffer[0] == core::protocol::kUsbOutputReportId) {
          OnUsbOutput(buffer, static_cast<size_t>(read_size));
        } else {
          DS5DUMMY_LOG(INFO, "[ep0] SET_REPORT type={} id=0x{:02X} ignored",
                report_type, report_id);
        }
        break;
      }
      case core::device::kHidSetIdle:
      case core::device::kHidSetProtocol:
        // 无数据段可收，但主机要一个正常的 status stage：ACK 即可，
        // 值本身不影响本设备行为。
        ep0.AckDatalessOut();
        break;
      default:
        DS5DUMMY_LOG(WARNING, "[ep0] class OUT 0x{:02X} — stalling", request.bRequest);
        ep0.Stall(false);
    }
    return;
  }

  // ---- IN 方向（设备 → 主机）：组一份 out 缓冲区再经 ep0 发出 ----
  uint8_t out[128] = {};
  size_t len = 0;

  switch (request.bRequest) {
    case core::device::kHidGetReport:
      if (report_type == core::device::kHidReportTypeFeature) {
        const size_t size =
            request.wLength < sizeof(out) ? request.wLength : sizeof(out);
        // bt::GetFeature 约定：buf[0] 进、出都是 report id。
        out[0] = report_id;
        int got = bt::GetFeature(out, size);
        if (got > 0) {
          // 真手柄的 MAC 已被内核蓝牙 HID 会话登记过，原样交出会让
          // hid_playstation probe 撞 -EEXIST，必须翻掉末位。
          detail::FlipReplyMac(out, static_cast<size_t>(got));
        } else if (report_id == core::protocol::kFeatureReportIdMac &&
                   size >= 2) {
          // 只有 0x09（MAC 报告）有可信的合成内容——拿用户配置里的
          // bt_addr 顶一份；其余 id 手柄没回就没有兜底可编。
          const int n = detail::FillFakeMac(out + 1, size - 1);
          if (n > 0) {
            got = 1 + n;  // 1 字节 report id + 合成 payload
            DS5DUMMY_LOG(WARNING,
                  "[ep0] feature 0x09: no controller reply, served bt_addr "
                  "MAC instead ({} bytes)",
                  got);
          }
        }
        if (got > 0) {
          len = static_cast<size_t>(got);
#if DS5DUMMY_DEBUG_LOG
          {
            // 十六进制预览只打前 16 字节：feature 报告可达 56B，全量刷屏。
            std::ostringstream oss;
            oss << std::hex << std::setfill('0');
            for (int i = 0; i < got && i < 16; ++i) {
              oss << ' ' << std::setw(2) << static_cast<int>(out[i]);
            }
            DS5DUMMY_DLOG(1,
                          "[ep0] GET_REPORT(FEATURE) 0x{:02X} len={} data:{}",
                          report_id, got, oss.str());
          }
#endif
        } else {
          DS5DUMMY_LOG(ERROR,
                "[ep0] GET_REPORT(FEATURE) 0x{:02X}: no controller reply and "
                "no fallback for this id, stalling (wLength={})",
                report_id, request.wLength);
        }
      } else if (report_type == core::device::kHidReportTypeInput) {
        // 不上链路：交最近一份已发布的输入报告——Init 的空闲报告，或
        // OnBtInput 换出来的最新版。
        len = core::protocol::kUsbInputReportSize;
        usb::LatestInputReport(out, len);
      }
      break;
    case core::device::kHidGetIdle:
    case core::device::kHidGetProtocol:
      // 统一回 1 字节 0：GET_IDLE 的 0 = 无限空闲周期，GET_PROTOCOL 的 0 =
      // boot protocol。给出格式合法的答复即可，主机流程不会因此改道。
      out[0] = 0;
      len = 1;
      break;
    default:
      break;  // 认不出来的 GET：len 保持 0，落到下面的 stall
  }

  // len==0 即"没能组出应答"——IN 方向 stall 让主机知道请求不被支持。
  if (len == 0) {
    DS5DUMMY_LOG(ERROR, "[ep0] GET bRequest=0x{:02X} type={} id=0x{:02X} — stalling",
          request.bRequest, report_type, report_id);
    ep0.Stall(true);
    return;
  }
  // HID 惯例：主机 wLength 是它愿意收的上限，多出来的部分截断不送。
  if (len > request.wLength) len = request.wLength;
  ep0.SendIn(out, len);
  if (report_type == core::device::kHidReportTypeFeature) {
    DS5DUMMY_LOG(INFO, "[ep0] served feature report 0x{:02X} ({} bytes)", report_id,
          len);
  }
}

void OnUsbOutput(const uint8_t* data, size_t len) {
  // 空数据、非 0x02 号报告：不属于本转发的输入，静默丢掉。
  if (len < 1) return;
  if (data[0] != core::protocol::kUsbOutputReportId) return;

  // payload 不足 47 字节时不更新状态：宁可用上一份，也不合成半个字段。
  if (len >= 1 + core::protocol::kUsbOutputPayloadSize) {
    detail::UpdateOutputState(data + 1);
  }

  // BT 0x31 报告体：[0]=id、[1]=seq（留给传输层）、[2]=flags、[3..]=payload，
  // 尾部 4 字节留给传输层写 CRC；未覆盖的字节保持零初始化值。
  uint8_t body[core::protocol::kBtReportSize] = {};
  body[0] = core::protocol::kBtOutputReportId;
  body[2] = 0x10;  // flags: rumble enabled

  const size_t payload = len - 1;  // 去掉 report id 后的字节数
  // 只转发 0x02 声明的 47 字节：读缓冲按端点 maxpacket（64）开，长包超出
  // 48 的尾部是主机填充，塞进 0x31 体就是把手柄没声明的字节当数据发出去。
  const size_t copied =
      payload < core::protocol::kUsbOutputPayloadSize
          ? payload
          : core::protocol::kUsbOutputPayloadSize;
  std::memcpy(body + 3, data + 1, copied);

  // 返回值刻意不看：队列满丢这一拍即可，输出报告是持续刷新的状态数据，
  // 下一份会整体顶上。
  bt::PostOutput(body, sizeof(body));
}

void OnBtInput(const uint8_t* data, size_t len) {
  // 调用方（bt 的读线程）交出的已是裸报告体：
  //   [0]     = 0x31（BT input report id）
  //   [1]     = flags（bit 1 = 本报告装的是麦克风 Opus 帧）
  //   [2..64] = 63 字节输入状态体；麦克风帧时则是 [3..73] = 71 字节 Opus
  // 两字节是 id + flags 的最小可判长度：不足就连"这是哪种报告"都定不下来。
  if (len < 2) {
    DS5DUMMY_DLOG(1, "[bridge] BT pkt too short: {} bytes", len);
    return;
  }

  DS5DUMMY_DLOG(1, "[bridge] BT pkt len={} header={:02x} {:02x}", len, data[0],
                data[1]);

  if (data[0] != core::protocol::kBtInputReportId) {
    DS5DUMMY_DLOG(1, "[bridge] unexpected BT report id, dropping");
    return;
  }

  // 两种 0x31 负载靠 flags 区分。bit1 置位时报告装的是麦克风帧，状态体的
  // 偏移不再成立，因此这一份只走上行解码链、不发布 USB 输入报告。
  if ((data[1] & detail::kMicFlagMask) != 0) {
    // 主机没在录音（或 audio 起不来）就没人消费这帧：直接丢，不给解码链灌
    // 没人要的数据。频率 100 Hz，丢弃不记日志。
    if (!mic_open.load() || !mic_sink) return;
    // 偏移 3 + 一帧 Opus 是这份报告的最小完整形态，短了就是残缺帧。
    if (len < detail::kMicReportSize) {
      DS5DUMMY_DLOG(1, "[bridge] mic frame too short: {} bytes", len);
      return;
    }
    mic_sink(data + detail::kMicPayloadOffset, core::protocol::kMicOpusSize);
    return;
  }

  // 状态体占到下标 64，短于 65 字节就是残缺帧，宁可整份丢掉。
  if (len < 65) {
    DS5DUMMY_LOG(WARNING, "[bridge] BT input report too short: {} bytes (need 65+)",
          len);
    return;
  }

  // 交接单位是整份报告：在这里摆好 [0x01][63 字节状态体]，usb 只按字节发端点。
  uint8_t report[core::protocol::kUsbInputReportSize];
  detail::BuildUsbInputReport(report, data + 2);
  usb::PostInputReport(report, sizeof(report));
}

bool PowerOff() {
  // 整包冻结：feature 报告的 CRC 已写死在字节里，控制通道只负责原样送出。
  if (!bt::SetFeature(detail::kPowerOffReport, detail::kPowerOffReportSize)) {
    DS5DUMMY_LOG(ERROR,
          "[bridge] power-off feature report 0x08 was not sent; the controller "
          "stays on");
    return false;
  }
  DS5DUMMY_LOG(INFO, "[bridge] Controller asked to power off (feature 0x08)");
  return true;
}

void SetMicSink(MicSink sink) {
  // 与 Init 同一个线程相位：装填发生在 bt::StartThreads() 之前，运行期只读。
  mic_sink = std::move(sink);
}

void OnMicOpen(bool open) {
  // 去重：当前状态已经是目标值就直接返回。PipeWire 的 state_changed 会在
  // STREAMING/PAUSED 之间来回迁移，每次迁移都发一份报告没有意义。
  bool expected = !open;
  if (!mic_open.compare_exchange_strong(expected, open)) return;

  // 两份形态整包冻结，直接交出去：PostOutput 只拷贝，seq 与 CRC 在入队点盖。
  const uint8_t* body =
      open ? detail::kMicStatusOn : detail::kMicStatusOff;
  // 队列满时这份状态报告按背压规则被丢（丢最新），手柄的麦克风状态会和主机
  // 预期一直不一致——这不是可以静默的噪声，记 ERROR。
  if (!bt::PostOutput(body, detail::kMicStatusReportSize)) {
    DS5DUMMY_LOG(ERROR,
          "[bridge] mic {} report dropped (send queue full) — the controller "
          "state stays stale until the next open/close",
          open ? "on" : "off");
    return;
  }
  DS5DUMMY_LOG(INFO, "[bridge] Controller microphone {}", open ? "on" : "off");
}

void OnAudioOutput(const uint8_t* haptics, const uint8_t* speaker) {
  if (haptics == nullptr || speaker == nullptr) {
    DS5DUMMY_LOG(ERROR, "[bridge] OnAudioOutput got a null segment pointer");
    return;
  }

  // 状态要在锁内取、报告在锁外拼，所以先落地一份 63 字节副本再用。
  uint8_t state_data[core::protocol::kSetStateShadowSize] = {};
  detail::CopyOutputState(state_data);

  uint8_t body[core::protocol::kBtAudioReportSize];
  detail::BuildAudioReport(body, audio_packet_counter, haptics, speaker,
                           state_data, detail::kAudioBufferLength,
                           mic_open.load());
  // 入队成功才递增计数器：丢包时线上计数器序列不留空洞（见 bridge.h）。
  if (bt::PostOutput(body, sizeof(body))) {
    ++audio_packet_counter;
  }
}

}  // namespace bridge
