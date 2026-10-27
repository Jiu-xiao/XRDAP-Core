// SPDX-License-Identifier: Apache-2.0
#pragma once

// 唯一的命令执行器：探针上所有进入 libxr DAP 实例的调用（USB 的 Step 和其他传输的
// ExecuteExternal）都经过这里，每次只有一条命令在执行；执行时记下命令属于哪条传输。
// 执行的轮次先到先得、直接交给排在前面的（TurnQueue），只在执行一条命令时持有：USB
// 等命令时不持有（先在 libxr 的 WaitForWork 里等，有命令了才排队执行 Step(0)），所以
// DAP_Info 和“忙”回复不取决于别的传输什么时候空闲。
// 它装在 DAP 实例上的命令钩子对要拥有探针的命令（command_class.hpp）先问 SessionOwner
// 这条传输能不能执行（不能就回忙），再交给平台的转发器（例如 CH32 的无线）、厂商扩展，
// 其余的标准命令由 libxr 执行。执行过的命令记进 DapActivity。装钩子时也把厂商命令的
// 长度函数交给 libxr（SetCommandHook 的第三个参数），DAP_ExecuteCommands /
// DAP_QueueCommands 里因此可以放厂商命令，与 CMSIS-DAP 参考实现相同。需要 libxr
// 1c06b874 或之后。
//
// The single command executor: every call into the libxr DAP instance on the probe
// (USB's Step and the other transports' ExecuteExternal) goes through here, one command
// at a time, and it notes which transport the command belongs to. Turns are first come
// first served and handed straight to the next in line (TurnQueue), and are held only
// while one command runs: USB does not hold one while waiting for a command (it waits in
// libxr's WaitForWork first and queues to run Step(0) once a command is there), so
// DAP_Info and busy replies never depend on another transport's idle time.
// The command hook it installs on the DAP instance first asks SessionOwner, for a command
// that needs to own the probe (command_class.hpp), whether that transport may run (busy
// reply otherwise), then hands the command to the platform's forwarder (for example the
// CH32 radio) and the vendor extension; libxr runs the remaining standard commands.
// Commands that ran are recorded in DapActivity. The hook is installed together with the
// vendor length function (the third argument of SetCommandHook), so vendor commands may
// sit inside DAP_ExecuteCommands / DAP_QueueCommands, as in the CMSIS-DAP reference
// implementation. Needs libxr 1c06b874 or later.

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "libxr_def.hpp"
#include "semaphore.hpp"
#include "thread.hpp"
#include "timebase.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/session/busy_reply.hpp"
#include "xrdap/session/command_class.hpp"
#include "xrdap/session/session_owner.hpp"
#include "xrdap/session/transport_policy.hpp"
#include "xrdap/session/turn_queue.hpp"
#include "xrdap/vendor/dap_vendor.hpp"

namespace XrDap
{

/// @brief 命令执行器 / Command executor.
/// @tparam DapT libxr DAP 类（提供 WaitForWork、Step、ExecuteExternal、带长度钩子的
///         SetCommandHook 和 SetSessionResetGate）/ libxr DAP class (providing
///         WaitForWork, Step, ExecuteExternal, SetCommandHook with the length hook and
///         SetSessionResetGate).
/// @tparam Policy 传输策略（transport_policy.hpp），平台在 xrdap_port.hpp 里选 /
///         Transport policy (transport_policy.hpp), chosen by the platform in
///         xrdap_port.hpp.
template <typename DapT, typename Policy = Port::TransportPolicy>
class Executor
{
 public:
  /// 是否有两条以上传输 / Whether there are two or more transports.
  static constexpr bool kMultiTransport = Policy::kMulti;

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

  /// @brief 登记平台自己的、不需要拥有探针的命令 / Register the platform's own commands
  ///        that need no ownership.
  /// @note 初始化时、命令开始执行之前调用；表的生命周期需长于本对象 / Call at
  ///       initialisation, before commands run; the table must outlive this object.
  void SetFreeCommands(const FreeCommand* commands, size_t count)
  {
    static_assert(kMultiTransport, "a single-transport probe has no commands to classify");
    platform_free_ = commands;
    platform_free_count_ = count;
  }

  /// @brief USB：等一条 USB 命令（或绑定、解绑），有了就排队执行 / USB: wait for a USB
  ///        command (or a bind or unbind) and queue to run it once there.
  /// @param wait_ms 等命令的最长时间；等待时不占执行轮次 / Longest wait for a command;
  ///        no turn is held while waiting.
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
    if constexpr (kMultiTransport)
    {
      Turn turn(*this, Transport::USB);
      current_ = Transport::USB;
      const LibXR::ErrorCode ec = dap_.Step(0);
      current_ = Transport::NONE;
      return ec;
    }
    else
    {
      return dap_.Step(0);
    }
  }

  /// @brief 别的传输：排队等到轮次，同步执行一条命令 / Other transports: queue for a
  ///        turn and run one command synchronously.
  /// @note 传输自己的任务里调用，不能在中断里；调用前已收齐命令，不要在持有输入时等待 /
  ///       Call from the transport's own task, not from an interrupt, with the command
  ///       already received; never wait for input inside it.
  void Execute(Transport t, const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
               uint16_t& out)
  {
    static_assert(kMultiTransport, "a single-transport probe has only USB");
    Turn turn(*this, t);
    current_ = t;
    out = 0U;
    (void)dap_.ExecuteExternal(req, len, resp, cap, out);
    current_ = Transport::NONE;
  }

