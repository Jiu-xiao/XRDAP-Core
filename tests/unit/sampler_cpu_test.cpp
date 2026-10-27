// SPDX-License-Identifier: Apache-2.0
// 采样不能饿死命令。判据：用户决定 B9（探针不拒绝任何周期，任何周期下都要能回应命令；
// 跟不上只表现为跳拍）和 docs/protocol.md 3.5（采样醒来的条件、两帧之间留给别的任务的
// 时间，以及有命令在等时采样最多再读一帧就让路）。这里用 1 us 一步的模拟：一个核、节拍
// 中断最高、采样任务其次、命令任务最低；中断、唤醒和读一帧的耗时都取悲观值，节拍比一帧
// 快。采样任务和 Sampler::Service 一样用 DecideWake（TickAccount 加 CommandYield）决定
// 何时醒、采哪一拍、是否让路；命令进入执行器时 Begin，做完时 End。
//
// Sampling must not starve commands. Oracles: user decision B9 (the probe refuses no
// period and must answer commands at any period; falling behind shows only as skipped
// ticks) and docs/protocol.md 3.5 (when sampling wakes, the time left to other tasks
// between two frames, and that sampling reads at most one more frame once a command
// waits). A simulation in 1 us steps: one core, the tick interrupt highest, the sampling
// task next, the command task lowest; interrupt, wake and frame costs are pessimistic
// and ticks come faster than a frame. Like Sampler::Service, the sampling task decides
// when to wake, which tick to sample and whether to yield with DecideWake (TickAccount
// plus CommandYield); a command calls Begin on entering the executor and End when done.
#include <cstdint>

#include "check.hpp"
#include "xrdap/vendor/sampling.hpp"
#include "xrdap/vendor/sampling.hpp"

using namespace XrDap::Vendor;

