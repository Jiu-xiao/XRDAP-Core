// SPDX-License-Identifier: Apache-2.0
// 采样的运行方式是编译期策略：任务策略有 RunTask / RunSamplerTask、没有 Poll，单循环策略
// 有 Poll、没有 RunTask / RunSamplerTask，调错入口编译不过；单循环策略里没有让路和信号量。
// 全部是 static_assert，运行时什么都不做。入口用 SFINAE 探测：成员函数模板在错误的策略下
// 替换失败，调用方得到"没有匹配的函数"。
//
// The way to run sampling is a compile-time policy: the task policy has RunTask /
// RunSamplerTask and no Poll, the single-loop policy has Poll and no RunTask /
// RunSamplerTask, and calling the wrong entry does not compile; the single-loop policy has
// no yielding and no semaphore. All static_asserts; nothing happens at run time. The entries
// are probed with SFINAE: the member function templates fail to substitute under the wrong
// policy, and a caller gets "no matching function".

#include <cstdint>
#include <type_traits>
#include <utility>

#include "xrdap/vendor/dap_vendor.hpp"

namespace
{
using XrDap::DapVendorT;
using XrDap::Vendor::CommandYield;
using XrDap::Vendor::LoopSampler;
using XrDap::Vendor::LoopSampling;
using XrDap::Vendor::TaskSampler;
using XrDap::Vendor::TaskSampling;
using XrDap::Vendor::TickTimer;

template <class T, class = void>
struct HasPoll : std::false_type
{
};
template <class T>
struct HasPoll<T, std::void_t<decltype(std::declval<T&>().Poll(std::declval<TickTimer&>()))>>
    : std::true_type
{
};

template <class T, class = void>
struct HasRunTask : std::false_type
{
};
template <class T>
struct HasRunTask<
    T, std::void_t<decltype(std::declval<T&>().RunTask(std::declval<TickTimer&>()))>>
    : std::true_type
{
};

template <class T, class = void>
struct HasRunSamplerTask : std::false_type
{
};
template <class T>
struct HasRunSamplerTask<
    T, std::void_t<decltype(std::declval<T&>().RunSamplerTask(std::declval<TickTimer&>()))>>
    : std::true_type
{
};

// 任务策略 / Task policy.
static_assert(!HasPoll<TaskSampler>::value, "Poll must not exist under the task policy");
static_assert(HasRunTask<TaskSampler>::value, "RunTask must exist under the task policy");
static_assert(!HasPoll<DapVendorT<TaskSampling>>::value,
              "DapVendor::Poll must not exist under the task policy");
static_assert(HasRunSamplerTask<DapVendorT<TaskSampling>>::value,
              "DapVendor::RunSamplerTask must exist under the task policy");

// 单循环策略 / Single-loop policy.
static_assert(HasPoll<LoopSampler>::value, "Poll must exist under the single-loop policy");
static_assert(!HasRunTask<LoopSampler>::value,
              "RunTask must not exist under the single-loop policy");
static_assert(HasPoll<DapVendorT<LoopSampling>>::value,
              "DapVendor::Poll must exist under the single-loop policy");
static_assert(!HasRunSamplerTask<DapVendorT<LoopSampling>>::value,
              "DapVendor::RunSamplerTask must not exist under the single-loop policy");

// 单循环策略里没有让路（CommandYield）也没有信号量：只有 active_、gen_、ticks_ 三个 32 位
// 字，比一个 CommandYield 还小。任务策略比它大得多 / The single-loop policy holds neither
// the yield (CommandYield) nor a semaphore: only the three 32-bit words active_, gen_ and
// ticks_, smaller than a CommandYield alone. The task policy is much larger.
static_assert(sizeof(LoopSampling) <= 3U * sizeof(uint32_t),
              "the single-loop policy is three words");
static_assert(sizeof(LoopSampling) < sizeof(CommandYield),
              "the single-loop policy holds no CommandYield");
static_assert(sizeof(TaskSampling) > sizeof(CommandYield), "the task policy holds one");

// 平台桩（tests/stub）选单循环策略，DapVendor 就是它 / The platform stub (tests/stub)
// picks the single-loop policy, and DapVendor is that.
static_assert(std::is_same_v<XrDap::DapVendor, DapVendorT<LoopSampling>>,
              "DapVendor follows Port::SamplerPolicy");
static_assert(std::is_same_v<XrDap::Port::SamplerPolicy, LoopSampling>,
              "the stub chooses the single-loop policy");
}  // namespace

int main() { return 0; }
