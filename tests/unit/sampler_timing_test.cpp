// SPDX-License-Identifier: Apache-2.0
#include "check.hpp"
#include "xrdap/vendor/sampling.hpp"

using namespace XrDap::Vendor;

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
