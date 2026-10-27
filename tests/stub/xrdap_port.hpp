// SPDX-License-Identifier: Apache-2.0
#pragma once

// 电脑上测试用的平台部分：单线程，锁不做任何事 / Platform part for the host tests:
// single-threaded, so the lock does nothing.

#include <cstdint>

namespace XrDap::Vendor
{
class LoopSampling;
}  // namespace XrDap::Vendor

namespace XrDap::Port
{

/// 采样的运行策略：单线程，主循环调用 Poll
/// Sampling policy: single-threaded; the main loop calls Poll.
using SamplerPolicy = Vendor::LoopSampling;

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
