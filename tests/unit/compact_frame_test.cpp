// SPDX-License-Identifier: Apache-2.0
// 紧凑帧编码和帧缓冲的逐帧取出。判据：docs/protocol.md 3.5（紧凑帧：回复的第 1 帧原样，
// 之后 h = (Δk << 1) | 有失败、Δt 两个无符号 LEB128，有失败才带位图，值原样）和 3.8 的
// SUB_READ 字节示例 / Compact frame encoding and taking frames one by one from the ring.
// Oracle: docs/protocol.md 3.5 (compact frames: the first frame of a reply as it is, then
// h = (Δk << 1) | failed and Δt as unsigned LEB128, the bitmap only when an item failed,
// the values unchanged) and the SUB_READ byte example of 3.8.
#include <initializer_list>
#include <vector>

#include "check.hpp"
#include "xrdap/vendor/sampling.hpp"

using namespace XrDap::Vendor;

namespace
{
using Bytes = std::vector<uint8_t>;

// 文档 3.5 的原帧：节拍号 4、时间戳 6（小端）、位图、值 / The plain frame of 3.5: tick 4,
// timestamp 6 (little-endian), bitmap, values.
Bytes Frame(uint32_t tick, uint64_t stamp, const Bytes& bitmap, const Bytes& values)
{
  Bytes f;
  for (int i = 0; i < 4; ++i)
  {
    f.push_back(static_cast<uint8_t>(tick >> (8 * i)));
  }
  for (int i = 0; i < 6; ++i)
  {
    f.push_back(static_cast<uint8_t>(stamp >> (8 * i)));
  }
  f.insert(f.end(), bitmap.begin(), bitmap.end());
  f.insert(f.end(), values.begin(), values.end());
  return f;
}

Bytes Join(std::initializer_list<Bytes> parts)
{
  Bytes all;
  for (const Bytes& part : parts)
  {
    all.insert(all.end(), part.begin(), part.end());
  }
  return all;
}

// 编码一帧，返回帧头 + 原帧其余部分 / Encode one frame: the head plus the rest of the
// plain frame.
Bytes Encode(CompactEncoder& e, const Bytes& raw)
{
  uint8_t head[CompactEncoder::kMaxHead];
  uint16_t skip = 0;
  const uint16_t n = e.Head(raw.data(), 1U, false, head, skip);
  Bytes out(head, head + n);
  out.insert(out.end(), raw.begin() + skip, raw.end());
  e.Accept(raw.data());
  return out;
}

// 把一帧放进缓冲，放不下时覆盖最旧的 / Put a frame into the ring, overwriting the oldest
// when full.
void Push(FrameRing& r, const Bytes& raw, bool failed = false, uint8_t bitmap_len = 1U)
{
  r.Push(raw.data(), static_cast<uint16_t>(raw.size()), false, bitmap_len, failed);
}

const Bytes kZero = {0, 0, 0, 0};
}  // namespace

// 文档 3.8：k = 6 只有条目 1 到期，k = 7 两个条目都到期 / The example of 3.8: only item 1
// is due at k = 6, both items at k = 7.
TEST(DocumentExampleOfCompactSubRead)
{
  const Bytes first = {0x06, 0x00, 0x00, 0x00, 0xB0, 0x59, 0x0F, 0x00,
                       0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00};
  const Bytes second = {0x07, 0x00, 0x00, 0x00, 0x98, 0x5D, 0x0F, 0x00, 0x00,
                        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x34, 0x12};
  FrameRing r;
  r.Reset();
  Push(r, first);
  Push(r, second);
  uint8_t out[64] = {};
  const auto t = r.TakeCompact(out, sizeof(out));
  CHECK_EQ(t.frames, 2U);
  CHECK(Bytes(out, out + t.bytes) ==
        Join({first, {0x02, 0xE8, 0x07, 0x08, 0x00, 0x00, 0x00, 0x34, 0x12}}));
}

