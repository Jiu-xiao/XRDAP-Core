// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针端周期采样（docs/protocol.md 3.5）。节拍定时器（TickTimer，平台实现）是唯一的
// 时钟：它的中断调用 OnTickIsr()，把节拍计数加上实际跨过的周期数；执行上下文调用 Sample()
// 时按 TickAccount 决定采哪一拍、跳过了几拍，时基只用来给帧打时间戳。每一拍在一个 MemAp::Access
// 里完成：确认这次运行还在（代号没变）、读到期的条目、把帧放进缓冲，所以停止后换了配置也
// 不会混进旧的帧。采样和命令在同一个执行上下文里一件一件做（executor.hpp），所以没有锁、
// 没有让路、没有第二个任务；要不要叫醒执行上下文由平台给的 WakeFn 决定。
//
// Probe-side periodic sampling (docs/protocol.md 3.5). The tick timer (TickTimer,
// implemented by the platform) is the only clock: its interrupt calls OnTickIsr(), which
// adds elapsed_ticks to the tick count. When the execution context calls Sample(),
// TickAccount decides which tick to sample and how many were skipped; the time base only
// stamps frames. Every tick completes inside one MemAp::Access: it checks the run is still current
// (same generation), reads the due items and puts the frame into the buffer, so a frame of
// an old run cannot land after a stop and a new configuration. Sampling and commands run
// one after the other in the same execution context (executor.hpp), so there is no lock,
// no yielding and no second task; whether the execution context is woken is up to the
// WakeFn the platform supplies.

#include <atomic>
#include <cstdint>

#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/protocol.hpp"
#include "xrdap/vendor/sampling.hpp"

namespace XrDap::Vendor
{

/// @brief 节拍定时器，平台实现 / Tick timer, implemented by the platform.
/// @note Start 和 Stop 都在执行上下文里调用，定时器中断因此可以分配在执行上下文所在的
///       核上。中断里调用 DapVendor::OnTickIsr() / Start and Stop are both called in the
///       execution context, so the timer interrupt can be allocated on its core. Its
///       interrupt calls DapVendor::OnTickIsr().
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

/// 采样的配置、帧的组装和帧缓冲 / Sampling configuration, frame assembly and the frame
/// buffer.
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
  /// @note 帧 = 节拍号 u32 + 时间戳 u48 + 状态位图 + 各到期条目的值；在一个 MemAp::Access
  ///       里调用 / Frame = tick u32 + timestamp u48 + status bitmap + the due items'
  ///       values; called inside a MemAp::Access.
  void SampleTick(const TickAccount::Step& step);

  MemAp& mem_;
  KeepFn keep_;
  void* keep_ctx_;

  // 以下在 MemAp::Access 里读写 / The members below are read and written inside a
  // MemAp::Access.
  uint32_t period_us_ = 0U;
  uint32_t session_ = 0U;

  // 以下只在执行上下文里用 / The members below are used only in the execution
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

  // 以下在 MemAp::Access 里读写 / The members below are read and written inside a
  // MemAp::Access.
  SubItem items_[kMaxItems];
  uint16_t count_ = 0U;
  bool stop_on_full_ = false;
  ApCsw csw_[kMaxAps];
  uint8_t csw_count_ = 0U;

  // 以下只在执行上下文里用 / The members below are used only in the execution
  // context.
  bool resync_pending_ = false;
  uint64_t resync_after_us_ = 0U;

  // 采样往里放帧，SUB_READ 取帧，都在执行上下文里 / Sampling pushes frames and SUB_READ
  // takes them, both in the execution context.
  FrameRing ring_;
};

/// 平台给的函数：节拍到了，叫醒执行上下文 / Function the platform supplies to wake the
/// execution context when a tick is due.
using WakeFn = void (*)(void* ctx, bool in_isr);

