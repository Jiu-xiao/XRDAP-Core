// SPDX-License-Identifier: Apache-2.0
#pragma once

// 采样的两个运行策略，平台在 xrdap_port.hpp 里二选一（Port::SamplerPolicy），编译期决定，
// 运行时没有分支。
//   TaskSampling：有 RTOS 的探针。采样任务运行 RunSamplerTask()，在信号量上无超时地阻塞；
//     节拍中断、Start、Stop 和最后一条命令做完都经这个信号量唤醒它。信号量在策略构造时就
//     存在，所以这些唤醒不依赖采样任务是否已经进入 RunSamplerTask：采样任务晚启动时，
//     先到的唤醒留在信号量里，任务一进来就收到。采样给命令让路（CommandYield）只在这个
//     策略里存在：命令和采样在不同的任务里，才会抢链路。
//   LoopSampling：没有 RTOS 的探针。主循环在两条 DAP 命令之间调用 Poll()，命令和采样在
//     同一个线程里串行，没有信号量，也没有让路。除节拍计数（中断加、主循环读）外都是普通
//     成员，节拍中断里不读时基。
// 两个策略有同一组成员函数，由 SamplerT 调用（接口见 sampler.hpp）；它们是互相独立的
// 类型，没有公共基类，用错入口（单循环调 RunSamplerTask、任务调 Poll）是编译错误。
//
// The two ways to run sampling, chosen by the platform in xrdap_port.hpp
// (Port::SamplerPolicy) at compile time, with no run-time branch.
//   TaskSampling: a probe with an RTOS. The sampling task runs RunSamplerTask() and blocks on
//     a semaphore with no timeout; the tick interrupt, Start, Stop and the last command to
//     finish all wake it through that semaphore. The semaphore exists from the policy's
//     construction, so these wakes do not depend on the sampling task having entered
//     RunSamplerTask: when the task starts late, a wake that came first stays in the
//     semaphore and the task receives it on entry. Sampling yielding to commands
//     (CommandYield) exists in this policy only: commands and sampling run in different
//     tasks, so they can compete for the link.
//   LoopSampling: a probe without an RTOS. The main loop calls Poll() between two DAP
//     commands; commands and sampling run one after another in the same thread, so there
//     is no semaphore and no yielding. Everything but the tick count (the interrupt adds,
//     the main loop reads) is a plain member, and the tick interrupt does not read the time
//     base.
// Both policies have the same member functions, called by SamplerT (the interface is in
// sampler.hpp). They are independent types with no common base, and calling the wrong
// entry (RunSamplerTask in a single loop, Poll in a task) is a compile error.

#include <atomic>
#include <cstdint>

#include "semaphore.hpp"
#include "timebase.hpp"
#include "xrdap/vendor/command_yield.hpp"
#include "xrdap/vendor/sampler_timing.hpp"
#include "xrdap/vendor/tick_account.hpp"

namespace XrDap::Vendor
{

/// 任务策略：采样在自己的任务里，信号量唤醒 / Task policy: sampling runs in its own task,
/// woken through a semaphore.
class TaskSampling
{
 public:
  /// 有 RunTask 入口、有让路 / Has the RunTask entry and the yield to commands.
  static constexpr bool kRunsInTask = true;

  /// @brief 是否正在采样 / Whether sampling is running.
  /// @note 任何上下文 / Any context.
  bool Active() const { return active_.load(std::memory_order_acquire) != 0U; }

  /// @brief 开始一次新的运行，并唤醒采样任务 / Begin a new run and wake the sampling task.
  /// @note DAP 执行上下文；采样任务是否已经运行都可以 / DAP execution context; the sampling
  ///       task may or may not be running yet.
  void RequestStart()
  {
    gen_.fetch_add(1U, std::memory_order_acq_rel);
    active_.store(1U, std::memory_order_release);
    wake_.Post();
  }

