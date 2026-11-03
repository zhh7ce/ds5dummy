/**
 * @file
 * @brief bt.h 的实现：链路生命周期、feature 缓存策略，以及两条线程的起停
 *
 * 本文件是门面的后端无关实现：`bt::` 里除两条线程本体之外的每个入口都在这里
 * 落地一次，然后转发到 §A 接缝（bt_detail.h）或数据平面（bt_plane.h）。
 * 三件事因此从"两个后端各自的约定"变成单源不变量：
 * - 建链先关后开、以及缓存冷启动（`Open()` 顶部与 `Close()` 各清一次，
 *   预取因此永远命中不到旧链路的条目）；
 * - feature 缓存的唯一写者是 `GetFeature()`——后端只负责上线现取。
 *
 * 线程编排也在这里落地，是因为顺序只有门面看得见：`plane::Start()` 必须早于
 * 两条线程（它们都要 poll 收工 fd），`plane::Finish()` 必须晚于两次 join
 * （它们还握着那两个 eventfd）。线程本体在 `bt_rx.cpp`、`bt_tx.cpp`。
 *
 * @note 线程模型：本文件的链路入口都在调用方线程上执行，不起线程、不加锁。
 *       feature 缓存的无锁前提是这里的时序：建链期由 worker 主线程预取，
 *       运行期只有 usb 的 ep0 线程经 `GetFeature()` 触碰它。改动这个时序
 *       要先给 bt_detail.cpp 的缓存本体补锁。
 * @see bt_plane.h —— 两条线程共享的运行状态与判死入口。
 */

#include "bt/bt.h"

#include <cstddef>
#include <cstdint>

#include "bt/bt_detail.h"
#include "bt/bt_plane.h"
#include "core/debug.h"
#include "core/protocol/report.h"
#include "loguru.hpp"

namespace {

/// 现取 feature 报告用的整份缓冲：DS5 的 feature 报告没有超过这个尺寸的，
/// 缓存因此总能存下完整一份，交给调用方时才按对方容量截断。
constexpr size_t kFeatureFetchSize = 512;

/// 建链后预取一遍 feature 报告。此刻缓存已被 `bt::Open()` 清空，命中分支
/// 走不到，每一份都是上线现取；取到取不到都不算建链失败——`GetFeature()`
/// 之后仍能按需现取，这里只是让常用的几份不用等第一次往返。
void PrefetchFeatureReports() {
  uint8_t buf[kFeatureFetchSize];
  for (uint8_t id : core::protocol::kFeatureReportIds) {
    buf[0] = id;  // 约定：首字节进、出都是 report id
    const int got = bt::GetFeature(buf, sizeof(buf));
    if (got > 0) {
      DS5DUMMY_LOG(INFO, "[bt] prefetch 0x{:02X} cached ({} bytes)", id, got);
    } else {
      DS5DUMMY_LOG(WARNING, "[bt] prefetch 0x{:02X} — no reply (fetched live later)",
            id);
    }
  }
}

}  // namespace

namespace bt {

bool Open(const std::string& mac) {
  // 两步都排在建链之前：带着上一条链路的 fd 或报告字节开新链路没有任何意义。
  detail::Close();
  detail::CacheClear();

  if (!detail::Open(mac)) return false;

  PrefetchFeatureReports();
  return true;
}

void Close() {
  detail::Close();
  // 缓存跟着链路作废：旧手柄的报告字节对新链路没有任何意义。
  detail::CacheClear();
}

int GetFeature(uint8_t* buf, size_t len) {
  // 三种情形都没有可交出的字节：空指针没处写、len < 2 连"首字节是 id"这个
  // 约定都装不下、链路没开则既查不了缓存也上不了线。
  if (buf == nullptr || len < 2 || !detail::IsOpen()) return -1;

  const int cached = detail::CacheLookup(buf, len);
  if (cached > 0) return cached;

  // 现取先进整份缓冲、再存缓存：直接往调用方的缓冲区取，会让主机 wLength
  // 偏小的那次请求把一份被截短的报告永久留在缓存里。
  uint8_t full[kFeatureFetchSize];
  full[0] = buf[0];
  const int got = detail::GetFeature(full, sizeof(full));
  if (got <= 0) return -1;

  // 缓存键取回包自身的 id（后端保证 full[0] 就是报告 id）：条目 id 与内容
  // 永远一致，迟到的回包不会串到别的请求名下——上游 feature_data 同样按
  // 回包里 packet[1] 建键。
  detail::CacheStore(full[0], full, static_cast<size_t>(got));
  // 缓存里现在有整份；按调用方容量截出去，同一份报告在线上只跑一次。
  return detail::CacheLookup(buf, len);
}

bool StartThreads(const Handlers& handlers) {
  if (plane::Running()) {
    // 重入拒绝：两套线程共用一条链路会互相踩 fd、seq 和发送顺序。
    DS5DUMMY_LOG(ERROR, "[bt] data plane already running");
    return false;
  }

  // 清零排在建 fd 之前：队列里不残留上一轮的报告，seq 也从 0 起。
  tx::Reset();
  if (!plane::Start(handlers)) return false;

  // 任一没起来就把平面整个收回——半成品（一条在跑、一条没跑）不留。
  if (!rx::Start() || !tx::Start()) {
    StopThreads();
    return false;
  }

  DS5DUMMY_LOG(INFO, "[bt] transport threads started on fd {}", detail::Fd());
  return true;
}

void StopThreads() {
  // 叫停在前：两条线程都在 poll 收工 fd，这是唯一能把睡等的写线程叫回来的动作。
  plane::RequestStop();
  // 无条件 join：线程可能已经因判死自己退出，句柄仍 joinable，不回收就是
  // std::terminate 在析构路上等着。
  rx::Join();
  tx::Join();
  // fd 与回调表只能在两条线程都不跑之后再关、之后再清。
  plane::Finish();
  tx::ClearQueue();
}

}  // namespace bt
