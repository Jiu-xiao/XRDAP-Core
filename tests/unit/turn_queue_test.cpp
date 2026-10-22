// SPDX-License-Identifier: Apache-2.0
// 执行轮次。判据：用户的要求（2026-10-22）——每条传输都能公平地执行，DAP_Info 和“忙”回复
// 不取决于别的传输什么时候空闲：等着的按到达先后拿到轮次，持有者离开时直接交给队首，
// 刚到的不能插队。
//
// Executor turns. Oracle: the user's requirement (2026-10-22) that every transport gets
// fair access and DAP_Info and busy replies never depend on another transport's idle
// time: waiters get the turn in arrival order, the holder hands it straight to the head
// of the queue, and a newcomer cannot jump the queue.
#include "xrdap/session/turn_queue.hpp"

#include "check.hpp"

using XrDap::Transport;
using XrDap::TurnQueue;

TEST(FreeTurnIsTakenAtOnce)
{
  TurnQueue q;
  CHECK(q.Arrive(Transport::TCP) == TurnQueue::Arrival::NOW);
  CHECK(q.Leave() == Transport::NONE);
  CHECK(q.Arrive(Transport::USB) == TurnQueue::Arrival::NOW);
}

TEST(WaitersAreServedInArrivalOrder)
{
  TurnQueue q;
  CHECK(q.Arrive(Transport::USB) == TurnQueue::Arrival::NOW);
  CHECK(q.Arrive(Transport::TCP) == TurnQueue::Arrival::QUEUED);
  CHECK(q.Arrive(Transport::SERIAL) == TurnQueue::Arrival::QUEUED);
  CHECK(q.Leave() == Transport::TCP);
  CHECK(q.Leave() == Transport::SERIAL);
  CHECK(q.Leave() == Transport::NONE);
}

TEST(HolderCannotTakeItBackAheadOfAWaiter)
{
  // USB 执行完马上又来一条：TCP 已在排队，轮次交给 TCP，USB 排在它后面 / USB comes back
  // right after its command: TCP is already queued, so the turn goes to TCP and USB
  // queues behind it.
  TurnQueue q;
  CHECK(q.Arrive(Transport::USB) == TurnQueue::Arrival::NOW);
  CHECK(q.Arrive(Transport::TCP) == TurnQueue::Arrival::QUEUED);
  CHECK(q.Leave() == Transport::TCP);
  CHECK(q.Arrive(Transport::USB) == TurnQueue::Arrival::QUEUED);
  CHECK(q.Leave() == Transport::USB);
  CHECK(q.Leave() == Transport::NONE);
}

TEST(FullQueueAsksToRetry)
{
  TurnQueue q;
  CHECK(q.Arrive(Transport::USB) == TurnQueue::Arrival::NOW);
  for (uint8_t i = 0; i < TurnQueue::kMaxWaiters; ++i)
  {
    CHECK(q.Arrive(Transport::TCP) == TurnQueue::Arrival::QUEUED);
  }
  CHECK(q.Arrive(Transport::SERIAL) == TurnQueue::Arrival::FULL);
  CHECK(q.Leave() == Transport::TCP);
  CHECK(q.Arrive(Transport::SERIAL) == TurnQueue::Arrival::QUEUED);
}