  /// @brief 请求停止，并唤醒采样任务去停定时器 / Request a stop and wake the sampling task
  ///        to stop the timer.
  /// @note DAP 执行上下文 / DAP execution context.
  void RequestStop()
  {
    active_.store(0U, std::memory_order_release);
    wake_.Post();
  }

  /// @brief 会话结束时采样自己停下（不再唤醒）/ Sampling stops itself when the session
  ///        ends (no wake needed).
  /// @note 只在采样任务里 / Sampling task only.
  void Deactivate() { active_.store(0U, std::memory_order_release); }

  /// @brief 当前这次运行的代号，Start 一次加一 / Generation of the current run, one more
  ///        per Start.
  /// @note 只在采样任务里 / Sampling task only.
  uint32_t Generation() const { return gen_.load(std::memory_order_acquire); }

  /// 节拍计数 / The tick count.
  /// @note 只在采样任务里 / Sampling task only.
  uint32_t Ticks() const { return ticks_.load(std::memory_order_relaxed); }

  /// @brief 节拍中断：计数加 elapsed_ticks，到达唤醒计数时记下这一刻并唤醒采样任务 /
  ///        Tick interrupt: add elapsed_ticks to the count; when it reaches the wake count,
  ///        note the time and wake the sampling task.
  /// @note 中断上下文，elapsed_ticks 不为 0；时基须能在中断里读 / Interrupt context,
  ///       elapsed_ticks is not 0; the time base must be readable from an interrupt.
  /// @note 期限被消耗一次后，采样任务重新装填前不会再唤醒：一帧很长时，反复的中断不能攒下
  ///       信号量，饿死命令处理 / Once the deadline is consumed there is no further wake
  ///       until the sampling task rearms it: while a long frame runs, repeated interrupts
  ///       must not accumulate semaphore posts and starve command handling.
  /// @note 加计数和读唤醒计数用 seq_cst，与 Arm 里装填唤醒计数和读计数配对：两边各写一个量
  ///       再读对方的，seq_cst 的全序保证至少一边看得到对方，不会既没唤醒、又没发现计数已
  ///       到（relaxed 序不保证；这一处原来是 relaxed）/ Raising the count and reading the
  ///       wake count are seq_cst, paired with rearming the wake count and reading the count
  ///       in Arm: each side writes one variable and then reads the other's, and the total
  ///       order of seq_cst guarantees at least one side sees the other, so a due tick can no
  ///       longer be both unwoken and unnoticed (relaxed order does not guarantee this, and
  ///       these four accesses were relaxed).
  void OnTickIsr(uint32_t elapsed_ticks)
  {
    const uint32_t count =
        ticks_.fetch_add(elapsed_ticks, std::memory_order_seq_cst) + elapsed_ticks;
    uint32_t wake_at = wake_at_.load(std::memory_order_seq_cst);
    if (TickWakes(count, wake_at) &&
        wake_at_.compare_exchange_strong(wake_at, count + kParkedTicks,
                                         std::memory_order_relaxed))
    {
      woke_us_.store(static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds()),
                     std::memory_order_relaxed);
      woke_tick_.store(count, std::memory_order_release);
      wake_.PostFromCallback(true);
    }
  }

  /// @brief 新的一次运行开始：计数清零，不记中断唤醒，不再停下 / A new run starts: clear
  ///        the count, forget any interrupt wake, no longer parked.
  /// @note 只在采样任务里，节拍定时器已停 / Sampling task only, tick timer stopped.
  void BeginRun()
  {
    ticks_.store(0U, std::memory_order_relaxed);
    woke_tick_.store(UINT32_MAX, std::memory_order_relaxed);
    yield_.Restart();
  }

  /// @brief 定下中断下一次唤醒的计数，并告诉调用方要采的节拍是否已经到了 / Set the count
  ///        of the interrupt's next wake, and tell the caller whether the tick to sample
  ///        has already come.
  /// @return true 时要采的节拍已到，中断不会再唤醒，调用方接着做 / True when it has come:
  ///         the interrupt will not wake again, so the caller carries on.
  /// @note 只在采样任务里。先装填、再读计数，都是 seq_cst，与 OnTickIsr 配对 / Sampling
  ///       task only. Rearm, then read the count, both seq_cst, paired with OnTickIsr.
  bool Arm(uint32_t next)
  {
    wake_at_.store(next, std::memory_order_seq_cst);
    return TickDue(ticks_.load(std::memory_order_seq_cst), next);
  }

