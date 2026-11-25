/**
 * @file audio_ring.h
 * @brief 字节环：单写单读、无锁，写多了覆盖最旧的数据
 *
 * 按字节计的环形缓冲。写入与读取可以分别由两个线程同时进行，不加锁；写进去的
 * 比读走的多时，最旧的未读数据被覆盖，写方永不阻塞。
 *
 * @note 读写尺寸由调用方控制。每次读写的长度必须是调用方那个"元素"的整数倍，
 *       否则读起点会落在元素中间——71 字节的 Opus 包会从中间开始解码（噪声），
 *       16 字节的 4 声道帧错开会让四个声道整体轮转。本类不检查，也不会报错。
 * @note 存储自己持有：构造时分配、析构时释放；不可拷贝、不可移动。
 */

#pragma once

#include <spa/utils/ringbuffer.h>

#include <cstddef>

namespace audio {

/**
 * @brief 一个字节环实例；容量在构造时固定，定长存储随对象存活。
 */
class AudioRing {
 public:
  /**
   * @brief 按容量分配存储，两个索引归零。
   *
   * @param capacity_bytes 容量，字节；不得小于单次最大写入量，否则那一批绕回来
   *                       会覆盖自己的前半段。
   */
  explicit AudioRing(size_t capacity_bytes);

  ~AudioRing();

  AudioRing(const AudioRing&) = delete;
  AudioRing& operator=(const AudioRing&) = delete;

  /// 容量（字节）。
  size_t capacity() const { return capacity_; }

  /**
   * @brief 当前可读字节数，纠偏到 [0, 容量]。
   *
   * 生产者也用它判断"攒够一段没有"，再决定要不要按门铃。
   */
  size_t Readable() const;

  /**
   * @brief 写入一批；满了覆盖最旧的未读数据。
   *
   * @param source 源数据。
   * @param bytes  字节数，不得超过容量。
   * @return 写入 true；source 为空或 bytes 超过容量时 false。
   */
  bool Write(const void* source, size_t bytes);

  /**
   * @brief 读最多 max_bytes 字节。
   *
   * @param destination 目标缓冲，至少 max_bytes 字节。
   * @param max_bytes   最多读多少；不够就少给。
   * @return 实读字节数，可为 0。积压超过容量时先跳过最旧的一整环。
   */
  size_t Read(void* destination, size_t max_bytes);

  /**
   * @brief 丢弃全部未读数据：读索引推到写索引。
   *
   * @note 只推进读索引，因此只能由读的那一方调用。
   */
  void Clear();

 private:
  std::byte* storage_;     ///< 自有存储，容量 bytes
  const size_t capacity_;  ///< 容量（字节）
  spa_ringbuffer ring_{};  ///< 两个自由递增的字节索引（SPA）
};

}  // namespace audio
