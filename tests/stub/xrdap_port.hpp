// SPDX-License-Identifier: Apache-2.0
#pragma once

// 电脑上测试用的平台部分：单线程，锁不做任何事 / Platform part for the host tests:
// single-threaded, so the lock does nothing.

#include <cstdint>

namespace XrDap::Port
{

/// 测试用的帧缓冲大小 / Frame buffer size used by the tests.
inline constexpr uint32_t kRingBytes = 256U;

/// 空锁 / No-op lock.
class SpinLock
{
 public:
  void Lock() {}
  void Unlock() {}
};

}  // namespace XrDap::Port