  /// @brief 停下等命令：中断不再按计数唤醒，由 CommandEnd 唤醒 / Parked for commands: the
  ///        interrupt no longer wakes on the count; CommandEnd wakes the task.
  /// @note 只在采样任务里 / Sampling task only.
  void Park(uint32_t count) { wake_at_.store(count + kParkedTicks, std::memory_order_relaxed); }

  /// @brief 醒来时的决定：节拍账目加命令让路 / Decision on a wake: tick accounting plus the
  ///        yield to commands.
  /// @note 只在采样任务里 / Sampling task only.
  WakeDecision Decide(TickAccount& account, uint32_t count)
  {
    return DecideWake(account, yield_, count);
  }

  /// @brief 一拍的占用时间：有这一拍的中断唤醒时刻时从它算起 / Busy time of one tick:
  ///        counted from the interrupt's wake when this tick has one.
  /// @note 只在采样任务里；规则见 FrameBusyUs / Sampling task only; see FrameBusyUs.
  uint32_t BusyUs(uint32_t end, uint32_t begin, uint32_t tick, uint32_t period) const
  {
    return FrameBusyUs(end, begin, woke_tick_.load(std::memory_order_acquire) == tick,
                       woke_us_.load(std::memory_order_relaxed), period);
  }

  /// 一帧读完或不读了 / A frame was read or will not be read.
  void FrameEnd() { yield_.FrameEnd(); }

  /// 一条命令开始等执行 / A command starts waiting to run.
  void CommandBegin() { yield_.Begin(); }

  /// 这条命令只读，不占链路 / This command is read-only and leaves the link alone.
  void CommandReadOnly() { yield_.ReadOnly(); }

  /// 一条命令执行完；最后一条做完且采样在停车时唤醒采样 / A command has run; the last
  /// one wakes sampling if it is parked.
  void CommandEnd()
  {
    if (yield_.End())
    {
      wake_.Post();
    }
  }

  /// @brief 阻塞到有唤醒 / Block until there is a wake.
  /// @note 只在采样任务里 / Sampling task only.
  void WaitForWork() { (void)wake_.Wait(UINT32_MAX); }

 private:
  /// 停下时中断唤醒的计数放到很远处，实际上不会到 / While parked or consumed, the wake
  /// count is put far away, where it is never reached in practice.
  static constexpr uint32_t kParkedTicks = 0x40000000U;

  std::atomic<uint32_t> active_{0U};  ///< 在运行吗：DAP 执行与采样任务写，任何上下文读 / Running: written by DAP execution and the sampling task, read anywhere.
  std::atomic<uint32_t> gen_{0U};  ///< 运行代号：DAP 执行写（Start），采样任务读 / Run generation: written by DAP execution (Start), read by the sampling task.
  std::atomic<uint32_t> ticks_{0U};  ///< 节拍计数：中断加，采样任务读和清零 / Tick count: the interrupt adds, the sampling task reads and clears.
  std::atomic<uint32_t> wake_at_{0U};  ///< 中断唤醒的计数：采样任务装填，中断消耗 / Interrupt wake count: armed by the sampling task, consumed by the interrupt.
  std::atomic<uint32_t> woke_tick_{0U};  ///< 中断唤醒时的节拍：中断写，采样任务读 / Tick at the interrupt's wake: written by the interrupt, read by the sampling task.
  std::atomic<uint32_t> woke_us_{0U};  ///< 中断唤醒时的 µs 低 32 位：中断写，采样任务读 / Low 32 bits of µs at the interrupt's wake: written by the interrupt, read by the sampling task.
  LibXR::Semaphore wake_{0};  ///< 采样任务的唤醒：DAP 执行、命令任务、中断投，采样任务等 / Sampling task wake: posted by DAP execution, command tasks and the interrupt, awaited by the sampling task.
  CommandYield yield_;  ///< 命令让路：命令任务与采样任务各写一部分（成员都是原子量）/ Yield to commands: command tasks and the sampling task each write part of it (all atomics).
};

