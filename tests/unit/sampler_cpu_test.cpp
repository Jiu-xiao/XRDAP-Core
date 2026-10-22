// SPDX-License-Identifier: Apache-2.0
// 采样不能饿死命令。判据：用户决定 B9（探针不拒绝任何周期，任何周期下都要能回应命令；
// 跟不上只表现为跳拍）和 docs/protocol.md 3.5（采样醒来的条件和两帧之间留给别的任务的
// 时间）。这里用 1 us 一步的模拟：一个核、节拍中断最高、采样任务其次、命令任务最低；
// 中断、唤醒和读一帧的耗时都取悲观值，节拍比一帧快。采样任务按 Sampler::RunTask 的
// 方式用 TickAccount 和 TickWakes / TickDue 决定何时醒、采哪一拍。
//
// Sampling must not starve commands. Oracles: user decision B9 (the probe refuses no
// period and must answer commands at any period; falling behind shows only as skipped
// ticks) and docs/protocol.md 3.5 (when sampling wakes, and the time left to other tasks
// between two frames). A simulation in 1 us steps: one core, the tick interrupt highest,
// the sampling task next, the command task lowest; interrupt, wake and frame costs are
// pessimistic and ticks come faster than a frame. The sampling task decides when to wake
// and which tick to sample with TickAccount and TickWakes / TickDue, as Sampler::RunTask
// does.
#include <cstdint>

#include "check.hpp"
#include "xrdap/vendor/tick_account.hpp"

using namespace XrDap::Vendor;

namespace
{
struct Result
{
  uint32_t ticks = 0;        // 中断数 / interrupts
  uint32_t sampled = 0;      // 采了的帧 / frames sampled
  uint32_t skipped = 0;      // 账目里的跳拍 / ticks skipped by the account
  uint32_t last_tick = 0;    // 最后采的节拍号 / last tick sampled
  uint32_t wakes = 0;        // 采样任务醒来次数 / sampling task wakes
  uint32_t commands = 0;     // 回应了的命令 / commands answered
  uint32_t max_latency = 0;  // 命令最长的回应时间 / longest command latency
};

// period：节拍周期；frame：读一帧的耗时；command：一条命令要的 CPU 时间，每 2 ms 来一条。
// period: tick period; frame: cost of one frame; command: CPU time one command needs,
// one every 2 ms.
Result Simulate(uint32_t period, uint32_t frame, uint32_t command, uint32_t run_us)
{
  constexpr uint32_t kIsrUs = 1U;  // 中断本身 / the interrupt itself
  constexpr uint32_t kWakeUs =
      4U;  // 切到采样任务、取锁、看账目 / switch in, lock, account
  constexpr uint32_t kCommandEvery = 2000U;

  Result r;
  TickAccount account;
  account.Begin();
  uint32_t count = 0U;    // 节拍计数 / tick count
  uint32_t wake_at = 0U;  // 中断在计数到这里时唤醒 / interrupt wakes at this count
  uint32_t posts = 1U;    // 信号量；开始采样时 Post 一次 / semaphore; Start posts once
  bool again = false;     // 不等信号量再跑一圈 / run again without waiting
  uint32_t isr_left = 0U;
  uint32_t sampler_left = 0U;
  bool in_frame = false;
  bool in_wake = false;
  uint32_t cmd_left = 0U;
  uint32_t cmd_since = 0U;
  bool cmd_pending = false;

  // 一帧或一次空醒结束：定下一次唤醒的计数 / A frame or an empty wake ends: set the
  // count of the next wake.
  auto settle = [&]()
  {
    wake_at = account.Next();
    again = TickDue(count, wake_at);
  };

  for (uint32_t t = 1; t <= run_us; ++t)
  {
    if (t % period == 0U)
    {
      ++count;
      ++r.ticks;
      isr_left += kIsrUs;
      if (TickWakes(count, wake_at))
      {
        ++posts;
      }
    }
    if (t % kCommandEvery == 0U && !cmd_pending)
    {
      cmd_pending = true;
      cmd_left = command;
      cmd_since = t;
    }

    if (isr_left > 0U)
    {
      --isr_left;
      continue;
    }
    if (sampler_left == 0U && !in_frame && !in_wake && (again || posts > 0U))
    {
      if (again)
      {
        again = false;
      }
      else
      {
        --posts;
      }
      ++r.wakes;
      in_wake = true;
      sampler_left = kWakeUs;
    }
    if (sampler_left > 0U)
    {
      if (--sampler_left == 0U)
      {
        if (in_wake)
        {
          in_wake = false;
          const TickAccount::Step step = account.OnWake(count);
          if (step.sample)
          {
            ++r.sampled;
            r.skipped += step.skipped;
            r.last_tick = step.tick;
            in_frame = true;
            sampler_left = frame;
          }
          else
          {
            settle();
          }
        }
        else if (in_frame)
        {
          in_frame = false;
          account.OnDone(count, frame, period);
          settle();
        }
      }
      continue;
    }
    if (cmd_pending && --cmd_left == 0U)
    {
      cmd_pending = false;
      ++r.commands;
      const uint32_t latency = t - cmd_since;
      r.max_latency = latency > r.max_latency ? latency : r.max_latency;
    }
  }
  return r;
}

void CheckServed(uint32_t period, uint32_t frame)
{
  constexpr uint32_t kRunUs = 200000U;
  // 命令占 10% 的 CPU，低于保证的约 1/5 减去节拍中断 / Commands use 10% of the CPU,
  // below the guaranteed about 1/5 minus the tick interrupts.
  constexpr uint32_t kCommandUs = 200U;
  const Result r = Simulate(period, frame, kCommandUs, kRunUs);
  // 命令每 2 ms 一条，全部在下一条到来前回应 / One command every 2 ms, each answered
  // before the next arrives.
  CHECK_EQ(r.commands, kRunUs / 2000U - 1U);
  CHECK(r.max_latency < 2000U);
  // 采样还在进行，没采的节拍全部记为跳拍 / Sampling still runs and every tick not
  // sampled is counted as skipped.
  CHECK(r.sampled > 0U);
  CHECK_EQ(r.sampled + r.skipped, r.last_tick + 1U);
  // 采样任务只为要采的节拍醒来（加上开始的一次和少量空醒）/ The sampling task wakes
  // only for ticks it samples (plus the start and a few empty wakes).
  CHECK(r.wakes <= r.sampled + r.sampled / 8U + 2U);
}
}  // namespace

TEST(CommandsServedWhenFrameJustUnderTwoPeriods)
{
  // 一帧比两个周期略短：旧规则下下一拍紧接着帧结束到来 / A frame just under two
  // periods: under the old rule the next tick came right after the frame ended.
  CheckServed(50U, 95U);
}

TEST(CommandsServedWhenFrameSlightlyLongerThanPeriod) { CheckServed(25U, 30U); }

TEST(CommandsServedAtShortPeriodLongFrame) { CheckServed(25U, 300U); }

TEST(CommandsServedAtTenMicroseconds) { CheckServed(10U, 40U); }

TEST(EveryTickSampledWhenFrameIsShort)
{
  // 周期 1 ms、一帧 100 us：一拍都不跳 / Period 1 ms, 100 us frames: no tick skipped.
  const Result r = Simulate(1000U, 100U, 300U, 100000U);
  CHECK_EQ(r.skipped, 0U);
  CHECK(r.sampled >= 99U);
}
