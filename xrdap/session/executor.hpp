// SPDX-License-Identifier: Apache-2.0
#pragma once

// 唯一的命令执行器：探针上所有进入 libxr DAP 实例的调用（USB 的 Step 和其他传输的
// ExecuteExternal）都经过这里，持同一把锁，每次只有一条命令在执行；执行时记下命令属于哪条
// 传输。它装在 DAP 实例上的命令钩子先问 SessionOwner 这条传输能不能执行（不能就回忙），
// 再交给平台的转发器（例如 CH32 的无线）、厂商扩展，其余的标准命令由 libxr 执行。
// 执行过的命令记进 DapActivity。
//
// The single command executor: every call into the libxr DAP instance on the probe
// (USB's Step and the other transports' ExecuteExternal) goes through here under one
// lock, so one command runs at a time, and it notes which transport the command belongs
// to. The command hook it installs on the DAP instance first asks SessionOwner whether
// that transport may run (busy reply otherwise), then hands the command to the
// platform's forwarder (for example the CH32 radio) and the vendor extension; libxr runs
// the remaining standard commands. Commands that ran are recorded in DapActivity.

#include <cstdint>

#include "libxr_def.hpp"
#include "mutex.hpp"
#include "timebase.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/protocol/dap_ids.hpp"
#include "xrdap/session/busy_reply.hpp"
#include "xrdap/session/session_owner.hpp"
#include "xrdap/vendor/dap_vendor.hpp"

namespace XrDap
{

/// @brief 命令执行器 / Command executor.
/// @tparam DapT libxr DAP 类（提供 Step、ExecuteExternal、SetCommandHook）/ libxr DAP
///         class (providing Step, ExecuteExternal and SetCommandHook).
template <typename DapT>
class Executor
{
 public:
  /// @brief 平台转发器：返回 true 表示命令已由平台处理并填好回复 / Platform forwarder:
  ///        returns true once the platform handled the command and filled the reply.
  /// @note 在 DAP 执行上下文里调用，已确认 t 拥有探针 / Called in the DAP execution
  ///       context after t was confirmed as the owner.
  using Forwarder = bool (*)(void* ctx, Transport t, const uint8_t* req, uint16_t len,
                             uint8_t* resp, uint16_t cap, uint16_t& out);

  /// @brief 构造并把命令钩子装到 dap 上 / Construct and install the command hook on
  ///        dap.
  /// @note 初始化时调用；参数的生命周期需长于本对象 / Call at initialisation; the
  ///       arguments must outlive this object.
  Executor(DapT& dap, SessionOwner& session, DapVendor& vendor, DapActivity& activity)
      : dap_(dap), session_(session), vendor_(vendor), activity_(activity)
  {
    dap_.SetCommandHook(&Executor::Hook, this);
  }

  /// @brief 设置平台转发器 / Set the platform forwarder.
  /// @note 初始化时、命令开始执行之前调用 / Call at initialisation, before commands run.
  void SetForwarder(Forwarder forwarder, void* ctx)
  {
    forwarder_ = forwarder;
    forwarder_ctx_ = ctx;
  }

  /// @brief USB：做一次 libxr DAP 的工作（等一条 USB 命令并执行）/ USB: do one unit of
  ///        libxr DAP work (wait for a USB command and run it).
  /// @param wait_ms 等命令的最长时间；锁在等待期间也被持有，其他传输最多多等这么久 /
  ///        Longest wait for a command; the lock is held while waiting, so other
  ///        transports wait at most this much longer.
  /// @note 平台的 DAP 线程（或单循环的主循环）里调用 / Call from the platform's DAP
  ///       thread (or the single main loop).
  LibXR::ErrorCode StepUsb(uint32_t wait_ms)
  {
    LibXR::Mutex::LockGuard guard(mutex_);
    current_ = Transport::USB;
    const LibXR::ErrorCode ec = dap_.Step(wait_ms);
    current_ = Transport::NONE;
    return ec;
  }

  /// @brief 别的传输：同步执行一条命令 / Other transports: run one command
  ///        synchronously.
  /// @note 传输自己的任务里调用，不能在中断里 / Call from the transport's own task, not
  ///       from an interrupt.
  void Execute(Transport t, const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
               uint16_t& out)
  {
    LibXR::Mutex::LockGuard guard(mutex_);
    current_ = t;
    out = 0U;
    (void)dap_.ExecuteExternal(req, len, resp, cap, out);
    current_ = Transport::NONE;
  }

  /// @brief 传输 t 断开（TCP 客户端关闭、串口链路停止、无线会话号变化）：t 是主人时结束
  ///        它的会话 / Transport t went away (TCP client closed, serial link stopped,
  ///        radio session id changed): end its session if t is the owner.
  /// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
  void EndSession(Transport t) { session_.End(t); }

  /// @brief t 是主人时刷新它的租期（例如无线 ping）/ Refresh t's lease if it is the
  ///        owner (for example a radio ping).
  /// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
  void TouchSession(Transport t)
  {
    session_.Touch(t, static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds()));
  }

 private:
  static bool Hook(void* ctx, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    return static_cast<Executor*>(ctx)->OnCommand(req, len, resp, cap, out);
  }

  // 在 DAP 执行上下文里、持锁时调用 / Called in the DAP execution context, under the
  // lock.
  bool OnCommand(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                 uint16_t& out)
  {
    if (req == nullptr || len < 1U)
    {
      return false;
    }
    const Transport t = current_;
    const uint8_t cmd = req[0];
    const auto now = static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds());
    if (!RunsWithoutOwner(cmd) && !session_.Admit(t, now))
    {
      WriteBusyReply(cmd, resp, cap, out);
      return true;
    }
    const bool wired = t == Transport::USB || t == Transport::SERIAL;
    if (forwarder_ != nullptr && forwarder_(forwarder_ctx_, t, req, len, resp, cap, out))
    {
      activity_.Observe(req, len, now, wired);
      return true;
    }
    switch (vendor_.OnCommand(req, len, resp, cap, out))
    {
      case DapVendor::Result::HANDLED:
        activity_.Observe(req, len, now, wired);
        return true;
      case DapVendor::Result::REFUSED:
        return true;
      case DapVendor::Result::PASS:
        break;
    }
    activity_.Observe(req, len, now, wired);
    if (cmd == Dap::kDisconnect)
    {
      // DAP_Disconnect 结束主人的会话，别的传输随后可以接管 / DAP_Disconnect ends the
      // owner's session; another transport may take over afterwards.
      session_.End(t);
    }
    return false;
  }

  DapT& dap_;
  SessionOwner& session_;
  DapVendor& vendor_;
  DapActivity& activity_;
  Forwarder forwarder_ = nullptr;
  void* forwarder_ctx_ = nullptr;

  LibXR::Mutex mutex_;
  Transport current_ = Transport::NONE;
};

}  // namespace XrDap
