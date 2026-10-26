// SPDX-License-Identifier: Apache-2.0
#pragma once

// XR 厂商扩展命令（0x80 到 0x85，协议版本
// 2）：多地址读写、指针链解引用、探针端周期采样。还可挂一条
// 附加命令（SetExtraCommand，例如 XRDAP-ESP 的 0x86 Wi-Fi 配置）。由 Executor 在 libxr
// DAP 执行上下文里调用，命令属于哪条传输、能不能执行由 SessionOwner 决定。协议见
// docs/protocol.md 第 3 节。
//
// 采样有两种运行方式，平台二选一：单循环的探针在主循环里调用 Poll()；有 RTOS 的探针建一个
// 任务运行 RunSamplerTask()。两种方式都要平台的节拍定时器，它的中断调用 OnTickIsr()。
// 采样和 DAP_Connect 状态都记在会话编号上，会话结束即停。
//
// XR vendor extension commands (0x80 to 0x85, protocol version 2): multi-address
// read/write, pointer-chain
// dereference and probe-side periodic sampling. One extra command can be attached
// (SetExtraCommand, e.g. XRDAP-ESP's 0x86 Wi-Fi configuration). Executor calls it in the
// libxr DAP execution context; SessionOwner decides which transport a command belongs to
// and whether it runs. See docs/protocol.md section 3.
//
// Sampling runs one of two ways, chosen by the platform: a single-loop probe calls
// Poll() in its main loop; a probe with an RTOS creates a task that runs
// RunSamplerTask(). Both need the platform's tick timer, whose interrupt calls
// OnTickIsr().
// Sampling and the DAP_Connect state are tied to the session number and lapse when the
// session ends.

#include <atomic>
#include <cstdint>

#include "debug/swd.hpp"
#include "xrdap/session/session_owner.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/sampler.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

namespace XrDap
{

/// XR 厂商扩展命令处理器 / XR vendor extension command handler.
class DapVendor
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

  /// @param swd SWD 实现，生命周期需长于本对象 / SWD implementation; must outlive this
  ///        object.
  /// @param session 会话主人，生命周期需长于本对象 / Session owner; must outlive this
  ///        object.
  DapVendor(LibXR::Debug::Swd& swd, SessionOwner& session)
      : session_(session), mem_(swd), sampler_(mem_, &DapVendor::KeepSampling, this)
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
  /// @note 与 DAP 执行在同一个线程 / Same thread as the DAP execution.
  void Poll(Vendor::TickTimer& timer) { sampler_.Poll(timer); }

  /// @brief 是否正在采样，单循环主循环据此不在等命令时阻塞 / Whether sampling runs, so a
  ///        single main loop does not block waiting for commands.
  /// @note 任何上下文 / Any context.
  bool Sampling() const { return sampler_.Active(); }
  Vendor::MemAp::Statistics MemoryStats() const { return mem_.Stats(); }
  Vendor::Sampler::Statistics SamplingStats() const { return sampler_.Stats(); }

  /// @brief 任务：采样任务的函数体，不返回 / Task: body of the sampling task; never
  ///        returns.
  /// @note 平台建的采样任务里调用；任务应为 SWD 所在核上优先级最高的任务 / Call from the
  ///       platform's sampling task, which should have the highest priority on the core
  ///       that drives SWD.
  [[noreturn]] void RunSamplerTask(Vendor::TickTimer& timer) { sampler_.RunTask(timer); }

  /// @brief 一条命令开始等执行 / A command starts waiting to run.
  /// @note 执行器在命令排队等执行轮次之前调用 / Called by the executor before the command
  ///       queues for an execution turn.
  void CommandBegin() { sampler_.CommandBegin(); }

  /// Mark the current frame fetch/status command as non-blocking for sampling.
  void CommandReadOnly() { sampler_.CommandReadOnly(); }

  /// @brief 一条命令执行完 / A command has run.
  /// @note 执行器在交出执行轮次之后调用 / Called by the executor after it hands over the
  ///       execution turn.
  void CommandEnd() { sampler_.CommandEnd(); }

  /// @brief 节拍定时器中断调用 / Called by the tick timer interrupt.
  /// @note 中断上下文 / Interrupt context.
  void OnTickIsr(uint32_t elapsed_ticks = 1U, uint32_t late_us = 0U)
  {
    sampler_.OnTickIsr(elapsed_ticks, late_us);
  }

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

 private:
  static bool KeepSampling(void* ctx);

  bool Connected();
  bool IsVendor(uint8_t cmd) const;
  bool HandleVendor(uint8_t cmd, const uint8_t* req, uint16_t len, uint8_t* resp,
                    uint16_t cap, uint16_t& out);
  bool CmdInfo(uint16_t len, uint8_t* resp, uint16_t& out);
  bool CmdRead(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);
  bool CmdWrite(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);
  bool CmdSubStart(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);
  bool CmdSubCtrl(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);
  bool CmdSubRead(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                  uint16_t& out);

  SessionOwner& session_;
  Vendor::MemAp mem_;
  Vendor::Sampler sampler_;

  /// 收到 DAP_Connect 时的会话编号，0 表示没有连接 / Session number at DAP_Connect, 0
  /// when not connected.
  std::atomic<uint32_t> connected_session_{0U};
  /// 发起采样的会话编号 / Session number that started sampling.
  std::atomic<uint32_t> sampling_session_{0U};

  uint8_t extra_cmd_ = 0U;
  uint32_t extra_feature_ = 0U;
  ExtraHandler extra_handler_ = nullptr;
  void* extra_ctx_ = nullptr;
};

}  // namespace XrDap
