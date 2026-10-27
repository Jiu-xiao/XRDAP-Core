// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace XrDap::Vendor
{
// Diagnostic latency starts at the requested deadline, including delayed ISR
// entry. Scheduling busy time starts at ISR entry, as before: work done before
// that was already CPU time available to other tasks.
inline uint32_t WakeLatencyUs(uint32_t begin, uint32_t isr, uint32_t late,
                              uint32_t lag_ticks, uint32_t period)
{
  const uint64_t elapsed = static_cast<uint32_t>(begin - isr) +
                           static_cast<uint64_t>(late) +
                           static_cast<uint64_t>(lag_ticks) * period;
  return elapsed > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(elapsed);
}

inline uint32_t FrameBusyUs(uint32_t end, uint32_t begin, bool woke_this_tick,
                            uint32_t isr, uint32_t period)
{
  const uint32_t start = woke_this_tick && begin - isr <= period ? isr : begin;
  return end - start;
}
}  // namespace XrDap::Vendor
