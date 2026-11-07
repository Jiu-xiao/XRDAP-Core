// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针端采样的零件：时间常数、节拍账目和帧缓冲。采样器本体在 sampler.hpp。
//
// The parts of probe-side sampling: time constants, tick accounting and the frame ring.
// The sampler itself is in sampler.hpp.

#include <cstdint>

// ===== Tick accounting 节拍账目 =====

// 采样节拍的账目（docs/protocol.md 3.5）。节拍定时器是唯一的时钟：它的中断每个周期把
// 节拍计数加上实际跨过的周期数。采样醒来时读计数 c，没读过的最新节拍就是 c；从上一次采到
// 的节拍到 c 之间没采的节拍都算跳过。来不及时不补做。
// 每帧之后定下一个要采的节拍：它晚于采完时的计数，并且从这一帧的节拍算起至少过了
// “这一帧的占用 + 间隙”，间隙为 max(kMinGapUs, 周期/8, 占用/4)。所以两帧之间同一核上
// 的别的任务至少有这段间隙（减去唤醒的延迟），约占这个核 1/5 以上的时间，与周期无关。
// 中断只在计数到达下一个要采的节拍时唤醒执行上下文（TickWakes），所以它只为要采的节拍
// 醒来，不会为跳过的节拍一拍拍空转。
//
// Sampling tick accounting (docs/protocol.md 3.5). The tick timer is the only clock: its
// interrupt adds the number of elapsed periods to the tick count. When sampling wakes it
// reads the count c, and the newest tick not yet read is c; every tick between the last
// one sampled and c counts as skipped. Late ticks are not made up. After each frame the
// next tick to sample is chosen: it is after the count when the frame ended, and at least
// "this frame's busy time + gap" after this frame's tick, with gap = max(kMinGapUs,
// period/8, busy/4). Other tasks on the same core therefore get at least that gap between
// two frames (less the wake latency), about 1/5 of the core or more, whatever the period.
// The interrupt wakes the execution context only when the count reaches the next tick to
// sample (TickWakes), so it wakes only for ticks it samples and does not spin through
// skipped ones.

namespace XrDap::Vendor
{

/// @brief 计数 a 是否已到达或越过 b，计数回绕也成立 / Whether count a has reached or
///        passed b, also across wraparound.
inline bool TickDue(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }

/// @brief 中断把计数加到 count 后是否唤醒采样：只在到达下一个要采的节拍时 / Whether the
///        interrupt wakes sampling after raising the count to `count`: when it
///        reaches or passes the next tick to sample.
/// @note 中断上下文 / Interrupt context.
inline bool TickWakes(uint32_t count, uint32_t wake_at)
{
  return TickDue(count, wake_at);
}

/// 节拍账目 / Tick accounting.
/// @note 只在执行上下文里用 / Used only in the execution context.
class TickAccount
{
 public:
  /// 两帧之间至少留给别的任务的时间 / Least time between two frames left to other
  /// tasks.
  static constexpr uint32_t kMinGapUs = 15U;

  /// 醒来时的决定 / Decision on a wake.
  struct Step
  {
    bool sample;       ///< 采这一拍 / Sample this tick.
    uint32_t tick;     ///< 节拍号 / Tick number.
    uint32_t skipped;  ///< 这次新跳过的节拍数 / Ticks newly skipped.
  };

  /// 一次采样开始：节拍 0 马上采 / A sampling run starts: tick 0 is sampled at once.
  void Begin();

  /// @brief 醒来，计数为 count / Woken with the tick count at `count`.
  Step OnWake(uint32_t count);

  /// @brief 刚采完 OnWake 给的节拍，定下一个要采的节拍 / The tick from OnWake has just
  ///        been sampled; choose the next tick to sample.
  /// @param count_after 采完时的计数 / Tick count when done.
  /// @param busy_us 这一拍占用的时间 / Time the tick took.
  /// @param period_us 周期 / Period.
  void OnDone(uint32_t count_after, uint32_t busy_us, uint32_t period_us);

