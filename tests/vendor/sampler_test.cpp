// SPDX-License-Identifier: Apache-2.0
// 采样器对着 ADIv5 的 posted 读模型跑（docs/protocol.md 3.5）：相邻的字合成一次突发；
// 失败的帧留下证据，后面的帧才恢复，一秒最多恢复一次；错过的节拍算跳过，不补。
//
// The sampler against an ADIv5 posted-read model (docs/protocol.md 3.5): adjacent words
// become one burst; a failed frame keeps the evidence and a later frame recovers, at most
// once per second; missed ticks count as skipped and are not made up.
#include "xrdap/vendor/sampler.hpp"

#include <cstdint>
#include <vector>

#include "fixture.hpp"
#include "test_clock.hpp"
#include "xrdap/vendor/mem_ap.hpp"

namespace
{
using XrDap::Vendor::FrameRing;
using XrDap::Vendor::Get16;
using XrDap::Vendor::Get32;
using XrDap::Vendor::Item;
using XrDap::Vendor::MemAp;
using XrDap::Vendor::Sampler;
using XrDapTest::ModelSwd;
using XrDapTest::NullTimer;
using XrDapTest::WordAt;

constexpr uint32_t kAddr = 0x08000000U;
constexpr uint32_t kPeriodUs = 10000U;

// 帧的格式（docs/protocol.md 3.5）：节拍号在 0，状态位图在 10，8 个条目以内值从 11 起 /
// The frame format (docs/protocol.md 3.5): tick at 0, status bitmap at 10, and with up
// to 8 items the values from 11.
constexpr unsigned kBitmap = 10U;
constexpr unsigned kValues = 11U;

struct Rig
{
  ModelSwd swd;
  MemAp mem{swd};
  Sampler sampler{mem, +[](void*, uint32_t) { return true; }, nullptr};
  NullTimer timer;
  uint8_t frame[64] = {};

  void Start(const std::vector<Item>& items)
  {
    // 跳过 SUB_START 的头，采样器只收条目 / Past the SUB_START head: the sampler takes
    // only the items.
    const auto request = XrDapTest::SubStart(kPeriodUs, 0U, items);
    const XrDap::Vendor::SubItemList list = {request.data() + 8,
                                             request.data() + request.size(),
                                             static_cast<uint16_t>(items.size())};
    CHECK_EQ(sampler.Start(list, kPeriodUs, false, 1U), 0x00U);
  }

  // 节拍 0 不用等 / Tick 0 needs no wait.
  FrameRing::Taken SampleNow()
  {
    sampler.Sample(timer);
    return sampler.TakeFrames(frame, sizeof(frame), false);
  }

  FrameRing::Taken Tick(uint32_t elapsed = 1U)
  {
    sampler.OnTickIsr(elapsed);
    return SampleNow();
  }
};

void CheckTwoWords(const Rig& r, const Item& a, const Item& b)
{
  CHECK_EQ(r.frame[kBitmap], 0U);
  CHECK_EQ(Get32(r.frame + kValues), ModelSwd::Value(a.ap, a.base));
  CHECK_EQ(Get32(r.frame + kValues + 4U), ModelSwd::Value(b.ap, b.base));
}

// 目标不在了：第一帧失败，第二帧试了一次恢复也失败 / The target is gone: the first frame
// fails, and the second tries one recovery, which fails too.
void LoseTheTarget(Rig& r)
{
  r.Start({WordAt(kAddr)});
  (void)r.SampleNow();
  r.swd.missing = true;
  (void)r.Tick();
  (void)r.Tick();
  CHECK_EQ(r.swd.resets, 1U);
}
}  // namespace

TEST(AFailedFrameKeepsTheEvidence)
{
  Rig r;
  r.Start({WordAt(kAddr)});
  (void)r.SampleNow();
  r.swd.desynced = true;
  const auto got = r.Tick();
  CHECK_EQ(got.frames, 1U);
  CHECK_EQ(got.bytes, 15U);
  CHECK_EQ(Get32(r.frame), 1U);
  CHECK_EQ(r.frame[kBitmap], 1U);
  CHECK_EQ(Get32(r.frame + kValues), 0U);
  CHECK_EQ(r.swd.resets, 0U);
}

// 恢复只重新进 SWD，失败的那一帧不补读 / Recovery only re-enters SWD; the failed frame is
// not read again.
TEST(TheNextFrameRecoversOnce)
{
  Rig r;
  r.Start({WordAt(kAddr)});
  (void)r.SampleNow();
  r.swd.desynced = true;
  (void)r.Tick();
  const auto got = r.Tick();
  CHECK_EQ(got.frames, 1U);
  CHECK_EQ(Get32(r.frame), 2U);
  CHECK_EQ(r.frame[kBitmap], 0U);
  CHECK_EQ(Get32(r.frame + kValues), ModelSwd::Value(0U, kAddr));
  CHECK_EQ(r.swd.resets, 1U);
  CHECK_EQ(r.swd.enters, 1U);
  CHECK_EQ(r.swd.memory_reads, 2U);
}

