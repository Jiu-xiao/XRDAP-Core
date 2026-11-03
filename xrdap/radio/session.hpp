// SPDX-License-Identifier: Apache-2.0
#pragma once

// 无线链路的会话规则（包见 packet.hpp）。纯逻辑，时间由调用方传入。
//
// 会话是经无线的一次主机会话，以 16 位会话号命名（0 = 没有会话）。主机探针每次新会话
// 取一个新的非零号。
//
// 主机（HostSession）：
//   H1 会话的第一条命令取新会话号，先发带它的 PING。
//   H2 会话打开、kHeartbeatUs 内没有发命令时，发一次带会话号的 PING（心跳）。
//   H3 DAP_Disconnect 或 kHostIdleUs 内电脑没有命令时会话结束，再发会话号为 0 的 PING
//      告诉远端。
// 远端（RemoteLease）：
//   R1 会话号为 0 的 PING 结束会话。
//   R2 带别的会话号的 PING 结束当前会话，以新号开始一个会话。
//   R3 主机的每个包（PING、COMMAND）续租；kLeaseUs 内一个都没有时会话结束。
//   R4 会话结束时清掉缓存的应答和在拼的命令，并停止会话开始的事（采样）。
//   R5 会话外的 COMMAND 不执行、不应答。
// 重复命令（AnswerCache）：会话内远端保留最后一个应答；同序号的命令用缓存应答，不再执行。
//
// Session rules of the radio link (see packet.hpp for the packets). Pure logic with the
// time passed in.
//
// A session is one host session over the radio, named by a 16-bit session id (0 = no
// session). The host probe picks a new non-zero id for every new session.
//
// Host (HostSession):
//   H1 The first command of a session gets a new id; a PING with it goes first.
//   H2 While a session is open and no command went out for kHeartbeatUs, a PING with the
//      id goes out (the heartbeat).
//   H3 The session ends on DAP_Disconnect or after kHostIdleUs without a command from
//      the PC; a PING with id 0 then tells the remote.
// Remote (RemoteLease):
//   R1 A PING with id 0 ends the session.
//   R2 A PING with another id ends the session and opens one with the new id.
//   R3 Every packet from the host (PING, COMMAND) renews the lease; kLeaseUs without one
//      ends the session.
//   R4 Ending a session clears the cached answer and the command being reassembled, and
//      stops what the session started (sampling).
//   R5 A COMMAND outside a session is not executed and not answered.
// Duplicates (AnswerCache): inside a session the remote keeps its last answer; a command
// with the same sequence is answered from the cache instead of being executed again.

#include <cstdint>

namespace XrDap::Radio
{

/// 心跳间隔（H2）/ Heartbeat interval (H2).
inline constexpr uint32_t kHeartbeatUs = 1'000'000;
/// 远端租期（R3）/ Remote lease (R3).
inline constexpr uint32_t kLeaseUs = 3'000'000;
/// 电脑多久没有命令主机结束会话（H3）/ PC silence after which the host ends the session
/// (H3).
inline constexpr uint32_t kHostIdleUs = 5'000'000;

/// 远端的会话租期 / The remote's session lease.
class RemoteLease
{
 public:
  /// 会话变化 / What changed.
  enum class Event : uint8_t
  {
    NONE,     ///< 无变化 / nothing changed
    ENDED,    ///< 会话结束（R1、R3）/ the session ended (R1, R3)
    REPLACED  ///< 旧会话结束，新会话开始（R2，或无会话时开始）/ the old session ended and
              ///< a new one began (R2, or a first one)
  };

  /// @brief 收到 PING / A PING arrived.
  Event OnPing(uint16_t session, uint32_t now_us);
  /// @brief 收到主机的 COMMAND 或 CONTROL 分片 / A COMMAND or CONTROL fragment arrived.
  /// @return 会话打开时为 true，即应处理该分片（R5）/ true while a session is open, i.e.
  ///         the fragment is to be processed (R5).
  bool OnCommand(uint32_t now_us);
  /// @brief 检查租期（R3），定期调用 / Check the lease (R3); call regularly.
  Event Tick(uint32_t now_us);

  /// 会话打开 / A session is open.
  bool Active() const { return session_ != 0U; }
  /// 当前会话号 / Current session id.
  uint16_t Session() const { return session_; }

 private:
  uint16_t session_ = 0;
  uint32_t heard_us_ = 0;
};

/// 主机的会话 / The host's session.
class HostSession
{
 public:
  /// 现在该发什么 / What to send now.
  enum class Action : uint8_t
  {
    NONE,       ///< 什么都不发 / nothing
    HEARTBEAT,  ///< 发 PING(id)（H2）/ send PING(id) (H2)
    END,        ///< 会话结束，发 PING(0)（H3）/ the session ended, send PING(0) (H3)
  };

  /// @param seed 第一个会话号的来源，例如启动后的微秒数 / Source of the first session id,
  ///        e.g. microseconds since start.
  explicit constexpr HostSession(uint32_t seed = 0) : next_(static_cast<uint16_t>(seed))
  {
  }

  /// @brief 一条命令要转发（H1）/ A command is about to be forwarded (H1).
  /// @return 新会话开始，应先发 PING(id) 时为 true / true when a new session begins and
  ///         a PING(id) must go first.
  bool OnCommand(uint32_t now_us);
  /// @brief 转发的命令发出去了，推迟心跳 / A forwarded command went out; defers the
  ///        heartbeat.
  void OnSent(uint32_t now_us) { sent_us_ = now_us; }
  /// @brief 会话结束（DAP_Disconnect）/ The session ends (DAP_Disconnect).
  /// @return 有会话要结束、应发 PING(0) 时为 true / true when a session ends and PING(0)
  ///         must go out.
  bool OnDisconnect();
  /// @brief 定期调用：心跳或空闲结束（H2、H3）/ Call regularly: heartbeat or idle end
  ///        (H2, H3).
  Action Tick(uint32_t now_us);

  /// 会话打开 / A session is open.
  bool Active() const { return session_ != 0U; }
  /// 当前会话号 / Current session id.
  uint16_t Session() const { return session_; }

 private:
  uint16_t session_ = 0;
  uint16_t next_;
  bool seeded_ = false;
  uint32_t command_us_ = 0;  // 电脑的上一条命令 / last command from the PC
  uint32_t sent_us_ = 0;     // 会话里上一次发包 / last packet sent in the session
};

/// 远端的应答缓存 / The remote's answer cache.
class AnswerCache
{
 public:
  /// @brief 这个序号的应答是否已缓存 / Whether the answer of this sequence is cached.
  bool Has(uint8_t sequence) const { return valid_ && sequence == sequence_; }
  /// @brief 记下缓存的是哪个序号的应答 / Note which sequence the cached answer belongs
  /// to.
  void Store(uint8_t sequence)
  {
    sequence_ = sequence;
    valid_ = true;
  }
  /// 会话结束时清空（R4）/ Cleared when a session ends (R4).
  void Clear() { valid_ = false; }

 private:
  uint8_t sequence_ = 0;
  bool valid_ = false;
};

}  // namespace XrDap::Radio
