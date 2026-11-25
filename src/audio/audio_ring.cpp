/**
 * @file audio_ring.cpp
 * @brief audio::AudioRing 实现：SPA 的两个索引 + 自有的定长存储
 *
 * 读写路径上只有原子操作与 memcpy：没有锁、没有再分配。索引的正确性由
 * `spa_ringbuffer` 保证（release 提交自己的索引、acquire 读对方的索引）；
 * 这里只加 SPA 不替调用方决定的那层策略——满了丢最旧。
 */

#include "audio/audio_ring.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace audio {

AudioRing::AudioRing(size_t capacity_bytes)
    : storage_(new std::byte[capacity_bytes]), capacity_(capacity_bytes) {
  assert(capacity_bytes > 0);
}

AudioRing::~AudioRing() { delete[] storage_; }

size_t AudioRing::Readable() const {
  // SPA 的读接口没有 const 修饰，但它只做原子读，不改结构体。
  spa_ringbuffer* ring = const_cast<spa_ringbuffer*>(&ring_);
  uint32_t index = 0;
  const int32_t fill = spa_ringbuffer_get_read_index(ring, &index);
  if (fill <= 0) return 0;  // 空的，或差值越过 2³¹（纠正留给 Read）
  return std::min(static_cast<size_t>(fill), capacity_);
}

bool AudioRing::Write(const void* source, size_t bytes) {
  if (bytes == 0) return true;
  if (source == nullptr) return false;
  // 一批装不下整环：绕回来会覆盖自己的前半段。容量下限由调用方保证。
  if (bytes > capacity_) return false;

  uint32_t write = 0;
  (void)spa_ringbuffer_get_write_index(&ring_, &write);
  // 不看剩余空间：写索引前进多少就覆盖多少最旧的数据，丢旧由读方下次取数据时
  // 发现。
  spa_ringbuffer_write_data(&ring_, storage_, static_cast<uint32_t>(capacity_),
                            static_cast<uint32_t>(write % capacity_), source,
                            static_cast<uint32_t>(bytes));
  // release 提交在 SPA 的 write_update 里：字节先落地，索引后可见。
  spa_ringbuffer_write_update(
      &ring_, static_cast<int32_t>(write + static_cast<uint32_t>(bytes)));
  return true;
}

size_t AudioRing::Read(void* destination, size_t max_bytes) {
  if (destination == nullptr || max_bytes == 0) return 0;

  uint32_t read = 0;
  int32_t fill = spa_ringbuffer_get_read_index(&ring_, &read);

  if (fill < 0) {
    // 读方缺席太久（流被暂停数小时而写方仍在推进），差值越过 2³¹ 翻了符号。
    // 这不是欠载，以写索引为基准只留最新一整环。
    uint32_t write = 0;
    (void)spa_ringbuffer_get_write_index(&ring_, &write);
    read = write - static_cast<uint32_t>(capacity_);
    spa_ringbuffer_read_update(&ring_, static_cast<int32_t>(read));
    fill = static_cast<int32_t>(capacity_);
  } else if (static_cast<size_t>(fill) > capacity_) {
    // 写方已覆盖最旧的未读数据：跳到最新一整环再读。
    const size_t skip = static_cast<size_t>(fill) - capacity_;
    read += static_cast<uint32_t>(skip);
    spa_ringbuffer_read_update(&ring_, static_cast<int32_t>(read));
    fill = static_cast<int32_t>(capacity_);
  }

  const size_t bytes = std::min(static_cast<size_t>(fill), max_bytes);
  if (bytes == 0) return 0;

  spa_ringbuffer_read_data(&ring_, storage_, static_cast<uint32_t>(capacity_),
                           static_cast<uint32_t>(read % capacity_), destination,
                           static_cast<uint32_t>(bytes));
  spa_ringbuffer_read_update(
      &ring_, static_cast<int32_t>(read + static_cast<uint32_t>(bytes)));
  return bytes;
}

void AudioRing::Clear() {
  // 读方跟着写方走：不碰写索引，因此不会变成第二个写者。acquire 保证看到的是
  // 一个已提交的写位置，而不是写方正在改的中间值。
  uint32_t write = 0;
  (void)spa_ringbuffer_get_write_index(&ring_, &write);
  spa_ringbuffer_read_update(&ring_, static_cast<int32_t>(write));
}

}  // namespace audio
