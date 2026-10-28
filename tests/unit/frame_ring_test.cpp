// SPDX-License-Identifier: Apache-2.0
// 采样帧缓冲。判据：docs/protocol.md 3.4（SUB_READ 只回整帧，剩余字节不含长度头）和
// 3.5（缓冲满时按 flags.bit0 丢新帧或覆盖最旧的帧，丢掉的帧计入丢帧数）。测试用缓冲
// 256 字节，每帧另占 2 字节长度头 / Sample frame buffer. Oracles: docs/protocol.md 3.4
// (SUB_READ returns whole frames only; bytes left exclude the length heads) and 3.5 (when
// full, flags.bit0 drops the new frame or overwrites the oldest; lost frames count as
// dropped). The test ring is 256 bytes; every frame takes 2 more bytes of length head.
#include "check.hpp"
#include "xrdap/vendor/sampling.hpp"

using XrDap::Vendor::FrameRing;

namespace
{
void Fill(uint8_t* f, uint16_t len, uint8_t tag)
{
  for (uint16_t i = 0; i < len; ++i)
  {
    f[i] = static_cast<uint8_t>(tag + i);
  }
}
}  // namespace

TEST(TakeReturnsWholeFramesInOrder)
{
  FrameRing r;
  r.Reset();
  uint8_t a[10];
  uint8_t b[20];
  Fill(a, 10, 0x10);
  Fill(b, 20, 0x40);
  r.Push(a, 10, true);
  r.Push(b, 20, true);
  uint8_t out[64] = {};
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 2U);
  CHECK_EQ(t.bytes, 30U);
  CHECK_EQ(t.remaining, 0U);
  CHECK_EQ(out[0], 0x10U);
  CHECK_EQ(out[9], 0x19U);
  CHECK_EQ(out[10], 0x40U);
  CHECK_EQ(out[29], 0x53U);
}

TEST(TakeNeverSplitsAFrame)
{
  FrameRing r;
  r.Reset();
  uint8_t a[10] = {};
  r.Push(a, 10, true);
  r.Push(a, 10, true);
  uint8_t out[64] = {};
  const auto t = r.Take(out, 15);  // 只放得下一帧 / room for one frame only
  CHECK_EQ(t.frames, 1U);
  CHECK_EQ(t.bytes, 10U);
  CHECK_EQ(t.remaining, 10U);  // 不含长度头 / length heads excluded
  const auto none = r.Take(out, 5);
  CHECK_EQ(none.frames, 0U);
  CHECK_EQ(none.remaining, 10U);
}

TEST(FullRingDropsNewFrameWhenAsked)
{
  FrameRing r;
  r.Reset();
  uint8_t f[50];
  for (int i = 0; i < 5; ++i)  // 5 x 52 = 260 > 256
  {
    Fill(f, 50, static_cast<uint8_t>(i * 50));
    r.Push(f, 50, true);
  }
  uint8_t out[256] = {};
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 4U);
  CHECK_EQ(t.dropped, 1U);
  CHECK_EQ(out[0], 0U);  // 最旧的还在 / the oldest stayed
}

TEST(FullRingOverwritesOldestOtherwise)
{
  FrameRing r;
  r.Reset();
  uint8_t f[50];
  for (int i = 0; i < 5; ++i)
  {
    Fill(f, 50, static_cast<uint8_t>(i * 50));
    r.Push(f, 50, false);
  }
  uint8_t out[256] = {};
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 4U);
  CHECK_EQ(t.dropped, 1U);
  CHECK_EQ(out[0], 50U);  // 第一帧被覆盖 / the first frame was overwritten
}

TEST(FramesWrapAroundTheEnd)
{
  FrameRing r;
  r.Reset();
  uint8_t f[100];
  uint8_t out[256] = {};
  // 推进到快到末尾，再放一帧跨过末尾 / Move close to the end, then push one frame
  // across it.
  for (int i = 0; i < 2; ++i)
  {
    Fill(f, 100, 0);
    r.Push(f, 100, true);
    (void)r.Take(out, sizeof(out));
  }
  Fill(f, 100, 7);
  r.Push(f, 100, true);
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 1U);
  CHECK_EQ(t.bytes, 100U);
  CHECK_EQ(out[0], 7U);
  CHECK_EQ(out[99], 106U);
}

TEST(CountersAccumulateUntilReset)
{
  FrameRing r;
  r.Reset();
  r.AddSkipped(3);
  r.AddSkipped(2);
  uint8_t out[8];
  CHECK_EQ(r.Take(out, sizeof(out)).skipped, 5U);
  r.Reset();
  CHECK_EQ(r.Take(out, sizeof(out)).skipped, 0U);
}
