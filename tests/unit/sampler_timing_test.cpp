// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/sampler_timing.hpp"

#include "check.hpp"
#include "xrdap/vendor/tick_account.hpp"

using namespace XrDap::Vendor;

TEST(LateIsrEntryCountsFromIsrNotFromDeadline)
{
  // ISR enters 650 us late, task starts 20 us later, frame ends before tick 1.
  const uint32_t busy = FrameBusyUs(980U, 670U, true, 650U, 1000U);
  CHECK_EQ(busy, 330U);
  TickAccount account;
  account.Begin();
  CHECK(account.OnWake(0U).sample);
  account.OnDone(0U, busy, 1000U);
  CHECK_EQ(account.Next(), 1U);
  CHECK_EQ(account.OnWake(1U).skipped, 0U);
}

TEST(BusyTimeSurvivesTimeWrap)
{
  CHECK_EQ(FrameBusyUs(25U, 5U, true, 0xFFFFFFF0U, 1000U), 41U);
  CHECK_EQ(FrameBusyUs(25U, 5U, false, 0xFFFFFFF0U, 1000U), 20U);
}

TEST(ElapsedTicksStillProduceRealGaps)
{
  TickAccount account;
  account.Begin();
  CHECK(account.OnWake(0U).sample);
  account.OnDone(0U, 330U, 1000U);
  const auto step = account.OnWake(4U);
  CHECK(step.sample);
  CHECK_EQ(step.skipped, 3U);
}
