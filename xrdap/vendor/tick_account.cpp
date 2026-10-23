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

bool TickAccount::Due(uint32_t count) const
{
  return TickDue(count, next_) && !(has_last_ && count == last_);
}

void TickAccount::Defer(uint32_t count)
{
  if (TickDue(count, next_))
  {
    next_ = count + 1U;
  }
}

TickAccount::Step TickAccount::OnWake(uint32_t count)
{
  // 计数没走到下一个要采的节拍：停止或开始时的唤醒，或多余的一次唤醒。
  // The count has not reached the next tick to sample: a stop or start wake, or a spare
  // wake.
  if (!TickDue(count, next_) || (has_last_ && count == last_))
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
  uint32_t gap = (period_us / 8U > kMinGapUs) ? period_us / 8U : kMinGapUs;
  gap = (busy_us / 4U > gap) ? busy_us / 4U : gap;
  // 从这一帧的节拍起要过的周期数，向上取整，至少 1 / Periods to pass from this frame's
  // tick, rounded up, at least 1.
  const uint64_t span = static_cast<uint64_t>(busy_us) + gap;
  uint64_t periods = (span + period_us - 1U) / period_us;
  periods = (periods < 1U) ? 1U : periods;
  periods = (periods > 0x7FFFFFFFU) ? 0x7FFFFFFFU : periods;
  uint32_t next = last_ + static_cast<uint32_t>(periods);
  if (TickDue(count_after, next))
  {
    next = count_after + 1U;
  }
  next_ = next;
}

}  // namespace XrDap::Vendor
