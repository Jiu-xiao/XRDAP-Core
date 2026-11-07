// SPDX-License-Identifier: Apache-2.0
#pragma once

// 无线测试共用的时钟、空中、假收发器、假执行器、假存储和一对探针。假收发器照
// xrdap/radio/transceiver.hpp 的契约实现，谁听得到谁只由 FakeRadio::Hear 决定。
//
// What the radio tests share: the clock, the air, the fake transceiver, executor and
// stores, and a pair of probes. The fake transceiver follows the contract in
// xrdap/radio/transceiver.hpp, and FakeRadio::Hear alone decides who hears whom.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

#include "check.hpp"
#include "xrdap/radio/radio_link.hpp"

namespace RadioTest
{

using namespace XrDap;
using namespace XrDap::Radio;

// 无线测试的时间：Core 只经 LibXR::Timebase 读时间，这个测试程序把它换成这个时钟（
// radio_clock.cpp）。每读一次走一小步，所以忙等总会结束；同样的调用序列得到同样的时间。
// Time for the radio tests: Core reads time only through LibXR::Timebase, which this test
// program replaces with this clock (radio_clock.cpp). Every read moves it a small step,
// so a busy-wait always ends, and the same calls see the same times.
class VirtualClock
{
 public:
  // 每次读走多少，µs；和主循环一轮读几次射频的时间差不多。 / The step of one read, µs,
  // about what a round of the main loop spends on the radio.
  static constexpr uint32_t kStepUs = 10;
  // NowUs() 截成 32 位，这里回绕。 / NowUs() truncates to 32 bits, which wrap here.
  static constexpr uint64_t kWrapUs = 1ULL << 32;
  // 一对探针开始时离回绕多久：时钟早已过了远端的沉默时限 2.5 s，短的用例走不到回绕，长的
  // 用例会跨过。 / How far from the wrap a pair starts: the clock is long past the
  // remote's silence limit of 2.5 s, short cases do not reach the wrap, and long ones
  // cross it.
  static constexpr uint32_t kLeadUs = 1'000'000;

  static void StartBeforeWrap(uint32_t lead_us) { now_us_ = kWrapUs - lead_us; }
  static uint64_t Read() { return now_us_ += kStepUs; }
  // 一下子过去这么久 / This much time passes at once.
  static void Advance(uint64_t us) { now_us_ += us; }

 private:
  static inline uint64_t now_us_ = kWrapUs - kLeadUs;
};

class FakeRadio;

/// 空中的一包 / One packet on air.
struct Flight
{
  const FakeRadio* from;
  // 发射时的频率、速率和身份 / Frequency, rate and identity it left with.
  LinkParams params;
  uint64_t begin_us;
  uint64_t end_us;
  bool collided;  ///< 和别的包重叠 / Overlapped another packet
  bool lost;      ///< 测试让它丢掉 / The test made it get lost
  std::vector<uint8_t> data;
};

/// 带宽按 2 MHz 算，两档速率相同 / The bandwidth, taken as 2 MHz at both rates.
inline bool Overlap(uint32_t a, uint32_t b)
{
  return (a > b ? a - b : b - a) < 2'000'000U;
}

/// 所有收发器共用的空中 / The air all transceivers share.
class Air
{
 public:
  void Join(FakeRadio* radio) { radios_.push_back(radio); }

  /// 发出一包：和空中频率相差不到一个带宽的包都撞坏 / Send a packet: it and any packet on
  /// air less than one bandwidth away are destroyed.
  void Launch(Flight flight)
  {
    flight.lost = lose && lose(flight);
    for (Flight& other : flights_)
    {
      if (other.end_us > flight.begin_us &&
          Overlap(other.params.frequency_hz, flight.params.frequency_hz))
      {
        other.collided = true;
        flight.collided = true;
        ++collisions;
      }
    }
    flights_.push_back(std::move(flight));
  }

  /// 把结束了的包交给听得到的收发器 / Hand the packets that ended to whoever hears them.
  inline void Land();

  /// 这一包丢掉时为 true / True makes this packet get lost.
  std::function<bool(const Flight&)> lose;
  uint32_t collisions = 0;

 private:
  std::vector<FakeRadio*> radios_;
  std::vector<Flight> flights_;
};

/// 假收发器，契约见 transceiver.hpp / Fake transceiver; the contract is in
/// transceiver.hpp.
class FakeRadio
{
 public:
  explicit FakeRadio(Air& air) : air_(air) { air_.Join(this); }

  LibXR::ErrorCode Start(const LinkParams& params)
  {
    air_.Land();
    params_ = params;
    rx_.clear();
    tx_end_us_ = 0;
    Listen();
    ++starts;
    return LibXR::ErrorCode::OK;
  }

