// SPDX-License-Identifier: Apache-2.0
#pragma once

// 主机怎么选数据信道、怎么把对方一起带过去。选点的纯逻辑在 channel.hpp。
//
// 配对和建立会话都在控制信道 F 上。主机在 F 上找到对方后选一个数据信道（锁定的、或扫描里
// 最好的；第一次建立时先扫描），在 F 上发 CHANNEL，收到 CHANNEL_ACK 后双方一起换过去，再
// PING 一次确认。只有主机扫描。会话中重发率过高或接收机“聋了”时，主机拉黑这个信道，换到
// 下一个最好的点；不成就回 F 重新建立。
//
// How the host picks a data channel and takes the peer along. The pure logic of choosing
// a point is in channel.hpp.
//
// Pairing and opening a session happen on the control channel F. Once the host has
// reached its peer on F it picks a data channel (the locked one, or the best of a scan;
// the first session scans first), sends CHANNEL on F, and on CHANNEL_ACK both move there
// and confirm with a PING. Only the host scans. When a session sees too many resends or a
// deaf receiver, the host bans the channel and moves to the next best point, or opens the
// session on F again when that fails.

#include <cstdint>

#include "xrdap/radio/channel.hpp"
#include "xrdap/radio/packet.hpp"
#include "xrdap/radio/radio_port.hpp"
#include "xrdap/radio/session.hpp"
#include "xrdap/radio/settings.hpp"

namespace XrDap::Radio
{

/// @brief 主机选信道 / The host's choice of channel.
template <typename RadioT>
class ChannelPicker
{
 public:
  /// @param settings 探针的设置（锁定的信道）/ The probe's settings (the locked channel).
  /// @param session 主机的会话 / The host's session.
  ChannelPicker(RadioPort<RadioT>& port, const Settings& settings,
                const HostSession& session)
      : port_(port), settings_(settings), session_(session)
  {
  }

  /// 锁定的信道，自动时为 kNoChannel / The locked channel, kNoChannel when automatic.
  uint8_t LockedChannel() const
  {
    return settings_.channel_lock != 0U && settings_.channel_lock <= kChannels
               ? static_cast<uint8_t>(settings_.channel_lock - 1U)
               : kNoChannel;
  }

  /// @brief 找到对方（第一次先扫描），需要时换到数据信道 / Find the peer (scanning first
  ///        the first time) and move to the data channel if needed.
  /// @return 对方在时为 true；换信道失败时留在对方所在的信道上 / true when the peer is
  ///         there; a failed change of channel stays where the peer is.
  bool Rendezvous()
  {
    if (!FindPeer())
    {
      return false;
    }
    if (port_.Channel() != kControlChannel)
    {
      return true;  // 对方还在数据信道上，接着用 / the peer is still on a data channel
    }
    if (LockedChannel() == kNoChannel && !channels_.Scanned())
    {
      ScanAll();
    }
    const uint8_t target = ChooseChannel();
    if (!SwitchTo(target))
    {
      channels_.Ban(target, NowUs64());
      return FindPeer();
    }
    return true;
  }

  /// @brief 会话中当前数据信道变差：拉黑它，换到下一个最好的点；不成就回 F / The data
  ///        channel turned bad during a session: ban it and move to the next best point;
  ///        when that fails, return to F.
  /// @return 回了 F、下一条命令要在 F 上重新建立时为 false / false when it returned to F
  ///         and the next command has to open the session there again.
  bool Reselect()
  {
    channels_.Ban(port_.Channel(), NowUs64());
    retries_.Reset();
    if (LockedChannel() != kNoChannel)
    {
      return true;  // 锁定时不换 / a locked channel stays
    }
    const uint8_t from = port_.Channel();
    const uint8_t target = ChooseChannel();
    if (SwitchTo(target))
    {
      return true;
    }
    channels_.Ban(target, NowUs64());
    if (stray_ == kNoChannel && from != kControlChannel)
    {
      NoteStray(from, session_.Session());
    }
    port_.TuneTo(kControlChannel);
    return false;
  }

  /// 记下对方可能还留着的数据信道。对方要到沉默超时才回 F，这段时间里 F 上找不到它 / Note
  /// a data channel the peer may still be on. It returns to F only after its silence
  /// timeout, and until then F does not find it.
  void NoteStray(uint8_t channel, uint16_t session)
  {
    stray_ = channel;
    stray_session_ = session;
  }

  /// 记一次往返 / Note one exchange.
  void NoteExchange(bool retried) { reselect_ = retries_.Add(retried) || reselect_; }

  /// 重发率过高，该换信道了（只报一次）/ Too many resends, time to change channel
  /// (reported once).
  bool TakeReselect()
  {
    const bool reselect = reselect_;
    reselect_ = false;
    return reselect;
  }

