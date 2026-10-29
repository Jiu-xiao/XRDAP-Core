// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/sampling.hpp"

#include <cstring>

// ===== Tick accounting 节拍账目 =====

namespace XrDap::Vendor
{

void TickAccount::Begin()
{
  has_last_ = false;
  last_ = 0U;
  next_ = 0U;
}

TickAccount::Step TickAccount::OnWake(uint32_t count)
{
  // 计数没走到下一个要采的节拍：停止或开始时的唤醒，或多余的一次唤醒。
  // The count has not reached the next tick to sample: a stop or start wake, or a spare
  // wake.
  if (!TickDue(count, next_) || (has_last_ && count == last_))
  {
    return {false, count, 0U};
  }
  const uint32_t skipped = has_last_ ? count - last_ - 1U : count;
  has_last_ = true;
  last_ = count;
  next_ = count + 1U;
  return {true, count, skipped};
}

void TickAccount::OnDone(uint32_t count_after, uint32_t busy_us, uint32_t period_us)
{
  uint32_t gap = (period_us / 8U > kMinGapUs) ? period_us / 8U : kMinGapUs;
  gap = (busy_us / 4U > gap) ? busy_us / 4U : gap;
  // 从这一帧的节拍起要过的周期数，向上取整，至少 1 / Periods to pass from this frame's
  // tick, rounded up, at least 1.
  const uint64_t span = static_cast<uint64_t>(busy_us) + gap;
  uint64_t periods = (span + period_us - 1U) / period_us;
  periods = (periods < 1U) ? 1U : periods;
  periods = (periods > 0x7FFFFFFFU) ? 0x7FFFFFFFU : periods;
  uint32_t next = last_ + static_cast<uint32_t>(periods);
  if (TickDue(count_after, next))
  {
    next = count_after + 1U;
  }
  next_ = next;
}

}  // namespace XrDap::Vendor

// ===== Frame ring 帧缓冲 =====

namespace XrDap::Vendor
{
namespace
{
constexpr uint32_t kMask = kRingBytes - 1U;
}  // namespace

void FrameRing::Reset()
{
  head_ = 0U;
  tail_ = 0U;
  used_ = 0U;
  payload_ = 0U;
  dropped_ = 0U;
  skipped_ = 0U;
}

// 长度头：低 12 位帧长，bit12..14 位图字节数，bit15 有失败 / Length head: low 12 bits
// the frame length, bits 12..14 the bitmap bytes, bit 15 an item failed.
constexpr uint16_t kLenMask = 0x0FFFU;
static_assert(kMaxFrameBytes <= kLenMask, "frame length must fit 12 bits");
static_assert(CompactEncoder::kMaxBitmap <= 7U, "bitmap bytes must fit 3 bits");

uint16_t FrameRing::Header(uint32_t pos) const
{
  return static_cast<uint16_t>(buf_[pos] | (buf_[(pos + 1U) & kMask] << 8));
}

uint16_t FrameRing::FrameLen(uint32_t pos) const { return Header(pos) & kLenMask; }

void FrameRing::CopyIn(uint32_t at, const uint8_t* src, uint32_t len)
{
  const uint32_t first = (at + len <= kRingBytes) ? len : kRingBytes - at;
  std::memcpy(buf_ + at, src, first);
  std::memcpy(buf_, src + first, len - first);
}

void FrameRing::CopyOut(uint32_t at, uint8_t* dst, uint32_t len) const
{
  const uint32_t first = (at + len <= kRingBytes) ? len : kRingBytes - at;
  std::memcpy(dst, buf_ + at, first);
  std::memcpy(dst + first, buf_, len - first);
}

void FrameRing::Push(const uint8_t* frame, uint16_t len, bool stop_on_full,
                     uint8_t bitmap_len, bool failed)
{
  while (used_ + len + 2U > kRingBytes)
  {
    if (stop_on_full || used_ == 0U)
    {
      ++dropped_;
      return;
    }
    const uint16_t old = FrameLen(tail_);
    tail_ = (tail_ + 2U + old) & kMask;
    used_ -= 2U + old;
    payload_ -= old;
    ++dropped_;
  }
  const uint16_t head =
      static_cast<uint16_t>(len | (bitmap_len << 12) | (failed ? 0x8000U : 0U));
  const uint8_t hdr[2] = {static_cast<uint8_t>(head), static_cast<uint8_t>(head >> 8)};
  CopyIn(head_, hdr, 2U);
  CopyIn((head_ + 2U) & kMask, frame, len);
  head_ = (head_ + 2U + len) & kMask;
  used_ += 2U + len;
  payload_ += len;
}

void FrameRing::Pop(uint32_t& pos, uint16_t flen)
{
  pos = (pos + 2U + flen) & kMask;
  used_ -= 2U + flen;
  payload_ -= flen;
}

void FrameRing::Fill(Taken& t) const
{
  t.remaining = payload_;
  t.dropped = dropped_;
  t.skipped = skipped_;
}

FrameRing::Taken FrameRing::Take(uint8_t* dst, uint32_t limit)
{
  Taken t;
  uint32_t pos = tail_;
  while (used_ != 0U)
  {
    const uint16_t flen = FrameLen(pos);
    if (t.bytes + flen > limit)
    {
      break;
    }
    CopyOut((pos + 2U) & kMask, dst + t.bytes, flen);
    Pop(pos, flen);
    t.bytes += flen;
    ++t.frames;
  }
  tail_ = pos;
  Fill(t);
  return t;
}

FrameRing::Taken FrameRing::TakeCompact(uint8_t* dst, uint32_t limit)
{
  Taken t;
  CompactEncoder encoder;
  uint32_t pos = tail_;
  while (used_ != 0U)
  {
    const uint16_t header = Header(pos);
    const uint16_t flen = header & kLenMask;
    const uint8_t bitmap_len = static_cast<uint8_t>((header >> 12) & 7U);
    const bool failed = (header & 0x8000U) != 0U;
    const uint32_t body = (pos + 2U) & kMask;
    // 一帧至少有节拍号、时间戳、位图和一个值 / A frame holds at least the tick, the
    // timestamp, the bitmap and one value.
    uint8_t raw[CompactEncoder::kMaxRawHead];
    CopyOut(body, raw, 10U + bitmap_len);
    uint8_t head[CompactEncoder::kMaxHead];
    uint16_t skip = 0U;
    const uint16_t head_len = encoder.Head(raw, bitmap_len, failed, head, skip);
    const uint32_t size = head_len + flen - skip;
    if (t.bytes + size > limit)
    {
      break;
    }
    std::memcpy(dst + t.bytes, head, head_len);
    CopyOut((body + skip) & kMask, dst + t.bytes + head_len, flen - skip);
    encoder.Accept(raw);
    Pop(pos, flen);
    t.bytes += size;
    ++t.frames;
  }
  tail_ = pos;
  Fill(t);
  return t;
}

void FrameRing::AddSkipped(uint32_t n) { skipped_ += n; }

}  // namespace XrDap::Vendor
