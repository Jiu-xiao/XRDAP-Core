// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针自己的射频：启动和换信道、发包、等应答。主机、远端和配对都经它用收发器。
//
// The probe's own radio: start-up and channel changes, sending, waiting for answers. The
// host, the remote and pairing all use the transceiver through it.

#include <cstdint>
#include <cstring>

#include "timebase.hpp"
#include "xrdap/radio/channel.hpp"
#include "xrdap/radio/identity.hpp"
#include "xrdap/radio/packet.hpp"
#include "xrdap/radio/settings.hpp"
#include "xrdap/radio/transceiver.hpp"

namespace XrDap::Radio
{

inline uint32_t NowUs()
{
  return static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds());
}

/// 不回绕的时间，给隔很久还要比较的记录用 / Time that does not wrap, for records compared
/// long afterwards.
inline uint64_t NowUs64() { return LibXR::Timebase::GetMicroseconds(); }

/// @brief 探针自己的射频 / The probe's own radio.
/// @tparam RadioT 收发器驱动（transceiver.hpp）/ Transceiver driver (transceiver.hpp).
template <typename RadioT>
class RadioPort
{
 public:
  explicit RadioPort(RadioT& radio) : radio_(radio) {}

  /// 按速率和身份启动，在控制信道上 / Start with this rate and identity, on the control
  /// channel.
  void Start(Rate rate, const Identity& identity)
  {
    params_.rate = rate;
    params_.frequency_hz = ChannelHz(kControlChannel);
    params_.crc_seed = identity.crc_seed;
    std::memcpy(params_.address, identity.address, sizeof(params_.address));
    (void)radio_.Start(params_);
    channel_ = kControlChannel;
  }

  /// 复位射频，留在当前信道 / Reset the radio and stay on the current channel.
  void Restart()
  {
    params_.frequency_hz = ChannelHz(channel_);
    (void)radio_.Start(params_);
  }

  void TuneTo(uint8_t channel)
  {
    if (channel == channel_)
    {
      return;
    }
    if (!running_ && !pairing_)
    {
      channel_ = channel;  // 射频没在用，下次启动在 F / radio off; it starts on F
      return;
    }
    FinishSending();
    (void)radio_.Tune(ChannelHz(channel));
    channel_ = channel;
  }

  /// 发出一包：上一包还在发射时等它发完，新包紧接着发，中间不回接收 / Send one packet:
  /// wait while the previous one is on air, then send right after it without receiving in
  /// between.
  void Transmit(const uint8_t* packet, uint8_t size)
  {
    while (radio_.Send(packet, size) == LibXR::ErrorCode::BUSY)
    {
    }
  }

  /// 等最后一包发完，转回接收 / Wait for the last packet and return to receive.
  void FinishSending()
  {
    while (radio_.Sending())
    {
      radio_.Poll();
    }
  }

  void SendMessage(uint8_t kind, uint8_t sequence, const uint8_t* data, uint16_t size)
  {
    const uint8_t fragment = Fragment();
    const uint8_t count = FragmentCount(size, fragment);
    for (uint8_t i = 0; i < count; ++i)
    {
      Transmit(tx_, BuildFragment(kind, sequence, data, size, fragment, i, tx_));
    }
    FinishSending();
  }

  void SendPing(uint8_t kind, uint8_t sequence, uint16_t session)
  {
    Transmit(tx_, BuildPing(kind, sequence, session, tx_));
    FinishSending();
  }

  /// 主机：发 PING(session)，timeout_us 内对方回 PONG 时为 true；每 PingWaitUs() 重发 /
  /// Host: send PING(session); true when the peer answers with a PONG within timeout_us;
  /// repeats every PingWaitUs().
  bool Ping(uint16_t session, uint32_t timeout_us)
  {
    if (!running_)
    {
      return false;
    }
    const uint8_t sequence = NextSequence();
    const uint32_t begin = NowUs();
    do
    {
      SendPing(kPing, sequence, session);
      const uint32_t deadline = NowUs() + PingWaitUs();
      while (static_cast<int32_t>(deadline - NowUs()) > 0)
      {
        radio_.Poll();
        uint16_t echoed = 0;
        const int n = radio_.Receive(rx_);
        if (n > 0 && rx_[0] == kPong && rx_[1] == sequence && ParsePing(rx_, n, echoed) &&
            echoed == session)
        {
          return true;
        }
      }
    } while (NowUs() - begin < timeout_us);
    return false;
  }

  /// 主机：wait_us 内等一个 kind 类型、序号为 sequence 的包，收到时它在 Rx() 里 / Host:
  /// wait up to wait_us for a packet of `kind` with `sequence`; it is in Rx() then.
  int AwaitReply(uint8_t kind, uint8_t sequence, uint32_t wait_us)
  {
    const uint32_t deadline = NowUs() + wait_us;
    while (static_cast<int32_t>(deadline - NowUs()) > 0)
    {
      radio_.Poll();
      const int n = radio_.Receive(rx_);
      if (n >= kHeader && rx_[0] == kind && rx_[1] == sequence)
      {
        return n;
      }
    }
    return 0;
  }

  /// 主机发出的下一个序号 / The next sequence the host sends.
  uint8_t NextSequence() { return ++host_sequence_; }

  uint8_t Fragment() const { return static_cast<uint8_t>(radio_.MaxPayload() - kHeader); }
  uint8_t Channel() const { return channel_; }

  /// 按设置在用无线 / The radio is in use according to the settings.
  bool Running() const { return running_; }
  void SetRunning(bool running) { running_ = running; }
  /// 配对占用着射频 / Pairing holds the radio.
  bool Pairing() const { return pairing_; }
  void SetPairing(bool pairing) { pairing_ = pairing; }

  RadioT& Radio() { return radio_; }
  uint8_t* Tx() { return tx_; }
  uint8_t* Rx() { return rx_; }

  /// 等一个 PING 的应答多久：kPacketWaitUs
  /// 加一段每次不同的偏移，让同时找对方的几对不会一直 撞在一起 / How long to wait for the
  /// answer of a PING: kPacketWaitUs plus an offset that changes every time, which keeps
  /// pairs looking for their peers at the same time from colliding again and again.
  uint32_t PingWaitUs() const
  {
    return kPacketWaitUs + ((NowUs() >> 3) ^ params_.crc_seed) % kPingOffsetUs;
  }

  static constexpr uint32_t kPingOffsetUs = 1'024;
  /// 发一次、不重发的 PING：心跳和 F 上的结束会话 / A PING sent once: the heartbeat and
  /// ending a session on F.
  static constexpr uint32_t kPingOnceUs = 0;
  /// PING 至多重发这么久 / A PING is sent again for up to this long.
  static constexpr uint32_t kPingBudgetUs = 30'000;

 private:
  RadioT& radio_;
  LinkParams params_{};
  uint8_t channel_ = kControlChannel;  // 射频现在所在的信道 / the channel the radio is on
  uint8_t host_sequence_ = 0;
  bool running_ = false;
  bool pairing_ = false;
  uint8_t tx_[256] = {};
  uint8_t rx_[256] = {};
};

}  // namespace XrDap::Radio