TEST(NextFramesCarryDeltas)
{
  CompactEncoder e;
  (void)Encode(e, Frame(7, 1000000, {0x00}, kZero));
  // Δk 1、Δt 1000 µs、无失败：h = 2，Δt = E8 07，值 / Δk 1, Δt 1000 µs, no failure:
  // h = 2, Δt = E8 07, the value.
  CHECK(Encode(e, Frame(8, 1001000, {0x00}, {0xDD, 0xCC, 0xBB, 0xAA})) ==
        (Bytes{0x02, 0xE8, 0x07, 0xDD, 0xCC, 0xBB, 0xAA}));
  // Δt 100：一字节 / Δt 100: one byte.
  CHECK(Encode(e, Frame(9, 1001100, {0x00}, kZero)) == (Bytes{0x02, 0x64, 0, 0, 0, 0}));
  // Δk 4（跳拍）、Δt 20000：三字节 / Δk 4 (skipped ticks), Δt 20000: three bytes.
  CHECK(Encode(e, Frame(13, 1021100, {0x00}, kZero)) ==
        (Bytes{0x08, 0xA0, 0x9C, 0x01, 0, 0, 0, 0}));
}

TEST(VariableLengthFieldsGrowAt128)
{
  CompactEncoder e;
  (void)Encode(e, Frame(0, 1000, {0x00}, kZero));
  // Δt 127 一字节，128 两字节 / Δt 127 takes one byte, 128 takes two.
  CHECK(Encode(e, Frame(1, 1127, {0x00}, kZero)) == (Bytes{0x02, 0x7F, 0, 0, 0, 0}));
  CHECK(Encode(e, Frame(2, 1255, {0x00}, kZero)) ==
        (Bytes{0x02, 0x80, 0x01, 0, 0, 0, 0}));
  // Δk 64 时 h = 128，两字节 / With Δk 64, h = 128 takes two bytes.
  CHECK(Encode(e, Frame(66, 1256, {0x00}, kZero)) ==
        (Bytes{0x80, 0x01, 0x01, 0, 0, 0, 0}));
}

TEST(TimestampDeltaWrapsAt48Bits)
{
  CompactEncoder e;
  (void)Encode(e, Frame(0, (uint64_t{1} << 48) - 5U, {0x00}, kZero));
  CHECK(Encode(e, Frame(1, 3, {0x00}, kZero)) == (Bytes{0x02, 0x08, 0, 0, 0, 0}));
}

TEST(HugeDeltasStillEncode)
{
  CompactEncoder e;
  (void)Encode(e, Frame(0, 0, {0x00}, kZero));
  // h = 0x1FFFFFFFE（33 位）占 5 字节，Δt = 2^48 - 1 占 7 字节，值 4 字节 / h =
  // 0x1FFFFFFFE (33 bits) in 5 bytes, Δt = 2^48 - 1 in 7, the value 4.
  CHECK(Encode(e, Frame(0xFFFFFFFFU, (uint64_t{1} << 48) - 1U, {0x00}, kZero)) ==
        (Bytes{0xFE, 0xFF, 0xFF, 0xFF, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F, 0,
               0, 0, 0}));
}

// limit 按编码后的字节算，放不下的帧留到下一次，下一次的第 1 帧又是原帧 / limit counts
// encoded bytes; a frame that does not fit waits for the next take, whose first frame is
// a plain frame again.
TEST(RingTakesCompactFrames)
{
  FrameRing r;
  r.Reset();
  const Bytes a = Frame(1, 1000, {0x00}, {0x0A, 0, 0, 0});
  const Bytes b = Frame(2, 2000, {0x00}, {0x0B, 0, 0, 0});
  const Bytes c = Frame(3, 3000, {0x01}, kZero);
  Push(r, a);
  Push(r, b);
  Push(r, c, true);
  uint8_t out[64] = {};
  // 原帧 15 + 紧凑帧 7（02 E8 07 值）= 22；第 3 帧（03 E8 07 01 值 = 8）放不下 / Plain 15
  // + compact 7 = 22; the third (8 bytes) does not fit.
  auto t = r.TakeCompact(out, 29);
  CHECK_EQ(t.frames, 2U);
  CHECK(Bytes(out, out + t.bytes) == Join({a, {0x02, 0xE8, 0x07, 0x0B, 0, 0, 0}}));
  // 剩余字节数按完整帧计 / Bytes left count whole plain frames.
  CHECK_EQ(t.remaining, 15U);
  t = r.TakeCompact(out, sizeof(out));
  CHECK_EQ(t.frames, 1U);
  CHECK(Bytes(out, out + t.bytes) == c);
  CHECK_EQ(t.remaining, 0U);
}

