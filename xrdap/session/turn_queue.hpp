// SPDX-License-Identifier: Apache-2.0
#pragma once

// 执行器的轮次：同一时间只有一条命令在执行，等着执行的按到达的先后排队。持有者离开时
// 轮次直接交给队首，不会被刚到的或优先级更高的任务抢走，所以每条传输等的只是排在前面的
// 命令执行的时间，与别的传输什么时候空闲无关。只记录排队，阻塞和唤醒由执行器用信号量
// 完成。
//
// The executor's turns: one command runs at a time, and those waiting to run queue in
// arrival order. When the holder leaves, the turn goes straight to the head of the queue
// and cannot be taken by a task that just arrived or has a higher priority, so a
// transport waits only for the commands queued ahead of it, never for another
// transport's idle time. This only keeps the queue; the executor blocks and wakes with
// semaphores.

#include <cstdint>

#include "xrdap/session/session_owner.hpp"
#include "xrdap_port.hpp"

namespace XrDap
{

/// 执行器的先到先得轮次 / First-come first-served turns of the executor.
/// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
class TurnQueue
{
 public:
  /// 最多同时排队的任务数 / Most tasks queued at once.
  static constexpr uint8_t kMaxWaiters = 8U;

  /// 到达的结果 / Outcome of an arrival.
  enum class Arrival : uint8_t
  {
    NOW,     ///< 轮次空闲，已经拿到 / The turn was free and is now held.
    QUEUED,  ///< 已排队，等交接 / Queued; wait for the hand-over.
    FULL,    ///< 队列满，稍后再来 / Queue full; try again later.
  };

  /// @brief 传输 t 的一个任务要执行 / A task of transport t wants to run.
  Arrival Arrive(Transport t);

  /// @brief 持有者离开 / The holder leaves.
  /// @return 拿到轮次的队首传输；没人排队时 NONE，轮次变为空闲 / The transport at
  ///         the head of the queue, which now holds the turn; NONE when nobody waits,
  ///         and the turn becomes free.
  Transport Leave();

 private:
  Port::SpinLock lock_;
  bool busy_ = false;
  Transport queue_[kMaxWaiters] = {};
  uint8_t head_ = 0U;
  uint8_t count_ = 0U;
};

}  // namespace XrDap