  uint8_t MaxPayload() const { return params_.rate == Rate::kLow ? 127 : 255; }

  LibXR::ErrorCode Send(const uint8_t* data, uint8_t size)
  {
    CHECK(size >= 1U && size <= MaxPayload());
    air_.Land();
    const uint64_t now = VirtualClock::Read();
    if (now < tx_end_us_)
    {
      return LibXR::ErrorCode::BUSY;
    }
    // 2 Mb/s 或 1.3 Mb/s，加收发切换、前导码和包头，最长的包也远小于契约里的 3 ms / The
    // rates are 2 Mb/s and 1.3 Mb/s; with the switch to transmit, preamble and header the
    // longest packet stays well below the 3 ms of the contract.
    const uint32_t us_per_byte = params_.rate == Rate::kLow ? 7U : 4U;
    tx_end_us_ = now + 100U + size * us_per_byte;
    listening_since_us_ = tx_end_us_;
    return_due_ = true;
    ++sent;
    air_.Launch({this, params_, now, tx_end_us_, false, false,
                 std::vector<uint8_t>(data, data + size)});
    return LibXR::ErrorCode::OK;
  }

  bool Sending() const { return VirtualClock::Read() < tx_end_us_; }

  void Poll()
  {
    air_.Land();
    if (return_due_ && !Sending())
    {
      return_due_ = false;
      ++receive_entries;
    }
    if (on_poll)
    {
      on_poll();
    }
  }

  int Receive(uint8_t* out)
  {
    air_.Land();
    if (rx_.empty())
    {
      return 0;
    }
    const auto packet = rx_.front();
    rx_.pop_front();
    if (packet.empty())
    {
      ++bad;
      return -1;
    }
    ++received;
    std::memcpy(out, packet.data(), packet.size());
    return static_cast<int>(packet.size());
  }

  LibXR::ErrorCode Tune(uint32_t frequency_hz)
  {
    CHECK(!Sending());
    air_.Land();
    params_.frequency_hz = frequency_hz;
    rx_.clear();
    Listen();
    return LibXR::ErrorCode::OK;
  }

  int16_t RssiInstant()
  {
    ++rssi_reads;
    return noise ? noise(params_.frequency_hz) : -200;
  }

  void PrepareSend() {}

  uint32_t TxPackets() const { return sent; }
  uint32_t TxErrors() const { return tx_errors; }
  uint32_t RxPackets() const { return received; }
  uint32_t RxErrors() const { return bad; }
  uint32_t Rearms() const { return rearms; }
  uint32_t Restarts() const { return starts; }

  // 谁听得到谁：这一包从头到尾都在接收、同频、同速率的收发器听得到，高速率还要同地址（低
  // 速率不比地址，协议 8.4 节）；CRC 初值不同时是坏包。发射一结束就算回到接收，不等
  // Poll()：单线程里主机阻塞时对方的主循环可能轮不到跑，真机上这时早已跑过 Poll() 了。
  // Who hears whom: a transceiver in receive for all of the packet, on the same frequency
  // and at the same rate; the high rate also needs the same address (the low rate does
  // not compare it, protocol section 8.4). Another CRC seed makes it a bad packet. It is
  // back in receive as soon as a transmission ends, without waiting for Poll(): on one
  // thread a probe's main loop may not get to run while the other is blocked, where on
  // real hardware Poll() would have run long before.
  void Hear(const Flight& flight)
  {
    if (flight.from == this || flight.begin_us < listening_since_us_ ||
        flight.params.frequency_hz != params_.frequency_hz ||
        flight.params.rate != params_.rate ||
        (params_.rate != Rate::kLow &&
         std::memcmp(flight.params.address, params_.address, 4) != 0))
    {
      return;
    }
    if (flight.params.crc_seed == params_.crc_seed)
    {
      rx_.push_back(flight.data);
    }
    else
    {
      rx_.emplace_back();
    }
  }

  uint32_t Frequency() const { return params_.frequency_hz; }

  // 等待中每次 Poll() 让别的探针跑一轮 / Lets other probes run a round on every Poll().
  std::function<void()> on_poll;
  // 某个频率上的 RSSI，0.5 dBm；没设时处处安静 / RSSI on a frequency, 0.5 dBm; quiet
  // everywhere when unset.
  std::function<int16_t(uint32_t)> noise;
  uint32_t sent = 0;
  uint32_t received = 0;
  uint32_t bad = 0;
  uint32_t tx_errors = 0;
  uint32_t rearms = 0;
  uint32_t starts = 0;
  uint32_t rssi_reads = 0;
  // Poll() 看到发完的次数，连着发的包之间没有 / How often Poll() saw a transmission end;
  // packets sent right after each other do not count.
  uint32_t receive_entries = 0;

