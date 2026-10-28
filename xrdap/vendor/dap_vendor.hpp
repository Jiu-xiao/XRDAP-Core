// SPDX-License-Identifier: Apache-2.0
#pragma once

// XR 厂商扩展命令（0x80 到 0x85，协议版本 2）：多地址读写、指针链解引用、探针端周期采样。
// 还可挂一条附加命令（SetExtraCommand，由平台挂上，例如 XRDAP-ESP 的 Wi-Fi 配置）。由
// Executor 在执行上下文里调用，采样也在那里一帧一帧地做（Sample）。DAP_Connect 记下它来自
// 哪条链路；连接在哪条链路上、链路丢了怎么办见 OnLinkLost。协议见 docs/protocol.md 第 3
// 节。
//
// XR vendor extension commands (0x80 to 0x85, protocol version 2): multi-address
// read/write, pointer-chain dereference and probe-side periodic sampling. One extra
// command can be attached (SetExtraCommand, attached by the platform, e.g. XRDAP-ESP's
// Wi-Fi configuration). Executor calls it in the execution context, where sampling is
// also done one frame at a time (Sample). DAP_Connect notes the link it came from; which
// link the connection is on and what happens when the link is lost: see OnLinkLost. See
// docs/protocol.md section 3.

#include <atomic>
#include <cstdint>

