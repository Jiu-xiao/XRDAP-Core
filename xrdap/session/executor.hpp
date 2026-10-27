// SPDX-License-Identifier: Apache-2.0
#pragma once

// 唯一的执行上下文：探针上所有进入 libxr DAP 实例的调用（USB 的 Step、别的链路投来的
// ExecuteExternal）和采样都在 Run() 这个循环里一件一件做，所以 SWD、DAP 类和厂商扩展都
// 只在一个线程里访问，没有锁。别的链路（串口、TCP、无线）的任务只投递请求、等应答
// （Execute）；单循环的板子里主循环就是这个循环。探针一次只使用一条链路由使用者保证。
//
// 一轮的顺序（决定 B9：命令最多被采样占一帧，跳拍照常计数）：
//   1. 有 USB 命令：Step(0) 执行一条；否则邮箱里有请求：执行一条；
//   2. 采样的节拍到了：采一帧（只采一帧，采完回头看有没有命令）；
//   3. 什么都没做：在 WaitForWork 里等，USB 命令、别的链路的投递和节拍中断（WakeStep）都会叫醒。
// 命令钩子先交给平台的转发器（例如 CH32 的无线），再交给厂商扩展，其余的标准命令由 libxr
// 执行。执行过的命令记进 DapActivity。装钩子时也把厂商命令的长度函数交给 libxr
// （SetCommandHook 的第三个参数），DAP_ExecuteCommands / DAP_QueueCommands 里因此可以放
// 厂商命令。需要 libxr 的 WakeStep（候选分支 release/0.6-candidate）。
//
// The one execution context: every call into the libxr DAP instance on the probe (USB's
// Step, ExecuteExternal for commands posted by other links) and sampling are done one after
// the other in the loop Run(), so SWD, the DAP class and the vendor extension are accessed
// by one thread only, without locks. The tasks of the other links (serial, TCP, radio) only
// post a request and wait for the reply (Execute); on a single-loop board the main loop is
// this loop. That the probe is used over one link at a time is up to the user.
//
// One round (this decides B9: a command loses at most one frame to sampling, skips are
// counted as usual):
//   1. a USB command: Step(0) runs one; otherwise a request in the mailbox: run one;
//   2. a sampling tick is due: sample ONE frame, then look for commands again;
//   3. nothing done: wait in WaitForWork; a USB command, a post from another link and the
//      tick interrupt (WakeStep) all wake it.
// The command hook first offers the command to the platform's forwarder (for example the
// CH32 radio), then to the vendor extension; libxr runs the remaining standard commands.
// Commands that ran are recorded in DapActivity. The hook is installed together with the
// vendor length function (the third argument of SetCommandHook), so vendor commands may sit
// inside DAP_ExecuteCommands / DAP_QueueCommands. Needs libxr's WakeStep (candidate branch
// release/0.6-candidate).

#include <atomic>
#include <cstdint>

#include "libxr_def.hpp"
#include "semaphore.hpp"
#include "timebase.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/session/transport.hpp"
#include "xrdap/vendor/dap_vendor.hpp"
#include "xrdap/vendor/sampler.hpp"

namespace XrDap
{

/// @brief 命令执行器 / Command executor.
/// @tparam DapT libxr DAP 类（提供 WaitForWork、WakeStep、Step、ExecuteExternal、带长度
///         钩子的 SetCommandHook 和 SetSessionResetGate）/ libxr DAP class (providing
///         WaitForWork, WakeStep, Step, ExecuteExternal, SetCommandHook with the length hook
///         and SetSessionResetGate).
template <typename DapT>
class Executor
{
 public:
  /// @brief 平台转发器：返回 true 表示命令已由平台处理并填好回复 / Platform forwarder:
  ///        returns true once the platform handled the command and filled the reply.
  /// @note 在 DAP 执行上下文里调用 / Called in the DAP execution context.
  using Forwarder = bool (*)(void* ctx, Transport t, const uint8_t* req, uint16_t len,
                             uint8_t* resp, uint16_t cap, uint16_t& out);

