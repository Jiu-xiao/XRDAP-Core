// SPDX-License-Identifier: Apache-2.0
#pragma once

// XR 厂商扩展命令（0x80 到 0x85，协议版本
// 2）：多地址读写、指针链解引用、探针端周期采样。还可挂一条
// 附加命令（SetExtraCommand，例如 XRDAP-ESP 的 0x86 Wi-Fi 配置）。由 Executor 在 libxr
// DAP 执行上下文里调用，命令属于哪条传输、能不能执行由 SessionOwner 决定。协议见
// docs/protocol.md 第 3 节。
//
// 采样有两种运行方式，平台在 xrdap_port.hpp 里用 Port::SamplerPolicy 二选一，编译期决定：
// 单循环的探针（LoopSampling）在主循环里调用 Poll()；有 RTOS 的探针（TaskSampling）建一个
// 任务运行 RunSamplerTask()。调用另一种方式的入口是编译错误。两种方式都要平台的节拍定时器，
// 它的中断调用 OnTickIsr()。采样和 DAP_Connect 状态都记在会话编号上，会话结束即停。
//
// XR vendor extension commands (0x80 to 0x85, protocol version 2): multi-address
// read/write, pointer-chain
// dereference and probe-side periodic sampling. One extra command can be attached
// (SetExtraCommand, e.g. XRDAP-ESP's 0x86 Wi-Fi configuration). Executor calls it in the
// libxr DAP execution context; SessionOwner decides which transport a command belongs to
// and whether it runs. See docs/protocol.md section 3.
//
// Sampling runs one of two ways, chosen by the platform at compile time with
// Port::SamplerPolicy in xrdap_port.hpp: a single-loop probe (LoopSampling) calls Poll()
// in its main loop; a probe with an RTOS (TaskSampling) creates a task that runs
// RunSamplerTask(). Calling the other way's entry is a compile error. Both need the
// platform's tick timer, whose interrupt calls OnTickIsr().
// Sampling and the DAP_Connect state are tied to the session number and lapse when the
// session ends.

#include <cstdint>
#include <type_traits>

#include "debug/swd.hpp"
#include "xrdap/session/session_owner.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/sampler.hpp"
#include "xrdap/vendor/sampler_policy.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"
#include "xrdap_port.hpp"

namespace XrDap
{

/// 与采样运行方式无关的命令处理：READ、WRITE、INFO、附加命令和长度检查 / Command
/// handling that does not depend on how sampling runs: READ, WRITE, INFO, the extra command
/// and the length check.
class DapVendorBase
{
 public:
  /// 一条命令的处理结果 / Outcome of one command.
  enum class Result : uint8_t
  {
    PASS,     ///< 标准命令，交给 libxr 执行 / Standard command, left to libxr.
    HANDLED,  ///< 已执行并填好回复 / Executed, reply filled in.
    REFUSED,  ///< 采样期间不执行，已回 0xFF / Not run while sampling, answered 0xFF.
  };

  /// 附加命令处理函数，返回 true 表示已填好回复 / Extra command handler; returns true
  /// once the reply is filled in.
  using ExtraHandler = bool (*)(void* ctx, const uint8_t* req, uint16_t len,
                                uint8_t* resp, uint16_t cap, uint16_t& out);

  /// @brief 挂一条附加命令，其 feature 并入 INFO 功能位 / Attach one extra command whose
  ///        feature bit is merged into INFO.
  /// @param cmd 命令字节，应在 0x85 之后 / Command byte, expected above 0x85.
  /// @param handler 至少有 2 字节回复空间时才被调用，回复不能超过给它的 cap /
  ///        Called only with at least 2 bytes of reply space; its reply must not exceed
  ///        the cap it is given.
  /// @note 初始化时、命令开始执行之前调用，只能挂一条 / Call at initialisation, before
  ///       commands run; only one is supported.
  void SetExtraCommand(uint8_t cmd, uint32_t feature, ExtraHandler handler, void* ctx);

  /// @brief DAP_ExecuteCommands / DAP_QueueCommands 里一条厂商命令的请求字节数：0x80 到
  ///        0x85，以及挂上的 0x86 / Request bytes of one vendor command inside
  ///        DAP_ExecuteCommands / DAP_QueueCommands: 0x80 to 0x85, and 0x86 when
  ///        attached.
  /// @return 不是这些命令或内容不完整时 false / False for other commands or incomplete
  ///         content.
  /// @note 纯解析，不取锁，任何上下文（包括中断）/ Pure parsing with no lock, any
  ///       context (interrupts included).
  bool RequestLength(const uint8_t* req, uint16_t avail, uint16_t& len) const;

 protected:
  DapVendorBase(LibXR::Debug::Swd& swd, SessionOwner& session)
      : session_(session), mem_(swd)
  {
  }
  ~DapVendorBase() = default;

  bool Connected();
  bool IsVendor(uint8_t cmd) const;
  bool CmdInfo(uint16_t len, uint8_t* resp, uint16_t& out);
  /// @param sampling 正在采样：沿用采样稳定的 CSW 位 / Sampling is running: reuse its
  ///        stable CSW bits.
  bool CmdRead(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out,
               bool sampling);
  bool CmdWrite(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);

  SessionOwner& session_;
  Vendor::MemAp mem_;

  /// 收到 DAP_Connect 时的会话编号，0 表示没有连接；只在 DAP 执行上下文里读写 / Session
  /// number at DAP_Connect, 0 when not connected; read and written in the DAP execution
  /// context only.
  uint32_t connected_session_ = 0U;

