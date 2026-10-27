// SPDX-License-Identifier: Apache-2.0
#pragma once

// 采样节拍的账目（docs/protocol.md 3.5）。节拍定时器是唯一的时钟：它的中断每个周期把
// 节拍计数加上实际跨过的周期数。采样醒来时读计数 c，没读过的最新节拍就是
// c；从上一次采到的节拍到 c 之间 没采的节拍都算跳过。来不及时不补做。
// 每帧之后定下一个要采的节拍：它晚于采完时的计数，并且从这一帧的节拍算起至少过了
// “这一帧的占用 + 间隙”，间隙为 max(kMinGapUs, 周期/8, 占用/4)。所以两帧之间同一核上
// 的别的任务至少有这段间隙（减去唤醒的延迟），约占这个核 1/5 以上的时间，与周期无关。
// 中断只在计数到达下一个要采的节拍时唤醒采样（TickWakes），采样任务因此只为要采的节拍
// 醒来，不会为跳过的节拍一拍拍空转。
//
// Sampling tick accounting (docs/protocol.md 3.5). The tick timer is the only clock: its
// interrupt adds the number of elapsed periods to the tick count. When sampling wakes it
// reads the count c, and the newest tick not yet read is c; every tick between the last
// one sampled and c counts as skipped. Late ticks are not made up. After each frame the
// next tick to sample is chosen: it is after the count when the frame ended, and at least
// "this frame's busy time + gap" after this frame's tick, with gap = max(kMinGapUs,
// period/8, busy/4). Other tasks on the same core therefore get at least that gap between
// two frames (less the wake latency), about 1/5 of the core or more, whatever the period.
// The interrupt wakes sampling only when the count reaches the next tick to sample
// (TickWakes), so the sampling task wakes only for ticks it samples and does not spin
// through skipped ones.

#include <cstdint>

namespace XrDap::Vendor
{

/// @brief 计数 a 是否已到达或越过 b，计数回绕也成立 / Whether count a has reached or
///        passed b, also across wraparound.
inline bool TickDue(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }

/// @brief 中断把计数加到 count 后是否唤醒采样：只在到达下一个要采的节拍时 / Whether the
///        interrupt wakes sampling after raising the count to `count`: when it
///        reaches or passes the next tick to sample.
/// @note 中断上下文 / Interrupt context.
inline bool TickWakes(uint32_t count, uint32_t wake_at)
{
  return TickDue(count, wake_at);
}

/// 节拍账目 / Tick accounting.
/// @note 只在采样的执行上下文里用（采样任务或单循环的主循环）/ Used only in the
///       sampling context (the sampling task or the single main loop).
class TickAccount
{
 public:
  /// 两帧之间至少留给别的任务的时间 / Least time between two frames left to other
  /// tasks.
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

  /// @brief 刚采完 OnWake 给的节拍，定下一个要采的节拍 / The tick from OnWake has just
  ///        been sampled; choose the next tick to sample.
  /// @param count_after 采完时的计数 / Tick count when done.
  /// @param busy_us 这一拍占用的时间 / Time the tick took.
  /// @param period_us 周期 / Period.
  void OnDone(uint32_t count_after, uint32_t busy_us, uint32_t period_us);

  /// @brief 计数为 count 时是否有节拍要采（OnWake 会采）/ Whether a tick is due at
  ///        count (OnWake would sample it).
  bool Due(uint32_t count) const;

  /// @brief 放弃到 count 为止到期的节拍，下一个要采的是 count + 1；放弃的在下次采时计入
  ///        跳拍 / Give up the ticks due up to count; the next to sample is count + 1,
  ///        and the ones given up count as skipped at the next sample.
  void Defer(uint32_t count);

  /// @brief 下一个要采的节拍 / Next tick to sample.
  uint32_t Next() const { return next_; }

 private:
  bool has_last_ = false;
  uint32_t last_ = 0U;
  uint32_t next_ = 0U;
};

}  // namespace XrDap::Vendor