/// 单循环策略：采样在主循环里，没有信号量 / Single-loop policy: sampling runs in the main
/// loop, with no semaphore.
class LoopSampling
{
 public:
  /// 有 Poll 入口、没有让路 / Has the Poll entry and no yield to commands.
  static constexpr bool kRunsInTask = false;

  /// @brief 是否正在采样 / Whether sampling is running.
  /// @note 只在主循环里 / Main loop only.
  bool Active() const { return active_; }

  /// 开始一次新的运行；主循环下一圈的 Poll 发现 / Begin a new run; the next Poll finds it.
  void RequestStart()
  {
    ++gen_;
    active_ = true;
  }

  /// 请求停止；主循环下一圈的 Poll 停定时器 / Request a stop; the next Poll stops the timer.
  void RequestStop() { active_ = false; }

  /// 会话结束时采样自己停下 / Sampling stops itself when the session ends.
  void Deactivate() { active_ = false; }

  /// 当前这次运行的代号 / Generation of the current run.
  uint32_t Generation() const { return gen_; }

  /// 节拍计数 / The tick count.
  uint32_t Ticks() const { return ticks_.load(std::memory_order_relaxed); }

  /// @brief 节拍中断：计数加 elapsed_ticks，不读时基，不唤醒 / Tick interrupt: add
  ///        elapsed_ticks to the count; no time base read, no wake.
  /// @note 中断上下文，elapsed_ticks 不为 0 / Interrupt context, elapsed_ticks is not 0.
  void OnTickIsr(uint32_t elapsed_ticks)
  {
    ticks_.fetch_add(elapsed_ticks, std::memory_order_relaxed);
  }

  /// @brief 新的一次运行开始：计数清零 / A new run starts: clear the count.
  /// @note 节拍定时器已停 / Tick timer stopped.
  void BeginRun() { ticks_.store(0U, std::memory_order_relaxed); }

  /// @brief 要采的节拍是否已经到了（Poll 下一圈本来就会再看，这里让 Service 的返回值一致）
  ///        / Whether the tick to sample has already come (the next Poll looks again anyway;
  ///        this keeps Service's return value the same in both policies).
  bool Arm(uint32_t next) { return TickDue(Ticks(), next); }

  /// 单循环不停车 / A single loop never parks.
  void Park(uint32_t) {}

  /// @brief 醒来时的决定：只有节拍账目，没有让路 / Decision on a wake: tick accounting
  ///        only, no yielding.
  WakeDecision Decide(TickAccount& account, uint32_t count)
  {
    return {account.OnWake(count), false};
  }

  /// @brief 一拍的占用时间，从读帧开始算：中断到这里是主循环在做命令，不是采样的开销 /
  ///        Busy time of one tick, counted from the start of the frame read: the time from
  ///        the interrupt to here is the main loop running a command, not a sampling cost.
  uint32_t BusyUs(uint32_t end, uint32_t begin, uint32_t, uint32_t) const
  {
    return end - begin;
  }

  /// 没有让路，什么都不做 / No yielding, nothing to do.
  void FrameEnd() {}
  void CommandBegin() {}
  void CommandReadOnly() {}
  void CommandEnd() {}

 private:
  bool active_ = false;  ///< 在运行吗：主循环读写 / Running: read and written by the main loop.
  uint32_t gen_ = 0U;  ///< 运行代号：主循环读写 / Run generation: read and written by the main loop.
  std::atomic<uint32_t> ticks_{0U};  ///< 节拍计数：中断加，主循环读和清零（定时器已停）/ Tick count: the interrupt adds, the main loop reads and clears it (timer stopped).
};

}  // namespace XrDap::Vendor