  /// @brief 下一个要采的节拍 / Next tick to sample.
  uint32_t Next() const { return next_; }

 private:
  bool has_last_ = false;
  uint32_t last_ = 0U;
  uint32_t next_ = 0U;
};

}  // namespace XrDap::Vendor

// ===== Frame ring 帧缓冲 =====

// 采样帧的环形缓冲。每帧前面有 2 字节小端长度头，只按整帧取出：低 12 位是帧长，高 4 位
// 记下紧凑帧要用的位图字节数（3 位）和是否有条目失败（1 位），采样时就知道，取帧时不必
// 再推算。采样写、SUB_READ 读，都在同一个执行上下文里顺序进行，所以没有锁。
//
// Ring buffer of sample frames. Each frame is preceded by a 2-byte little-endian length
// head and is only taken out whole: the low 12 bits are the frame length, the high 4 bits
// keep what compact frames need, the bitmap bytes (3 bits) and whether an item failed (1
// bit), known when sampling so that taking need not work them out again. Sampling writes
// and SUB_READ reads, both in the one execution context and one after the other, so there
// is no lock.

#include <cstdint>

#include "xrdap/vendor/protocol.hpp"
#include "xrdap_port.hpp"

namespace XrDap::Vendor
{

/// 采样帧环形缓冲 / Ring buffer of sample frames.
/// @note 都只在执行上下文里调用，不能在中断里 / All of them are called in the execution
///       context only, never from an interrupt.
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
  /// @param bitmap_len 帧里位图的字节数（紧凑帧用）/ Bitmap bytes in the frame (for
  ///        compact frames).
  /// @param failed 位图里有失败的条目（紧凑帧用）/ The bitmap marks a failed item (for
  ///        compact frames).
  void Push(const uint8_t* frame, uint16_t len, bool stop_on_full,
            uint8_t bitmap_len = 0U, bool failed = false);
  /// 取出不超过 limit 字节的整帧，帧之间不留长度头 / Take whole frames, at most limit
  /// bytes, without the length headers.
  Taken Take(uint8_t* dst, uint32_t limit);
  /// @brief 同 Take，但编成紧凑帧（docs/protocol.md 3.5），limit 按编码后的字节算 / As
  ///        Take, but encoded as compact frames (docs/protocol.md 3.5); limit counts the
  ///        encoded bytes.
  Taken TakeCompact(uint8_t* dst, uint32_t limit);
  /// 记下跳过的节拍 / Count skipped ticks.
  void AddSkipped(uint32_t n);

 private:
  uint16_t Header(uint32_t pos) const;
  uint16_t FrameLen(uint32_t pos) const;
  void Pop(uint32_t& pos, uint16_t flen);
  void Fill(Taken& t) const;
  void CopyIn(uint32_t at, const uint8_t* src, uint32_t len);
  void CopyOut(uint32_t at, uint8_t* dst, uint32_t len) const;

  uint8_t buf_[kRingBytes] = {};
  uint32_t head_ = 0U;
  uint32_t tail_ = 0U;
  uint32_t used_ = 0U;
  uint32_t payload_ = 0U;
  uint32_t dropped_ = 0U;
  uint32_t skipped_ = 0U;
};

}  // namespace XrDap::Vendor

// ===== Compact frames 紧凑帧 =====

// SUB_READ 的紧凑帧（docs/protocol.md 3.5）：一次回复里第 1 帧原样，之后每帧的帧头换成
// h = (Δk << 1) | 有失败、Δt（µs），都是无符号 LEB128；只有有失败时才带位图；值原样。
// Δk、Δt 相对回复里的上一帧。缓冲里存的仍是原帧，编码只在取帧时做。
//
// Compact frames of SUB_READ (docs/protocol.md 3.5): the first frame of a reply goes as
// it is; every later frame's head becomes h = (Δk << 1) | failed and Δt (µs), both
// unsigned LEB128; the bitmap follows only when an item failed; the values are unchanged.
// Δk and Δt are relative to the previous frame of the reply. The ring keeps the plain
// frames; encoding happens only when frames are taken.

