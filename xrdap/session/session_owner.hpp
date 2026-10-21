// SPDX-License-Identifier: Apache-2.0
#pragma once

// 会话主人：同一时间只有一条传输（USB、串口、TCP、无线）拥有探针和 SWD。第一条执行非
// DAP_Info 命令的传输成为主人，此后别的传输的命令一律回“忙”（DAP_Info 照常执行），直到
// 主人的会话结束：主人发了 DAP_Disconnect、平台报告这条传输断开（End）、USB 总线复位，
// 或有租期的传输（串口、无线）超过租期没有动静。每个会话有一个编号，采样和 DAP_Connect
// 状态都记在会话编号上，会话结束它们随之失效。协议见 docs/protocol.md 第 3.1 节。
//
// Session owner: at any time one transport (USB, serial, TCP, radio) owns the probe and
// SWD. The first transport that runs a command other than DAP_Info becomes the owner;
// from then on commands from any other transport are answered busy (DAP_Info still runs)
// until the owner's session ends: the owner sent DAP_Disconnect, the platform reports the
// transport gone (End), the USB bus reset, or a leased transport (serial, radio) stayed
// silent past its lease. Every session has a number; sampling and the DAP_Connect state
// are tied to it and lapse when it ends. See docs/protocol.md section 3.1.

#include <cstddef>
#include <cstdint>

#include "xrdap_port.hpp"

namespace XrDap
{

/// 驱动探针的传输 / Transport that drives the probe.
enum class Transport : uint8_t
{
  NONE = 0,    ///< 没有主人 / No owner.
  USB = 1,     ///< USB 接口 / USB interface.
  SERIAL = 2,  ///< 串口链路 / Serial link.
  TCP = 3,     ///< TCP 连接（含 USB/IP）/ TCP connection (including USB/IP).
  RADIO = 4,   ///< 无线对端（远端探针上）/ Radio peer (on the remote probe).
};

/// 传输种类数 / Number of transport kinds.
inline constexpr size_t kTransportCount = 5U;

/// 串口的默认租期 / Default serial lease.
inline constexpr uint32_t kSerialLeaseMs = 5000U;

/// 会话主人 / Session owner.
/// @note 所有方法都可以在任何任务里调用（内部用 Port::SpinLock 保护，持有时间很短），
///       但不能在中断里调用 / Every method may be called from any task (guarded inside
///       by a short Port::SpinLock hold) but not from an interrupt.
class SessionOwner
{
 public:
  /// 读取 USB 总线复位计数的函数 / Function returning the USB bus-reset count.
  using ResetCountFn = uint32_t (*)(void* ctx);

  /// @brief 设置一种传输的租期：超过这么久没有动静，会话结束；0 表示没有租期 / Set
  ///        the lease of a transport kind: its session ends after this long without
  ///        activity; 0 means no lease.
  /// @note 初始化时调用 / Call at initialisation.
  void SetLease(Transport t, uint32_t lease_ms);

  /// @brief 设置 USB 复位计数来源；计数变化即结束 USB 会话 / Set the USB reset-count
  ///        source; a changed count ends the USB session.
  /// @note 初始化时调用；fn 会在任何调用本类的任务里被调用 / Call at initialisation; fn
  ///       is called from whichever task calls this class.
  void SetUsbResetSource(ResetCountFn fn, void* ctx);

  /// @brief 一条命令要执行：没有主人时 t 成为主人，t 是主人时刷新租期 / A command is
  ///        about to run: t becomes the owner when there is none, and refreshes its
  ///        lease when it is the owner.
  /// @return t 可以执行时 true，别的传输拥有探针时 false（回忙）/ True when t may run,
  ///         false when another transport owns the probe (answer busy).
  bool Admit(Transport t, uint32_t now_ms);

  /// @brief t 是主人时刷新租期（例如无线 ping）/ Refresh the lease when t is the owner
  ///        (for example a radio ping).
  void Touch(Transport t, uint32_t now_ms);

  /// @brief t 是主人时结束它的会话（断开、DAP_Disconnect、无线会话号变化）/ End t's
  ///        session when t is the owner (closed, DAP_Disconnect, radio session id
  ///        changed).
  void End(Transport t);

  /// @brief 编号为 session 的会话是否还在 / Whether session number `session` is still
  ///        alive.
  /// @note 顺带检查租期和 USB 复位，过期的会话在这里结束 / Also checks the lease and the
  ///       USB reset; an expired session ends here.
  bool Alive(uint32_t session, uint32_t now_ms);

  /// 当前会话编号，没有会话时为 0 / Current session number, 0 without a session.
  uint32_t Session() const;

  /// 当前主人 / Current owner.
  Transport Owner() const;

 private:
  void ExpireLocked(uint32_t now_ms);
  void EndLocked();

  mutable Port::SpinLock lock_;
  uint32_t lease_ms_[kTransportCount] = {};
  ResetCountFn usb_reset_fn_ = nullptr;
  void* usb_reset_ctx_ = nullptr;

  Transport owner_ = Transport::NONE;
  uint32_t session_ = 0U;
  uint32_t next_session_ = 1U;
  uint32_t last_ms_ = 0U;
  uint32_t usb_reset_base_ = 0U;
};

}  // namespace XrDap
