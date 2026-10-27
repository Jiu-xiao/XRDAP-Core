// SPDX-License-Identifier: Apache-2.0
#pragma once

// 唯一的命令执行器：探针上所有进入 libxr DAP 实例的调用（USB 的 Step 和其他链路的
// ExecuteExternal）都经过这里，用一把互斥量保证一次只执行一条命令；执行时记下命令从哪条
// 链路来。探针一次只使用一条链路由使用者保证，这里不排队、不分主人、不回“忙”：同时有
// 两条链路发命令时，后拿到锁的等前一条执行完。USB 等命令时不持有锁（先在 libxr 的
// WaitForWork 里等，有命令了才拿锁执行 Step(0)）。
// 它装在 DAP 实例上的命令钩子先交给平台的转发器（例如 CH32 的无线），再交给厂商扩展，其余
// 的标准命令由 libxr 执行。执行过的命令记进 DapActivity。装钩子时也把厂商命令的长度函数
// 交给 libxr（SetCommandHook 的第三个参数），DAP_ExecuteCommands / DAP_QueueCommands 里
// 因此可以放厂商命令，与 CMSIS-DAP 参考实现相同。需要 libxr 1c06b874 或之后。
//
// The single command executor: every call into the libxr DAP instance on the probe (USB's
// Step and the other links' ExecuteExternal) goes through here, and one mutex keeps it to
// one command at a time; it notes which link the command came from. That the probe is used
// over one link at a time is up to the user, so nothing here queues, picks an owner or
// answers busy: when two links send commands at once, the one that gets the lock second
// waits for the first to finish. USB does not hold the lock while waiting for a command
// (it waits in libxr's WaitForWork first and takes the lock to run Step(0) once a command
// is there).
// The command hook it installs on the DAP instance first offers the command to the
// platform's forwarder (for example the CH32 radio), then to the vendor extension; libxr
// runs the remaining standard commands. Commands that ran are recorded in DapActivity. The
// hook is installed together with the vendor length function (the third argument of
// SetCommandHook), so vendor commands may sit inside DAP_ExecuteCommands /
// DAP_QueueCommands, as in the CMSIS-DAP reference implementation. Needs libxr 1c06b874 or
// later.

#include <cstdint>

#include "libxr_def.hpp"
#include "mutex.hpp"
#include "timebase.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/session/transport.hpp"
#include "xrdap/vendor/dap_vendor.hpp"

namespace XrDap
{

/// @brief 命令执行器 / Command executor.
/// @tparam DapT libxr DAP 类（提供 WaitForWork、Step、ExecuteExternal、带长度钩子的
///         SetCommandHook 和 SetSessionResetGate）/ libxr DAP class (providing
///         WaitForWork, Step, ExecuteExternal, SetCommandHook with the length hook and
///         SetSessionResetGate).
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
  }

  /// @brief 设置平台转发器 / Set the platform forwarder.
  /// @note 初始化时、命令开始执行之前调用 / Call at initialisation, before commands run.
  void SetForwarder(Forwarder forwarder, void* ctx)
  {
    forwarder_ = forwarder;
    forwarder_ctx_ = ctx;
  }

  /// @brief USB：等一条 USB 命令（或绑定、解绑），有了就拿锁执行 / USB: wait for a USB
  ///        command (or a bind or unbind) and take the lock to run it once there.
  /// @param wait_ms 等命令的最长时间；等待时不持有锁 / Longest wait for a command; the
  ///        lock is not held while waiting.
  /// @return OK 做了一件事；TIMEOUT 期限内没有命令 / OK when something was done;
  ///         TIMEOUT when no command came in time.
  /// @note 平台的 DAP 线程（或单循环的主循环）里调用 / Call from the platform's DAP
  ///       thread (or the single main loop).
  LibXR::ErrorCode StepUsb(uint32_t wait_ms)
  {
    if (dap_.WaitForWork(wait_ms) != LibXR::ErrorCode::OK)
    {
      return LibXR::ErrorCode::TIMEOUT;
    }
    Run run(*this, Transport::USB);
    return dap_.Step(0);
  }

  /// @brief 别的链路：拿锁，同步执行一条命令 / Other links: take the lock and run one
  ///        command synchronously.
  /// @note 链路自己的任务里调用，不能在中断里；调用前已收齐命令，不要在持有输入时等待 /
  ///       Call from the link's own task, not from an interrupt, with the command already
  ///       received; never wait for input inside it.
  void Execute(Transport t, const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
               uint16_t& out)
  {
    Run run(*this, t);
    out = 0U;
    (void)dap_.ExecuteExternal(req, len, resp, cap, out);
  }

  /// @brief 链路 t 丢了（TCP 客户端关闭、串口链路停止或太久没有请求、无线会话结束）：
  ///        连接在 t 上时，连接和采样作废 / Link t is gone (TCP client closed, serial link
  ///        stopped or silent too long, radio session ended): when the connection is on
  ///        t, the connection and sampling lapse.
  /// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
  void LinkLost(Transport t) { vendor_.OnLinkLost(t); }

 private:
  /// 一条命令的执行：构造时拿锁并记下链路，析构时放锁 / The execution of one command:
  /// takes the lock and notes the link on construction, releases the lock on
  /// destruction.
  class Run
  {
   public:
    Run(Executor& e, Transport t) : e_(e)
    {
      // 从等锁起就算在等：采样最多再读一帧就让路 / Waiting counts from the lock request
      // on: sampling reads at most one more frame, then yields.
      e_.vendor_.CommandBegin();
      e_.lock_.Lock();
      e_.vendor_.SetCurrentLink(t);
      e_.current_ = t;
    }
    ~Run()
    {
      e_.current_ = Transport::NONE;
      e_.lock_.Unlock();
      e_.vendor_.CommandEnd();
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

   private:
    Executor& e_;
  };

  static bool Hook(void* ctx, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    return static_cast<Executor*>(ctx)->OnCommand(req, len, resp, cap, out);
  }

  // USB 绑定或解绑后 libxr 要复位 SWD 的会话状态时来问：连接在 USB 上或没有连接时才复位，
  // 连接在别的链路上时它的 SWD 状态不动。在 Step 线程里、持锁时调用 / libxr asks before
  // resetting the SWD session state after a USB bind or unbind: reset only when the
  // connection is on USB or there is none; while it is on another link, its SWD state is
  // left alone. Called in the Step thread, under the lock.
  static bool MayResetSession(void* ctx)
  {
    const Transport link = static_cast<Executor*>(ctx)->vendor_.ConnectedLink();
    return link == Transport::USB || link == Transport::NONE;
  }

  // libxr 解析 DAP_ExecuteCommands / DAP_QueueCommands 时问厂商命令的长度。接收端扫描
  // 命令时也会调用，可能在 USB 中断里，所以只做解析：不取锁、不阻塞 / libxr asks for a
  // vendor command's length while parsing DAP_ExecuteCommands / DAP_QueueCommands. The
  // receive-side command scan calls it too, possibly in the USB interrupt, so it only
  // parses: no lock, no blocking.
  static bool LengthHook(void* ctx, const uint8_t* req, uint16_t avail, uint16_t& len)
  {
    return static_cast<Executor*>(ctx)->vendor_.RequestLength(req, avail, len);
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
  LibXR::Mutex lock_;
  Transport current_ = Transport::NONE;
};

}  // namespace XrDap