namespace
{
struct Result
{
  uint32_t ticks = 0;         // 中断数 / interrupts
  uint32_t sampled = 0;       // 采了的帧 / frames sampled
  uint32_t skipped = 0;       // 账目里的跳拍 / ticks skipped by the account
  uint32_t last_tick = 0;     // 最后采的节拍号 / last tick sampled
  uint32_t wakes = 0;         // 采样任务醒来次数 / sampling task wakes
  uint32_t commands = 0;      // 回应了的命令 / commands answered
  uint32_t max_latency = 0;   // 命令从到达到做完最长的时间 / longest arrival-to-done
  uint32_t max_executor = 0;  // 命令在执行器里最长的时间 / longest time in the executor
};

// 一个核上的耗时 / Costs on one core.
struct Costs
{
  uint32_t period;        // 节拍周期 / tick period
  uint32_t frame;         // 读一帧 / reading one frame
  uint32_t wake = 4U;     // 切到采样任务、取链路锁 / switch to sampling, take the lock
  uint32_t resume = 0U;   // 命令任务每次重新拿到 CPU 的切换 / command task switching in
  uint32_t receive = 0U;  // 命令进执行器之前的接收和拆帧 / receiving and unframing
};

constexpr uint32_t kIsrUs = 1U;  // 中断本身 / the interrupt itself

// 命令每 2 ms 来一条：先 receive us 收包拆帧，再进执行器（Begin）做 command us，做完
// End。帧的占用和 Sampler::Service 一样从节拍中断唤醒的那一刻量到读完 / One command
// every 2 ms: receive us of receiving and unframing, then the executor (Begin) for
// command us, then End. A frame's busy time is measured as Sampler::Service does, from
// the moment the tick interrupt woke the task to the end of the read.
Result Simulate(const Costs& c, uint32_t command, uint32_t run_us)
{
  constexpr uint32_t kCommandEvery = 2000U;
  constexpr uint32_t kParkedTicks = 0x40000000U;

  Result r;
  TickAccount account;
  account.Begin();
  CommandYield yield;
  uint32_t count = 0U;    // 节拍计数 / tick count
  uint32_t wake_at = 0U;  // 中断在计数到这里时唤醒 / interrupt wakes at this count
  uint32_t posts = 1U;    // 信号量；开始采样时 Post 一次 / semaphore; Start posts once
  bool again = false;     // 不等信号量再跑一圈 / run again without waiting
  uint32_t woke_tick = UINT32_MAX;
  uint32_t woke_t = 0U;
  uint32_t frame_start = 0U;
  uint32_t frame_tick = 0U;
  uint32_t isr_left = 0U;
  uint32_t sampler_left = 0U;
  bool in_frame = false;
  bool in_wake = false;
  bool cmd_pending = false;
  bool cmd_in_executor = false;
  uint32_t cmd_receive_left = 0U;
  uint32_t cmd_left = 0U;
  uint32_t cmd_since = 0U;
  uint32_t cmd_entered = 0U;
  bool cmd_was_running = false;
  uint32_t resume_left = 0U;

  // 一帧或一次空醒结束：定下一次唤醒的计数 / A frame or an empty wake ends: set the
  // count of the next wake.
  auto settle = [&]()
  {
    wake_at = account.Next();
    again = TickDue(count, wake_at);
  };

  for (uint32_t t = 1; t <= run_us; ++t)
  {
    if (t % c.period == 0U)
    {
      ++count;
      ++r.ticks;
      isr_left += kIsrUs;
      if (TickWakes(count, wake_at))
      {
        // Sampler::OnTickIsr consumes a wake deadline until Service rearms it.
        wake_at = count + kParkedTicks;
        woke_tick = count;
        woke_t = t;
        ++posts;
      }
    }
    if (t % kCommandEvery == 0U && !cmd_pending)
    {
      cmd_pending = true;
      cmd_in_executor = false;
      cmd_receive_left = c.receive;
      cmd_left = command;
      cmd_since = t;
    }

    if (isr_left > 0U)
    {
      // 中断返回同一个任务不算切换 / Returning from an interrupt to the same task is
      // not a switch.
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
      sampler_left = c.wake;
    }
    if (sampler_left > 0U)
    {
      cmd_was_running = false;
      if (--sampler_left == 0U)
      {
        if (in_wake)
        {
          in_wake = false;
          const WakeDecision d = DecideWake(account, yield, count);
          if (d.parked)
          {
            wake_at = count + kParkedTicks;
          }
          else if (d.step.sample)
          {
            ++r.sampled;
            r.skipped += d.step.skipped;
            r.last_tick = d.step.tick;
            in_frame = true;
            frame_start = t;
            frame_tick = d.step.tick;
            sampler_left = c.frame;
          }
          else
          {
            settle();
          }
        }
        else if (in_frame)
        {
          in_frame = false;
          uint32_t start = frame_start;
          if (woke_tick == frame_tick && frame_start - woke_t <= c.period)
          {
            start = woke_t;
          }
          account.OnDone(count, t - start, c.period);
          yield.FrameEnd();
          settle();
        }
      }
      continue;
    }
    if (!cmd_pending)
    {
      cmd_was_running = false;
      continue;
    }
    if (!cmd_was_running)
    {
      cmd_was_running = true;
      resume_left = c.resume;
    }
    if (resume_left > 0U)
    {
      --resume_left;
      continue;
    }
    if (!cmd_in_executor)
    {
      if (cmd_receive_left > 0U)
      {
        --cmd_receive_left;
        continue;
      }
      cmd_in_executor = true;
      cmd_entered = t;
      yield.Begin();
    }
    if (--cmd_left == 0U)
    {
      cmd_pending = false;
      if (yield.End())
      {
        ++posts;
      }
      ++r.commands;
      const uint32_t latency = t - cmd_since;
      const uint32_t executor = t - cmd_entered;
      r.max_latency = latency > r.max_latency ? latency : r.max_latency;
      r.max_executor = executor > r.max_executor ? executor : r.max_executor;
    }
  }
  return r;
}

void CheckAccounting(const Result& r)
{
  // 采样还在进行，没采的节拍全部记为跳拍 / Sampling still runs and every tick not
  // sampled is counted as skipped.
  CHECK(r.sampled > 0U);
  CHECK_EQ(r.sampled + r.skipped, r.last_tick + 1U);
}

void CheckServed(const Costs& c, uint32_t command)
{
  constexpr uint32_t kRunUs = 200000U;
  const Result r = Simulate(c, command, kRunUs);
  // 命令每 2 ms 一条，全部在下一条到来前回应 / One command every 2 ms, each answered
  // before the next arrives.
  CHECK_EQ(r.commands, kRunUs / 2000U - 1U);
  CHECK(r.max_latency < 2000U);
  CheckAccounting(r);
  // 采样任务只为要采的节拍醒来（加上开始、每条命令让路后至多两次唤醒和少量空醒）/
  // The sampling task wakes only for ticks it samples (plus the start, at most two
  // wakes per command after yielding, and a few empty wakes).
  CHECK(r.wakes <= r.sampled + r.sampled / 8U + 2U * r.commands + 2U);
}

// 命令进执行器后最多被采样占去一帧，另有两次唤醒（读那一帧、决定停下）和随之的两次
// 切换，其余是它自己的耗时和节拍中断 / Once in the executor a command loses at most one
// frame to sampling, plus two wakes (reading that frame, deciding to park) and the two
// switches they cause; the rest is its own cost and the tick interrupts.
uint32_t ExecutorBound(const Costs& c, uint32_t command)
{
  const uint32_t work = 2U * c.wake + c.frame + 2U * c.resume + command;
  return work + work * kIsrUs / c.period + 2U * kIsrUs + 2U;
}

// 命令占 10% 的 CPU，低于两帧之间保证的约 1/5 / Commands use 10% of the CPU, below the
// about 1/5 guaranteed between frames.
constexpr uint32_t kLightCommandUs = 200U;
}  // namespace

