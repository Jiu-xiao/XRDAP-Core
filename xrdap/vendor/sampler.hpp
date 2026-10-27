// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针端周期采样（docs/protocol.md 3.5）。节拍定时器（TickTimer，平台实现）是唯一的
// 时钟：它的中断调用
// OnTickIsr()，把节拍计数加上实际跨过的周期数；采样醒来后按 TickAccount 决定采哪一拍、
// 跳过了几拍，时基只用来给帧打时间戳。每一拍在 MemAp::Lock 里完成：确认这次运行还在
// （代号没变）、读到期的条目、把帧放进缓冲，所以停止后换了配置也不会混进旧的帧。
// 运行方式由策略类型 Policy 在编译期决定（sampler_policy.hpp），Sampler 只留两种方式共用
// 的部分：帧的组装、MEM-AP 访问、重新同步、节拍账目和帧缓冲。
//   任务（TaskSampling）：平台建一个任务运行 RunTask()，任务在信号量上无超时地阻塞，计数到达
//     下一个要采的节拍、Start、Stop 和让路结束都唤醒它。任务应为 SWD 所在核上优先级最高的
//     任务。它不会饿死同一核上的命令处理：任务只为要采的节拍醒来，两帧之间至少空出
//     max(15 us, 周期/8, 占用/4)（见 tick_account.hpp），所以任何周期下命令处理都至少有
//     这个核约 1/5 的时间（节拍中断本身除外）。
//   单循环（LoopSampling，没有 RTOS）：主循环在两条 DAP 命令之间调用 Poll()，计数变了就采
//     一拍。
//
// Probe-side periodic sampling (docs/protocol.md 3.5). The tick timer (TickTimer,
// implemented by the platform) is the only clock: its interrupt calls OnTickIsr(), which
// adds elapsed_ticks to the tick count. On waking, TickAccount decides which tick to sample
// and how many were skipped; the time base only stamps frames. Every tick completes under
// MemAp::Lock: it checks the run is still current (same generation), reads the due items
// and puts the frame into the buffer, so a frame of an old run cannot land after a stop
// and a new configuration. The way to run is chosen at compile time by the policy type
// Policy (sampler_policy.hpp); Sampler keeps only what both ways share: frame assembly,
// MEM-AP access, resynchronisation, tick accounting and the frame buffer.
//   Task (TaskSampling): the platform creates a task that runs RunTask(); it blocks on a
//     semaphore with no timeout, and the count reaching the next tick to sample, Start,
//     Stop and the end of a yield wake it. The task should have the highest priority on
//     the core that drives SWD. It does not starve command handling on that core: the task
//     wakes only for ticks it samples, and at least max(15 us, period/8, busy/4) is left
//     free between two frames (see tick_account.hpp), so at any period command handling
//     gets at least about 1/5 of the core (the tick interrupts themselves aside).
//   Single loop (LoopSampling, no RTOS): the main loop calls Poll() between DAP commands
//     and samples a tick when the count changed.

#include <cstdint>
#include <type_traits>

#include "xrdap/vendor/frame_ring.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/sampler_policy.hpp"
#include "xrdap/vendor/tick_account.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

namespace XrDap::Vendor
{

/// @brief 节拍定时器，平台实现 / Tick timer, implemented by the platform.
/// @note Start 和 Stop 都在采样的执行上下文里调用（采样任务或单循环），定时器中断因此
///       可以分配在采样所在的核上。中断里调用 DapVendor::OnTickIsr() / Start and Stop
///       are both called in the sampling context (sampling task or single loop), so the
///       timer interrupt can be allocated on the sampling core. Its interrupt calls
///       DapVendor::OnTickIsr().
class TickTimer
{
 public:
  /// @brief 从现在起每 period_us 中断一次 / Interrupt every period_us from now on.
  virtual void Start(uint32_t period_us) = 0;
  /// 停止中断 / Stop interrupting.
  virtual void Stop() = 0;

