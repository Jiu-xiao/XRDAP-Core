// SPDX-License-Identifier: Apache-2.0
#pragma once

// 远端一侧：接收命令、执行、发应答。会话规则见 session.hpp。
//
// 远端收到会话号为 0 的 PING、新会话号或 3 s
// 没有包时结束会话，并停止会话开始的采样。在数据 信道上 2.5 s
// 没听到主机，或会话结束时，自己回到控制信道。应答逐包发出，不阻塞主循环。
//
// The remote side: receive commands, execute them, send the answers. Session rules:
// session.hpp.
//
// The remote ends a session on a PING with id 0, on a new id, or after 3 s without a
// packet, and then stops the sampling the session started. It returns to the control
// channel on its own after 2.5 s on a data channel without hearing the host, and when a
// session ends. Answers go out packet by packet without blocking the main loop.

#include <cstdint>

#include "xrdap/radio/channel.hpp"
#include "xrdap/radio/packet.hpp"
#include "xrdap/radio/radio_port.hpp"
#include "xrdap/radio/session.hpp"
#include "xrdap/session/transport.hpp"

namespace XrDap::Radio
{

/// @brief 远端一侧 / The remote side.
/// @tparam ExecT 命令执行器（Executor<DapT>）/ Command executor (Executor<DapT>).
template <typename RadioT, typename ExecT>
class RemoteLink
{
 public:
  /// 应答发出后要做的事（等着的设置变更）/ What to do once an answer is out (a pending
  /// settings change).
  using AnswerDone = void (*)(void* ctx);

  RemoteLink(RadioPort<RadioT>& port, ExecT& exec, AnswerDone answer_done, void* ctx)
      : port_(port), exec_(exec), answer_done_(answer_done), ctx_(ctx)
  {
  }

  /// 主循环每轮调用 / Call every round of the main loop.
  void Serve()
  {
    if (PumpAnswer())
    {
      return;
    }
    RadioT& radio = port_.Radio();
    uint8_t* const rx = port_.Rx();
    radio.Poll();
    const int n = radio.Receive(rx);
    const uint32_t now = NowUs();
    if (n < kHeader)
    {
      if (lease_.Tick(now) == RemoteLease::Event::ENDED)
      {
        EndSession();
      }
      if (port_.Channel() != kControlChannel && now - heard_host_us_ >= kChannelSilenceUs)
      {
        port_.TuneTo(
            kControlChannel);  // 主机不在这个信道上了 / the host left this channel
      }
      return;
    }
    heard_host_us_ = now;
    const uint8_t sequence = rx[1];
    if (rx[0] == kChannel)
    {
      ChangeChannel(n, sequence, now);
      return;
    }
    if (rx[0] == kPing)
    {
      AnswerPing(n, sequence, now);
      return;
    }
    const bool control = rx[0] == kControl;
    if ((rx[0] != kCommand && !control) || !lease_.OnCommand(now))  // R5
    {
      return;
    }
    if (!ReceiveCommand(n))
    {
      return;
    }
    if (cache_.Has(commands_.Sequence()))
    {
      // 主机没收到应答：重发缓存，不再执行一次。 / The host did not get the answer:
      // send it again instead of executing twice.
      ++duplicates_;
      StartAnswer(commands_.Sequence());
      return;
    }
    radio.PrepareSend();  // 切到 FS 与执行重叠 / the switch to FS overlaps the execution
    executing_ = true;
    uint16_t size = 0;
    if (control)
    {
      exec_.ControlHere(command_, commands_.Size(), answer_, sizeof(answer_), size);
    }
    else
    {
      exec_.ExecuteHere(Transport::RADIO, command_, commands_.Size(), answer_,
                        sizeof(answer_), size);
    }
    executing_ = false;
    answer_size_ = size;
    cache_.Store(commands_.Sequence());
    StartAnswer(commands_.Sequence());
  }

  /// 重启或停用射频前，先发完还在路上的应答 / Before the radio restarts or stops, an
  /// answer still on its way is finished.
  void FinishAnswer()
  {
    while (PumpAnswer())
    {
    }
  }

  /// R4：清掉会话留下的东西 / R4: drop what a session left behind.
  void Reset()
  {
    if (lease_.OnPing(0, NowUs()) != RemoteLease::Event::NONE)
    {
      EndSession();
    }
    commands_.Reset();
    cache_.Clear();
  }

  /// 应答发出后再调用 AnswerDone / Call AnswerDone once the answer is out.
  void DeferUntilAnswered() { done_pending_ = true; }

  /// 正在执行一条经无线来的请求 / A request from the radio is being executed.
  bool Executing() const { return executing_; }
  bool Answering() const { return answering_; }
  /// 会话打开（对方经无线在用本探针）/ A session is open (the peer drives this probe over
  /// the radio).
  bool Active() const { return lease_.Active(); }
  /// 从缓存应答的次数 / Answers sent from the cache.
  uint32_t Duplicates() const { return duplicates_; }

 private:
  // 在数据信道上这么久没听到主机就回 F：长于心跳（1 s）加一次扫描，短于租期（3 s），所以
  // 回 F 不结束会话。
  // Return to F after this long on a data channel without hearing the host: longer than
  // the heartbeat (1 s) plus a scan, shorter than the lease (3 s), so that returning to F
  // does not end the session.
  static constexpr uint32_t kChannelSilenceUs = 2'500'000;

  // R4：清掉结束的会话留下的东西，连接在无线上时作废。 / R4: drop what the ended session
  // left behind; a connection on the radio lapses.
  void EndSession()
  {
    commands_.Reset();
    cache_.Clear();
    exec_.LinkLost(Transport::RADIO);
    port_.TuneTo(kControlChannel);
  }