TEST(CommandsServedWhenFrameJustUnderTwoPeriods)
{
  // 一帧比两个周期略短：旧规则下下一拍紧接着帧结束到来 / A frame just under two
  // periods: under the old rule the next tick came right after the frame ended.
  CheckServed({50U, 95U}, kLightCommandUs);
}

TEST(CommandsServedWhenFrameSlightlyLongerThanPeriod)
{
  CheckServed({25U, 30U}, kLightCommandUs);
}

TEST(CommandsServedAtShortPeriodLongFrame) { CheckServed({25U, 300U}, kLightCommandUs); }

TEST(CommandsServedAtTenMicroseconds) { CheckServed({10U, 40U}, kLightCommandUs); }

TEST(CommandsServedWhenWakeCostsAlmostAsMuchAsTheRead)
{
  // r2 S3 串口、1 个条目、50 us：读帧本身很短，唤醒、切换和取锁又占了一截，命令任务每次
  // 重新拿到 CPU 也要切换 / r2 S3 serial, 1 item, 50 us: the read itself is short, the
  // wake, switch and lock take another chunk, and the command task pays a switch each
  // time it gets the CPU back.
  CheckServed({50U, 25U, 12U, 6U}, 300U);
}

TEST(CommandInExecutorWaitsAtMostOneFrame)
{
  // 节拍 10 us、一帧 40 us，命令 300 us、每次重新拿到 CPU 切换 6 us：进执行器以后最多
  // 再等一帧，之后的节拍都放弃并计为跳拍 / 10 us ticks, 40 us frames, a 300 us
  // command paying 6 us per switch back in: once in the executor it waits for at most
  // one more frame, and the ticks after that are given up and counted as skipped.
  const Costs c{10U, 40U, 4U, 6U, 50U};
  const Result r = Simulate(c, 300U, 200000U);
  CHECK_EQ(r.commands, 99U);
  CHECK(r.max_executor <= ExecutorBound(c, 300U));
  CheckAccounting(r);
}

TEST(CommandInExecutorWaitsAtMostOneFrameWithLongFrames)
{
  // 一帧 300 us 远长于 25 us 的节拍 / 300 us frames, far longer than the 25 us tick.
  const Costs c{25U, 300U, 10U, 6U, 50U};
  const Result r = Simulate(c, 300U, 200000U);
  CHECK_EQ(r.commands, 99U);
  CHECK(r.max_executor <= ExecutorBound(c, 300U));
  CheckAccounting(r);
}

TEST(EveryTickSampledWhenFrameIsShort)
{
  // 周期 1 ms、一帧 100 us，命令和节拍有重叠也一拍都不跳：只有命令在等时又到了第二帧
  // 才让路 / Period 1 ms, 100 us frames: no tick skipped even though commands overlap
  // ticks; sampling yields only when a second frame comes due while a command waits.
  const Result r = Simulate({1000U, 100U}, 300U, 100000U);
  CHECK_EQ(r.skipped, 0U);
  CHECK(r.sampled >= 99U);
}
