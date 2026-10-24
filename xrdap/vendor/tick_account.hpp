// SPDX-License-Identifier: Apache-2.0
#pragma once

// 采样节拍的账目（docs/protocol.md 3.5）。节拍定时器是唯一的时钟：它的中断每个周期把
// 节拍计数加 1。采样醒来时读计数 c，没读过的最新节拍就是 c；从上一次采到的节拍到 c 之间
// 没采的节拍都算跳过。来不及时不补做：采样过程中计数又走了，就等下一个节拍再采。一拍
// 占用超过周期减去 max(周期/8, kMinGapUs) 时再让出一拍，给同一核上的其他任务留出时间。
//
// Sampling tick accounting (docs/protocol.md 3.5). The tick timer is the only clock: its
// interrupt adds 1 to the tick count every period. When sampling wakes it reads the count
// c, and the newest tick not yet read is c; every tick between the last one sampled and
// c counts as skipped. Late ticks are not made up: if the count moved on while sampling,
// the next sample waits for the next tick. When one tick is busy for more than the period
// minus max(period/8, kMinGapUs), one more tick is given up so other tasks on the same
// core get time.

#include <cstdint>

namespace XrDap::Vendor
{

/// 节拍账目 / Tick accounting.
/// @note 只在采样的执行上下文里用（采样任务或单循环的主循环）/ Used only in the
///       sampling context (the sampling task or the single main loop).
class TickAccount
{
 public:
  /// 每个周期至少留给别的任务的时间 / Least time per period left to other tasks.
  static constexpr uint32_t kMinGapUs = 15U;

  /// 醒来时的决定 / Decision on a wake.
  struct Step
  {
    bool sample;       ///< 采这一拍 / Sample this tick.
    uint32_t tick;     ///< 节拍号 / Tick number.
    uint32_t skipped;  ///< 这次新跳过的节拍数 / Ticks newly skipped.
  };

  /// 一次采样开始：节拍 0 马上采 / A sampling run starts: tick 0 is sampled at once.
  void Begin();

  /// @brief 醒来，计数为 count / Woken with the tick count at `count`.
  Step OnWake(uint32_t count);

  /// @brief 刚采完 OnWake 给的节拍 / The tick from OnWake has just been sampled.
  /// @param count_after 采完时的计数 / Tick count when done.
  /// @param busy_us 这一拍占用的时间 / Time the tick took.
  /// @param period_us 周期 / Period.
  void OnDone(uint32_t count_after, uint32_t busy_us, uint32_t period_us);

 private:
  bool has_last_ = false;
  uint32_t last_ = 0U;
  uint32_t next_ = 0U;
};

}  // namespace XrDap::Vendor