/// 采样配置、节拍和帧缓冲 / Sampling configuration, ticks and frame buffer.
/// @note 除 OnTickIsr 外都只在执行上下文里调用 / Everything but OnTickIsr is called in the
///       execution context only.
class Sampler : public SamplerCore
{
 public:
  Sampler(MemAp& mem, KeepFn keep, void* keep_ctx) : SamplerCore(mem, keep, keep_ctx) {}

  /// @brief 用这组已校验过的条目开始采样：读各 AP 的 CSW，清空缓冲，节拍 0 马上采 /
  ///        Start sampling with these validated items: read every AP's CSW, clear the
  ///        buffer, and sample tick 0 at once.
  /// @param session 发起采样的连接的代数，采样随它结束 / Epoch of the connection that
  ///        starts sampling; sampling ends with it.
  /// @return kStatusOk；读 CSW 失败时 kStatusNoTarget / kStatusOk; kStatusNoTarget
  ///         when a CSW read fails.
  /// @note 只在没有采样时调用 / Only while not sampling.
  uint8_t Start(const SubItemList& items, uint32_t period_us, bool stop_on_full,
                uint32_t session)
  {
    const uint8_t status = Configure(items, period_us, stop_on_full, session);
    if (status != kStatusOk)
    {
      return status;
    }
    ClearFrames();
    ++gen_;
    active_.store(1U, std::memory_order_relaxed);
    return kStatusOk;
  }

  /// 停止采样；下一次 Sample() 停定时器 / Stop sampling; the next Sample() stops the timer.
  void Stop() { active_.store(0U, std::memory_order_relaxed); }

  /// @brief 是否正在采样 / Whether sampling is running.
  /// @note 任何上下文 / Any context.
  bool Active() const { return active_.load(std::memory_order_relaxed) != 0U; }

  /// @brief 设置叫醒执行上下文的函数；不设置时节拍中断只计数（单循环的探针）/ Set the
  ///        function that wakes the execution context; without it the tick interrupt only
  ///        counts (a single-loop probe).
  /// @note 初始化时调用 / Call at initialisation.
  void SetWake(WakeFn fn, void* ctx)
  {
    wake_ = fn;
    wake_ctx_ = ctx;
  }

  /// @brief 节拍定时器中断调用：计数加 elapsed_ticks；设了 WakeFn 时，计数到达或越过要采的
  ///        节拍就叫醒执行上下文一次 / Called by the tick timer interrupt: adds
  ///        elapsed_ticks to the count; with a WakeFn set, wakes the execution context once
  ///        when the count reaches or passes the next tick to sample.
  /// @note 中断上下文。期限被消耗一次后，执行上下文重新装填前不会再唤醒：一帧很长时，反复的
  ///       中断不能攒下唤醒 / Interrupt context. Once the deadline is consumed there is no
  ///       further wake until the execution context rearms it: while a long frame runs,
  ///       repeated interrupts must not accumulate wakes.
  /// @note 加计数和读唤醒计数用 seq_cst，与 Arm 里装填唤醒计数和读计数配对：两边各写一个量
  ///       再读对方的，seq_cst 的全序保证至少一边看得到对方，不会既没唤醒、又没发现计数已
  ///       到 / Raising the count and reading the wake count are seq_cst, paired with
  ///       rearming the wake count and reading the count in Arm: each side writes one
  ///       variable and then reads the other's, and the total order of seq_cst guarantees
  ///       at least one side sees the other, so a due tick cannot be both unwoken and
  ///       unnoticed.
  void OnTickIsr(uint32_t elapsed_ticks = 1U)
  {
    if (elapsed_ticks == 0U)
    {
      return;
    }
    const uint32_t count =
        ticks_.fetch_add(elapsed_ticks, std::memory_order_seq_cst) + elapsed_ticks;
    if (wake_ == nullptr)
    {
      return;
    }
    uint32_t wake_at = wake_at_.load(std::memory_order_seq_cst);
    if (TickWakes(count, wake_at) &&
        wake_at_.compare_exchange_strong(wake_at, count + kParkedTicks,
                                         std::memory_order_relaxed))
    {
      wake_(wake_ctx_, true);
    }
  }

