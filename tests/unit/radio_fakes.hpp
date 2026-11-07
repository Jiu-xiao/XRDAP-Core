// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace RadioTest
{

// 无线测试的时间：Core 只经 LibXR::Timebase 读时间，这个测试程序把它换成这个时钟
// （radio_clock.cpp）。每读一次走一小步，所以忙等总会结束；同样的调用序列得到同样的时间。
// Time for the radio tests: Core reads time only through LibXR::Timebase, which this test
// program replaces with this clock (radio_clock.cpp). Every read moves it a small step,
// so a busy-wait always ends, and the same calls see the same times.
class VirtualClock
{
 public:
  // 每次读走多少，µs；和主循环一轮读几次射频的时间差不多。 / The step of one read, µs,
  // about what a round of the main loop spends on the radio.
  static constexpr uint32_t kStepUs = 10;
  // NowUs() 截成 32 位，这里回绕。 / NowUs() truncates to 32 bits, which wrap here.
  static constexpr uint64_t kWrapUs = 1ULL << 32;
  // 一对探针开始时离回绕多久：短的用例走不到，长的用例会跨过。 / How far from the wrap a
  // pair starts: short cases do not reach it, long ones cross it.
  static constexpr uint32_t kLeadUs = 1'000'000;

  static void StartBeforeWrap(uint32_t lead_us) { now_us_ = kWrapUs - lead_us; }
  static uint64_t Read() { return now_us_ += kStepUs; }

 private:
  static inline uint64_t now_us_ = kWrapUs - kLeadUs;
};

}  // namespace RadioTest
