// SPDX-License-Identifier: Apache-2.0
#pragma once

// 主机一侧：把一条命令发给远端并等应答。会话规则见 session.hpp。
//
// 会话走无线时，主机在 1 s 没有命令后发心跳 PING，DAP_Disconnect 或电脑 5 s 没有命令时
// 结束会话（会话号为 0 的 PING）。转发一条命令时阻塞到应答到达或超时。
//
// The host side: send one command to the remote and wait for the answer. Session rules:
// session.hpp.
//
// While a session runs over the radio the host sends a heartbeat PING after 1 s without a
// command, and ends the session (PING with id 0) on DAP_Disconnect or after 5 s without a
// command from the PC. It blocks while it forwards a command, until the answer arrives or
// the wait runs out.

#include <cstdint>

#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/radio/channel_picker.hpp"
#include "xrdap/radio/forwarding.hpp"
#include "xrdap/radio/packet.hpp"
#include "xrdap/radio/radio_port.hpp"
#include "xrdap/radio/session.hpp"
#include "xrdap/radio/settings.hpp"

namespace XrDap::Radio
{

/// @brief 主机一侧 / The host side.
template <typename RadioT>
class HostLink
{
 public:
  /// 主机的计数，从启动起累计 / The host's counters since start.
  struct Stats
  {
    uint32_t commands = 0;
    uint32_t retries = 0;
    uint32_t failures = 0;
    uint32_t last_rtt_us = 0;
    uint32_t max_rtt_us = 0;
  };

  HostLink(RadioPort<RadioT>& port, const Settings& settings)
      : port_(port), channels_(port, settings, session_)
  {
  }

  /// 需要时打开会话（H1），并确认对方在 / Open the session if needed (H1) and make sure
  /// the peer is there.
  bool ReachPeer()
  {
    const uint32_t now = NowUs();
    const bool fresh = session_.OnCommand(now);
    if (fresh || peer_lost_ || now - heard_peer_us_ >= kQuietUs)
    {
      // 新会话在 F 上建立；会话中先在当前信道上 PING（可能只丢了一个包），不通再回 F。
      // A new session opens on F; within a session ping on the current channel first (a
      // single packet may have been lost), and return to F only when that fails.
      if (!(!fresh && port_.Ping(session_.Session(), port_.kPingBudgetUs)) &&
          !channels_.Rendezvous())
      {
        return false;
      }
      peer_lost_ = false;
      heard_peer_us_ = NowUs();
      session_.OnSent(heard_peer_us_);
    }
    return true;
  }

  /// @brief 转发一条命令或控制请求 / Forward one command or control request.
  /// @return 拿到应答时为 true；否则 out 是失败应答 / true with the answer; otherwise out
  ///         holds a failure answer.
  bool Forward(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
               uint16_t& out, bool control)
  {
    // 控制请求不是 DAP 命令：没有 SWJ 时钟要记，也没有 DAP_Disconnect 结束会话。
    // A control request is not a DAP command: no SWJ clock to note, and no DAP_Disconnect
    // to end the session.
    const uint32_t hz = control ? 0U : SwjClockHz(req, len);
    if (hz != 0U)
    {
      swd_hz_ = hz;
    }
    if (!ReachPeer())
    {
      peer_lost_ = true;
      out = Fail(req, resp, cap);
      return false;
    }
    const bool disconnect = !control && IsCommand(req, len, DapCmd::DISCONNECT);
    if (!Exchange(req, len, resp, cap, out,
                  control ? kControlExecutionUs : ExecutionUs(req, len, swd_hz_),
                  !disconnect, control))
    {
      peer_lost_ = true;
      out = Fail(req, resp, cap);
      return false;
    }
    heard_peer_us_ = NowUs();
    session_.OnSent(heard_peer_us_);
    if (disconnect && session_.OnDisconnect())
    {
      EndSession();  // H3
    }
    else if (channels_.TakeReselect() && !channels_.Reselect())
    {
      peer_lost_ = true;
    }
    return true;
  }

