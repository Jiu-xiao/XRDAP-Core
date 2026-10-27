// SPDX-License-Identifier: Apache-2.0
#include "xrdap/session/turn_queue.hpp"

namespace XrDap
{

TurnQueue::Arrival TurnQueue::Arrive(Transport t)
{
  lock_.Lock();
  Arrival result = Arrival::NOW;
  if (!busy_)
  {
    busy_ = true;
  }
  else if (count_ >= kMaxWaiters)
  {
    result = Arrival::FULL;
  }
  else
  {
    queue_[(head_ + count_) % kMaxWaiters] = t;
    ++count_;
    result = Arrival::QUEUED;
  }
  lock_.Unlock();
  return result;
}

Transport TurnQueue::Leave()
{
  lock_.Lock();
  Transport next = Transport::NONE;
  if (count_ == 0U)
  {
    busy_ = false;
  }
  else
  {
    next = queue_[head_];
    head_ = static_cast<uint8_t>((head_ + 1U) % kMaxWaiters);
    --count_;
  }
  lock_.Unlock();
  return next;
}

}  // namespace XrDap
