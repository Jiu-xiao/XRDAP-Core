// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace XrDap::Vendor
{
// Scheduling busy time starts at ISR entry: work done before that was already CPU
// time available to other tasks.
inline uint32_t FrameBusyUs(uint32_t end, uint32_t begin, bool woke_this_tick,
                            uint32_t isr, uint32_t period)
{
  const uint32_t start = woke_this_tick && begin - isr <= period ? isr : begin;
  return end - start;
}
}  // namespace XrDap::Vendor
