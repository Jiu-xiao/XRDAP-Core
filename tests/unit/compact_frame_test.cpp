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
  const uint16_t n = e.Head(raw.data(), 1, raw[10] != 0U, head, skip);
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

// 缓冲按长度头里记下的位图字节数和失败位编码；limit 按编码后的字节算，放不下的帧留到
// 下一次，下一次的第 1 帧又是原帧 / The ring encodes with the bitmap bytes and failure
// bit kept in the length head; limit counts encoded bytes, a frame that does not fit
// waits for the next take, whose first frame is a plain frame again.
TEST(RingTakesCompactFrames)
{
  FrameRing r;
  r.Reset();
  const auto a = Frame(1, 1000, 0, 0xA);
  const auto b = Frame(2, 2000, 0, 0xB);
  const auto c = Frame(3, 3000, 0x01, 0);
  r.Push(a.data(), 15, false, 1, false);
  r.Push(b.data(), 15, false, 1, false);
  r.Push(c.data(), 15, false, 1, true);
  uint8_t out[64] = {};
  // 原帧 15 + 紧凑帧 7（02 E8 07 值）= 22；第 3 帧（03 E8 07 01 值 = 8）放不下 / Plain 15
  // + compact 7 = 22; the third (8 bytes) does not fit.
  auto t = r.TakeCompact(out, 29);
  CHECK_EQ(t.frames, 2U);
  CHECK_EQ(t.bytes, 22U);
  CHECK_EQ(t.remaining, 15U);
  CHECK(std::vector<uint8_t>(out, out + 15) == a);
  CHECK(std::vector<uint8_t>(out + 15, out + 22) ==
        (std::vector<uint8_t>{0x02, 0xE8, 0x07, 0x0B, 0, 0, 0}));
  t = r.TakeCompact(out, sizeof(out));
  CHECK_EQ(t.frames, 1U);
  CHECK(std::vector<uint8_t>(out, out + 15) == c);
  CHECK_EQ(t.remaining, 0U);
}

// 长度头的高位不影响原帧的取出 / The high bits of the length head do not affect plain
// takes.
TEST(PlainTakeIgnoresTheMetadata)
{
  FrameRing r;
  r.Reset();
  const auto c = Frame(3, 3000, 0x01, 0x55);
  r.Push(c.data(), 15, false, 1, true);
  uint8_t out[32] = {};
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 1U);
  CHECK_EQ(t.bytes, 15U);
  CHECK(std::vector<uint8_t>(out, out + 15) == c);
}