 protected:
  ~TickTimer() = default;
};

/// @brief 当前时刻，µs / The current time in µs.
/// @note 只在 sampler.cpp 里定义一份，不内联：时基读数的展开比一次调用大得多，Service 里
///       有好几处要用 / Defined once in sampler.cpp and not inlined: the time base read
///       expands to much more than a call, and Service needs it in several places.
uint64_t SamplerNowUs();

/// 两种运行方式共用的采样部分：配置、帧的组装、帧缓冲 / The part of sampling both ways
/// share: configuration, frame assembly and the frame buffer.
class SamplerCore
{
 public:
  /// @brief 每拍末尾调用，返回 false 时采样停止；session 是发起采样的会话编号 / Called at
  ///        the end of every tick; sampling stops when it returns false. session is the
  ///        number of the session that started sampling.
  using KeepFn = bool (*)(void* ctx, uint32_t session);

  /// @brief 清空帧缓冲 / Clear the frame buffer.
  /// @note DAP 执行上下文 / DAP execution context.
  void ClearFrames() { ring_.Reset(); }

  /// @brief 取出整帧（SUB_READ）/ Take whole frames (SUB_READ).
  /// @note DAP 执行上下文 / DAP execution context.
  FrameRing::Taken TakeFrames(uint8_t* dst, uint32_t limit)
  {
    return ring_.Take(dst, limit);
  }

 protected:
  SamplerCore(MemAp& mem, KeepFn keep, void* keep_ctx)
      : mem_(mem), keep_(keep), keep_ctx_(keep_ctx)
  {
  }
  ~SamplerCore() = default;

  /// @brief 保存这组已校验过的条目，读各 AP 的 CSW / Save these validated items and read
  ///        every AP's CSW.
  /// @return kStatusOk；读 CSW 失败时 kStatusNoTarget / kStatusOk; kStatusNoTarget when a
  ///         CSW read fails.
  uint8_t Configure(const SubItemList& items, uint32_t period_us, bool stop_on_full,
                    uint32_t session);

  /// @brief 读一拍里到期的条目，组成帧放进缓冲 / Read the items due in one tick and put
  ///        the frame into the buffer.
  /// @param step 这一拍的节拍号和漏拍数 / The tick number and skipped count of this tick.
  /// @note 帧 = 节拍号 u32 + 时间戳 u48 + 状态位图 + 各到期条目的值；持有 MemAp::Lock
  ///       时调用 / Frame = tick u32 + timestamp u48 + status bitmap + the due items'
  ///       values; called while holding MemAp::Lock.
  void SampleTick(const TickAccount::Step& step);

  MemAp& mem_;
  KeepFn keep_;
  void* keep_ctx_;

  // 以下在 MemAp::Lock 里读写 / The members below are read and written under
  // MemAp::Lock.
  uint32_t period_us_ = 0U;
  uint32_t session_ = 0U;

  // 以下只在采样的执行上下文里用 / The members below are used only in the sampling
  // context.
  uint32_t run_gen_ = 0U;
  bool timer_on_ = false;
  TickAccount account_;

  /// @brief 停定时器（若开着）/ Stop the timer if it is on.
  /// @note 不内联：平台的定时器类型对 Service 保持未知，调用不会被去虚化后展开进 Service /
  ///       Out of line: the platform's timer type stays unknown to Service, so the calls are
  ///       not devirtualised and expanded into it.
  void StopTimer(TickTimer& timer);

  /// 以 period_us_ 开定时器 / Start the timer with period_us_.
  void StartTimer(TickTimer& timer);

  /// 新的一次运行：不再等待重新同步 / A new run: no resynchronisation pending.
  void ResetResync()
  {
    resync_pending_ = false;
    resync_after_us_ = 0U;
  }

 private:
  struct ApCsw
  {
    uint8_t ap;
    uint32_t base_csw;
    bool valid;
  };

  const ApCsw* FindCsw(uint8_t ap) const;