  /// @brief 执行上下文每一轮调用：按需开关定时器，到了要采的节拍就采一帧，再定下一次唤醒的
  ///        计数 / Called once per round of the execution context: start or stop the timer
  ///        as needed, sample one frame when the tick to sample came, then set the count of
  ///        the next wake.
  /// @return true 时下一个要采的节拍已经到了，不等唤醒再来一次 / True when the next tick to
  ///         sample has already come, to run again without waiting.
  bool Sample(TickTimer& timer)
  {
    MemAp::Access access(mem_);
    uint32_t count = Ticks();
    if (!Active())
    {
      StopTimer(timer);
      return false;
    }
    if (gen_ != run_gen_)
    {
      // 新的一次采样：计数清零后再开定时器，节拍 0 马上采 / A new run: the count is
      // cleared before the timer starts, and tick 0 is sampled at once.
      StopTimer(timer);
      ticks_.store(0U, std::memory_order_relaxed);
      count = 0U;
      account_.Begin();
      ResetResync();
      run_gen_ = gen_;
      StartTimer(timer);
    }

    const TickAccount::Step step = account_.OnWake(count);
    if (step.sample)
    {
      const auto t0 = static_cast<uint32_t>(SamplerNowUs());
      SampleTick(step);
      const auto busy = static_cast<uint32_t>(SamplerNowUs()) - t0;
      account_.OnDone(Ticks(), busy, period_us_);

      if (!keep_(keep_ctx_, session_))
      {
        active_.store(0U, std::memory_order_relaxed);
        StopTimer(timer);
        return false;
      }
    }
    return Arm(account_.Next());
  }

  /// 是否该进 Sample：新的一次采样要开定时器，停了的要关定时器，运行中的看节拍到了没有 /
  /// Whether to enter Sample: a new run has to start the timer, a stopped one has to stop it,
  /// a running one looks at whether its tick came.
  bool Due() const
  {
    if (!Active())
    {
      return timer_on_;
    }
    return gen_ != run_gen_ || TickDue(Ticks(), account_.Next());
  }

 private:
  /// 节拍计数 / The tick count.
  uint32_t Ticks() const { return ticks_.load(std::memory_order_relaxed); }

  /// @brief 定下中断下一次唤醒的计数，并告诉调用方要采的节拍是否已经到了 / Set the count
  ///        of the interrupt's next wake, and tell the caller whether the tick to sample
  ///        has already come.
  /// @note 先装填、再读计数，都是 seq_cst，与 OnTickIsr 配对 / Rearm, then read the count,
  ///       both seq_cst, paired with OnTickIsr.
  bool Arm(uint32_t next)
  {
    wake_at_.store(next, std::memory_order_seq_cst);
    return TickDue(ticks_.load(std::memory_order_seq_cst), next);
  }

  /// 唤醒计数放到很远处，实际上不会到 / The wake count is put far away, where it is never
  /// reached in practice.
  static constexpr uint32_t kParkedTicks = 0x40000000U;

  std::atomic<uint32_t> active_{0U};   ///< 在运行吗 / Running.
  uint32_t gen_ = 0U;                  ///< 运行代号，Start 加一 / Run generation, +1 per Start.
  std::atomic<uint32_t> ticks_{0U};    ///< 节拍计数：中断加，执行上下文读和清零 / Tick count: the interrupt adds, the execution context reads and clears.
  std::atomic<uint32_t> wake_at_{0U};  ///< 中断唤醒的计数：执行上下文装填，中断消耗 / Interrupt wake count: armed by the execution context, consumed by the interrupt.
  WakeFn wake_ = nullptr;
  void* wake_ctx_ = nullptr;
};

}  // namespace XrDap::Vendor