namespace XrDap::Vendor
{

/// 紧凑帧编码器，一次 SUB_READ 回复用一个 / Compact frame encoder, one per SUB_READ
/// reply.
class CompactEncoder
{
 public:
  /// 位图最长的字节数 / Longest bitmap in bytes.
  static constexpr uint16_t kMaxBitmap = (kMaxItems + 7U) / 8U;
  /// 原帧开头要给 Head() 的字节数上限：节拍号 4 + 时间戳 6 + 位图 / Most bytes of a
  /// plain frame's start Head() needs: tick 4 + timestamp 6 + bitmap.
  static constexpr uint16_t kMaxRawHead = 10U + kMaxBitmap;
  /// 最长的紧凑帧头：h 5 + Δt 7 + 位图 / Longest compact head: h 5 + Δt 7 + bitmap.
  static constexpr uint16_t kMaxHead = 5U + 7U + kMaxBitmap;

  /// @brief 编出一帧的帧头，不改编码器的状态 / Encode one frame's head without changing
  ///        the encoder's state.
  /// @param raw 原帧开头的 10 + bitmap_len 字节 / The first 10 + bitmap_len bytes of the
  ///        plain frame.
  /// @param bitmap_len 这一帧的位图字节数 / Bitmap bytes of this frame.
  /// @param failed 位图里有失败的条目 / The bitmap marks a failed item.
  /// @param head 至少 kMaxHead 字节 / At least kMaxHead bytes.
  /// @param raw_skip 原帧里被帧头取代的字节数，原帧其余部分原样跟在帧头后面 / Bytes of
  ///        the plain frame the head replaces; the rest of the plain frame follows the
  ///        head unchanged.
  /// @return 帧头字节数；回复的第 1 帧为 0（整帧原样）/ Bytes of the head; 0 for the
  ///         first frame of a reply (the whole frame as it is).
  uint16_t Head(const uint8_t* raw, uint16_t bitmap_len, bool failed, uint8_t* head,
                uint16_t& raw_skip) const
  {
    if (first_)
    {
      raw_skip = 0U;
      return 0U;
    }
    const uint32_t dk = Get32(raw) - tick_;
    const uint64_t dt = (Get48(raw + 4) - stamp_) & kStampMask;
    uint16_t n = PutVarint(head, (static_cast<uint64_t>(dk) << 1) | (failed ? 1U : 0U));
    n = static_cast<uint16_t>(n + PutVarint(head + n, dt));
    if (failed)
    {
      for (uint16_t i = 0; i < bitmap_len; ++i)
      {
        head[n++] = raw[10U + i];
      }
    }
    raw_skip = static_cast<uint16_t>(10U + bitmap_len);
    return n;
  }

  /// @brief 这一帧放进了回复，后面的帧相对它编码 / This frame went into the reply; later
  ///        frames are encoded relative to it.
  void Accept(const uint8_t* raw)
  {
    first_ = false;
    tick_ = Get32(raw);
    stamp_ = Get48(raw + 4);
  }

 private:
  static constexpr uint64_t kStampMask = (uint64_t{1} << 48) - 1U;

  // 无符号 LEB128：每字节低 7 位，高位为 1 表示还有下一字节 / Unsigned LEB128: 7 bits
  // per byte, the high bit set when another byte follows.
  static uint16_t PutVarint(uint8_t* p, uint64_t v)
  {
    uint16_t n = 0U;
    while (v >= 0x80U)
    {
      p[n++] = static_cast<uint8_t>(v | 0x80U);
      v >>= 7;
    }
    p[n++] = static_cast<uint8_t>(v);
    return n;
  }

  bool first_ = true;
  uint32_t tick_ = 0U;
  uint64_t stamp_ = 0U;
};

}  // namespace XrDap::Vendor
