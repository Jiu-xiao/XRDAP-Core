// SPDX-License-Identifier: Apache-2.0
// 采样给命令让路。判据：用户决定 B9 和 docs/protocol.md 3.5——有命令在等时采样最多
// 再读一帧，之后的节拍放弃并计为跳拍，最后一条命令做完时唤醒采样、从下一个节拍起再采；
// 没有命令在等时什么都不变。
//
// Sampling yields to commands. Oracles: user decision B9 and docs/protocol.md 3.5: with
// a command waiting sampling reads at most one more frame, later ticks are given up and
// counted as skipped, the last command to finish wakes sampling, which resumes from the
// next tick; nothing changes with no command waiting.
#include "xrdap/vendor/command_yield.hpp"

#include "check.hpp"

using namespace XrDap::Vendor;

namespace
{
// 采一拍：决定后若要采就当作 10 us 读完 / Sample one tick: if the decision says so,
// treat it as read in 10 us.
WakeDecision Wake(TickAccount& a, CommandYield& y, uint32_t count)
{
  const WakeDecision d = DecideWake(a, y, count);
  if (d.step.sample)
  {
    a.OnDone(count, 10U, 1000U);
    y.FrameEnd();
  }
  return d;
}
}  // namespace

TEST(NoCommandSamplesEveryTick)
{
  TickAccount a;
  a.Begin();
  CommandYield y;
  for (uint32_t c = 0; c < 4; ++c)
  {
    CHECK(Wake(a, y, c).step.sample);
  }
}

TEST(OneMoreFrameThenYield)
{
  TickAccount a;
  a.Begin();
  CommandYield y;
  CHECK(Wake(a, y, 0).step.sample);
  y.Begin();
  CHECK(Wake(a, y, 1).step.sample);  // 命令在等时还能再采一帧 / one more frame
  const WakeDecision d = Wake(a, y, 2);
  CHECK(!d.step.sample);
  CHECK(d.parked);
  CHECK(Wake(a, y, 3).parked);  // 还在等 / still waiting
}

TEST(LastCommandWakesAndSamplingResumesNextTick)
{
  TickAccount a;
  a.Begin();
  CommandYield y;
  CHECK(Wake(a, y, 0).step.sample);
  y.Begin();
  y.Begin();
  CHECK(Wake(a, y, 1).step.sample);
  CHECK(Wake(a, y, 2).parked);
  CHECK(!y.End());  // 还有一条在等 / one still waits
  CHECK(y.End());   // 最后一条：唤醒采样 / the last one: wake sampling
  // 唤醒时计数为 4：不在这时补采，从节拍 5 起再采，2 到 4 计为跳拍 / Woken at count
  // 4: no catch-up sample now; sampling resumes at tick 5 and 2 to 4 count as skipped.
  const WakeDecision resumed = Wake(a, y, 4);
  CHECK(!resumed.step.sample);
  CHECK(!resumed.parked);
  CHECK_EQ(a.Next(), 5U);
  const WakeDecision next = Wake(a, y, 5);
  CHECK(next.step.sample);
  CHECK_EQ(next.step.skipped, 3U);
}

TEST(CommandDoneBeforeSecondFrameLeavesNoSkip)
{
  // 命令在下一个节拍前做完：没有停下，也就没有跳拍 / The command finishes before the
  // next tick: no park, so no skip.
  TickAccount a;
  a.Begin();
  CommandYield y;
  CHECK(Wake(a, y, 0).step.sample);
  y.Begin();
  CHECK(Wake(a, y, 1).step.sample);
  CHECK(!y.End());  // 采样没停下，不用唤醒 / sampling is not parked, no wake needed
  const WakeDecision d = Wake(a, y, 2);
  CHECK(d.step.sample);
  CHECK_EQ(d.step.skipped, 0U);
}

TEST(FrameInProgressIsTheOneFrame)
{
  // 命令开始等时正在读一帧：这一帧就是它要等的那一帧，下一个到期的节拍让路 / A frame
  // is being read when the command starts waiting: that frame is the one it waits for,
  // and the next due tick yields.
  TickAccount a;
  a.Begin();
  CommandYield y;
  CHECK(DecideWake(a, y, 0).step.sample);
  y.Begin();  // 读帧当中 / while the frame is read
  a.OnDone(0, 10U, 1000U);
  y.FrameEnd();
  CHECK(Wake(a, y, 1).parked);
}

TEST(RestartClearsThePark)
{
  TickAccount a;
  a.Begin();
  CommandYield y;
  CHECK(Wake(a, y, 0).step.sample);
  y.Begin();
  CHECK(Wake(a, y, 1).step.sample);
  CHECK(Wake(a, y, 2).parked);
  (void)y.End();
  a.Begin();
  y.Restart();
  // 新的一次采样，节拍 0 马上采 / A new run: tick 0 at once.
  CHECK(Wake(a, y, 0).step.sample);
}
