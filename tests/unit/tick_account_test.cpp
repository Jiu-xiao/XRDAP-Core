// SPDX-License-Identifier: Apache-2.0
// 节拍账目。判据：用户决定 C4（定时器是唯一时钟，中断只把节拍计数加 1，按计数差算跳拍）、
// B9（能否跟上只看跳拍、丢帧、缓冲占用）和 docs/protocol.md 3.5（来不及时不补做；一拍占用
// 太久时让出一拍）/ Tick accounting. Oracles: user decisions C4 (the timer is the only
// clock, the interrupt only adds 1 to the tick count, skips come from count differences)
// and B9 (keep-up is judged by skipped ticks, dropped frames and ring fill), and
// docs/protocol.md 3.5 (late ticks are not made up; a tick that takes too long gives one
// up).
#include "xrdap/vendor/sampling.hpp"

#include "check.hpp"

using XrDap::Vendor::TickAccount;
using XrDap::Vendor::TickWakes;

TEST(WakeWhenMergedTicksPassDeadline)
{
  CHECK(!TickWakes(6U, 7U));
  CHECK(TickWakes(7U, 7U));
  CHECK(TickWakes(10U, 7U));
  CHECK(!TickWakes(0xFFFFFFFEU, 1U));
  CHECK(TickWakes(1U, 0xFFFFFFFEU));
  CHECK(TickWakes(0U, 0U));
  CHECK(!TickWakes(3U, 3U + 0x40000000U));  // Parked command.
}

namespace
{
constexpr uint32_t kPeriod = 1000U;
constexpr uint32_t kShort = 100U;  // 一拍很快做完 / a tick done quickly
}  // namespace

TEST(FirstTickIsZero)
{
  TickAccount a;
  a.Begin();
  const auto s = a.OnWake(0);
  CHECK(s.sample);
  CHECK_EQ(s.tick, 0U);
  CHECK_EQ(s.skipped, 0U);
}

TEST(EveryTickOnTimeSkipsNothing)
{
  TickAccount a;
  a.Begin();
  for (uint32_t c = 0; c < 5; ++c)
  {
    const auto s = a.OnWake(c);
    CHECK(s.sample);
    CHECK_EQ(s.tick, c);
    CHECK_EQ(s.skipped, 0U);
    a.OnDone(c, kShort, kPeriod);
  }
}

TEST(CountJumpIsSkipped)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  a.OnDone(0, kShort, kPeriod);
  const auto s = a.OnWake(3);  // 1、2 没采 / 1 and 2 were missed
  CHECK(s.sample);
  CHECK_EQ(s.tick, 3U);
  CHECK_EQ(s.skipped, 2U);
}

TEST(SameCountTwiceSamplesOnce)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  a.OnDone(0, kShort, kPeriod);
  CHECK(a.OnWake(1).sample);
  a.OnDone(1, kShort, kPeriod);
  CHECK(!a.OnWake(1).sample);  // 停止时的唤醒，计数没变 / a stop wake, count unchanged
}

TEST(TickPassedWhileSamplingIsNotMadeUp)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  a.OnDone(1, kShort, kPeriod);  // 采的时候节拍 1 到了 / tick 1 came while sampling
  CHECK(!a.OnWake(1).sample);
  const auto s = a.OnWake(2);
  CHECK(s.sample);
  CHECK_EQ(s.tick, 2U);
  CHECK_EQ(s.skipped, 1U);
}

TEST(BusyTickGivesOneUp)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  // 900 + max(1000/8, 15) > 1000：再让出一拍 / one more tick is given up
  a.OnDone(0, 900, kPeriod);
  CHECK(!a.OnWake(1).sample);
  const auto s = a.OnWake(2);
  CHECK(s.sample);
  CHECK_EQ(s.skipped, 1U);
}

TEST(BusyBelowGapKeepsEveryTick)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  // 800 + max(1000/8, 800/4) = 1000，不超过周期 / not above the period
  a.OnDone(0, 800, kPeriod);
  CHECK(a.OnWake(1).sample);
}

TEST(GapGrowsWithBusyTime)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  // 875 + max(125, 218) > 1000：长帧留给别的任务至少占用的 1/4 / A long frame leaves
  // other tasks at least a quarter of its busy time.
  a.OnDone(0, 875, kPeriod);
  CHECK(!a.OnWake(1).sample);
}

TEST(GapAppliesAcrossSeveralPeriods)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  // 周期 50 us、一帧 95 us，采完时节拍 1 已到：95 + 23 = 118 us，下一个要采的是
  // 节拍 3，不是紧接着到来的节拍 2 / Period 50 us, a 95 us frame, tick 1 already came:
  // 95 + 23 = 118 us, so the next tick to sample is 3, not tick 2 arriving right away.
  a.OnDone(1, 95, 50);
  CHECK_EQ(a.Next(), 3U);
  CHECK(!a.OnWake(2).sample);
  const auto s = a.OnWake(3);
  CHECK(s.sample);
  CHECK_EQ(s.skipped, 2U);
}

TEST(CountWrapsAround)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0U);
  // 分几步走到回绕附近，每步不到 2^31 拍 / A few steps to near the wrap, each under
  // 2^31.
  a.OnDone(0x60000000U, 10, 1000);
  CHECK(a.OnWake(0x60000001U).sample);
  a.OnDone(0xC0000000U, 10, 1000);
  CHECK(a.OnWake(0xC0000001U).sample);
  a.OnDone(0xFFFFFFFDU, 10, 1000);
  CHECK_EQ(a.Next(), 0xFFFFFFFEU);
  CHECK(a.OnWake(0xFFFFFFFEU).sample);
  a.OnDone(0xFFFFFFFEU, 10, 1000);
  CHECK(a.OnWake(0xFFFFFFFFU).sample);
  a.OnDone(0xFFFFFFFFU, 10, 1000);
  CHECK_EQ(a.Next(), 0U);
  const auto s = a.OnWake(0U);
  CHECK(s.sample);
  CHECK_EQ(s.skipped, 0U);
}

TEST(MinimumGapForShortPeriods)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  // 周期 100 us：留 max(15, 12, 21) = 21 us；占用 86 us 时 86 + 21 > 100 / Period
  // 100 us leaves max(15, 12, 21) = 21 us; 86 us busy gives 86 + 21 > 100.
  a.OnDone(0, 86, 100);
  CHECK(!a.OnWake(1).sample);
}

TEST(BeginRestartsAtZero)
{
  TickAccount a;
  a.Begin();
  (void)a.OnWake(0);
  a.OnDone(0, kShort, kPeriod);
  (void)a.OnWake(5);
  a.OnDone(5, kShort, kPeriod);
  a.Begin();
  const auto s = a.OnWake(0);
  CHECK(s.sample);
  CHECK_EQ(s.tick, 0U);
  CHECK_EQ(s.skipped, 0U);
}