// 反复读可能有外设副作用，所以目标不在时一秒最多恢复一次（sampler.cpp）/ Repeated reads
// can have peripheral side effects, so while the target is gone recovery runs at most
// once per second (sampler.cpp).
TEST(RecoveryRunsAtMostOncePerSecond)
{
  Rig r;
  LoseTheTarget(r);
  for (int i = 0; i < 5; ++i)
  {
    (void)r.Tick();
    CHECK_EQ(r.frame[kBitmap], 1U);
  }
  XrDapTest::AdvanceClockUs(999999U);
  (void)r.Tick();
  CHECK_EQ(r.swd.resets, 1U);
}

TEST(RecoveryRunsAgainAfterASecond)
{
  Rig r;
  LoseTheTarget(r);
  XrDapTest::AdvanceClockUs(1000000U);
  (void)r.Tick();
  CHECK_EQ(r.swd.resets, 2U);
}

TEST(AdjacentWordsShareOneTarWrite)
{
  Rig r;
  const Item a = WordAt(kAddr);
  const Item b = WordAt(kAddr + 4U);
  r.Start({a, b});
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 1U);
  CheckTwoWords(r, a, b);
}

TEST(WordsAcrossThe1KiBWindowTakeTwoTarWrites)
{
  Rig r;
  const Item a = WordAt(kAddr + 0x3FCU);
  const Item b = WordAt(kAddr + 0x400U);
  r.Start({a, b});
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 2U);
  CheckTwoWords(r, a, b);
}

TEST(WordsWithAGapTakeTwoTarWrites)
{
  Rig r;
  const Item a = WordAt(kAddr);
  const Item b = WordAt(kAddr + 8U);
  r.Start({a, b});
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 2U);
  CheckTwoWords(r, a, b);
}

TEST(WordsOnTwoApsTakeTwoTarWrites)
{
  Rig r;
  const Item a = WordAt(kAddr, 0U);
  const Item b = WordAt(kAddr + 4U, 1U);
  r.Start({a, b});
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 2U);
  CheckTwoWords(r, a, b);
}

TEST(AHalfwordIsNotPartOfABurst)
{
  Rig r;
  const Item a = WordAt(kAddr);
  Item b = WordAt(kAddr + 4U);
  b.size = 2U;
  r.Start({a, b});
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 2U);
  CHECK_EQ(r.frame[kBitmap], 0U);
  CHECK_EQ(Get32(r.frame + kValues), ModelSwd::Value(0U, a.base));
  CHECK_EQ(Get16(r.frame + kValues + 4U), ModelSwd::Value(0U, b.base) & 0xFFFFU);
}

// 指针条目多读一次指针，所以是三次 TAR 写 / A pointer item also reads the pointer, so
// there are three TAR writes.
TEST(APointerItemIsNotPartOfABurst)
{
  Rig r;
  Item b = WordAt(kAddr + 4U);
  b.n = 1U;
  r.Start({WordAt(kAddr), b});
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 3U);
  CHECK_EQ(r.frame[kBitmap], 0U);
}

// 第二次读就失败：第一个字还没确认，两个字都算失败，失败的地址不重读 / The second read
// fails: the first word is not confirmed yet, so both count as failed, and the failed
// address is not read again.
TEST(AFaultInABurstFailsEveryUnconfirmedWord)
{
  Rig r;
  r.Start({WordAt(kAddr), WordAt(kAddr + 4U)});
  r.swd.fail_drw = 2U;
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 1U);
  CHECK_EQ(r.frame[kBitmap], 3U);
  CHECK_EQ(r.swd.memory_reads, 1U);
}

TEST(BadRdbuffParityFailsOnlyTheLastWord)
{
  Rig r;
  r.Start({WordAt(kAddr), WordAt(kAddr + 4U)});
  r.swd.parity_rdbuff = true;
  (void)r.SampleNow();
  CHECK_EQ(r.swd.tar_writes, 1U);
  CHECK_EQ(r.frame[kBitmap], 2U);
  CHECK_EQ(Get32(r.frame + kValues), ModelSwd::Value(0U, kAddr));
}

TEST(MissedTicksCountAsSkippedAndAreNotMadeUp)
{
  Rig r;
  r.Start({WordAt(kAddr)});
  (void)r.SampleNow();
  const auto got = r.Tick(4U);
  CHECK_EQ(got.frames, 1U);
  CHECK_EQ(Get32(r.frame), 4U);
  CHECK_EQ(got.skipped, 3U);
  CHECK_EQ(got.dropped, 0U);
}

TEST(NoTickNoFrame)
{
  Rig r;
  r.Start({WordAt(kAddr)});
  (void)r.SampleNow();
  CHECK_EQ(r.Tick(0U).frames, 0U);
}