 private:
  void Listen()
  {
    return_due_ = false;
    listening_since_us_ = VirtualClock::Read();
  }

  Air& air_;
  LinkParams params_{};
  bool return_due_ = false;
  uint64_t tx_end_us_ = 0;
  uint64_t listening_since_us_ = 0;
  std::deque<std::vector<uint8_t>> rx_;
};

inline void Air::Land()
{
  const uint64_t now = VirtualClock::Read();
  for (size_t i = 0; i < flights_.size();)
  {
    if (flights_[i].end_us > now)
    {
      ++i;
      continue;
    }
    if (!flights_[i].collided && !flights_[i].lost)
    {
      for (FakeRadio* radio : radios_)
      {
        radio->Hear(flights_[i]);
      }
    }
    flights_.erase(flights_.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

/// 假执行器，只留 RadioLink 用到的部分 / Fake executor with only what RadioLink uses.
class FakeExec
{
 public:
  using Forwarder = bool (*)(void*, Transport, const uint8_t*, uint16_t, uint8_t*,
                             uint16_t, uint16_t&);
  void SetForwarder(Forwarder f, void* ctx)
  {
    forwarder_ = f;
    ctx_ = ctx;
  }
  // USB 来一条命令：转发钩子不处理时在本地执行 / A command from USB: runs locally when
  // the forwarder hook does not take it.
  std::vector<uint8_t> Usb(const std::vector<uint8_t>& req)
  {
    uint8_t resp[kMaxMessage];
    uint16_t out = 0;
    if (!forwarder_(ctx_, Transport::USB, req.data(), static_cast<uint16_t>(req.size()),
                    resp, sizeof(resp), out))
    {
      ++local;
      return {0xEE};
    }
    return {resp, resp + out};
  }
  // 远端执行：应答 = 命令号 + 命令倒序 / Remote execution: answer = command id + the
  // command reversed.
  void ExecuteHere(Transport t, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    uint16_t forwarded = 0;
    CHECK(!forwarder_(ctx_, t, req, len, resp, cap, forwarded));  // RADIO 不转发
    ++executed;
    VirtualClock::Advance(execute_us);
    last.assign(req, req + len);
    if (marker != 0U && len >= 2U && req[1] != marker)
    {
      ++foreign;
    }
    resp[0] = req[0];
    for (uint16_t i = 1; i < len && i < cap; ++i)
    {
      resp[i] = req[len - i];
    }
    out = len;
  }
  void ControlHere(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                   uint16_t& out)
  {
    if (control == nullptr || !control(req, len, resp, cap, out))
    {
      out = Control::StatusOnly(req[0], Control::kStatusUnsupported, resp, cap);
    }
  }
  void LinkLost(Transport t)
  {
    CHECK(t == Transport::RADIO);
    ++lost;
  }

  std::function<bool(const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t&)> control;
  std::vector<uint8_t> last;  ///< 上一条执行的命令 / the last command executed
  uint32_t executed = 0;
  // 一条命令执行多久 / How long one command executes.
  uint32_t execute_us = 0;
  uint32_t local = 0;
  uint32_t lost = 0;
  // 几对探针同时工作时命令第二个字节是所属的对：不是 0 时核对，对不上的算进 foreign。 /
  // With several pairs at once the second byte of a command names its pair: when not 0 it
  // is checked, and a mismatch counts in foreign.
  uint8_t marker = 0;
  uint32_t foreign = 0;

 private:
  Forwarder forwarder_ = nullptr;
  void* ctx_ = nullptr;
};

class FakeStore final : public Store
{
 public:
  bool Save(const Settings& s) override
  {
    saved = s;
    ++saves;
    return ok;
  }
  Settings saved{};
  uint32_t saves = 0;
  bool ok = true;
};

// 保存设置要一段时间：Save 立即返回，这段时间里 Busy() 为真，测试让远端既不运行也收不到
// 包，和它阻塞在写 Flash 上一样，主机却还能继续发。 / A save that takes time: Save
// returns at once and Busy() holds for that time; the test keeps the remote from running
// and receiving meanwhile, as when it blocks writing flash, while the host can go on
// sending.
class SlowStore final : public Store
{
 public:
  bool Save(const Settings&) override
  {
    ++saves;
    until_us_ = VirtualClock::Read() + save_us;
    return true;
  }
  bool Busy() const { return VirtualClock::Read() < until_us_; }
  uint32_t save_us = 0;
  uint32_t saves = 0;

 private:
  uint64_t until_us_ = 0;
};

using Link = RadioLink<FakeRadio, FakeExec>;

inline constexpr uint8_t kUidA[kUidSize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
inline constexpr uint8_t kUidB[kUidSize] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

// 一对探针：a 接电脑，b 接目标 / A pair: a on the PC, b on the target.
template <typename StoreB>
class PairWith
{
  // 先于别的成员构造 / Constructed before the other members.
  Air own_air_;

 public:
  // 单独一对，用自己的空中，时钟重新开始 / A pair alone on its own air; the clock starts
  // again.
  explicit PairWith(bool paired, Rate rate = Rate::kHigh)
      : PairWith(own_air_, kUidA, kUidB)
  {
    VirtualClock::StartBeforeWrap(VirtualClock::kLeadUs);
    Begin(paired, rate);
  }
  // 配对好的一对，和别的对共用空中 / A paired pair, sharing the air with others.
  PairWith(Air& shared, const uint8_t* a_id, const uint8_t* b_id, Rate rate)
      : PairWith(shared, a_id, b_id)
  {
    Begin(true, rate);
  }

  Air& air;
  FakeRadio radio_a, radio_b;
  FakeExec exec_a, exec_b;
  FakeStore store_a;
  StoreB store_b;
  uint8_t uid_a[kUidSize], uid_b[kUidSize];
  Link a{radio_a, exec_a, store_a, uid_a};
  Link b{radio_b, exec_b, store_b, uid_b};

 private:
  PairWith(Air& the_air, const uint8_t* a_id, const uint8_t* b_id)
      : air(the_air), radio_a(the_air), radio_b(the_air)
  {
    std::memcpy(uid_a, a_id, kUidSize);
    std::memcpy(uid_b, b_id, kUidSize);
    // 只有主机在等应答时阻塞；远端不阻塞，不必反过来驱动主机 / Only the host blocks,
    // waiting for answers; the remote never blocks and need not drive the host.
    radio_a.on_poll = [this] { (void)b.Service(); };
  }

  void Begin(bool paired, Rate rate)
  {
    Settings s;
    s.paired = paired ? 1 : 0;
    s.link_id = paired ? LinkId(uid_a, uid_b) : 0;
    s.rate = rate;
    a.Begin(s, false);
    b.Begin(s, false);
  }
};

using Pair = PairWith<FakeStore>;

inline std::vector<uint8_t> Command(uint8_t id, uint16_t size)
{
  std::vector<uint8_t> c(size);
  c[0] = id;
  for (uint16_t i = 1; i < size; ++i)
  {
    c[i] = static_cast<uint8_t>(i * 13U);
  }
  return c;
}

/// FakeExec 对这条命令的应答 / FakeExec's answer to this command.
inline std::vector<uint8_t> Expected(const std::vector<uint8_t>& c)
{
  std::vector<uint8_t> e(c.size());
  e[0] = c[0];
  for (size_t i = 1; i < c.size(); ++i)
  {
    e[i] = c[c.size() - i];
  }
  return e;
}

/// 让一块探针的主循环跑 us / Run a probe's main loop for us.
inline void RunFor(Link& link, uint32_t us)
{
  const uint32_t begin = NowUs();
  while (NowUs() - begin < us)
  {
    (void)link.Service();
  }
}

// 一个点安静、其余忙碌的噪声 / Noise with one quiet point and the rest busy.
inline std::function<int16_t(uint32_t)> QuietAt(uint8_t quiet,
                                                uint8_t second = kNoChannel)
{
  return [quiet, second](uint32_t hz) -> int16_t
  {
    if (hz == ChannelHz(quiet))
    {
      return -200;
    }
    return hz == ChannelHz(second) ? -190 : -150;
  };
}

inline uint8_t ControlStatus(Link& link, std::vector<uint8_t> req,
                             std::vector<uint8_t>* answer)
{
  uint8_t resp[128];
  uint16_t out = 0;
  CHECK(link.HandleControl(req.data(), static_cast<uint16_t>(req.size()), resp,
                           sizeof(resp), out));
  if (answer != nullptr)
  {
    answer->assign(resp, resp + out);
  }
  return out >= 2 ? resp[1] : 0xFF;
}

inline StatsAnswer ReadStats(Link& link)
{
  std::vector<uint8_t> bytes;
  CHECK_EQ(ControlStatus(link, {Op::kStats}, &bytes), Status::kOk);
  StatsAnswer answer{};
  CHECK_EQ(bytes.size(), sizeof(answer));
  std::memcpy(&answer, bytes.data(), std::min(bytes.size(), sizeof(answer)));
  return answer;
}

}  // namespace RadioTest
