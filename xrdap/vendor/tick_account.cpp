// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/tick_account.hpp"

namespace XrDap::Vendor
{

void TickAccount::Begin()
{
  has_last_ = false;
  last_ = 0U;
  next_ = 0U;
}

TickAccount::Step TickAccount::OnWake(uint32_t count)
{
  // 计数没走到下一个可以采的节拍：停止时的唤醒、采样中已经过去的节拍或让出的节拍。
  // The count has not reached the next tick that may be sampled: a stop wake, a tick
  // that passed while sampling, or a tick given up.
  if (count < next_ || (has_last_ && count == last_))
  {
    return {false, count, 0U};
  }
  const uint32_t skipped = has_last_ ? count - last_ - 1U : count;
  has_last_ = true;
  last_ = count;
  next_ = count + 1U;
  return {true, count, skipped};
}

void TickAccount::OnDone(uint32_t count_after, uint32_t busy_us, uint32_t period_us)
{
  uint32_t next = last_ + 1U;
  if (count_after > last_)
  {
    next = count_after + 1U;
  }
  const uint32_t gap = (period_us / 8U > kMinGapUs) ? period_us / 8U : kMinGapUs;
  if (next == last_ + 1U && busy_us + gap > period_us)
  {
    next = last_ + 2U;
  }
  next_ = next;
}

}  // namespace XrDap::Vendor
