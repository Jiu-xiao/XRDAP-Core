// SPDX-License-Identifier: Apache-2.0
// 只能亮灭的状态灯。判据：Frame 的规定（亮度 0 为熄灭；活动脉冲在命令到达时为 1000，
// 66 ms 内线性降到 0；脉冲大于 500 时只能亮灭的灯熄灭）/ An on/off-only status light.
// Oracle: what Frame prescribes (level 0 is off; the activity pulse is 1000 when a
// command arrives and falls linearly to 0 within 66 ms; an on/off-only light is dark
// while the pulse is above 500).
#include "xrdap/light/status_light.hpp"

#include "check.hpp"

using namespace XrDap;
using namespace XrDap::Light;

namespace
{
Frame MakeFrame(uint16_t level, uint16_t pulse) { return Frame{kRed, level, pulse}; }
}  // namespace

TEST(LitOnOffFollowsLevelAndPulse)
{
  CHECK(LitOnOff(MakeFrame(200, 0)));
  CHECK(LitOnOff(MakeFrame(1000, 500)));
  CHECK(!LitOnOff(MakeFrame(1000, 501)));
  CHECK(!LitOnOff(MakeFrame(200, 1000)));
  CHECK(!LitOnOff(MakeFrame(0, 0)));
  CHECK(!LitOnOff(MakeFrame(0, 500)));
}

TEST(LitOnOffAcrossOneActivityPulse)
{
  // 命令到达那一刻脉冲为 1000，33 ms 后正好降到 500 / The pulse is 1000 when the command
  // arrives and is exactly 500 after 33 ms.
  StatusLight rule(0U);
  const DapActivity::State idle{0U, false, false, true};
  const DapActivity::State busy{1U, false, false, true};
  CHECK(LitOnOff(rule.Update(Link::WIRED, Setting::NONE, idle, 1000U)));
  const Frame arrive = rule.Update(Link::WIRED, Setting::NONE, busy, 1000U);
  CHECK_EQ(arrive.pulse, 1000U);
  CHECK(!LitOnOff(arrive));
  const Frame before = rule.Update(Link::WIRED, Setting::NONE, busy, 1032U);
  CHECK(before.pulse > 500U);
  CHECK(!LitOnOff(before));
  const Frame half = rule.Update(Link::WIRED, Setting::NONE, busy, 1033U);
  CHECK_EQ(half.pulse, 500U);
  CHECK(LitOnOff(half));
  CHECK(LitOnOff(rule.Update(Link::WIRED, Setting::NONE, busy, 1100U)));
}

TEST(LitOnOffIsDarkInTheBlinkOffPhaseOfAChangingSetting)
{
  // 改设置时紫色快闪，熄灭的相位亮度为 0 / While a setting changes the purple blinks and
  // its off phase has level 0.
  StatusLight rule(0U);
  const DapActivity::State idle{0U, false, false, false};
  bool lit = false;
  bool dark = false;
  for (uint32_t t = 1000U; t < 2000U; t += 10U)
  {
    const bool on = LitOnOff(rule.Update(Link::NONE, Setting::CHANGING, idle, t));
    lit = lit || on;
    dark = dark || !on;
  }
  CHECK(lit);
  CHECK(dark);
}
