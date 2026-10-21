// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针端周期采样。第 k 拍的目标时刻为 start + k × period，不累计漂移；来不及时跳过节拍，
// 不补做。两种运行方式，节拍逻辑相同：
//   单循环（没有 RTOS）：主循环在两条 DAP 命令之间调用 Poll()，到期就读这一拍。一拍读完
//     必定回到主循环处理一条命令，采样不会饿死 DAP 命令。
//   任务：平台建一个任务运行 RunTask()。节拍定时器（TickTimer，平台实现）每拍在中断里
//     调用 WakeFromIsr() 唤醒任务，任务在节拍之间阻塞，所以它可以是所在核上优先级最高的
//     任务：节拍准时，别的任务（DAP 命令、网络、串口、无线）在节拍之间运行。每个周期至少
//     给别的任务留 1/8 周期、最少 kMinGapUs，否则跳一拍。
//
// Probe-side periodic sampling. Tick k is due at start + k × period with no accumulated
// drift; a late tick is skipped, not made up. Two ways to run it, with the same tick
// logic:
//   Single loop (no RTOS): the main loop calls Poll() between DAP commands and reads the
//     tick once it is due. After every tick control returns to the main loop, which
//     handles one command, so sampling never starves DAP commands.
//   Task: the platform creates a task that runs RunTask(). A tick timer (TickTimer,
//     implemented by the platform) calls WakeFromIsr() from its interrupt on every tick;
//     the task blocks in between, so it can be the highest-priority task on its core:
//     ticks stay on time and other tasks (DAP commands, network, serial, radio) run in
//     the gaps. Each period leaves at least 1/8 of the period, and no less than
//     kMinGapUs, to other tasks; otherwise one more tick is skipped.

#include <atomic>
#include <cstdint>

#include "frame_ring.hpp"
#include "mem_ap.hpp"
#include "semaphore.hpp"
#include "vendor_protocol.hpp"

namespace XrDap::Vendor
{

/// @brief 节拍定时器，平台实现（RunTask 用）/ Tick timer, implemented by the platform
///        (used by RunTask).
/// @note Start 和 Stop 都在采样任务里调用，定时器中断因此可以分配在采样所在的核上 /
///       Start and Stop are both called from the sampling task, so the timer interrupt
///       can be allocated on the sampling core.
class TickTimer
{
 public:
  /// @brief 从现在起在 start_us + k × period_us 时刻调用 Sampler::WakeFromIsr() /
  ///        Call Sampler::WakeFromIsr() at start_us + k × period_us from now on.
  /// @param start_us 第 0 拍的时刻，与 LibXR::Timebase 同一时基 / Time of tick 0 on the
  ///        LibXR::Timebase clock.
  /// @return 定时器做不到（例如周期超出计数范围）时返回 false，任务改为每 1 ms 醒一次 /
  ///         False when the timer cannot do it (for example the period exceeds its
  ///         counter); the task then wakes every 1 ms instead.
  virtual bool Start(uint64_t start_us, uint32_t period_us) = 0;
  /// 停止中断 / Stop interrupting.
  virtual void Stop() = 0;

 protected:
  ~TickTimer() = default;
};

/// 采样配置、节拍调度和帧缓冲 / Sampling configuration, tick scheduling and frame buffer.
class Sampler
{
 public:
  /// @brief 每拍末尾调用，返回 false 时采样停止 / Called at the end of every tick;
  ///        sampling stops when it returns false.
  using KeepFn = bool (*)(void* ctx);

  /// SUB_SET 的估计结果 / Estimate reported by SUB_SET.
  struct Estimate
  {
    /// 一帧的最大字节数 / Maximum bytes of one frame.
    uint16_t max_frame = 0;
    /// 估计的单帧耗时（微秒）/ Estimated frame time in microseconds.
    uint32_t frame_us = 0;
  };

  Sampler(MemAp& mem, KeepFn keep, void* keep_ctx)
      : mem_(mem), keep_(keep), keep_ctx_(keep_ctx)
  {
  }