  // 以下在 MemAp::Lock 里读写 / The members below are read and written under
  // MemAp::Lock.
  SubItem items_[kMaxItems];
  uint16_t count_ = 0U;
  bool stop_on_full_ = false;
  ApCsw csw_[kMaxAps];
  uint8_t csw_count_ = 0U;

  // 以下只在采样的执行上下文里用 / The members below are used only in the sampling
  // context.
  bool resync_pending_ = false;
  uint64_t resync_after_us_ = 0U;

  // 采样往里放帧，DAP 执行上下文取帧和清空；它自己的自旋锁保护 / Sampling pushes frames,
  // the DAP execution context takes and clears them; guarded by its own spin lock.
  FrameRing ring_;
};

/// 采样配置、节拍和帧缓冲；Policy 是 TaskSampling 或 LoopSampling / Sampling
/// configuration, ticks and frame buffer; Policy is TaskSampling or LoopSampling.
template <class Policy>
class SamplerT : public SamplerCore
{
 public:
  SamplerT(MemAp& mem, KeepFn keep, void* keep_ctx) : SamplerCore(mem, keep, keep_ctx) {}

  /// @brief 用这组已校验过的条目开始采样：读各 AP 的 CSW，清空缓冲，节拍 0 马上采 /
  ///        Start sampling with these validated items: read every AP's CSW, clear the
  ///        buffer, and sample tick 0 at once.
  /// @param session 发起采样的会话编号，采样随它结束 / Number of the session that starts
  ///        sampling; sampling ends with it.
  /// @return kStatusOk；读 CSW 失败时 kStatusNoTarget / kStatusOk; kStatusNoTarget
  ///         when a CSW read fails.
  /// @note DAP 执行上下文，只在没有采样时调用 / DAP execution context, only while not
  ///       sampling.
  uint8_t Start(const SubItemList& items, uint32_t period_us, bool stop_on_full,
                uint32_t session)
  {
    const uint8_t status = Configure(items, period_us, stop_on_full, session);
    if (status != kStatusOk)
    {
      return status;
    }
    ClearFrames();
    policy_.RequestStart();
    return kStatusOk;
  }

  /// @brief 停止采样，并等正在读的一拍读完：返回后采样不再驱动 SWD / Stop sampling and
  ///        wait for a tick in progress: on return sampling no longer drives SWD.
  /// @note DAP 执行上下文；不能在持有 MemAp::Lock 时调用 / DAP execution context; must
  ///       not be called while holding a MemAp::Lock.
  void Stop()
  {
    policy_.RequestStop();
    // 正在读的一拍持有链路；取一次锁就等到它读完，之后的拍在锁内看到已停止而不读。
    // A tick in progress holds the link; taking the lock once waits for it, and later
    // ticks see the stop under the lock and do not read.
    MemAp::Lock lock(mem_);
  }

  /// @brief 是否正在采样 / Whether sampling is running.
  /// @note 任务策略：任何上下文；单循环策略：只在主循环里 / Task policy: any context;
  ///       single-loop policy: the main loop only.
  bool Active() const { return policy_.Active(); }

  /// @brief 节拍定时器中断调用：计数加 elapsed_ticks；任务策略下到达或越过要采的节拍时
  ///        记下这一刻并唤醒采样任务 / Called by the tick timer interrupt: adds
  ///        elapsed_ticks to the count; under the task policy, when the count reaches or
  ///        passes the next tick, notes the time and wakes the sampling task.
  /// @note 中断上下文；任务策略下平台的时基须能在中断里读 / Interrupt context; under the
  ///       task policy the platform's time base must be readable from an interrupt.
  void OnTickIsr(uint32_t elapsed_ticks = 1U)
  {
    if (elapsed_ticks != 0U)
    {
      policy_.OnTickIsr(elapsed_ticks);
    }
  }

