// DS5Dummy — ep0 控制面与 class 请求策略的单测
//
// 两个被测面都从 fd 注入：bridge::OnClassRequest 收一个 usb::Ep0Responder，
// socketpair 当 ep0 就能看见它到底回了哪些字节；usb::plane::RunEp0 从
// plane::Get() 取端点，三条 socketpair 经 usb::Adopt 装进去就能驱动整条
// 事件循环。全程不碰 FunctionFS、不需要 root、不需要手柄。
//
// 为什么 socketpair 够用：本层对端点只做 read/write，而 SOCK_SEQPACKET 保留
// 消息边界，正好对应 f_fs "一次 setup / 一次 data stage 是一段独立长度" 的
// 语义。区别在零长 read/write 的副作用——f_fs 里那是 STALL 与 status ACK 的
// 触发器：零长 read 在 socketpair 上什么也不留下，零长 write 会变成一条空
// 消息。所以下面两个方向分开断言："IN stall/ACK = 没消息"、"OUT stall = 空
// 消息"，空消息不是 stall 语义本身，那一步留在硬件清单里。

#include "usb/usb_plane.h"

#include <gtest/gtest.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "bridge/bridge.h"
#include "bt/bt.h"
#include "core/device/descriptors.h"
#include "core/device/hid.h"
#include "core/protocol/report.h"
#include "usb/usb.h"