 private:
  // 在 F 上找对方：DAP_Connect 和回退重建给这么久。F 忙碌 40% 时，3 ms 一次的 PING 在
  // 这段时间里全部失败的可能可以忽略。
  // Reaching the peer on F: DAP_Connect and a fallback get this long. With F busy 40% of
  // the time, a PING every 3 ms practically never fails for this long.
  static constexpr uint32_t kRendezvousUs = 300'000;
  // 换信道：CHANNEL 至多重发这么久；每 kSwitchProbe 次没有 ACK，就到目标信道上 PING
  // 一次（对方可能已经换过去，只是 ACK 丢了）。
  // Changing channel: CHANNEL is sent again for up to this long; after every
  // kSwitchProbe sends without an ACK the host pings on the target channel once (the peer
  // may have moved and only the ACK was lost).
  static constexpr uint32_t kSwitchUs = 100'000;
  static constexpr uint32_t kSwitchProbe = 4;
  // 扫描：每 2 个索引（1 MHz）测一个点，每点先等 kSettleUs 再读 kDwellUs；扫 kScanPasses
  // 遍，每个点留最差的一遍，共约 0.6 s。首次扫描在 DAP_Connect 里，要短于工具的 USB 超时
  // （OpenOCD 1 s）。
  // Scan: one point every 2 indices (1 MHz), each read for kDwellUs after kSettleUs;
  // kScanPasses passes, keeping the worst pass of each point, about 0.6 s in all. The
  // first scan runs inside DAP_Connect and must stay below the tools' USB timeout
  // (OpenOCD: 1 s).
  static constexpr uint32_t kSettleUs = 300;
  static constexpr uint32_t kDwellUs = 1'500;
  static constexpr uint8_t kScanStep = 2;
  static constexpr uint8_t kScanPasses = 4;

  // 测一个点；调用方之后回到原信道。 / Measure one point; the caller returns to its
  // channel afterwards.
  ChannelSample Measure(uint8_t channel)
  {
    port_.TuneTo(channel);
    LibXR::Timebase::DelayMicroseconds(kSettleUs);
    RssiMeter meter;
    const uint32_t begin = NowUs();
    do
    {
      meter.Add(port_.Radio().RssiInstant());
    } while (NowUs() - begin < kDwellUs);
    return meter.Result();
  }

  void ScanAll()
  {
    const uint8_t back = port_.Channel();
    channels_.ClearSamples();
    for (uint8_t pass = 0; pass < kScanPasses; ++pass)
    {
      for (uint8_t channel = 0; channel < kChannels; channel += kScanStep)
      {
        channels_.Merge(channel, Measure(channel));
      }
    }
    port_.TuneTo(back);
  }

  // 该用的数据信道：锁定的，或扫描里最好的；都没有时为 F。
  // The data channel to use: the locked one or the best of the scan; F when there is
  // none.
  uint8_t ChooseChannel()
  {
    const uint8_t locked = LockedChannel();
    if (locked != kNoChannel)
    {
      return locked;
    }
    const uint8_t best = channels_.Best(NowUs64());
    return best != kNoChannel ? best : kControlChannel;
  }

  // 在当前信道上让对方换到 target，双方都换过去并 PING 通时为
  // true。失败时主机留在当前信道 或 F 上，对方可能已在 target 上，它沉默超时后会回 F。
  // Make the peer move to `target` from the current channel; true once both are there and
  // a PING went through. On failure the host stays on the current channel or F; the peer
  // may be on `target` already and returns to F after its silence timeout.
  bool SwitchTo(uint8_t target)
  {
    if (target == port_.Channel())
    {
      return true;
    }
    const uint16_t session = session_.Session();
    const uint8_t from = port_.Channel();
    const uint32_t begin = NowUs();
    for (uint32_t sends = 1; NowUs() - begin < kSwitchUs; ++sends)
    {
      const uint8_t sequence = port_.NextSequence();
      port_.Transmit(port_.Tx(),
                     BuildChannel(kChannel, sequence, session, target, port_.Tx()));
      port_.FinishSending();
      uint16_t echoed = 0;
      uint8_t channel = kNoChannel;
      const int n = port_.AwaitReply(kChannelAck, sequence, port_.PingWaitUs());
      const bool acked = n > 0 && ParseChannel(port_.Rx(), n, echoed, channel) &&
                         echoed == session && channel == target;
      if (!acked && sends % kSwitchProbe != 0U)
      {
        continue;
      }
      port_.TuneTo(target);
      if (port_.Ping(session, acked ? port_.kPingBudgetUs : port_.kPingOnceUs))
      {
        return true;
      }
      port_.TuneTo(from);
      if (acked)
      {
        // 对方换过去了却不通。 / The peer moved but does not answer.
        NoteStray(target, session);
        return false;
      }
    }
    // 对方可能收到了 CHANNEL，只是 ACK 丢了：记下目标信道，找对方时先去那里。
    // The peer may have got CHANNEL and only the ACK was lost: note the target, to look
    // there first.
    if (from == kControlChannel)
    {
      NoteStray(target, session);
    }
    return false;
  }

  // 找到对方。先试它可能留着的数据信道；同一个会话时它留在那里，别的会话号让它结束旧会话
  // 回 F。再试 F。
  // Find the peer. First the data channel it may have stayed on: with the same session it
  // stays there, and another id makes it end the old session and return to F. Then F.
  bool FindPeer()
  {
    const uint16_t session = session_.Session();
    const uint8_t left = port_.Channel();
    if (stray_ != kNoChannel)
    {
      const uint8_t stray = stray_;
      const uint16_t stray_session = stray_session_;
      stray_ = kNoChannel;
      port_.TuneTo(stray);
      if (port_.Ping(session, port_.kPingBudgetUs) && stray_session == session)
      {
        return true;
      }
    }
    port_.TuneTo(kControlChannel);
    if (port_.Ping(session, kRendezvousUs))
    {
      return true;
    }
    if (left != kControlChannel)
    {
      NoteStray(left, session);
    }
    return false;
  }

  RadioPort<RadioT>& port_;
  const Settings& settings_;
  const HostSession& session_;
  ChannelTable channels_;
  RetryWindow retries_;
  uint8_t stray_ = kNoChannel;  // 对方可能留着的信道 / channel the peer may be on
  uint16_t stray_session_ = 0;  // 记下时的会话号 / the session noted with it
  bool reselect_ = false;       // 重发率过高，命令之后换信道 / too many resends
};

}  // namespace XrDap::Radio
