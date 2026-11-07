// SPDX-License-Identifier: Apache-2.0
#pragma once

// 执行器测试共用的东西：libxr DAP 类的替身、一个探针、执行上下文的线程和带期限的等待。
// 替身和 libxr 一样：USB 命令经 Feed 到达，WaitForWork 和 Step 在没有工作时阻塞到超时，
// 多出的唤醒由 Step 吸收。
//
// What the executor tests share: a stand-in for the libxr DAP class, a probe, the
// execution context's thread and a wait with a deadline. Like libxr, the stand-in takes
// USB commands through Feed, WaitForWork and Step block up to their timeout when there is
// no work, and Step absorbs a spare wake-up.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "../vendor/fixture.hpp"
#include "libxr_type.hpp"
#include "semaphore.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/session/executor.hpp"
#include "xrdap/vendor/dap_vendor.hpp"

namespace XrDapTest
{

class FakeDap
{
 public:
  using ErrorCode = LibXR::ErrorCode;
  using Hook = bool (*)(void*, const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t&);
  using LengthHook = bool (*)(void*, const uint8_t*, uint16_t, uint16_t&);
  using SessionResetGate = bool (*)(void*);

  class VendorPort
  {
   public:
    virtual ErrorCode OnVendorOut(bool, uint8_t, LibXR::ConstRawData) = 0;
    virtual LibXR::ConstRawData OnVendorIn(bool, uint8_t) = 0;

   protected:
    ~VendorPort() = default;
  };

  void SetCommandHook(Hook hook, void* ctx, LengthHook)
  {
    hook_ = hook;
    ctx_ = ctx;
  }
  void SetSessionResetGate(SessionResetGate gate, void* ctx)
  {
    gate_ = gate;
    gate_ctx_ = ctx;
  }
  void SetVendorPort(VendorPort* port) { port_ = port; }

  ErrorCode WaitForWork(uint32_t timeout_ms)
  {
    if (timeout_ms != 0U)
    {
      waits.fetch_add(1U);
    }
    if (usb_.Wait(timeout_ms) != ErrorCode::OK)
    {
      return ErrorCode::TIMEOUT;
    }
    usb_.Post();
    return ErrorCode::OK;
  }

  ErrorCode Step(uint32_t timeout_ms)
  {
    if (usb_.Wait(timeout_ms) != ErrorCode::OK)
    {
      return ErrorCode::TIMEOUT;
    }
    if (usb_cmds_.load() == 0U)
    {
      return ErrorCode::OK;  // 多出的唤醒，没有命令 / A spare wake-up, no command.
    }
    usb_cmds_.fetch_sub(1U);
    const uint8_t req[2] = {usb_cmd_.load(), 0x01};
    uint8_t resp[64];
    uint16_t out = 0U;
    (void)Run(req, sizeof(req), resp, sizeof(resp), out);
    usb_done.fetch_add(1U);
    return ErrorCode::OK;
  }

  ErrorCode ExecuteExternal(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                            uint16_t& out)
  {
    return Run(req, len, resp, cap, out);
  }

  void WakeStep(bool)
  {
    wakes.fetch_add(1U);
    usb_.Post();
  }

  /// 一条 USB 命令到了 / A USB command arrives.
  void Feed(uint8_t cmd)
  {
    usb_cmd_ = cmd;
    usb_cmds_.fetch_add(1U);
    usb_.Post();
  }

  /// libxr 在 USB 绑定或解绑后要复位 SWD 时问的门 / The gate libxr asks when it wants to
  /// reset SWD after a USB bind or unbind.
  bool SessionResetAllowed() { return gate_(gate_ctx_); }

  VendorPort* port_ = nullptr;
  /// 执行完的 USB 命令数 / USB commands that have run.
  std::atomic<uint32_t> usb_done{0U};
  /// WakeStep 的次数 / Calls of WakeStep.
  std::atomic<uint32_t> wakes{0U};
  /// 进入带超时等待的次数 / Times a wait with a timeout was entered.
  std::atomic<uint32_t> waits{0U};
  /// 执行上下文的线程，ExecThread 起来之前没有 / The execution context's thread; none
  /// before an ExecThread starts.
  std::atomic<std::thread::id> execution_thread{};
  /// 不在执行上下文的线程上进 DAP 类的调用数：只要是 0，就不可能有两条命令同时在里面 /
  /// Calls into the DAP class off the execution context's thread: while this is 0, two
  /// commands cannot be inside it at once.
  std::atomic<uint32_t> off_thread_calls{0U};
  /// 每条命令的第二个字节，按执行的顺序 / The second byte of each command, in the order
  /// they ran.
  std::vector<uint8_t> markers;

