// SPDX-License-Identifier: Apache-2.0
#pragma once

// 电脑上测试用的平台部分 / Platform part for the host tests.

#include <cstdint>

namespace XrDap::Port
{

/// 测试用的帧缓冲大小 / Frame buffer size used by the tests.
inline constexpr uint32_t kRingBytes = 256U;

}  // namespace XrDap::Port
