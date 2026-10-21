// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针端周期采样（docs/protocol.md 3.5）。节拍定时器（TickTimer，平台实现）是唯一的
// 时钟：它的中断调用 OnTickIsr()，只把节拍计数加 1 并唤醒采样。采样醒来后按 TickAccount
// 决定采哪一拍、跳过了几拍；时基只用来给帧打时间戳。两种运行方式，逻辑相同：
//   任务：平台建一个任务运行
//   RunTask()，任务在信号量上无超时地阻塞，停止也经信号量唤醒它。
//     任务应为 SWD 所在核上优先级最高的任务。
//   单循环（没有 RTOS）：主循环在两条 DAP 命令之间调用 Poll()，计数变了就采一拍。
// 每一拍在 MemAp::Lock 里完成：确认这次运行还在（代号没变）、读到期的条目、把帧放进
// 缓冲，所以停止后换了配置也不会混进旧的帧。
//
// Probe-side periodic sampling (docs/protocol.md 3.5). The tick timer (TickTimer,
// implemented by the platform) is the only clock: its interrupt calls OnTickIsr(), which
// only adds 1 to the tick count and wakes sampling. On waking, TickAccount decides which
// tick to sample and how many were skipped; the time base only stamps frames. Two ways
// to run it, with the same logic:
//   Task: the platform creates a task that runs RunTask(); it blocks on a semaphore with
//     no timeout, and a stop wakes it through the semaphore too. The task should have
//     the highest priority on the core that drives SWD.
//   Single loop (no RTOS): the main loop calls Poll() between DAP commands and samples a
//     tick when the count changed.
// Every tick completes under MemAp::Lock: it checks the run is still current (same
// generation), reads the due items and puts the frame into the buffer, so a frame of an
// old run cannot land after a stop and a new configuration.

#include <atomic>
#include <cstdint>

#include "semaphore.hpp"
#include "xrdap/vendor/frame_ring.hpp"
#include "xrdap/vendor/mem_ap.hpp"
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

/// 采样配置、节拍和帧缓冲 / Sampling configuration, ticks and frame buffer.
class Sampler
{
 public:
  /// @brief 每拍末尾调用，返回 false 时采样停止 / Called at the end of every tick;
  ///        sampling stops when it returns false.
  using KeepFn = bool (*)(void* ctx);

  Sampler(MemAp& mem, KeepFn keep, void* keep_ctx)
      : mem_(mem), keep_(keep), keep_ctx_(keep_ctx)
  {
  }

  /// @brief 用这组已校验过的条目开始采样：读各 AP 的 CSW，清空缓冲，节拍 0 马上采 /
  ///        Start sampling with these validated items: read every AP's CSW, clear the
  ///        buffer, and sample tick 0 at once.
  /// @return kStatusOk；读 CSW 失败时 kStatusNoTarget / kStatusOk; kStatusNoTarget
  ///         when a CSW read fails.
  /// @note DAP 执行上下文，只在没有采样时调用 / DAP execution context, only while not
  ///       sampling.
  uint8_t Start(const SubItemList& items, uint32_t period_us, bool stop_on_full);

  /// @brief 停止采样，并等正在读的一拍读完：返回后采样不再驱动 SWD / Stop sampling and
  ///        wait for a tick in progress: on return sampling no longer drives SWD.
  /// @note DAP 执行上下文；不能在持有 MemAp::Lock 时调用 / DAP execution context; must
  ///       not be called while holding a MemAp::Lock.
  void Stop();

  /// @brief 是否正在采样 / Whether sampling is running.
  /// @note 任何上下文 / Any context.
  bool Active() const { return active_.load(std::memory_order_acquire); }

  /// @brief 清空帧缓冲和计数 / Clear the frame buffer and the counters.
  /// @note DAP 执行上下文 / DAP execution context.
  void ClearFrames() { ring_.Reset(); }

  /// @brief 取出整帧（SUB_READ）/ Take whole frames (SUB_READ).
  /// @note DAP 执行上下文 / DAP execution context.
  FrameRing::Taken TakeFrames(uint8_t* dst, uint32_t limit)
  {
    return ring_.Take(dst, limit);
  }

  /// @brief 节拍定时器中断调用：计数加 1，任务方式下唤醒采样任务 / Called by the tick
  ///        timer interrupt: adds 1 to the count and wakes the sampling task in task
  ///        mode.
  /// @note 中断上下文 / Interrupt context.
  void OnTickIsr()
  {
    ticks_.fetch_add(1U, std::memory_order_relaxed);
    if (task_mode_.load(std::memory_order_relaxed))
    {
      wake_.PostFromCallback(true);
    }
  }

  /// @brief 任务方式：采样任务的函数体，不返回 / Task mode: body of the sampling task;
  ///        never returns.
  /// @note 平台建的采样任务里调用 / Call from the platform's sampling task.
  [[noreturn]] void RunTask(TickTimer& timer);

  /// @brief 单循环：主循环每圈调用 / Single loop: called on every main-loop pass.
  /// @note 与 DAP 执行在同一个线程 / Same thread as the DAP execution.
  void Poll(TickTimer& timer) { Service(timer); }

 private:
  struct ApCsw
  {
    uint8_t ap;
    uint32_t base_csw;
    bool valid;
  };

  const ApCsw* FindCsw(uint8_t ap) const;
  void Service(TickTimer& timer);
  void SampleTick(const TickAccount::Step& step);

  MemAp& mem_;
  KeepFn keep_;
  void* keep_ctx_;

  std::atomic<bool> active_{false};
  std::atomic<uint32_t> gen_{0U};
  std::atomic<uint32_t> ticks_{0U};
  std::atomic<bool> task_mode_{false};
  LibXR::Semaphore wake_{0};

  // 以下在 MemAp::Lock 里读写 / The members below are read and written under
  // MemAp::Lock.
  SubItem items_[kMaxItems];
  uint16_t count_ = 0U;
  uint32_t period_us_ = 0U;
  bool stop_on_full_ = false;
  ApCsw csw_[kMaxAps];
  uint8_t csw_count_ = 0U;

  // 以下只在采样的执行上下文里用 / The members below are used only in the sampling
  // context.
  uint32_t run_gen_ = 0U;
  bool timer_on_ = false;
  TickAccount account_;

  FrameRing ring_;
};

}  // namespace XrDap::Vendor
