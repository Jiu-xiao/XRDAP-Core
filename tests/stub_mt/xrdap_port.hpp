// SPDX-License-Identifier: Apache-2.0
#pragma once

// 多线程测试用的平台部分：自旋锁是真的，采样线程和测试线程会同时访问帧缓冲。
// tests/stub 里的是空锁，只能给单线程测试用 / Platform part for the multi-threaded host
// tests: the spin lock is real, because the sampling thread and the test thread touch the
// frame ring at the same time. The one in tests/stub is a no-op for single-threaded tests.

#include <atomic>
#include <cstdint>
#include <thread>

namespace XrDap::Vendor
{
class TaskSampling;
}  // namespace XrDap::Vendor

namespace XrDap::Session
{
class MultiTransport;
}  // namespace XrDap::Session

namespace XrDap::Port
{

/// 采样的运行策略：采样在自己的线程里，RunSamplerTask
/// Sampling policy: sampling runs in its own thread, RunSamplerTask.
using SamplerPolicy = Vendor::TaskSampling;

/// 传输策略：测试按多传输 / Transport policy: the tests use several transports.
using TransportPolicy = Session::MultiTransport;

/// 测试用的帧缓冲大小 / Frame buffer size used by the tests.
inline constexpr uint32_t kRingBytes = 4096U;

/// 自旋锁 / Spin lock.
class SpinLock
{
 public:
  void Lock()
  {
    while (flag_.test_and_set(std::memory_order_acquire))
    {
      std::this_thread::yield();
    }
  }
  void Unlock() { flag_.clear(std::memory_order_release); }

 private:
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

}  // namespace XrDap::Port