namespace {

/// 组一条 setup 包；wIndex 一律 0（我方设备只有一个接口）。
usb_ctrlrequest MakeRequest(uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t length) {
  usb_ctrlrequest request_structure{};
  request_structure.bRequestType = request_type;
  request_structure.bRequest = request;
  request_structure.wValue = value;
  request_structure.wIndex = 0;
  request_structure.wLength = length;
  return request_structure;
}

// bRequestType = 类型位 | 接收者 interface | 方向位。
constexpr uint8_t kClassOut =
    static_cast<uint8_t>(USB_TYPE_CLASS | USB_RECIP_INTERFACE);
constexpr uint8_t kClassIn =
    static_cast<uint8_t>(USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_IN);
constexpr uint8_t kStandardIn =
    static_cast<uint8_t>(USB_TYPE_STANDARD | USB_RECIP_INTERFACE | USB_DIR_IN);

/// HID 类请求的 wValue：高字节 report type、低字节 report id。
uint16_t ReportValue(uint8_t report_type, uint8_t report_id) {
  return static_cast<uint16_t>((static_cast<uint16_t>(report_type) << 8) |
                               report_id);
}

// ---- bridge::OnClassRequest：策略层到底回了什么 ----

class BridgeClassRequest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds_), 0);
    bt::StopThreads();  // 清空进程级发送队列，窗口量从零起算
    bridge::Init("AA:BB:CC:DD:EE:FF", 2);
  }

  void TearDown() override {
    bt::StopThreads();
    if (fds_[0] >= 0) ::close(fds_[0]);
    if (fds_[1] >= 0) ::close(fds_[1]);
  }

  /// 在"ep0 的另一端"上跑一次策略分派；fds_[1] 就是主机看到的那一侧。
  void Deliver(const usb_ctrlrequest& request) {
    usb::Ep0Responder ep0{fds_[0]};
    bridge::OnClassRequest(request, ep0);
  }

  /// 预置一段 OUT 数据段，等 ReadData() 来取。
  void SendPayload(const uint8_t* data, size_t len) {
    ASSERT_EQ(::send(fds_[1], data, len, 0), static_cast<ssize_t>(len));
  }

  ssize_t Receive(uint8_t* buf, size_t cap) {
    return ::recv(fds_[1], buf, cap, 0);
  }

  /// IN 方向的 stall/ACK 都是零长 read：socketpair 上不留任何痕迹。
  /// 真正的 stall 语义归硬件验证。
  bool NothingSent() {
    uint8_t byte = 0;
    return ::recv(fds_[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN;
  }

  /// OUT 方向的 stall 是零长 write：SOCK_SEQPACKET 会把它当成一条空消息交给
  /// 对端，所以这里能看见那个边界。
  bool ZeroLengthMessageSent() {
    uint8_t byte = 0;
    return ::recv(fds_[1], &byte, 1, MSG_DONTWAIT) == 0;
  }

  int fds_[2] = {-1, -1};
};

// GET_REPORT(INPUT) 不上链路：交出去的就是 usb 数据面最近一份已发布报告。
TEST_F(BridgeClassRequest, GetReportInputServesLatestSlot) {
  std::array<uint8_t, core::protocol::kUsbInputReportSize> report{};
  report.fill(0x5A);
  report[0] = core::protocol::kUsbInputReportId;
  usb::PostInputReport(report.data(), report.size());

  Deliver(MakeRequest(kClassIn, core::device::kHidGetReport,
                      ReportValue(core::device::kHidReportTypeInput, 0),
                      static_cast<uint16_t>(report.size())));

  std::array<uint8_t, 128> out{};
  const ssize_t received = Receive(out.data(), out.size());
  ASSERT_EQ(received, static_cast<ssize_t>(report.size()));
  EXPECT_EQ(std::memcmp(out.data(), report.data(), report.size()), 0);
}

// 主机 wLength 是它愿意收的上限：应答按它截断，不多送。
TEST_F(BridgeClassRequest, GetReportInputHonoursWLength) {
  std::array<uint8_t, core::protocol::kUsbInputReportSize> report{};
  report.fill(0x33);
  report[0] = core::protocol::kUsbInputReportId;
  usb::PostInputReport(report.data(), report.size());

  Deliver(MakeRequest(kClassIn, core::device::kHidGetReport,
                      ReportValue(core::device::kHidReportTypeInput, 0), 5));

  std::array<uint8_t, 128> out{};
  const ssize_t received = Receive(out.data(), out.size());
  ASSERT_EQ(received, 5);
  EXPECT_EQ(std::memcmp(out.data(), report.data(), 5), 0);
}

// 测试里不连 bt，0x09 走的是"手柄没回复 → 用配置 MAC 合成"的兜底路径。
TEST_F(BridgeClassRequest, GetReportFeatureMacFallsBackToConfiguredBtAddr) {
  Deliver(MakeRequest(kClassIn, core::device::kHidGetReport,
                      ReportValue(core::device::kHidReportTypeFeature,
                                  core::protocol::kFeatureReportIdMac),
                      20));

  std::array<uint8_t, 64> out{};
  const ssize_t received = Receive(out.data(), out.size());
  ASSERT_EQ(received, 20);
  EXPECT_EQ(out[0], core::protocol::kFeatureReportIdMac);
  // payload 是反序 BD_ADDR，且末位翻一位（躲开内核 HID 会话的 -EEXIST）：
  // AA:BB:CC:DD:EE:FF → FF EE DD CC BB AA → FE EE DD CC BB AA。
  const std::array<uint8_t, 6> want{0xFE, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
  EXPECT_EQ(std::memcmp(out.data() + 1, want.data(), want.size()), 0);
  for (size_t i = 1 + want.size(); i < 20; ++i) EXPECT_EQ(out[i], 0);
}

// 0x09 的兜底也只写到 wLength 为止。
TEST_F(BridgeClassRequest, GetReportFeatureMacHonoursWLength) {
  Deliver(MakeRequest(kClassIn, core::device::kHidGetReport,
                      ReportValue(core::device::kHidReportTypeFeature,
                                  core::protocol::kFeatureReportIdMac),
                      8));

  std::array<uint8_t, 64> out{};
  const ssize_t received = Receive(out.data(), out.size());
  ASSERT_EQ(received, 8);
  EXPECT_EQ(out[0], core::protocol::kFeatureReportIdMac);
  EXPECT_EQ(out[1], 0xFE);
}

// 除 0x09 外的 feature id 没有可信的兜底内容：链路没回就 STALL。
TEST_F(BridgeClassRequest, GetReportFeatureWithoutReplyAndNoFallbackStalls) {
  Deliver(MakeRequest(kClassIn, core::device::kHidGetReport,
                      ReportValue(core::device::kHidReportTypeFeature, 0x20),
                      64));
  EXPECT_TRUE(NothingSent());
}

// GET_IDLE / GET_PROTOCOL 各回 1 字节 0：格式合法即可，值不改变本设备行为。
TEST_F(BridgeClassRequest, GetIdleAndProtocolAnswerOneZeroByte) {
  for (const uint8_t request :
       {core::device::kHidGetIdle, core::device::kHidGetProtocol}) {
    Deliver(MakeRequest(kClassIn, request, 0, 4));
    std::array<uint8_t, 8> out{};
    const ssize_t received = Receive(out.data(), out.size());
    ASSERT_EQ(received, 1) << "bRequest=" << static_cast<int>(request);
    EXPECT_EQ(out[0], 0) << "bRequest=" << static_cast<int>(request);
  }
}

// 认不出来的 IN 请求没有应答可组：STALL。
TEST_F(BridgeClassRequest, UnknownClassGetStalls) {
  Deliver(MakeRequest(kClassIn, 0x04, 0, 64));
  EXPECT_TRUE(NothingSent());
}

// SET_REPORT(OUTPUT, 0x02)：数据段被读走并转给 bt。入队是这条路径的可观测
// 证据——读不到数据段就没有任何东西可入队。
TEST_F(BridgeClassRequest, SetReportOutputIsConsumedAndQueued) {
  std::array<uint8_t, core::protocol::kUsbOutputReportSize> payload{};
  payload[0] = core::protocol::kUsbOutputReportId;
  payload[1] = 0x11;
  SendPayload(payload.data(), payload.size());
  bt::StatsWindow();  // 只看本请求带来的增量

  Deliver(MakeRequest(kClassOut, core::device::kHidSetReport,
                      ReportValue(core::device::kHidReportTypeOutput,
                                  core::protocol::kUsbOutputReportId),
                      static_cast<uint16_t>(payload.size())));

  EXPECT_TRUE(NothingSent());
  EXPECT_GE(bt::StatsWindow().queue_high_water, 1u);
}

// 类型不是 OUTPUT 的 SET_REPORT：数据段照样要读走（否则会污染下一条 setup），
// 但不往链路转发。
TEST_F(BridgeClassRequest, SetReportOtherTypesAreConsumedWithoutForwarding) {
  std::array<uint8_t, core::protocol::kUsbOutputReportSize> payload{};
  payload[0] = core::protocol::kUsbOutputReportId;
  SendPayload(payload.data(), payload.size());
  bt::StatsWindow();

  Deliver(MakeRequest(kClassOut, core::device::kHidSetReport,
                      ReportValue(core::device::kHidReportTypeInput, 0x01),
                      static_cast<uint16_t>(payload.size())));

  EXPECT_TRUE(NothingSent());
  EXPECT_EQ(bt::StatsWindow().queue_high_water, 0u);
}

// wLength=0 的 SET_REPORT 没有数据段：读回 0 字节即放弃，不进链路也不 stall。
TEST_F(BridgeClassRequest, SetReportWithoutDataReadsNothing) {
  Deliver(MakeRequest(kClassOut, core::device::kHidSetReport,
                      ReportValue(core::device::kHidReportTypeOutput,
                                  core::protocol::kUsbOutputReportId),
                      0));
  EXPECT_TRUE(NothingSent());
}

// 无数据段的 OUT 请求要一个正常的 status stage：ACK 即可，值本身不生效。
TEST_F(BridgeClassRequest, SetIdleAndProtocolAckWithoutReply) {
  for (const uint8_t request :
       {core::device::kHidSetIdle, core::device::kHidSetProtocol}) {
    Deliver(MakeRequest(kClassOut, request, 0, 0));
    EXPECT_TRUE(NothingSent()) << "bRequest=" << static_cast<int>(request);
  }
}

// 六个类请求之外的 OUT 一律 STALL。
TEST_F(BridgeClassRequest, UnknownClassOutStalls) {
  Deliver(MakeRequest(kClassOut, 0x0C, 0, 0));
  EXPECT_TRUE(ZeroLengthMessageSent());
}

// ---- usb::plane::RunEp0：事件循环本身 ----

class Ep0Loop : public ::testing::Test {
 protected:
  static usb::plane::DataPlane& State() { return usb::plane::Get(); }

  void SetUp() override {
    usb::StopThreads();  // 幂等；确保没有数据面线程在跑
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ep0_), 0);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, in_), 0);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, out_), 0);
    ASSERT_TRUE(usb::Adopt({ep0_[0], in_[0], out_[0]}));
    State().handlers = {};
    State().running = false;
    {
      std::lock_guard<std::mutex> lock(State().input_mutex);
      State().link_up = false;
    }
  }

  void TearDown() override {
    State().running = false;
    State().handlers = {};
    // 认领的那三个端点归 plane 关；主机侧（peer）归测试自己关。
    usb::Close();
    for (int* pair : {ep0_, in_, out_}) {
      if (pair[1] >= 0) ::close(pair[1]);
    }
  }

  /// 一条不带 setup 的事件（ENABLE/DISABLE/...）。
  static usb_functionfs_event Event(uint8_t type) {
    usb_functionfs_event event{};
    event.type = type;
    return event;
  }

  /// 一条 SETUP 事件。
  static usb_functionfs_event SetupEvent(const usb_ctrlrequest& request) {
    usb_functionfs_event event{};
    event.u.setup = request;
    event.type = static_cast<uint8_t>(FUNCTIONFS_SETUP);
    return event;
  }

  void Push(const usb_functionfs_event& event) {
    ASSERT_EQ(::write(ep0_[1], &event, sizeof(event)),
              static_cast<ssize_t>(sizeof(event)));
  }

  void PushData(const void* data, size_t len) {
    ASSERT_EQ(::write(ep0_[1], data, len), static_cast<ssize_t>(len));
  }

  /// 事件供给结束：ep0 上再没有可读的消息，循环会在下一次 read 收到 EOF。
  void FinishEvents() { ::shutdown(ep0_[1], SHUT_WR); }

  ssize_t Receive(uint8_t* buf, size_t cap) {
    return ::recv(ep0_[1], buf, cap, 0);
  }

  /// 跑循环直到 EOF；调用前必须已经把这一段事件全部推进 socket。
  void RunLoop() {
    State().running = true;
    usb::plane::RunEp0();
  }

  int ep0_[2] = {-1, -1};
  int in_[2] = {-1, -1};
  int out_[2] = {-1, -1};
};

