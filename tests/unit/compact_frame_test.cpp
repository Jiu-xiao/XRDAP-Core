// SPDX-License-Identifier: Apache-2.0
// 紧凑帧编码和帧缓冲的逐帧取出。判据：docs/protocol.md 3.5（紧凑帧：回复的第 1 帧原样，
// 之后 h = (Δk << 1) | 有失败、Δt 两个无符号 LEB128，有失败才带位图，值原样）/ Compact
// frame encoding and taking frames one by one from the ring. Oracle: docs/protocol.md 3.5
// (compact frames: the first frame of a reply as it is, then h = (Δk << 1) | failed and
// Δt as unsigned LEB128, the bitmap only when an item failed, the values unchanged).
#include <vector>

#include "check.hpp"
#include "xrdap/vendor/sampling.hpp"

using namespace XrDap::Vendor;

namespace
{
// 原帧：节拍号、时间戳、一字节位图、一个 u32 值 / Plain frame: tick, timestamp, a
// one-byte bitmap, one u32 value.
std::vector<uint8_t> Frame(uint32_t tick, uint64_t stamp, uint8_t bitmap, uint32_t value)
{
  std::vector<uint8_t> f(15);
  Put32(f.data(), tick);
  Put48(f.data() + 4, stamp);
  f[10] = bitmap;
  Put32(f.data() + 11, value);
  return f;
}

// 编码一帧，返回帧头 + 原帧其余部分 / Encode one frame: the head plus the rest of the
// plain frame.
std::vector<uint8_t> Encode(CompactEncoder& e, const std::vector<uint8_t>& raw)
{
  uint8_t head[CompactEncoder::kMaxHead];
  uint16_t skip = 0;
  const uint16_t n = e.Head(raw.data(), 1, head, skip);
  std::vector<uint8_t> out(head, head + n);
  out.insert(out.end(), raw.begin() + skip, raw.end());
  e.Accept(raw.data());
  return out;
}
}  // namespace

TEST(FirstFrameOfAReplyIsWhole)
{
  CompactEncoder e;
  const auto raw = Frame(7, 1000000, 0, 0x11223344);
  CHECK(Encode(e, raw) == raw);
}

TEST(NextFramesCarryDeltas)
{
  CompactEncoder e;
  (void)Encode(e, Frame(7, 1000000, 0, 1));
  // Δk 1、Δt 1000 µs、无失败：h = 2，Δt = E8 07，值 / Δk 1, Δt 1000 µs, no failure:
  // h = 2, Δt = E8 07, the value.
  CHECK(Encode(e, Frame(8, 1001000, 0, 0xAABBCCDD)) ==
        (std::vector<uint8_t>{0x02, 0xE8, 0x07, 0xDD, 0xCC, 0xBB, 0xAA}));
  // Δt 100：一字节 / Δt 100: one byte.
  CHECK(Encode(e, Frame(9, 1001100, 0, 0)) ==
        (std::vector<uint8_t>{0x02, 0x64, 0, 0, 0, 0}));
  // Δk 4（跳拍）、Δt 20000：三字节 / Δk 4 (skipped ticks), Δt 20000: three bytes.
  CHECK(Encode(e, Frame(13, 1021100, 0, 0)) ==
        (std::vector<uint8_t>{0x08, 0xA0, 0x9C, 0x01, 0, 0, 0, 0}));
}

TEST(FailedFrameKeepsItsBitmap)
{
  CompactEncoder e;
  (void)Encode(e, Frame(0, 50, 0, 0));
  // 有失败：h = (1 << 1) | 1 = 3，Δt 10，位图 01，值 / Failed: h = 3, Δt 10, bitmap 01,
  // the value.
  CHECK(Encode(e, Frame(1, 60, 0x01, 0)) ==
        (std::vector<uint8_t>{0x03, 0x0A, 0x01, 0, 0, 0, 0}));
}

TEST(TimestampDeltaWrapsAt48Bits)
{
  CompactEncoder e;
  (void)Encode(e, Frame(0, (uint64_t{1} << 48) - 5U, 0, 0));
  CHECK(Encode(e, Frame(1, 3, 0, 0)) == (std::vector<uint8_t>{0x02, 0x08, 0, 0, 0, 0}));
}

TEST(HugeDeltasStillEncode)
{
  CompactEncoder e;
  (void)Encode(e, Frame(0, 0, 0, 0));
  const auto out = Encode(e, Frame(0xFFFFFFFFU, (uint64_t{1} << 48) - 1U, 0, 0));
  // h 33 位 5 字节，Δt 48 位 7 字节，值 4 字节 / h 33 bits in 5 bytes, Δt 48 bits in 7,
  // the value 4.
  CHECK_EQ(out.size(), 16U);
}

TEST(RingTakesFramesOneByOne)
{
  FrameRing r;
  r.Reset();
  const auto a = Frame(1, 10, 0, 0xA);
  const auto b = Frame(2, 20, 0, 0xB);
  r.Push(a.data(), static_cast<uint16_t>(a.size()), false);
  r.Push(b.data(), static_cast<uint16_t>(b.size()), false);
  CHECK_EQ(r.FrontLen(), 15U);
  uint8_t got[15];
  r.CopyFront(11, got, 4);
  CHECK_EQ(Get32(got), 0xAU);
  r.DropFront();
  CHECK_EQ(r.Status().remaining, 15U);
  r.CopyFront(0, got, 15);
  CHECK_EQ(Get32(got), 2U);
  r.DropFront();
  CHECK_EQ(r.FrontLen(), 0U);
  CHECK_EQ(r.Status().remaining, 0U);
}