  /// 主循环每轮调用：心跳和空闲结束（H2、H3）/ Call every round of the main loop: the
  /// heartbeat and the idle end (H2, H3).
  void Serve(uint32_t now)
  {
    switch (session_.Tick(now))
    {
      case HostSession::Action::HEARTBEAT:  // H2
        if (port_.Ping(session_.Session(), port_.kPingOnceUs))
        {
          heard_peer_us_ = NowUs();
        }
        else
        {
          peer_lost_ = true;
        }
        break;
      case HostSession::Action::END:  // H3
        EndSession();
        break;
      case HostSession::Action::NONE:
      default:
        break;
    }
  }

  /// 结束会话，回到 F / End the session and return to F.
  void Reset()
  {
    peer_lost_ = false;
    if (session_.OnDisconnect())
    {
      EndSession();
    }
    else if (port_.Running())
    {
      port_.TuneTo(kControlChannel);
    }
  }

  /// 放弃刚打开的会话：没人应答，不发 PING / Give up a session just opened: nobody
  /// answered, so no PING goes out.
  void Abandon() { (void)session_.OnDisconnect(); }

  bool Active() const { return session_.Active(); }
  uint8_t LockedChannel() const { return channels_.LockedChannel(); }
  const Stats& Counters() const { return stats_; }

 private:
  // 主机等待：第一次等命令的执行加第一个应答包，之后每次加倍，至多 kMaxWaitUs，直到总共
  // 过了 kBudgetUs 加执行时间。每到一个应答包就再等 kPacketWaitUs，长应答不会被截断。
  // 远端还在执行时重发的命令无害：远端从缓存应答。
  // Host waits: the first wait covers the command's execution plus the first answer
  // packet; every further wait doubles, up to kMaxWaitUs, until kBudgetUs (plus the
  // execution) has passed. Each answer packet that arrives extends the wait by
  // kPacketWaitUs, so a long answer is not cut off. A command sent again while the
  // remote still executes it is harmless: the remote answers it from its cache.
  static constexpr uint32_t kMaxWaitUs = 200'000;
  static constexpr uint32_t kBudgetUs = 1'000'000;
  // 控制请求可能让远端保存设置，写 Flash 时它听不到也不应答。第一次等待要盖住这段时间，
  // 否则下面的规则会在它还在写时换信道。应答一到就结束等待，不必等满。
  // A control request may make the remote store its settings; while it writes flash it
  // neither hears nor answers. The first wait covers that time, or the rule below changes
  // channel while it writes. The answer ends the wait as soon as it arrives.
  static constexpr uint32_t kControlExecutionUs = 250'000;
  // 收不到包的接收机：收发器可能停止接收而发射照常（在 SX1281 上见过）：它停在接收、什么
  // 都收不到，远端却听到每次重发并应答；重新进入接收没用，复位有用。信道被堵时表现相同。发了
  // kDeafSends 次、kDeafUs 内一个包（好坏都算）都没收到时，转发在数据信道上换信道，
  // 在 F 或锁定的信道上复位射频，一次之后继续。
  // A deaf receiver: a transceiver can stop receiving while its transmit still works
  // (seen on SX1281): it stays in receive and gets nothing, while the remote hears every
  // resend and answers it; entering receive again does not help, a reset does. A jammed
  // channel looks the same. After kDeafSends sends and kDeafUs without any packet, good
  // or bad, the forward changes channel when on a data channel, and resets the radio on F
  // or on a locked channel, once, and goes on.
  static constexpr uint32_t kDeafSends = 3;
  static constexpr uint32_t kDeafUs = 60'000;
  // 这么久没听到对方，下一条命令先 PING。 / After this long without hearing the peer,
  // the next command pings first.
  static constexpr uint32_t kQuietUs = 1'000'000;

  // DAP_ExecuteCommands 的子命令可能只剩一两个字节的空间。 / A sub-command of
  // DAP_ExecuteCommands may have only a byte or two of room left.
  static uint16_t Fail(const uint8_t* req, uint8_t* resp, uint16_t cap)
  {
    if (cap >= 1U)
    {
      resp[0] = req[0];
    }
    if (cap >= 2U)
    {
      resp[1] = Byte(DapStatus::ERROR);
    }
    return cap < 2U ? cap : 2U;
  }