  void ChangeChannel(int n, uint8_t sequence, uint32_t now)
  {
    uint16_t session = 0;
    uint8_t channel = kNoChannel;
    if (!ParseChannel(port_.Rx(), n, session, channel) || !IsChannel(channel) ||
        session != lease_.Session() || !lease_.OnCommand(now))
    {
      return;
    }
    port_.Transmit(port_.Tx(),
                   BuildChannel(kChannelAck, sequence, session, channel, port_.Tx()));
    port_.FinishSending();
    port_.TuneTo(channel);
  }

  void AnswerPing(int n, uint8_t sequence, uint32_t now)
  {
    uint16_t session = 0;
    if (!ParsePing(port_.Rx(), n, session))
    {
      return;
    }
    const bool changed = lease_.OnPing(session, now) != RemoteLease::Event::NONE;
    // 先在 PING 来的信道上应答，再结束会话（结束时回 F）。 / Answer on the channel the
    // PING came on first, then end the session (which returns to F).
    port_.SendPing(kPong, sequence, lease_.Session());
    if (changed)
    {
      EndSession();
    }
  }

  // 收下命令的一个分片；其余分片紧跟着到，在这里接着收，不等主循环下一圈。命令齐了、这一串
  // 也发完了（到了最后一片，或 kPacketWaitUs 没有下一片）时为 true：重发可能在中途就把
  // 命令拼齐，而主机发射时听不到应答。
  // Take one fragment of a command; the others follow right behind it and are received
  // here instead of on the next turns of the main loop. True once the command is complete
  // and the burst is over (its last fragment arrived, or none for kPacketWaitUs): a
  // resend can complete the command halfway, and the host cannot hear an answer while it
  // transmits.
  bool ReceiveCommand(int n)
  {
    RadioT& radio = port_.Radio();
    uint8_t* const rx = port_.Rx();
    const uint8_t kind = rx[0];
    auto added = commands_.Add(rx, n, port_.Fragment());
    bool complete = IsComplete(added);
    bool last = IsLastFragment(rx);
    uint32_t deadline = NowUs() + kPacketWaitUs;
    while ((complete ? !last : added == Reassembler::Result::INCOMPLETE) &&
           static_cast<int32_t>(deadline - NowUs()) > 0)
    {
      radio.Poll();
      const int m = radio.Receive(rx);
      if (m < kHeader || rx[0] != kind || !lease_.OnCommand(NowUs()))
      {
        continue;
      }
      // 已拼齐的命令还在缓冲里等执行，别的消息不能覆盖它；主机会重发那条消息。
      // The completed command waits in the buffer and another message must not overwrite
      // it; the host sends that message again.
      if (complete && !commands_.IsSameMessage(rx))
      {
        break;
      }
      added = commands_.Add(rx, m, port_.Fragment());
      complete = complete || IsComplete(added);
      last = IsLastFragment(rx);
      deadline = NowUs() + kPacketWaitUs;
    }
    return complete;
  }

  // 拼齐了，或者是刚拼齐的消息的重发（从缓存应答）。 / Complete, or a resend of the
  // message just completed (answered from the cache).
  static bool IsComplete(Reassembler::Result result)
  {
    return result == Reassembler::Result::COMPLETE ||
           result == Reassembler::Result::DUPLICATE;
  }

  void StartAnswer(uint8_t sequence)
  {
    answer_sequence_ = sequence;
    answer_index_ = 0;
    answer_packet_ = 0;
    answer_count_ = FragmentCount(answer_size_, port_.Fragment());
    answering_ = true;
  }

  // 推进应答：上一包发完就发下一包，中间不回接收；全部发出后做等着的事。返回 true
  // 表示还没 发完。 Advance the answer: send the next packet once the previous one is
  // out, without receiving in between; when all are out, do what was waiting. Returns
  // true while the answer is not finished.
  bool PumpAnswer()
  {
    if (!answering_)
    {
      return false;
    }
    RadioT& radio = port_.Radio();
    if (answer_index_ < answer_count_)
    {
      if (answer_packet_ == 0U)
      {
        answer_packet_ = BuildFragment(kAnswer, answer_sequence_, answer_, answer_size_,
                                       port_.Fragment(), answer_index_, port_.Tx());
      }
      if (radio.Send(port_.Tx(), answer_packet_) != LibXR::ErrorCode::BUSY)
      {
        answer_packet_ = 0;
        ++answer_index_;
      }
      return true;
    }
    if (radio.Sending())
    {
      radio.Poll();
      return true;
    }
    answering_ = false;
    if (done_pending_)
    {
      done_pending_ = false;
      answer_done_(ctx_);
    }
    return false;
  }

  RadioPort<RadioT>& port_;
  ExecT& exec_;
  AnswerDone answer_done_;
  void* ctx_;

  uint8_t command_[kMaxMessage] = {};
  uint8_t answer_[kMaxMessage] = {};
  uint16_t answer_size_ = 0;
  Reassembler commands_{command_, sizeof(command_)};
  RemoteLease lease_;
  AnswerCache cache_;
  uint32_t heard_host_us_ = 0;  // 上一个主机的包 / last packet from the host
  uint32_t duplicates_ = 0;
  bool executing_ = false;
  bool done_pending_ = false;
  bool answering_ = false;
  uint8_t answer_sequence_ = 0;
  uint8_t answer_index_ = 0;
  uint8_t answer_count_ = 0;
  // Tx() 里待发的应答包长，0 为还没生成。 / Size of the answer packet waiting in Tx(), 0
  // before it is built.
  uint8_t answer_packet_ = 0;
};

}  // namespace XrDap::Radio
