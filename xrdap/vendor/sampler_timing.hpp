// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace XrDap::Vendor
{
/// @brief 一拍采样的占用时间（µs，32 位回绕差）/ Busy time of one sampling tick (µs,
///        32-bit wrapping difference).
/// @param end 这一拍做完的时刻 / Time the tick finished.
/// @param begin 开始读帧的时刻 / Time the frame read began.
/// @param woke_this_tick 这一拍是由节拍中断唤醒的 / The tick was woken by its interrupt.
/// @param isr 节拍中断唤醒的时刻 / Time of the tick interrupt's wake.
/// @param period 采样周期 / Sampling period.
/// @return end 减去起点，起点是 isr 或 begin / end minus the start, which is isr or begin.
/// @note 占用从节拍中断唤醒的那一刻算起，包括唤醒、切换任务和取链路锁的时间：只量读帧
///       的时间会少算，周期接近一帧时留给命令的间隙就只是纸面上的。没有这一拍的中断时刻
///       时（开始时的节拍 0、不等唤醒接着做的一拍）从读帧开始算。单循环里中断到这里之间
///       是主循环在做完手上的命令，不是采样的开销，所以只有任务策略（TaskSampling）用本函数，
///       单循环策略（LoopSampling）直接从读帧开始算 / Busy
///       time counts from the moment the tick interrupt woke us, including the wake, the
///       task switch and taking the link lock: timing only the frame read undercounts, and
///       with a period close to one frame the gap left to commands exists only on paper.
///       Without this tick's interrupt time (tick 0 at the start, a tick done without
///       waiting for a wake) it counts from the start of the frame read. In a single loop
///       the time from the interrupt to here is the main loop finishing its current
///       command, not a sampling cost, so only the task policy (TaskSampling) calls this
///       function; the single-loop policy (LoopSampling) counts from the start of the frame
///       read.
inline uint32_t FrameBusyUs(uint32_t end, uint32_t begin, bool woke_this_tick,
                            uint32_t isr, uint32_t period)
{
  const uint32_t start = woke_this_tick && begin - isr <= period ? isr : begin;
  return end - start;
}
}  // namespace XrDap::Vendor