  // 转发一条命令并等应答；1 s 加 execution_us 内收到完整应答时为 true。more 表示之后还有
  // 命令，应答到达后射频提前准备发射。
  // Forward one command and wait for its answer; true with a complete answer within 1 s
  // plus execution_us. `more` means more commands follow: the radio prepares to transmit
  // once the answer is in.
  bool Exchange(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                uint16_t& out, uint32_t execution_us, bool more, bool control)
  {
    if (!port_.Running() || len == 0U || len > kMaxMessage)
    {
      return false;
    }
    const uint8_t sequence = port_.NextSequence();
    ++stats_.commands;
    const uint32_t budget = kBudgetUs + execution_us;
    Reassembler answers(resp, cap);
    uint32_t wait = kPacketWaitUs + execution_us;
    const uint32_t begin = NowUs();
    uint32_t sends = 0;
    uint32_t heard = begin;  // 上一个包，好坏都算 / last packet, good or bad
    bool reset = false;
    RadioT& radio = port_.Radio();
    uint8_t* const rx = port_.Rx();
    while (NowUs() - begin < budget)
    {
      port_.SendMessage(control ? kControl : kCommand, sequence, req, len);
      ++sends;
      uint32_t deadline = NowUs() + wait;
      while (static_cast<int32_t>(deadline - NowUs()) > 0)
      {
        radio.Poll();
        const int n = radio.Receive(rx);
        if (n != 0)
        {
          heard = NowUs();
        }
        if (n < kHeader || rx[0] != kAnswer || rx[1] != sequence)
        {
          continue;
        }
        if (answers.Add(rx, n, port_.Fragment()) == Reassembler::Result::COMPLETE)
        {
          // 会话的下一条命令不必再从接收切出来。 / The next command of the session
          // goes out without the switch out of receive.
          if (more)
          {
            radio.PrepareSend();
          }
          out = answers.Size();
          stats_.retries += sends - 1U;
          stats_.last_rtt_us = NowUs() - begin;
          if (stats_.last_rtt_us > stats_.max_rtt_us)
          {
            stats_.max_rtt_us = stats_.last_rtt_us;
          }
          channels_.NoteExchange(sends > 1U);
          return true;
        }
        deadline = NowUs() + kPacketWaitUs;
      }
      if (!reset && sends >= kDeafSends && NowUs() - heard >= kDeafUs)
      {
        reset = true;
        if (port_.Channel() != kControlChannel && channels_.LockedChannel() == kNoChannel)
        {
          if (!channels_.Reselect())
          {
            peer_lost_ = true;
          }
          if (peer_lost_ && channels_.Rendezvous())
          {
            peer_lost_ = false;
          }
        }
        else
        {
          port_.Restart();
        }
        heard = NowUs();
      }
      wait = wait * 2U < kMaxWaitUs ? wait * 2U : kMaxWaitUs;
    }
    stats_.retries += sends - 1U;
    ++stats_.failures;
    channels_.NoteExchange(true);
    return false;
  }

  // 结束会话（PING 0，H3）并回 F。在数据信道上重发到对方应答，免得它留在那里等沉默超时；
  // F 上发一次。
  // End the session (PING 0, H3) and return to F. On a data channel it is sent until the
  // peer answers, so that the peer does not stay there until its silence timeout; on F it
  // is sent once.
  void EndSession()
  {
    const uint8_t from = port_.Channel();
    if (!port_.Ping(0,
                    from != kControlChannel ? port_.kPingBudgetUs : port_.kPingOnceUs) &&
        from != kControlChannel)
    {
      channels_.NoteStray(from, 0);
    }
    port_.TuneTo(kControlChannel);
  }

  RadioPort<RadioT>& port_;
  HostSession session_;
  ChannelPicker<RadioT> channels_;
  bool peer_lost_ = false;
  uint32_t heard_peer_us_ = 0;
  uint32_t swd_hz_ = kDefaultSwdHz;
  Stats stats_;
};

}  // namespace XrDap::Radio
