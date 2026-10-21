// SPDX-License-Identifier: Apache-2.0
#include "status_light.hpp"

namespace XrDap::Light
{
namespace
{
/// 调试器已连接时主色与紫色、目标运行中时红蓝来回一次的时间 / One round between the
/// main colour and purple while connected, and between red and blue while running.
constexpr uint32_t kConnectedPeriodMs = 2000U;
constexpr uint32_t kRunningPeriodMs = 1000U;
/// 正在连接时紫色呼吸一次的时间和最暗处（千分比）/ Breathing period while joining and
/// its lowest point (permille).
constexpr uint32_t kJoiningPeriodMs = 2000U;
constexpr uint32_t kJoiningLow = 100U;
/// 改设置时快闪的半周期 / Half period of the fast blink while changing a setting.
constexpr uint32_t kSettingBlinkMs = 100U;

// 三角波，千分比：从 low 升到 1000 再回到 low，用时 period_ms / Triangle wave in
// permille: from low up to 1000 and back to low within period_ms.
uint32_t Triangle(uint32_t ms, uint32_t period_ms, uint32_t low)
{
  const uint32_t phase = ms % period_ms;
  const uint32_t half = period_ms / 2U;
  const uint32_t up = (phase < half) ? phase : period_ms - phase;
  return low + (1000U - low) * up / half;
}

// a 到 b 之间按千分比混色 / Mix a towards b by permille.
Rgb Mix(Rgb a, Rgb b, uint32_t permille)
{
  const auto mix = [permille](uint8_t x, uint8_t y)
  { return static_cast<uint8_t>((x * (1000U - permille) + y * permille) / 1000U); };
  return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

// 色相 0 到 1535 的饱和色 / Saturated colour for hue 0 to 1535.
Rgb Hue(uint32_t h)
{
  const auto x = static_cast<uint8_t>(h & 0xFFU);
  switch ((h >> 8) % 6U)
  {
    case 0:
      return {255, x, 0};
    case 1:
      return {static_cast<uint8_t>(255U - x), 255, 0};
    case 2:
      return {0, 255, x};
    case 3:
      return {0, static_cast<uint8_t>(255U - x), 255};
    case 4:
      return {x, 0, 255};
    default:
      return {255, 0, static_cast<uint8_t>(255U - x)};
  }
}
}  // namespace

StatusLight::StatusLight(uint32_t now_ms)
    : start_ms_(now_ms), pulse_ms_(now_ms - kPulseMs)
{
}

Frame StatusLight::Update(Link link, Setting setting,
                          const Vendor::DapActivity::State& dap, uint32_t now_ms)
{
  const uint32_t t = now_ms - start_ms_;
  if (dap.commands != seen_commands_)
  {
    seen_commands_ = dap.commands;
    pulse_pending_ = t >= kBootMs;
  }
  if (t < kBootMs)
  {
    return {Hue(t * 1536U / kBootMs), 1000U, 0U};
  }
  if (setting == Setting::APPLIED)
  {
    return {kPurple, 1000U, 0U};
  }
  if (setting == Setting::CHANGING)
  {
    return {kPurple, ((t / kSettingBlinkMs) % 2U == 0U) ? uint16_t{1000U} : uint16_t{0U},
            0U};
  }

  // 有新命令且上一个脉冲已结束时开始下一个；命令不断时按 kPulseMs 稳定脉动。
  if (pulse_pending_ && now_ms - pulse_ms_ >= kPulseMs)
  {
    pulse_ms_ = now_ms;
    pulse_pending_ = false;
  }
  const uint32_t age = now_ms - pulse_ms_;
  const uint32_t pulse = (age < kPulseMs) ? 1000U - age * 1000U / kPulseMs : 0U;

  Rgb color = kPurple;
  uint32_t rest = kRestLevel;
  switch (link)
  {
    case Link::WIRED:
      color = kRed;
      break;
    case Link::WIRELESS:
      color = kBlue;
      break;
    case Link::JOINING:
      rest = kRestLevel * Triangle(t, kJoiningPeriodMs, kJoiningLow) / 1000U;
      break;
    case Link::NONE:
      break;
  }
  if (dap.running)
  {
    color = Mix(kRed, kBlue, Triangle(t, kRunningPeriodMs, 0U));
  }
  else if (dap.connected)
  {
    color = Mix(color, kPurple, Triangle(t, kConnectedPeriodMs, 0U));
  }
  const uint32_t level = rest + (1000U - kRestLevel) * pulse / 1000U;
  return {color, static_cast<uint16_t>(level), static_cast<uint16_t>(pulse)};
}

}  // namespace XrDap::Light
