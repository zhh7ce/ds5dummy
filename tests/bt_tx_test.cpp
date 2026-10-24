// bt::PostOutput 单测：入队点的参数校验、队列深度与丢最新策略、Stats 窗口口径。
//
// 全程不调 bt::StartThreads()：没有链路、没有读写两条传输线程，被测对象就是那个
// 进程级队列本身。这也是 bt::PostOutput 的门面契约之一——它是非阻塞纯函数，
// 线程起来之前就能收东西。
//
// 队列与计数都是进程级状态，一个测试进程里跑完所有用例，所以：
//   - 每个用例前后 bt::StopThreads()：它是幂等的收尾，也是唯一能把队列清空的公开入口
//     （没有为测试开的 reset）。
//   - bt::StopThreads() **不清**计数：累计量（dropped 等）活整个进程，窗口量
//     （queue_high_water）只由 bt::StatsWindow() 读走即清。因此用例一律先读一次
//     当基线、比增量——worker 的 5 秒统计本来就是这么算的。

#include "bt/bt.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

// 一份合法的输出报告体：[0] 报告 id，[1] 留给 bt::PostOutput 打 seq，
// 其余填内容签名以便区分是哪一份。长度取真实的 0x31 报告。
std::vector<uint8_t> make_body(size_t len, uint8_t report_id = 0x31) {
    std::vector<uint8_t> body(len, 0);
    body[0] = report_id;
    if (len > 2) body[2] = 0x10;
    return body;
}

class BtTxPostOutput : public ::testing::Test {
protected:
    void SetUp() override {
        bt::StopThreads();
        baseline_ = bt::StatsWindow();  // 顺带清掉窗口量，下面的断言只看本用例的增量
    }
    void TearDown() override { bt::StopThreads(); }

    // 本用例开始时的累计丢弃份数。
    uint64_t dropped_baseline() const { return baseline_.dropped; }

    bt::Stats baseline_{};
};

// 空指针连报告 id 都没有：入队点必须在拷贝之前拒掉。
TEST_F(BtTxPostOutput, RejectsNullPointer) {
    EXPECT_FALSE(bt::PostOutput(nullptr, 78));
}

// byte[0] 是报告 id、byte[1] 是 seq 槽、末尾 4 字节是 CRC 的位置——三者不能重叠，
// 否则打 seq 会踩进 CRC 窗口，或者 CRC 写到缓冲区外面。
TEST_F(BtTxPostOutput, RejectsBodiesWithoutRoomForSeqAndCrc) {
    for (size_t len = 0; len < 6; ++len) {
        const std::vector<uint8_t> body = make_body(len == 0 ? 1 : len);
        EXPECT_FALSE(bt::PostOutput(body.data(), len)) << "len=" << len;
    }
}

// kMinOutputBody 本身必须放行：边界值拒了就等于真实的短报告也发不出去。
TEST_F(BtTxPostOutput, AcceptsShortestLegalBody) {
    const std::vector<uint8_t> body = make_body(6);
    EXPECT_TRUE(bt::PostOutput(body.data(), body.size()));
}

// 上限不能设得比真实报告小：0x31 与 0x36 两份都必须收得下。
TEST_F(BtTxPostOutput, AcceptsBothRealReportSizes) {
    // 0x31 状态报告 78B，0x36 音频报告 398B = kMaxOutputBody 的边界。
    const std::vector<uint8_t> s = make_body(78);
    const std::vector<uint8_t> a = make_body(bt::kMaxOutputBody, 0x36);
    EXPECT_TRUE(bt::PostOutput(s.data(), s.size()));
    EXPECT_TRUE(bt::PostOutput(a.data(), a.size()));
}

// 超出槽位容量的一律拒收：定长 SendItem 装不下，截断着入队更糟。
TEST_F(BtTxPostOutput, RejectsBodyLongerThanMax) {
    const std::vector<uint8_t> body = make_body(bt::kMaxOutputBody + 1, 0x36);
    EXPECT_FALSE(bt::PostOutput(body.data(), body.size()));
}

