/**
 * @file
 * @brief bt 后端共用件：HIDP 剥头、可写等待、feature 缓存本体
 *
 * feature 缓存是进程级单例：一个 worker 进程只服务一只手柄，每个 report id
 * 存一份就够了。它刻意不加锁，因为读写它的线程时序是错开的——建链阶段由
 * worker 主线程预取（此时其它线程还没起来），运行期只有 usb 的 ep0 线程经
 * GetFeature() 读写它，而 Close()/CacheClear() 发生在所有线程 join 之后。
 * 改动这个时序（比如让传输线程也查缓存）就必须先给这里补锁。
 */

#include "bt/bt_detail.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <unordered_map>
#include <vector>

namespace {

struct FeatureCache {
  /// report id → 完整报告字节（含首字节 report id），条目按 id 唯一、后来者覆盖。
  std::unordered_map<uint8_t, std::vector<uint8_t>> entries;
};

/// feature 缓存的唯一实例：函数内 static，进程级一份。条目只在 §B 的三个函数
/// 里增删，锁的缺失靠调用线程的时序保证（见本文件头注）。
FeatureCache& GetCache() {
  static FeatureCache cache;
  return cache;
}

}  // namespace

namespace bt::detail {

size_t StripHidpDataIn(uint8_t* buf, size_t n) {
  // 空输入没有可剥的头，返回 0 让调用方把它当"没有报告"处理。
  if (n < 1) return 0;
  // 原地左移一格：memmove 保证重叠区间搬运合法。
  std::memmove(buf, buf + 1, n - 1);
  return n - 1;
}

IoResult WaitWritable(int fd, int deadline_ms) {
  using std::chrono::steady_clock;
  // 只有正数才是"总预算"语义；0/负值原样透传给 poll()（不等/一直等）。
  const bool bounded = deadline_ms > 0;
  const auto deadline = steady_clock::now() +
                        std::chrono::milliseconds(deadline_ms);

  for (;;) {
    // 把 deadline 换算成本轮 poll 的剩余预算，防止多轮 EINTR 重试把总等待
    // 时间放大成 n × deadline。
    if (bounded) {
      const auto remaining = std::chrono::duration_cast<
                                 std::chrono::milliseconds>(
                                 deadline - steady_clock::now())
                                 .count();
      if (remaining <= 0) return kAgain;  // 预算耗尽：暂不可写，可重试
      // 只在剩余不足时收紧：poll 的超时是"这一轮最多等多久"，本轮预算必须
      // ≤ 剩余，否则一轮就把总预算花超。
      if (remaining < deadline_ms) deadline_ms = static_cast<int>(remaining);
    }

    struct pollfd pfd = {fd, POLLOUT, 0};
    const int pr = ::poll(&pfd, 1, deadline_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;  // 信号打断不算超时，继续等剩余预算
      return kFatal;
    }
    if (pr == 0) return kAgain;                        // 预算内没等到可写
    if (pfd.revents & (POLLHUP | POLLNVAL)) return kClosed;  // 挂断/fd 失效：链路没了
    if (pfd.revents & POLLERR) return kFatal;                // 写方向出错：不是"等"能解决的
    return kOk;
  }
}

int CacheLookup(uint8_t* buf, size_t len) {
  if (buf == nullptr || len == 0) return -1;

  FeatureCache& cache = GetCache();
  const auto it = cache.entries.find(buf[0]);
  if (it == cache.entries.end()) return -1;

  // 缓存条目可能比调用方缓冲区长：按容量截断交出去，调用方拿到的是前缀。
  const size_t copied = std::min(it->second.size(), len);
  std::memcpy(buf, it->second.data(), copied);
  return static_cast<int>(copied);
}

void CacheStore(uint8_t report_id, const uint8_t* data, size_t len) {
  // 空数据没什么可缓存的，直接忽略而不是存一个空条目占住这个 id。
  if (data == nullptr || len == 0) return;
  GetCache().entries[report_id].assign(data, data + len);
}

void CacheClear() { GetCache().entries.clear(); }

}  // namespace bt::detail
