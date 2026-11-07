// SPDX-License-Identifier: Apache-2.0
#include "test_clock.hpp"

#include <atomic>

#include "timebase.hpp"

namespace
{
std::atomic<uint64_t> g_now_us{0U};
}  // namespace

void XrDapTest::AdvanceClockUs(uint64_t us) { g_now_us.fetch_add(us); }

LibXR::MicrosecondTimestamp LibXR::Timebase::GetMicroseconds()
{
  return MicrosecondTimestamp(g_now_us.load());
}

LibXR::MillisecondTimestamp LibXR::Timebase::GetMilliseconds()
{
  return MillisecondTimestamp(static_cast<uint32_t>(g_now_us.load() / 1000U));
}
