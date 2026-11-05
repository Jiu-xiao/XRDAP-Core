// SPDX-License-Identifier: Apache-2.0
#pragma once

// 配对时交换 HELLO，身份见 identity.hpp。第一个听到的探针就是对方，所以一次只配一对。
//
// Exchanging HELLOs while pairing; identity: identity.hpp. The first probe heard is the
// peer, so pair one pair at a time.

#include <cstdint>
#include <cstring>

#include "xrdap/radio/identity.hpp"
#include "xrdap/radio/radio_port.hpp"

namespace XrDap::Radio
{

/// @brief 配对的过程 / The pairing process.
template <typename RadioT>
class PairingAgent
{
 public:
  /// @param uid 本机芯片 UID，kUidSize 字节 / This chip's UID, kUidSize bytes.
  PairingAgent(RadioPort<RadioT>& port, const uint8_t* uid) : port_(port), uid_(uid) {}

  /// 开始；射频已经用默认身份启动 / Begin; the radio has started with the default
  /// identity.
  void Begin()
  {
    peer_known_ = false;
    done_ = false;
    begin_us_ = NowUs();
    next_hello_us_ = begin_us_;
  }

  /// 主循环每轮调用；过程结束时为 true / Call every round of the main loop; true when the
  /// process is over.
  bool Serve()
  {
    const uint32_t now = NowUs();
    if ((done_ && now - done_at_us_ >= kLingerUs) ||
        (!done_ && now - begin_us_ >= kPairUs))
    {
      return true;
    }
    RadioT& radio = port_.Radio();
    uint8_t* const rx = port_.Rx();
    radio.Poll();
    if (radio.Sending())
    {
      return false;
    }
    if (static_cast<int32_t>(now - next_hello_us_) >= 0)
    {
      (void)radio.Send(port_.Tx(),
                       BuildHello(uid_, peer_known_ ? peer_ : nullptr, port_.Tx()));
      // 由 UID 得出的抖动让两块探针不同步发送。 / A jitter from the UID keeps two
      // probes from sending in lockstep.
      next_hello_us_ = now + kHelloEveryUs + Fnv1a(uid_, kUidSize) % kHelloJitterUs;
      return false;
    }
    const int n = radio.Receive(rx);
    if (n != kHelloSize || rx[0] != kHello)
    {
      return false;
    }
    const uint8_t* from = rx + 1;
    const uint8_t* names = rx + 1 + kUidSize;
    if (std::memcmp(from, uid_, kUidSize) == 0)
    {
      return false;
    }
    if (!peer_known_)
    {
      std::memcpy(peer_, from, kUidSize);
      peer_known_ = true;
      next_hello_us_ = now;  // 立即告诉对方 / tell the peer about it at once
    }
    if (std::memcmp(from, peer_, kUidSize) != 0)
    {
      return false;  // 第三块探针 / a third probe
    }
    if (!done_ && std::memcmp(names, uid_, kUidSize) == 0)
    {
      done_ = true;
      done_at_us_ = now;
    }
    return false;
  }

  /// 双方都确认了对方 / Both confirmed each other.
  bool Done() const { return done_; }
  /// 对方的 UID / The peer's UID.
  const uint8_t* Peer() const { return peer_; }

 private:
  // 按 kHelloEveryUs 加由 UID 得出的抖动发 HELLO，至多 kPairUs；完成后再应答 kLingerUs。
  // HELLO every kHelloEveryUs plus a jitter from the UID, for up to kPairUs; once done,
  // answer for another kLingerUs.
  static constexpr uint32_t kPairUs = 10'000'000;
  static constexpr uint32_t kLingerUs = 1'000'000;
  static constexpr uint32_t kHelloEveryUs = 20'000;
  static constexpr uint32_t kHelloJitterUs = 8'000;

  RadioPort<RadioT>& port_;
  const uint8_t* uid_;
  bool peer_known_ = false;
  bool done_ = false;
  uint32_t begin_us_ = 0;
  uint32_t done_at_us_ = 0;
  uint32_t next_hello_us_ = 0;
  uint8_t peer_[kUidSize] = {};
};

}  // namespace XrDap::Radio