  /// @brief 实测这组条目一帧的耗时，成功时保存配置（SUB_SET）；失败时之前的配置和缓冲
  ///        不变 / Measure the frame time of these items and store the configuration on
  ///        success (SUB_SET); on failure the previous configuration and buffer stay.
  /// @return kStatusOk；估计耗时超过周期或帧太大时 kStatusResource；SWD 访问失败时
  ///         kStatusNoTarget / kStatusOk; kStatusResource when the estimate exceeds the
  ///         period or the frame is too big; kStatusNoTarget when SWD access fails.
  uint8_t Configure(const SubItemList& items, uint32_t period_us, bool stop_on_full,
                    Estimate& est);
  /// 是否有有效配置 / Whether a valid configuration exists.
  bool Configured() const { return configured_; }
  /// 是否正在采样 / Whether sampling is running.
  bool Active() const { return active_.load(std::memory_order_acquire); }
  /// 最近一次标定的单次访问耗时（微秒），0 表示还没有标定 / Last calibrated per-access
  /// time in microseconds; 0 before any calibration.
  uint32_t AccessUs() const { return access_us_; }

  /// 读好各 AP 的 CSW 后开始采样；失败时返回 false / Read every AP's CSW, then start;
  /// false on failure.
  bool Start();
  /// @brief 停止采样，并等正在读的一拍读完：返回后采样不再驱动 SWD / Stop sampling and
  ///        wait for a tick in progress: on return sampling no longer drives SWD.
  /// @note 不能在持有 MemAp::Lock 时调用 / Must not be called while holding a
  ///       MemAp::Lock.
  void Stop();
  /// 清空帧缓冲和计数 / Clear the frame buffer and the counters.
  void ClearFrames() { ring_.Reset(); }
  /// 取出整帧（SUB_READ）/ Take whole frames (SUB_READ).
  FrameRing::Taken TakeFrames(uint8_t* dst, uint32_t limit)
  {
    return ring_.Take(dst, limit);
  }

  /// 单循环：主循环每圈调用，到期就读一拍 / Single loop: called on every main-loop pass;
  /// reads a tick once it is due.
  void Poll();

  /// @brief 任务：采样任务的函数体，不返回 / Task: body of the sampling task; never
  ///        returns.
  /// @param timer 节拍定时器，可为空（每 1 ms 醒一次）/ Tick timer; may be null (wake
  ///        every 1 ms).
  [[noreturn]] void RunTask(TickTimer* timer);
  /// 任务：节拍定时器中断调用 / Task: called by the tick timer interrupt.
  void WakeFromIsr() { wake_.PostFromCallback(true); }

 private:
  struct ApCsw
  {
    uint8_t ap;
    uint32_t base_csw;
    bool valid;
  };

  // 任务方式：被唤醒后离节拍不到这么久时忙等，补上中断和切换的延迟
  static constexpr uint32_t kSpinUs = 30U;
  // 任务方式：每个周期至少留给别的任务的时间
  static constexpr uint32_t kMinGapUs = 15U;

  bool MeasureFrame(const SubItemList& items, uint32_t& us);
  const ApCsw* FindCsw(uint8_t ap) const;
  void SampleTick(const uint16_t* order, uint16_t due, uint8_t* frame, uint16_t len);
  uint64_t Tick(uint64_t k, uint64_t start, uint32_t period, bool leave_gap);
  void SleepUntil(uint64_t deadline_us, bool timed);
  void Run(uint32_t gen, TickTimer* timer);

  MemAp& mem_;
  KeepFn keep_;
  void* keep_ctx_;

  std::atomic<bool> active_{false};
  std::atomic<uint32_t> gen_{0};
  LibXR::Semaphore wake_{0};

  SubItem items_[kMaxItems];
  uint16_t count_ = 0U;
  uint32_t period_us_ = 0U;
  bool stop_on_full_ = false;
  bool configured_ = false;
  uint32_t access_us_ = 0U;
  ApCsw csw_[kMaxAps];
  uint8_t csw_count_ = 0U;
  uint64_t start_us_ = 0U;
  uint64_t poll_tick_ = 0U;

  FrameRing ring_;
};

}  // namespace XrDap::Vendor