  /// @brief 构造并把命令钩子装到 dap 上 / Construct and install the command hook on
  ///        dap.
  /// @note 初始化时调用；参数的生命周期需长于本对象 / Call at initialisation; the
  ///       arguments must outlive this object.
  Executor(DapT& dap, DapVendor& vendor, DapActivity& activity)
      : dap_(dap), vendor_(vendor), activity_(activity)
  {
    dap_.SetCommandHook(&Executor::Hook, this, &Executor::LengthHook);
    dap_.SetSessionResetGate(&Executor::MayResetSession, this);
    vendor_.SetWake(&Executor::Wake, this);
  }

  /// @brief 设置平台转发器 / Set the platform forwarder.
  /// @note 初始化时、命令开始执行之前调用 / Call at initialisation, before commands run.
  void SetForwarder(Forwarder forwarder, void* ctx)
  {
    forwarder_ = forwarder;
    forwarder_ctx_ = ctx;
  }

  /// @brief 执行上下文的一轮 / One round of the execution context.
  /// @param timer 采样用的节拍定时器 / The tick timer sampling uses.
  /// @param wait_ms 什么都没做时最长等多久 / Longest wait when nothing was done.
  /// @note 只由执行上下文（平台的 DAP 线程，或单循环的主循环）循环调用 / Called in a loop by
  ///       the execution context only (the platform's DAP thread, or the single main loop).
  void Run(Vendor::TickTimer& timer, uint32_t wait_ms)
  {
    bool worked = false;
    if (dap_.WaitForWork(0U) == LibXR::ErrorCode::OK)
    {
      Scope scope(*this, Transport::USB);
      (void)dap_.Step(0U);
      worked = true;
    }
    else
    {
      worked = ServeOne();
    }
    if (vendor_.SampleDue())
    {
      (void)vendor_.Sample(timer);
      worked = true;
    }
    if (!worked && pending_.load(std::memory_order_acquire) == 0U)
    {
      (void)dap_.WaitForWork(wait_ms);
    }
  }

  /// @brief 别的链路：把一条命令交给执行上下文，等它执行完 / Other links: hand a command
  ///        to the execution context and wait until it has run.
  /// @note 链路自己的任务里调用，不能在中断里；每条链路同时只有一条在途；执行上下文要在
  ///       跑 Run() / Call from the link's own task, not from an interrupt; one in flight
  ///       per link; the execution context must be running Run().
  void Execute(Transport t, const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
               uint16_t& out)
  {
    Slot& slot = slots_[SlotOf(t)];
    slot.t = t;
    slot.req = req;
    slot.len = len;
    slot.resp = resp;
    slot.cap = cap;
    slot.out = &out;
    pending_.fetch_or(1U << SlotOf(t), std::memory_order_release);
    dap_.WakeStep(false);
    (void)slot.done.Wait(UINT32_MAX);
  }

  /// @brief 链路 t 丢了（TCP 客户端关闭、串口链路停止或太久没有请求、无线会话结束）：
  ///        连接在 t 上时，连接和采样作废 / Link t is gone (TCP client closed, serial link
  ///        stopped or silent too long, radio session ended): when the connection is on
  ///        t, the connection and sampling lapse.
  /// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
  void LinkLost(Transport t) { vendor_.OnLinkLost(t); }

 private:
  /// 一条命令的执行范围：记下命令从哪条链路来 / The scope of one command's execution: notes
  /// which link the command came from.
  class Scope
  {
   public:
    Scope(Executor& e, Transport t) : e_(e)
    {
      e_.vendor_.SetCurrentLink(t);
      e_.current_ = t;
    }
    ~Scope() { e_.current_ = Transport::NONE; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    Executor& e_;
  };

  /// 别的链路投来的一条请求 / A request posted by another link.
  struct Slot
  {
    Transport t = Transport::NONE;
    const uint8_t* req = nullptr;
    uint16_t len = 0U;
    uint8_t* resp = nullptr;
    uint16_t cap = 0U;
    uint16_t* out = nullptr;
    LibXR::Semaphore done;
  };

  static constexpr uint32_t kSlots = 3U;  ///< SERIAL、TCP、RADIO