// class 请求原样交给回调、标准描述符由本层直接应答、EOF 后回调一次并落下链路。
TEST_F(Ep0Loop, DelegatesClassRequestsAndServesHidDescriptors) {
  std::vector<usb_ctrlrequest> class_requests;
  int ep0_closed = 0;
  State().handlers.on_class_request =
      [&](const usb_ctrlrequest& request, usb::Ep0Responder& ep0) {
        class_requests.push_back(request);
        const std::array<uint8_t, 2> reply{0xAA, 0x55};
        ep0.SendIn(reply.data(), reply.size());
      };
  State().handlers.on_ep0_closed = [&] { ++ep0_closed; };

  const usb_ctrlrequest class_request =
      MakeRequest(kClassIn, core::device::kHidGetReport,
                  ReportValue(core::device::kHidReportTypeInput, 0), 64);
  Push(SetupEvent(class_request));
  // 0x21 = HID class descriptor，0x22 = report descriptor。
  Push(SetupEvent(MakeRequest(kStandardIn, USB_REQ_GET_DESCRIPTOR, 0x2100, 64)));
  Push(SetupEvent(
      MakeRequest(kStandardIn, USB_REQ_GET_DESCRIPTOR, 0x2200, 4096)));
  FinishEvents();
  RunLoop();

  ASSERT_EQ(class_requests.size(), 1u);
  EXPECT_EQ(class_requests[0].bRequest, core::device::kHidGetReport);
  EXPECT_EQ(class_requests[0].wValue,
            ReportValue(core::device::kHidReportTypeInput, 0));

  std::array<uint8_t, 4096> out{};

  const ssize_t reply_len = Receive(out.data(), out.size());
  ASSERT_EQ(reply_len, 2);
  EXPECT_EQ(out[0], 0xAA);
  EXPECT_EQ(out[1], 0x55);

  const ssize_t hid_len = Receive(out.data(), out.size());
  ASSERT_EQ(hid_len, static_cast<ssize_t>(core::device::kFfsHidDescLength));
  EXPECT_EQ(std::memcmp(out.data(),
                        core::device::kFfsFsDescriptors +
                            core::device::kFfsHidDescOffset,
                        core::device::kFfsHidDescLength),
            0);

  const ssize_t report_len = Receive(out.data(), out.size());
  ASSERT_EQ(report_len,
            static_cast<ssize_t>(sizeof(core::device::kHidReportDescriptor)));
  EXPECT_EQ(std::memcmp(out.data(), core::device::kHidReportDescriptor,
                        sizeof(core::device::kHidReportDescriptor)),
            0);

  EXPECT_EQ(ep0_closed, 1);
  std::lock_guard<std::mutex> lock(State().input_mutex);
  EXPECT_FALSE(State().link_up);
}