// 锁住 kQueueMax 与"满则丢最新"这条背压策略：改深度或改成丢最旧都会在这里失败。
TEST_F(BtTxPostOutput, FillsQueueToDepthThenDropsNewest) {
    const uint64_t dropped_before = dropped_baseline();
    const std::vector<uint8_t> body = make_body(bt::kMaxOutputBody, 0x36);
    for (size_t i = 0; i < bt::kQueueMax; ++i) {
        ASSERT_TRUE(bt::PostOutput(body.data(), body.size())) << "item " << i;
    }
    // 满了：被拒的是刚来的这一份（丢最新），已入队的 seq 因此没有空洞。
    EXPECT_FALSE(bt::PostOutput(body.data(), body.size()));
    const bt::Stats s = bt::StatsWindow();
    EXPECT_EQ(s.dropped, dropped_before + 1);
    EXPECT_EQ(s.queue_high_water, bt::kQueueMax);
}

// 窗口量的定义就是"两次读取之间"：读走即清，第二次必须看到 0。
TEST_F(BtTxPostOutput, HighWaterIsWindowedAndReadsClear) {
    const std::vector<uint8_t> body = make_body(78);
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(bt::PostOutput(body.data(), body.size()));
    }
    EXPECT_EQ(bt::StatsWindow().queue_high_water, 3u);
    // 读走即清：第二次调只覆盖"自上次以来"的区间，此时没有新的入队。
    EXPECT_EQ(bt::StatsWindow().queue_high_water, 0u);
}

// 累计量与窗口量的区别：dropped 跨窗口存活，StatsWindow() 读它不清它。
TEST_F(BtTxPostOutput, DroppedCountIsCumulativeAcrossWindows) {
    const uint64_t dropped_before = dropped_baseline();
    const std::vector<uint8_t> body = make_body(78);
    for (size_t i = 0; i <= bt::kQueueMax; ++i) {
        bt::PostOutput(body.data(), body.size());
    }
    EXPECT_EQ(bt::StatsWindow().dropped, dropped_before + 1);
    EXPECT_EQ(bt::StatsWindow().dropped, dropped_before + 1);  // 累计量不清零
}

// bt::StopThreads() 只清队列，不动计数：worker 退出前的最后一次统计仍然看得到
// 整段运行的累计。清零发生在 bt::StartThreads() 里的 tx::Reset()，只在起线程前跑。
TEST_F(BtTxPostOutput, StopClearsQueueButKeepsCounters) {
    const uint64_t dropped_before = dropped_baseline();
    const std::vector<uint8_t> body = make_body(78);
    for (size_t i = 0; i <= bt::kQueueMax; ++i) {
        bt::PostOutput(body.data(), body.size());
    }
    bt::StopThreads();
    EXPECT_EQ(bt::StatsWindow().dropped, dropped_before + 1);
}

// 入队不等于上链路：两条传输线程没起来，写侧与读侧的计数都必须停在 0。
TEST_F(BtTxPostOutput, WithoutTransportThreadTxAndRxCountersStayZero) {
    const std::vector<uint8_t> body = make_body(bt::kMaxOutputBody, 0x36);
    ASSERT_TRUE(bt::PostOutput(body.data(), body.size()));
    const bt::Stats s = bt::StatsWindow();
    EXPECT_EQ(s.tx_31, 0u);   // 计数发生在排空时，不是入队时
    EXPECT_EQ(s.tx_36, 0u);
    EXPECT_EQ(s.rx_reports, 0u);
    EXPECT_EQ(s.write_count, 0u);
}

// 收尾之后队列必须重新装得下：否则一轮丢包会让后续每轮都一入队就满。
TEST_F(BtTxPostOutput, StopRefillsQueueForTheNextRound) {
    const std::vector<uint8_t> body = make_body(78);
    for (size_t i = 0; i < bt::kQueueMax; ++i) {
        ASSERT_TRUE(bt::PostOutput(body.data(), body.size()));
    }
    ASSERT_FALSE(bt::PostOutput(body.data(), body.size()));

    bt::StopThreads();  // 幂等收尾：队列清空，但窗口量 high water 还留着 8
    EXPECT_EQ(bt::StatsWindow().queue_high_water, bt::kQueueMax);

    // 重新起一轮：读走基线之后队列是空的，再入队一份就是新的最深度。
    EXPECT_TRUE(bt::PostOutput(body.data(), body.size()));
    EXPECT_EQ(bt::StatsWindow().queue_high_water, 1u);
}

} // namespace
