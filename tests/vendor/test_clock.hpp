// SPDX-License-Identifier: Apache-2.0
#pragma once

// 这些测试的时钟：链接时顶替 libxr 的 Timebase，只有测试拨它才走，所以结果不看机器快慢 /
// The clock of these tests: it replaces libxr's Timebase at link time and moves only when
// a test moves it, so results do not depend on how fast the machine is.

#include <cstdint>

namespace XrDapTest
{

void AdvanceClockUs(uint64_t us);

}  // namespace XrDapTest