#include "debug/swd.hpp"
#include "xrdap/session/transport.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/protocol.hpp"
#include "xrdap/vendor/sampler.hpp"
#include "xrdap/vendor/sampling.hpp"

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

  /// 附加命令的请求字节数，规则同 RequestLength / Request bytes of the extra command,
  /// the same rules as RequestLength.
  using ExtraLength = bool (*)(const uint8_t* req, uint16_t avail, uint16_t& len);

  /// @brief 把一个功能位并入 INFO / Merge one feature bit into INFO.
  /// @note 初始化时调用 / Call at initialisation.
  void AddFeature(uint32_t bit) { added_features_ |= bit; }

  /// @brief 挂一条附加命令，其 feature 并入 INFO 功能位 / Attach one extra command whose
  ///        feature bit is merged into INFO.
  /// @param cmd 命令字节，应在 0x85 之后 / Command byte, expected above 0x85.
  /// @param length 算这条命令的请求字节数，DAP_ExecuteCommands / DAP_QueueCommands 里要用
  ///        / Works out the request bytes of this command, needed inside
  ///        DAP_ExecuteCommands / DAP_QueueCommands.
  /// @param handler 至少有 2 字节回复空间时才被调用，回复不能超过给它的 cap /
  ///        Called only with at least 2 bytes of reply space; its reply must not exceed
  ///        the cap it is given.
  /// @note 初始化时、命令开始执行之前调用，只能挂一条 / Call at initialisation, before
  ///       commands run; only one is supported.
  void SetExtraCommand(uint8_t cmd, uint32_t feature, ExtraHandler handler, void* ctx,
                       ExtraLength length);

  /// 读取 USB 总线复位计数的函数 / Function returning the USB bus-reset count.
  using ResetCountFn = uint32_t (*)(void* ctx);

  /// @brief 设置 USB 复位计数来源：连接在 USB 上时计数变了，连接和采样作废 / Set the USB
  ///        reset-count source: when the connection is on USB and the count changes, the
  ///        connection and sampling lapse.
  /// @note 初始化时调用；fn 只能读一个计数，不能阻塞或取锁 / Call at initialisation; fn
  ///       must only read a counter and must not block or take a lock.
  void SetUsbResetSource(ResetCountFn fn, void* ctx)
  {
    usb_reset_fn_ = fn;
    usb_reset_ctx_ = ctx;
  }

  /// @brief 链路 t 丢了（TCP 断开、串口链路停止或太久没有请求、无线会话结束）：连接在 t
  ///        上时，连接和采样作废 / Link t is gone (TCP closed, serial link stopped or
  ///        silent too long, radio session ended): when the connection is on t, the
  ///        connection and sampling lapse.
  /// @note 任何任务，不能在中断里 / Any task, not from an interrupt.
  void OnLinkLost(Transport t)
  {
    if (connected_link_.load() == static_cast<uint32_t>(t))
    {
      Drop();
    }
  }

  /// @brief 当前连接在哪条链路上，没有连接为 NONE / The link the connection is on, NONE
  ///        when not connected.
  Transport ConnectedLink()
  {
    CheckUsbReset();
    return static_cast<Transport>(connected_link_.load());
  }

  /// @brief 下一条命令从哪条链路来 / Which link the next command comes from.
  /// @note 执行器在执行前调用 / Called by the executor before it runs a command.
  void SetCurrentLink(Transport t) { current_link_ = t; }

  /// @brief DAP_ExecuteCommands / DAP_QueueCommands 里一条厂商命令的请求字节数：0x80 到
  ///        0x85，以及挂上的附加命令 / Request bytes of one vendor command inside
  ///        DAP_ExecuteCommands / DAP_QueueCommands: 0x80 to 0x85, and the extra command
  ///        when attached.
  /// @return 不是这些命令或内容不完整时 false / False for other commands or incomplete
  ///         content.
  /// @note 纯解析，不取锁，任何上下文（包括中断）/ Pure parsing with no lock, any
  ///       context (interrupts included).
  bool RequestLength(const uint8_t* req, uint16_t avail, uint16_t& len) const;

 public:
  /// @param swd SWD 实现，生命周期需长于本对象 / SWD implementation; must outlive this
  ///        object.
  explicit DapVendor(LibXR::Debug::Swd& swd)
      : mem_(swd), sampler_(mem_, &DapVendor::KeepSampling, this)
  {
  }

  bool Connected();
  /// 采样发起时的链路代数是否还有效 / Whether the link epoch sampling started in is
  /// still valid.
  bool LinkAlive(uint32_t epoch);
  bool IsVendor(uint8_t cmd) const;
  bool CmdInfo(uint16_t len, uint8_t* resp, uint16_t& out);
  /// @param sampling 正在采样：沿用采样稳定的 CSW 位 / Sampling is running: reuse its
  ///        stable CSW bits.
  bool CmdRead(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out,
               bool sampling);
  bool CmdWrite(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out);

  /// USB 复位计数变了且连接在 USB 上：链路丢了 / The USB reset count changed while the
  /// connection is on USB: the link is gone.
  void CheckUsbReset();
  /// 连接和采样就此作废 / The connection and sampling lapse here.
  void Drop();

  Vendor::MemAp mem_;

  /// 当前这条命令从哪条链路来，Executor 在执行前设置；只在 DAP 执行上下文里读写 /
  /// Which link the current command came from, set by the Executor before it runs; read
  /// and written in the DAP execution context only.
  Transport current_link_ = Transport::NONE;
  /// DAP_Connect 成功的链路，NONE 表示没有连接 / The link whose DAP_Connect succeeded,
  /// NONE when not connected.
  std::atomic<uint32_t> connected_link_{static_cast<uint32_t>(Transport::NONE)};
  /// 链路代数：连接作废（DAP_Disconnect、链路丢了、USB 复位）就加一，采样记着开始时的
  /// 代数，变了就停 / Link epoch: incremented whenever the connection lapses
  /// (DAP_Disconnect, link lost, USB reset); sampling remembers the epoch it started in
  /// and stops once it changes.
  std::atomic<uint32_t> epoch_{1U};
  /// 连接成功时的代数 / The epoch at the successful connection.
  uint32_t connected_epoch_ = 0U;
  ResetCountFn usb_reset_fn_ = nullptr;
  void* usb_reset_ctx_ = nullptr;
  uint32_t usb_reset_base_ = 0U;

  uint8_t extra_cmd_ = 0U;
  uint32_t extra_feature_ = 0U;
  uint32_t added_features_ = 0U;
  ExtraHandler extra_handler_ = nullptr;
  ExtraLength extra_length_ = nullptr;
  void* extra_ctx_ = nullptr;

 public:
  /// @brief 处理一条命令 / Handle one command.
  /// @note 只在 libxr DAP 执行上下文里调用（Executor 的钩子）/ Call only in the libxr DAP
  ///       execution context (Executor's hook).
  Result OnCommand(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                   uint16_t& out);

  /// @brief 执行上下文每一轮调用：到了要采的节拍就采一帧 / Called once per round of the
  ///        execution context: samples one frame when a tick is due.
  /// @return true 时下一个要采的节拍已经到了 / True when the next tick is already due.
  bool Sample(Vendor::TickTimer& timer) { return sampler_.Sample(timer); }

  /// 是否有节拍到期要采 / Whether a tick is due.
  bool SampleDue() const { return sampler_.Due(); }

  /// @brief 是否正在采样 / Whether sampling runs.
  /// @note 任何上下文 / Any context.
  bool Sampling() const { return sampler_.Active(); }

  /// @brief 设置叫醒执行上下文的函数，节拍到了时由节拍中断调用；不设置时中断只计数 /
  ///        Set the function that wakes the execution context, called by the tick
  ///        interrupt when a tick is due; without it the interrupt only counts.
  /// @note 初始化时调用 / Call at initialisation.
  void SetWake(Vendor::WakeFn fn, void* ctx) { sampler_.SetWake(fn, ctx); }

  /// @brief 节拍定时器中断调用 / Called by the tick timer interrupt.
  /// @note 中断上下文 / Interrupt context.
  void OnTickIsr(uint32_t elapsed_ticks = 1U) { sampler_.OnTickIsr(elapsed_ticks); }

 private:
  static bool KeepSampling(void* ctx, uint32_t epoch);

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

  Vendor::Sampler sampler_;
};

}  // namespace XrDap