 private:
  // 钩子没处理的标准命令由“libxr”回复成功 / Standard commands the hook leaves are
  // answered OK by the "libxr".
  ErrorCode Run(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                uint16_t& out)
  {
    const std::thread::id owner = execution_thread.load();
    const bool on_thread =
        owner == std::thread::id{} || owner == std::this_thread::get_id();
    if (on_thread)
    {
      markers.push_back(len >= 2U ? req[1] : 0U);
    }
    else
    {
      off_thread_calls.fetch_add(1U);
    }
    out = 0U;
    // 线程不对时不碰执行上下文的东西，只记下来 / On the wrong thread it leaves the
    // execution context's state alone and only takes note.
    if (!on_thread || !hook_(ctx_, req, len, resp, cap, out))
    {
      resp[0] = req[0];
      resp[1] = 0x00U;
      out = 2U;
    }
    return ErrorCode::OK;
  }

  Hook hook_ = nullptr;
  void* ctx_ = nullptr;
  SessionResetGate gate_ = nullptr;
  void* gate_ctx_ = nullptr;
  LibXR::Semaphore usb_{0};
  std::atomic<uint8_t> usb_cmd_{0U};
  std::atomic<uint32_t> usb_cmds_{0U};
};

struct Probe
{
  ModelSwd swd;
  XrDap::DapVendor vendor{swd};
  XrDap::DapActivity activity;
  FakeDap dap;
  XrDap::Executor<FakeDap> exec{dap, vendor, activity};
};

// 等条件成立；期限到了还不成立就返回 false，调用者 CHECK 它。期限只管失败时等多久，不是对
// 速度的要求 / Waits for a condition; returns false when it still does not hold at the
// deadline, for the caller to CHECK. The deadline only bounds how long a failure takes;
// it does not require any speed.
template <typename Cond>
bool WaitFor(Cond cond)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!cond())
  {
    if (std::chrono::steady_clock::now() >= deadline)
    {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// 执行上下文的线程照 ESP 的 DAP 线程那样循环
// Run。没事时一直等，不靠超时醒来，所以漏掉的唤醒 一定让测试失败 / The execution
// context's thread loops Run like ESP's DAP thread. With nothing to do it waits with no
// timeout to fall back on, so a missed wake-up always fails the test.
class ExecThread
{
 public:
  explicit ExecThread(Probe& p)
      : p_(p),
        thread_(
            [this]()
            {
              p_.dap.execution_thread.store(std::this_thread::get_id());
              NullTimer timer;
              while (!stop_.load())
              {
                p_.exec.Run(timer, UINT32_MAX);
              }
            })
  {
  }
  ~ExecThread()
  {
    stop_.store(true);
    p_.dap.WakeStep(false);
    thread_.join();
  }
  std::thread::id Id() const { return thread_.get_id(); }

 private:
  Probe& p_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// 链路的任务发一条命令，等它执行完，返回回复的第一个字节 / A link's task sends one
// command, waits until it has run, and returns the first byte of the reply.
inline uint8_t Send(Probe& p, XrDap::Transport t, uint8_t cmd)
{
  const uint8_t req[2] = {cmd, 0x01};
  uint8_t resp[64] = {};
  uint16_t out = 0U;
  p.exec.Execute(t, req, sizeof(req), resp, sizeof(resp), out);
  return resp[0];
}

// USB 的命令经 libxr 的 DAP 类到达 / USB commands arrive through the libxr DAP class.
inline void SendOverUsb(Probe& p, uint8_t cmd)
{
  const uint32_t before = p.dap.usb_done.load();
  p.dap.Feed(cmd);
  CHECK(WaitFor([&]() { return p.dap.usb_done.load() != before; }));
}

// 在执行上下文里直接执行一条命令，返回回复的第一个字节 / Runs one command directly in the
// execution context and returns the first byte of the reply.
inline uint8_t SendHere(Probe& p, XrDap::Transport t, uint8_t cmd)
{
  const uint8_t req[2] = {cmd, 0x01};
  uint8_t resp[64] = {};
  uint16_t out = 0U;
  p.exec.ExecuteHere(t, req, sizeof(req), resp, sizeof(resp), out);
  return resp[0];
}

// 连接并开始采样，一个条目，周期 2000 µs。执行线程起来之前调用 / Connects and starts
// sampling one item with a 2000 µs period. Call it before the execution thread starts.
inline void ConnectAndStartSampling(Probe& p)
{
  (void)SendHere(p, XrDap::Transport::TCP, XrDap::Byte(XrDap::DapCmd::CONNECT));
  const auto start = SubStart(2000U, 0U, {WordAt(0x08000000U)});
  uint8_t resp[64] = {};
  uint16_t out = 0U;
  p.exec.ExecuteHere(XrDap::Transport::TCP, start.data(),
                     static_cast<uint16_t>(start.size()), resp, sizeof(resp), out);
  CHECK_EQ(resp[1], 0x00U);
  CHECK(p.vendor.Sampling());
}

}  // namespace XrDapTest