  /// @brief 一条命令开始等执行：之后采样最多再读一帧就让路，直到命令都做完（单循环策略
  ///        下什么都不做）/ A command starts waiting to run: sampling then reads at most
  ///        one more frame and yields until every command has run (nothing under the
  ///        single-loop policy).
  /// @note 命令的任务里，排队等执行轮次之前 / The command's task, before it queues for an
  ///       execution turn.
  void CommandBegin() { policy_.CommandBegin(); }

  /// SUB_READ and status queries do not touch SWD; let sampling continue while they run.
  void CommandReadOnly() { policy_.CommandReadOnly(); }

  /// @brief 一条命令执行完；最后一条做完时唤醒让路的采样 / A command has run; the last
  ///        one wakes sampling that yielded.
  /// @note 命令的任务里 / The command's task.
  void CommandEnd() { policy_.CommandEnd(); }

  /// @brief 任务策略：采样任务的函数体，不返回 / Task policy: body of the sampling task;
  ///        never returns.
  /// @note 平台建的采样任务里调用；单循环策略下没有这个函数 / Call from the platform's
  ///       sampling task; not available under the single-loop policy.
  template <class P = Policy, std::enable_if_t<P::kRunsInTask, int> = 0>
  [[noreturn]] void RunTask(TickTimer& timer)
  {
    for (;;)
    {
      policy_.WaitForWork();
      while (Service(timer))
      {
      }
    }
  }

  /// @brief 单循环策略：主循环每圈调用 / Single-loop policy: called on every main-loop
  ///        pass.
  /// @note 与 DAP 执行在同一个线程；任务策略下没有这个函数 / Same thread as the DAP
  ///       execution; not available under the task policy.
  template <class P = Policy, std::enable_if_t<!P::kRunsInTask, int> = 0>
  void Poll(TickTimer& timer)
  {
    (void)Service(timer);
  }

 private:
  /// @brief 醒来一次：按需开关定时器，到了要采的节拍就采一拍，再定下一次唤醒的计数 /
  ///        One wake: start or stop the timer as needed, sample a tick when the one to
  ///        sample came, then set the count of the next wake.
  /// @return true 时要采的节拍已经到了，不等唤醒再来一次 / True when the next tick to
  ///         sample has already come, to run again without waiting.
  bool Service(TickTimer& timer)
  {
    MemAp::Lock lock(mem_);
    uint32_t count = policy_.Ticks();
    if (!policy_.Active())
    {
      StopTimer(timer);
      return false;
    }
    const uint32_t gen = policy_.Generation();
    if (gen != run_gen_)
    {
      // 新的一次采样：计数清零后再开定时器，节拍 0 马上采 / A new run: the count is
      // cleared before the timer starts, and tick 0 is sampled at once.
      StopTimer(timer);
      policy_.BeginRun();
      count = 0U;
      account_.Begin();
      ResetResync();
      run_gen_ = gen;
      StartTimer(timer);
    }

    const WakeDecision decision = policy_.Decide(account_, count);
    if (decision.parked)
    {
      policy_.Park(count);
      return false;
    }
    const TickAccount::Step& step = decision.step;
    if (step.sample)
    {
      const auto t0 = static_cast<uint32_t>(SamplerNowUs());
      SampleTick(step);
      const uint32_t busy = policy_.BusyUs(static_cast<uint32_t>(SamplerNowUs()), t0,
                                           step.tick, period_us_);
      account_.OnDone(policy_.Ticks(), busy, period_us_);
      policy_.FrameEnd();

      if (!keep_(keep_ctx_, session_))
      {
        policy_.Deactivate();
        StopTimer(timer);
        return false;
      }
    }
    return policy_.Arm(account_.Next());
  }

  Policy policy_;
};

/// 任务策略的采样 / Sampler under the task policy.
using TaskSampler = SamplerT<TaskSampling>;
/// 单循环策略的采样 / Sampler under the single-loop policy.
using LoopSampler = SamplerT<LoopSampling>;

}  // namespace XrDap::Vendor
