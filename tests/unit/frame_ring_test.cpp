// SPDX-License-Identifier: Apache-2.0
// 采样帧缓冲。判据：docs/protocol.md 3.4（SUB_READ 只回整帧，帧的总长不超过上限，剩余字节
// 不含长度头；SUB_START 和 op 2 清空缓冲和计数器；一帧加 2 字节长度头不大于缓冲才接受）和
// 3.5（缓冲满时按 flags.bit0 丢新帧或覆盖最旧的帧，丢掉的帧计入丢帧数）。测试用缓冲 256
// 字节，每帧另占 2 字节长度头 / Sample frame buffer. Oracles: docs/protocol.md 3.4
// (SUB_READ returns whole frames only, at most the limit in total; bytes left exclude the
// length heads; SUB_START and op 2 clear the buffer and the counters; a frame is accepted
// when it plus its 2-byte length head is not larger than the buffer) and 3.5 (when full,
// flags.bit0 drops the new frame or overwrites the oldest; lost frames count as dropped).
// The test ring is 256 bytes; every frame takes 2 more bytes of length head.
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

TEST(TakeNeverSplitsAFrame)
{
  FrameRing r;
  r.Reset();
  uint8_t a[10] = {};
  r.Push(a, 10, true);
  r.Push(a, 10, true);
  uint8_t out[64] = {};
  const auto t = r.Take(out, 19);  // 差 1 字节放不下第二帧 / 1 byte short of the second
  CHECK_EQ(t.frames, 1U);
  CHECK_EQ(t.bytes, 10U);
  CHECK_EQ(t.remaining, 10U);  // 不含长度头 / length heads excluded
  const auto none = r.Take(out, 9);
  CHECK_EQ(none.frames, 0U);
  CHECK_EQ(none.remaining, 10U);
  // 帧的总长不超过上限：正好等于时放得下 / The frames total at most the limit: exactly
  // the limit fits.
  CHECK_EQ(r.Take(out, 10).frames, 1U);
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
  CHECK_EQ(t.remaining, 0U);
  CHECK_EQ(out[0], 0U);      // 最旧的还在 / the oldest stayed
  CHECK_EQ(out[150], 150U);  // 第 4 帧没被新帧改写 / the new frame left frame 4 alone
}

TEST(FullRingOverwritesOldestOtherwise)
{
  FrameRing r;
  r.Reset();
  uint8_t f[50];
  for (int i = 0; i < 12; ++i)  // 12 x 52 字节绕过缓冲末尾 / 12 x 52 bytes pass the end
  {
    Fill(f, 50, static_cast<uint8_t>(i * 10));
    r.Push(f, 50, false);
  }
  uint8_t out[256] = {};
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 4U);
  CHECK_EQ(t.dropped, 8U);
  CHECK_EQ(t.remaining, 0U);
  CHECK_EQ(out[0], 80U);     // 前 8 帧被覆盖 / the first 8 frames were overwritten
  CHECK_EQ(out[150], 110U);  // 最新的一帧在最后 / the newest frame comes last
}

TEST(LargeFrameOverwritesSeveralOldFrames)
{
  FrameRing r;
  r.Reset();
  uint8_t f[150];
  for (int i = 0; i < 4; ++i)
  {
    Fill(f, 50, static_cast<uint8_t>(i * 50));
    r.Push(f, 50, false);
  }
  // 要 152 字节，缓冲里已用 208：覆盖两帧才放得下 / It needs 152 bytes with 208 in use:
  // two frames have to go.
  Fill(f, 150, 0x07);
  r.Push(f, 150, false);
  uint8_t out[256] = {};
  const auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 3U);
  CHECK_EQ(t.dropped, 2U);
  CHECK_EQ(t.bytes, 250U);
  CHECK_EQ(t.remaining, 0U);
  CHECK_EQ(out[0], 100U);
  CHECK_EQ(out[100], 0x07U);
}

TEST(FrameFillingTheRingExactlyIsKept)
{
  uint8_t f[255];
  Fill(f, 255, 0);
  uint8_t out[255] = {};
  FrameRing r;
  r.Reset();
  r.Push(f, 254, true);  // 254 + 2 = 256
  auto t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 1U);
  CHECK_EQ(t.bytes, 254U);
  CHECK_EQ(t.dropped, 0U);
  r.Push(f, 255, true);  // 255 + 2 > 256
  t = r.Take(out, sizeof(out));
  CHECK_EQ(t.frames, 0U);
  CHECK_EQ(t.dropped, 1U);
}

// 长度头和帧都可能跨过缓冲末尾 / The length head and the frame may both cross the end of
// the ring.
TEST(FramesSurviveEveryRingPosition)
{
  FrameRing r;
  r.Reset();
  // 一帧占 11 字节，与缓冲的 256 互质，256 轮走遍每个起点 / A frame takes 11 bytes,
  // coprime with the 256 of the ring, so 256 rounds start at every position.
  for (uint32_t round = 0; round < 256U; ++round)
  {
    uint8_t f[9];
    Fill(f, 9, static_cast<uint8_t>(round));
    r.Push(f, 9, true);
    uint8_t out[9] = {};
    const auto t = r.Take(out, sizeof(out));
    CHECK_EQ(t.frames, 1U);
    for (uint16_t i = 0; i < 9U; ++i)
    {
      CHECK_EQ(out[i], f[i]);
    }
  }
}

TEST(ResetClearsFramesAndCounters)
{
  FrameRing r;
  r.Reset();
  uint8_t f[255] = {};
  r.Push(f, 10, true);
  r.Push(f, 255, true);  // 放不下，丢帧数 1 / does not fit, one dropped
  r.AddSkipped(3);
  r.AddSkipped(2);
  uint8_t out[8];
  const auto before = r.Take(out, sizeof(out));
  CHECK_EQ(before.frames, 0U);
  CHECK_EQ(before.remaining, 10U);
  CHECK_EQ(before.dropped, 1U);
  CHECK_EQ(before.skipped, 5U);
  r.Reset();
  const auto after = r.Take(out, sizeof(out));
  CHECK_EQ(after.remaining, 0U);
  CHECK_EQ(after.dropped, 0U);
  CHECK_EQ(after.skipped, 0U);
}