// ENABLE/DISABLE/UNBIND 是链路状态的唯一来源；BIND/SUSPEND/RESUME 不改变它，
// 且不能打断后续事件的处理。
TEST_F(Ep0Loop, LinkUpTracksEnableAndDisable) {
  std::vector<bool> link_up_seen;
  const usb_ctrlrequest class_request =
      MakeRequest(kClassIn, core::device::kHidGetIdle, 0, 1);
  State().handlers.on_class_request =
      [&](const usb_ctrlrequest&, usb::Ep0Responder& ep0) {
        {
          std::lock_guard<std::mutex> lock(State().input_mutex);
          link_up_seen.push_back(State().link_up);
        }
        const uint8_t byte = 0;
        ep0.SendIn(&byte, 1);
      };

  Push(Event(static_cast<uint8_t>(FUNCTIONFS_BIND)));
  Push(Event(static_cast<uint8_t>(FUNCTIONFS_ENABLE)));
  Push(SetupEvent(class_request));
  Push(Event(static_cast<uint8_t>(FUNCTIONFS_SUSPEND)));
  Push(Event(static_cast<uint8_t>(FUNCTIONFS_DISABLE)));
  Push(SetupEvent(class_request));
  Push(Event(static_cast<uint8_t>(FUNCTIONFS_RESUME)));
  Push(Event(static_cast<uint8_t>(FUNCTIONFS_UNBIND)));
  Push(SetupEvent(class_request));
  FinishEvents();
  RunLoop();

  EXPECT_EQ(link_up_seen, (std::vector<bool>{true, false, false}));
}