  uint8_t extra_cmd_ = 0U;
  uint32_t extra_feature_ = 0U;
  ExtraHandler extra_handler_ = nullptr;
  void* extra_ctx_ = nullptr;
};

/// @brief XR 厂商扩展命令处理器；Policy 是采样的运行策略（TaskSampling 或 LoopSampling）/
///        XR vendor extension command handler; Policy is the sampling policy
///        (TaskSampling or LoopSampling).
/// @note 平台用 DapVendor（即 DapVendorT<Port::SamplerPolicy>）/ Platforms use DapVendor,
///       which is DapVendorT<Port::SamplerPolicy>.
template <class Policy>
class DapVendorT : public DapVendorBase
{
 public:
  /// @param swd SWD 实现，生命周期需长于本对象 / SWD implementation; must outlive this
  ///        object.
  /// @param session 会话主人，生命周期需长于本对象 / Session owner; must outlive this
  ///        object.
  DapVendorT(LibXR::Debug::Swd& swd, SessionOwner& session)
      : DapVendorBase(swd, session), sampler_(mem_, &DapVendorT::KeepSampling, this)
  {
  }

  /// @brief 处理一条命令 / Handle one command.
  /// @note 只在 libxr DAP 执行上下文里调用（Executor 的钩子），调用方已确认这条命令的
  ///       传输拥有探针 / Call only in the libxr DAP execution context (Executor's hook),
  ///       after the caller confirmed the command's transport owns the probe.
  Result OnCommand(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                   uint16_t& out);

  /// @brief 单循环：主循环在两条 DAP 命令之间调用，新节拍到了就采一拍 / Single loop:
  ///        call between DAP commands in the main loop; samples a tick once a new one
  ///        came.
  /// @note 与 DAP 执行在同一个线程；任务策略下没有这个函数 / Same thread as the DAP
  ///       execution; not available under the task policy.
  template <class P = Policy, std::enable_if_t<!P::kRunsInTask, int> = 0>
  void Poll(Vendor::TickTimer& timer)
  {
    sampler_.Poll(timer);
  }

  /// @brief 是否正在采样，单循环主循环据此不在等命令时阻塞 / Whether sampling runs, so a
  ///        single main loop does not block waiting for commands.
  /// @note 任务策略：任何上下文；单循环策略：只在主循环里 / Task policy: any context;
  ///       single-loop policy: the main loop only.
  bool Sampling() const { return sampler_.Active(); }

  /// @brief 任务：采样任务的函数体，不返回 / Task: body of the sampling task; never
  ///        returns.
  /// @note 平台建的采样任务里调用；任务应为 SWD 所在核上优先级最高的任务；单循环策略下没有
  ///       这个函数 / Call from the platform's sampling task, which should have the highest
  ///       priority on the core that drives SWD; not available under the single-loop
  ///       policy.
  template <class P = Policy, std::enable_if_t<P::kRunsInTask, int> = 0>
  [[noreturn]] void RunSamplerTask(Vendor::TickTimer& timer)
  {
    sampler_.RunTask(timer);
  }

  /// @brief 一条命令开始等执行（单循环策略下是空函数）/ A command starts waiting to run
  ///        (an empty function under the single-loop policy).
  /// @note 执行器在命令排队等执行轮次之前调用 / Called by the executor before the command
  ///       queues for an execution turn.
  void CommandBegin() { sampler_.CommandBegin(); }

  /// Mark the current frame fetch/status command as non-blocking for sampling.
  void CommandReadOnly() { sampler_.CommandReadOnly(); }

  /// @brief 一条命令执行完（单循环策略下是空函数）/ A command has run (an empty function
  ///        under the single-loop policy).
  /// @note 执行器在交出执行轮次之后调用 / Called by the executor after it hands over the
  ///       execution turn.
  void CommandEnd() { sampler_.CommandEnd(); }

  /// @brief 节拍定时器中断调用 / Called by the tick timer interrupt.
  /// @note 中断上下文 / Interrupt context.
  void OnTickIsr(uint32_t elapsed_ticks = 1U) { sampler_.OnTickIsr(elapsed_ticks); }

 private:
  static bool KeepSampling(void* ctx, uint32_t session);

  bool HandleVendor(uint8_t cmd, const uint8_t* req, uint16_t len, uint8_t* resp,
                    uint16_t cap, uint16_t& out);
  bool CmdSubStart(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);
  bool CmdSubCtrl(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);
  /// @brief 取走采样缓冲里的帧，回复带头和帧 / Take frames from the sampling buffer; the
  ///        reply carries the head and the frames.
  /// @param req 请求 / Request.
  /// @param len 请求长度 / Request length.
  /// @param resp 回复缓冲 / Reply buffer.
  /// @param cap 回复缓冲容量 / Capacity of the reply buffer.
  /// @param out 回复长度 / Reply length.
  /// @note 不在采样时状态为 0x02，回复照样带头和剩下的帧，上位机据此知道采样已停（例如
  ///       会话结束）/ While not sampling the status is 0x02 and the reply still carries
  ///       the head and the frames left, so the host learns that sampling stopped (for
  ///       example because the session ended).
  bool CmdSubRead(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                  uint16_t& out);

  Vendor::SamplerT<Policy> sampler_;
};

/// 平台选定的采样策略下的命令处理器 / The command handler under the platform's chosen
/// sampling policy.
using DapVendor = DapVendorT<Port::SamplerPolicy>;

}  // namespace XrDap