  /// @brief 传输 t 断开（TCP 客户端关闭、串口链路停止、无线会话号变化）：t 是主人时结束
  ///        它的会话 / Transport t went away (TCP client closed, serial link stopped,
  ///        radio session id changed): end its session if t is the owner.
  /// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
  void EndSession(Transport t)
  {
    static_assert(kMultiTransport, "a single-transport probe has only USB");
    session_.End(t);
  }

  struct CommandStatistics
  {
    uint32_t last_usb_owner_command;
    uint32_t last_tcp_owner_command;
    uint32_t busy_replies;
  };
  CommandStatistics CommandStats() const
  {
    return {last_usb_owner_command_.load(), last_tcp_owner_command_.load(),
            busy_replies_.load()};
  }

  /// @brief t 是主人时刷新它的租期（例如无线 ping）/ Refresh t's lease if it is the
  ///        owner (for example a radio ping).
  /// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
  void TouchSession(Transport t)
  {
    static_assert(kMultiTransport, "a single-transport probe has only USB");
    session_.Touch(t, static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds()));
  }

 private:
  /// 一条命令的执行轮次：构造时排队等到，析构时交给下一个 / The turn for one command:
  /// queued for and taken on construction, handed to the next on destruction.
  class Turn
  {
   public:
    Turn(Executor& e, Transport t) : e_(e)
    {
      // 从排队起就算在等：采样最多再读一帧就让路 / Waiting counts from queueing on:
      // sampling reads at most one more frame, then yields.
      e_.vendor_.CommandBegin();
      TurnQueue::Arrival a = e_.queue_.turns.Arrive(t);
      while (a == TurnQueue::Arrival::FULL)
      {
        LibXR::Thread::Sleep(1);
        a = e_.queue_.turns.Arrive(t);
      }
      if (a == TurnQueue::Arrival::QUEUED)
      {
        (void)e_.Wake(t).Wait(UINT32_MAX);
      }
    }
    ~Turn()
    {
      const Transport next = e_.queue_.turns.Leave();
      if (next != Transport::NONE)
      {
        e_.Wake(next).Post();
      }
      e_.vendor_.CommandEnd();
    }
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;

   private:
    Executor& e_;
  };

  LibXR::Semaphore& Wake(Transport t) { return queue_.wake[static_cast<uint8_t>(t)]; }

  static bool Hook(void* ctx, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    return static_cast<Executor*>(ctx)->OnCommand(req, len, resp, cap, out);
  }

  // USB 绑定或解绑后 libxr 要复位 SWD 的会话状态时来问：只在 USB 拥有探针或没有主人时
  // 复位，别的传输拥有探针时它的 SWD 状态不动。在 Step 线程里、执行轮次之内调用 /
  // libxr asks before resetting the SWD session state after a USB bind or unbind: reset
  // only when USB owns the probe or nobody does; while another transport owns it, its SWD
  // state is left alone. Called in the Step thread, within an execution turn.
  static bool MayResetSession(void* ctx)
  {
    const Transport owner = static_cast<Executor*>(ctx)->session_.Owner();
    return owner == Transport::USB || owner == Transport::NONE;
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
    const Transport t = kMultiTransport ? current_ : Transport::USB;
    const auto now = static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds());
    if constexpr (kMultiTransport)
    {
      const uint8_t cmd = req[0];
      if (ClassifyCommand(req, len, platform_free_, platform_free_count_) ==
          CommandClass::OWNER)
      {
        if (!session_.Admit(t, now))
        {
          busy_replies_.fetch_add(1U);
          WriteBusyReply(cmd, resp, cap, out);
          return true;
        }
        if (t == Transport::USB)
        {
          last_usb_owner_command_.store(cmd);
        }
        else if (t == Transport::TCP)
        {
          last_tcp_owner_command_.store(cmd);
        }
      }
    }
    else
    {
      // 只有 USB：每条命令都由它准入，没有会话时开始一个 / USB only: it admits every
      // command, starting a session when there is none.
      (void)session_.Admit(t, now);
    }
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

  // 一条命令执行了，不管由转发器、厂商扩展还是 libxr 执行：记进 DapActivity；
  // DAP_Disconnect 结束主人的会话，别的传输随后可以接管。
  // A command ran, whether the forwarder, the vendor extension or libxr ran it: record it
  // in DapActivity; DAP_Disconnect ends the owner's session, and another transport may
  // take over afterwards.
  void Ran(Transport t, const uint8_t* req, uint16_t len, uint32_t now)
  {
    activity_.Observe(req, len, now, t == Transport::USB || t == Transport::SERIAL);
    if (req[0] == Byte(DapCmd::DISCONNECT))
    {
      session_.End(t);
    }
  }

  DapT& dap_;
  SessionOwner& session_;
  DapVendor& vendor_;
  DapActivity& activity_;
  Forwarder forwarder_ = nullptr;
  void* forwarder_ctx_ = nullptr;

  const FreeCommand* platform_free_ = nullptr;
  size_t platform_free_count_ = 0U;
  /// 排队的状态，只有多传输才有 / Queueing state, present only with several transports.
  struct QueueState
  {
    TurnQueue turns;
    /// 每条传输一个，交接轮次时唤醒排队的任务 / One per transport; wakes the queued task
    /// on a hand-over.
    LibXR::Semaphore wake[static_cast<uint8_t>(Transport::RADIO) + 1U];
  };
  struct NoQueueState
  {
  };
  std::conditional_t<kMultiTransport, QueueState, NoQueueState> queue_;
  Transport current_ = Transport::NONE;
  std::atomic<uint32_t> last_usb_owner_command_{0xFFU};
  std::atomic<uint32_t> last_tcp_owner_command_{0xFFU};
  std::atomic<uint32_t> busy_replies_{0U};
};

}  // namespace XrDap