// 帧的总长不超过 limit：正好等于时放得下，失败的帧带着位图 / The frames total at most
// limit: exactly limit fits, and the failed frame carries its bitmap.
TEST(CompactTakeMayFillTheLimitExactly)
{
  FrameRing r;
  r.Reset();
  const Bytes a = Frame(1, 1000, {0x00}, {0x0A, 0, 0, 0});
  Push(r, a);
  Push(r, Frame(2, 2000, {0x00}, {0x0B, 0, 0, 0}));
  Push(r, Frame(3, 3000, {0x01}, kZero), true);
  uint8_t out[64] = {};
  const auto t = r.TakeCompact(out, 30);
  CHECK_EQ(t.frames, 3U);
  CHECK(
      Bytes(out, out + t.bytes) ==
      Join({a, {0x02, 0xE8, 0x07, 0x0B, 0, 0, 0}, {0x03, 0xE8, 0x07, 0x01, 0, 0, 0, 0}}));
}

// 32 个条目的位图是 4 字节，缓冲要记住它的长度 / The bitmap of 32 items takes 4 bytes and
// the ring has to remember that length.
TEST(RingRemembersAFourByteBitmap)
{
  const Bytes values(32, 0x5A);
  const Bytes clean = {0x00, 0x00, 0x00, 0x00};
  const Bytes failed = {0x00, 0x00, 0x00, 0x80};
  const Bytes first = Frame(0, 100, clean, values);
  FrameRing r;
  r.Reset();
  Push(r, first, false, 4U);
  Push(r, Frame(1, 110, clean, values), false, 4U);
  Push(r, Frame(2, 120, failed, values), true, 4U);
  uint8_t out[128] = {};
  const auto t = r.TakeCompact(out, sizeof(out));
  CHECK_EQ(t.frames, 3U);
  CHECK(Bytes(out, out + t.bytes) ==
        Join({first, {0x02, 0x0A}, values, {0x03, 0x0A}, failed, values}));
}

// 长度头、帧头和值都可能跨过缓冲末尾 / The length head, the frame head and the values may
// all cross the end of the ring.
TEST(CompactFramesSurviveEveryRingPosition)
{
  FrameRing r;
  r.Reset();
  // 三帧共占 51 字节，与缓冲的 256 互质，256 轮走遍每个起点 / Three frames take 51 bytes,
  // coprime with the 256 of the ring, so 256 rounds start at every position.
  for (uint32_t round = 0; round < 256U; ++round)
  {
    const Bytes values = {static_cast<uint8_t>(round), 0xA1, 0xB2, 0xC3};
    const Bytes a = Frame(round, 1000, {0x00}, values);
    Push(r, a);
    Push(r, Frame(round + 1U, 2000, {0x00}, values));
    Push(r, Frame(round + 2U, 3000, {0x01}, values), true);
    uint8_t out[64] = {};
    const auto t = r.TakeCompact(out, sizeof(out));
    CHECK(Bytes(out, out + t.bytes) ==
          Join({a, {0x02, 0xE8, 0x07}, values, {0x03, 0xE8, 0x07, 0x01}, values}));
  }
}

// 缓冲里为紧凑帧记下的东西不影响原帧的取出 / What the ring keeps for compact frames does
// not affect plain takes.
TEST(PlainTakeIgnoresTheMetadata)
{
  FrameRing r;
  r.Reset();
  const Bytes c = Frame(3, 3000, {0x01}, {0x55, 0, 0, 0});
  Push(r, c, true);
  uint8_t out[32] = {};
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 1U);
  CHECK(Bytes(out, out + t.bytes) == c);
}
