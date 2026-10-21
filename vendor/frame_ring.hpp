// SPDX-License-Identifier: Apache-2.0
#pragma once

// 采样帧的环形缓冲。每帧前面有 2 字节小端长度，只按整帧取出。采样写、SUB_READ 读，两边
// 可以在不同的任务、不同的核上，经平台的自旋锁（xrdap_port.hpp 的 Port::SpinLock）互斥。
// 不用会阻塞的互斥量：SUB_READ 拷贝时采样任务来写，会让优先级最高的采样任务睡下、经优先级
// 继承来回切换，短周期下节拍因此迟到。
//
// Ring buffer of sample frames. Each frame is preceded by a 2-byte little-endian length
// and is only taken out whole. Sampling writes and SUB_READ reads, possibly in different
// tasks or on different cores, under the platform's spinlock (Port::SpinLock from
// xrdap_port.hpp). A blocking mutex is not used: a tick that arrives while SUB_READ
// copies would put the highest-priority sampling task to sleep and switch back and forth
// through priority inheritance, making ticks late at short periods.

#include <cstdint>

#include "vendor_protocol.hpp"
#include "xrdap_port.hpp"

namespace XrDap::Vendor
{

/// 采样帧环形缓冲 / Ring buffer of sample frames.
class FrameRing
{
 public:
  /// 一次取帧的结果 / Result of one take.
  struct Taken
  {
    /// 拷出的字节数 / Bytes copied out.
    uint32_t bytes = 0;
    /// 拷出的帧数 / Frames copied out.
    uint16_t frames = 0;
    /// 缓冲里剩下的帧数据字节数（不含长度头）/ Frame bytes left (length headers
    /// excluded).
    uint32_t remaining = 0;
    /// 累计丢弃的帧数 / Frames dropped so far.
    uint32_t dropped = 0;
    /// 累计跳过的节拍数 / Ticks skipped so far.
    uint32_t skipped = 0;
  };

  /// 清空缓冲和计数 / Clear the buffer and the counters.
  void Reset();
  /// @brief 放入一帧 / Put one frame in.
  /// @param stop_on_full 缓冲满时丢新帧；否则丢最旧的帧 / When full, drop the new frame;
  ///        otherwise drop the oldest frames.
  void Push(const uint8_t* frame, uint16_t len, bool stop_on_full);
  /// 取出不超过 limit 字节的整帧，帧之间不留长度头 / Take whole frames, at most limit
  /// bytes, without the length headers.
  Taken Take(uint8_t* dst, uint32_t limit);
  /// 记下跳过的节拍 / Count skipped ticks.
  void AddSkipped(uint32_t n);

 private:
  static_assert((kRingBytes & (kRingBytes - 1U)) == 0U,
                "kRingBytes must be a power of two");

  uint16_t FrameLen(uint32_t pos) const;
  void CopyIn(uint32_t at, const uint8_t* src, uint32_t len);
  void CopyOut(uint32_t at, uint8_t* dst, uint32_t len) const;

  Port::SpinLock lock_;
  uint8_t buf_[kRingBytes] = {};
  uint32_t head_ = 0U;
  uint32_t tail_ = 0U;
  uint32_t used_ = 0U;
  uint32_t payload_ = 0U;
  uint32_t dropped_ = 0U;
  uint32_t skipped_ = 0U;
};

}  // namespace XrDap::Vendor