  static uint32_t SlotOf(Transport t)
  {
    return static_cast<uint32_t>(t) - static_cast<uint32_t>(Transport::SERIAL);
  }

  /// 执行邮箱里的一条请求，各链路轮流 / Run one request from the mailbox, links in turn.
  bool ServeOne()
  {
    const uint32_t pending = pending_.load(std::memory_order_acquire);
    if (pending == 0U)
    {
      return false;
    }
    uint32_t i = next_;
    while ((pending & (1U << i)) == 0U)
    {
      i = (i + 1U) % kSlots;
    }
    next_ = (i + 1U) % kSlots;
    Slot& slot = slots_[i];
    {
      Scope scope(*this, slot.t);
      *slot.out = 0U;
      (void)dap_.ExecuteExternal(slot.req, slot.len, slot.resp, slot.cap, *slot.out);
    }
    pending_.fetch_and(~(1U << i), std::memory_order_release);
    slot.done.Post();
    return true;
  }

  static void Wake(void* ctx, bool in_isr)
  {
    static_cast<Executor*>(ctx)->dap_.WakeStep(in_isr);
  }

  static bool Hook(void* ctx, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    return static_cast<Executor*>(ctx)->OnCommand(req, len, resp, cap, out);
  }

  // USB 绑定或解绑后 libxr 要复位 SWD 的会话状态时来问：连接在 USB 上或没有连接时才复位，
  // 连接在别的链路上时它的 SWD 状态不动。在执行上下文里调用 / libxr asks before
  // resetting the SWD session state after a USB bind or unbind: reset only when the
  // connection is on USB or there is none; while it is on another link, its SWD state is
  // left alone. Called in the execution context.
  static bool MayResetSession(void* ctx)
  {
    const Transport link = static_cast<Executor*>(ctx)->vendor_.ConnectedLink();
    return link == Transport::USB || link == Transport::NONE;
  }

  // libxr 解析 DAP_ExecuteCommands / DAP_QueueCommands 时问厂商命令的长度。接收端扫描
  // 命令时也会调用，可能在 USB 中断里，所以只做解析：不阻塞 / libxr asks for a
  // vendor command's length while parsing DAP_ExecuteCommands / DAP_QueueCommands. The
  // receive-side command scan calls it too, possibly in the USB interrupt, so it only
  // parses: no blocking.
  static bool LengthHook(void* ctx, const uint8_t* req, uint16_t avail, uint16_t& len)
  {
    return static_cast<Executor*>(ctx)->vendor_.RequestLength(req, avail, len);
  }

  // 在执行上下文里调用 / Called in the execution context.
  bool OnCommand(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                 uint16_t& out)
  {
    if (req == nullptr || len < 1U)
    {
      return false;
    }
    const Transport t = current_;
    const auto now = static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds());
    if (forwarder_ != nullptr && forwarder_(forwarder_ctx_, t, req, len, resp, cap, out))
    {
      Ran(t, req, len, now);
      return true;
    }
    switch (vendor_.OnCommand(req, len, resp, cap, out))
    {
      case DapVendor::Result::HANDLED:
        Ran(t, req, len, now);
        return true;
      case DapVendor::Result::REFUSED:
        return true;
      case DapVendor::Result::PASS:
        break;
    }
    Ran(t, req, len, now);
    return false;
  }

  // 一条命令执行了，不管由转发器、厂商扩展还是 libxr 执行：记进 DapActivity / A command
  // ran, whether the forwarder, the vendor extension or libxr ran it: record it in
  // DapActivity.
  void Ran(Transport t, const uint8_t* req, uint16_t len, uint32_t now)
  {
    activity_.Observe(req, len, now, t == Transport::USB || t == Transport::SERIAL);
  }

  DapT& dap_;
  DapVendor& vendor_;
  DapActivity& activity_;
  Forwarder forwarder_ = nullptr;
  void* forwarder_ctx_ = nullptr;
  Slot slots_[kSlots];
  std::atomic<uint32_t> pending_{0U};
  uint32_t next_ = 0U;
  Transport current_ = Transport::NONE;
};

}  // namespace XrDap