// 未知标准请求走 STALL 分支：它既不能送数据，也不能吃掉下一条 setup。
TEST_F(Ep0Loop, UnknownStandardRequestDoesNotEatFollowingSetup) {
  int class_calls = 0;
  State().handlers.on_class_request =
      [&](const usb_ctrlrequest&, usb::Ep0Responder& ep0) {
        ++class_calls;
        const uint8_t byte = 0x7E;
        ep0.SendIn(&byte, 1);
      };

  // 0x02 = configuration descriptor：不归用户态答，本层直接 stall。
  Push(SetupEvent(MakeRequest(kStandardIn, USB_REQ_GET_DESCRIPTOR, 0x0200, 64)));
  Push(SetupEvent(MakeRequest(kClassIn, core::device::kHidGetIdle, 0, 1)));
  FinishEvents();
  RunLoop();

  EXPECT_EQ(class_calls, 1);
  std::array<uint8_t, 8> out{};
  ASSERT_EQ(Receive(out.data(), out.size()), 1);
  EXPECT_EQ(out[0], 0x7E);
}

// 端到端：SET_REPORT 的 data stage 与下一条 setup 共用同一个 fd，顺序必须靠
// "先读数据段、再读事件"维持。入队成功同时证明那段数据确实被 read 走了。
TEST_F(Ep0Loop, SetReportDataStageIsConsumedBeforeNextSetup) {
  bridge::Init("AA:BB:CC:DD:EE:FF", 2);
  State().handlers.on_class_request =
      [](const usb_ctrlrequest& request, usb::Ep0Responder& ep0) {
        bridge::OnClassRequest(request, ep0);
      };

  std::array<uint8_t, core::protocol::kUsbOutputReportSize> payload{};
  payload[0] = core::protocol::kUsbOutputReportId;
  payload[1] = 0x42;

  Push(SetupEvent(MakeRequest(kClassOut, core::device::kHidSetReport,
                              ReportValue(core::device::kHidReportTypeOutput,
                                          core::protocol::kUsbOutputReportId),
                              static_cast<uint16_t>(payload.size()))));
  PushData(payload.data(), payload.size());
  Push(SetupEvent(MakeRequest(kClassIn, core::device::kHidGetReport,
                              ReportValue(core::device::kHidReportTypeInput, 0),
                              64)));
  FinishEvents();
  bt::StatsWindow();  // 入队发生在 RunLoop 里，基线先清零
  RunLoop();

  std::array<uint8_t, 128> out{};
  const ssize_t received = Receive(out.data(), out.size());
  ASSERT_EQ(received,
            static_cast<ssize_t>(core::protocol::kUsbInputReportSize));
  EXPECT_EQ(out[0], core::protocol::kUsbInputReportId);
  EXPECT_GE(bt::StatsWindow().queue_high_water, 1u);
}

}  // namespace
