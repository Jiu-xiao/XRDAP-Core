// SPDX-License-Identifier: Apache-2.0
// 几对探针共用内存里的一段“空中”：频率相差不到一个带宽、时间重叠的包都丢掉，包要发射一段
// 时间。这里不模拟近距离阻塞和接收灵敏度，带宽外完全不干扰。判据：docs/protocol.md 第 8
// 节： 几对共用 F，不同的对靠 CRC 初值和链路地址分开，变差的信道被换掉。
//
// 单线程模拟：主机阻塞在一次转发里时，由它的 Poll
// 带动别的探针，别的主机的命令就嵌套在这里
// 跑，它们的包和外层的包在空中重叠。每轮每个主机一条命令，每轮换一个主机在最外层。第一次
// 建立会话要扫描 0.6 s，这段时间外层不动，所以各对先单独建立一次。
//
// Several pairs of probes share an in-memory "air": packets less than one bandwidth apart
// that overlap in time are lost, and a packet takes time on air. Blocking at short range
// and receiver sensitivity are not modelled, and there is no interference beyond the
// bandwidth. Oracle: docs/protocol.md section 8: the pairs share F, the CRC seed and the
// link address keep them apart, and a channel that turned bad is left.
//
// A single-thread simulation: a host blocked in a forward drives the other probes from
// its Poll, so the other hosts' commands run nested there and their packets overlap the
// outer ones on air. Every host sends one command per round, and each round another host
// is the outermost. The first session scans for 0.6 s, during which the outer frames
// stand still, so each pair opens one session on its own first.
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "check.hpp"
#include "radio_fakes.hpp"
#include "xrdap/radio/radio_link.hpp"

using namespace XrDap;
using namespace XrDap::Radio;

namespace
{
uint32_t Now() { return static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds()); }

// 2 Mb/s GFSK 的带宽按 2 MHz 算 / The bandwidth of 2 Mb/s GFSK, taken as 2 MHz.
constexpr uint32_t kBandwidthHz = 2'000'000;

bool Overlap(uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) < kBandwidthHz; }

class AirRadio;

// 所有收发器共用的空中 / The air all transceivers share.
class Air
{
 public:
  struct Flight
  {
    AirRadio* from;
    uint32_t hz;
    uint32_t end_us;
    bool collided;
    std::vector<uint8_t> data;
  };

  void Join(AirRadio* radio) { radios_.push_back(radio); }

  // 结束了的包送给同频、不在发射的收发器。 / Packets that ended reach the transceivers on
  // their frequency that are not transmitting.
  void Advance(uint32_t now);

  std::vector<Flight> flights;

 private:
  std::vector<AirRadio*> radios_;
};

// 接口同 xrobot-org/SX1281 的假收发器 / Fake transceiver with the interface of
// xrobot-org/SX1281.
class AirRadio
{
 public:
  explicit AirRadio(Air& air) : air_(air) { air_.Join(this); }

  LibXR::ErrorCode Start(const LinkParams& config)
  {
    config_ = config;
    rx_.clear();
    sending_ = false;
    return LibXR::ErrorCode::OK;
  }
  uint8_t MaxPayload() const { return config_.rate == Rate::kLow ? 127 : 255; }
  void PrepareSend() {}
  LibXR::ErrorCode Tune(uint32_t frequency_hz)
  {
    config_.frequency_hz = frequency_hz;
    rx_.clear();
    return LibXR::ErrorCode::OK;
  }
  int16_t RssiInstant() { return noise(config_.frequency_hz); }
  uint32_t TxPackets() const { return 0; }
  uint32_t TxErrors() const { return 0; }
  uint32_t RxPackets() const { return 0; }
  uint32_t RxErrors() const { return bad; }
  uint32_t Rearms() const { return 0; }
  uint32_t Restarts() const { return 0; }
  uint32_t Frequency() { return config_.frequency_hz; }

  LibXR::ErrorCode Send(const uint8_t* data, uint8_t size)
  {
    const uint32_t now = Now();
    air_.Advance(now);
    if (sending_ && static_cast<int32_t>(tx_end_us_ - now) > 0)
    {
      return LibXR::ErrorCode::BUSY;
    }
    // 2 Mb/s 或 1.3 Mb/s，加前导码和包头 / 2 Mb/s or 1.3 Mb/s, plus preamble and header.
    const uint32_t us_per_byte = config_.rate == Rate::kLow ? 7U : 4U;
    Air::Flight flight{this, config_.frequency_hz, now + 40U + size * us_per_byte, false,
                       std::vector<uint8_t>(data, data + size)};
    for (auto& other : air_.flights)
    {
      if (Overlap(other.hz, flight.hz))
      {
        other.collided = true;
        flight.collided = true;
      }
    }
    tx_end_us_ = flight.end_us;
    sending_ = true;
    air_.flights.push_back(std::move(flight));
    return LibXR::ErrorCode::OK;
  }

  bool Sending() const { return sending_; }

  void Poll()
  {
    {
      const uint32_t now = Now();
      air_.Advance(now);
      if (sending_ && static_cast<int32_t>(tx_end_us_ - now) <= 0)
      {
        sending_ = false;
      }
    }
    if (on_poll)
    {
      on_poll();
    }
  }

  int Receive(uint8_t* out)
  {
    air_.Advance(Now());
    if (rx_.empty())
    {
      return 0;
    }
    const auto packet = rx_.front();
    rx_.pop_front();
    if (packet.empty())
    {
      ++bad;
      return -1;  // 坏包 / a bad packet
    }
    std::memcpy(out, packet.data(), packet.size());
    return static_cast<int>(packet.size());
  }

  // 空中送来一包。低速率不比链路地址（协议 8.4 节），别的对的包收得到但 CRC 错；高速率的
  // 地址不同，收不到。 / A packet from the air. The low rate does not compare link
  // addresses (protocol section 8.4), so another pair's packet arrives with a CRC error;
  // at the high rate the addresses differ and it is not received at all.
  void Deliver(const Air::Flight& flight, uint32_t now)
  {
    if (flight.from == this || flight.hz != config_.frequency_hz ||
        (sending_ && static_cast<int32_t>(tx_end_us_ - now) > 0) ||
        flight.from->config_.rate != config_.rate ||
        (config_.rate != Rate::kLow &&
         std::memcmp(flight.from->config_.address, config_.address, 4) != 0))
    {
      return;
    }
    if (flight.from->config_.crc_seed == config_.crc_seed)
    {
      rx_.push_back(flight.data);
    }
    else
    {
      rx_.emplace_back();
    }
  }

  std::function<void()> on_poll;
  std::function<int16_t(uint32_t)> noise;
  uint32_t bad = 0;

 private:
  Air& air_;
  LinkParams config_{};
  bool sending_ = false;
  uint32_t tx_end_us_ = 0;
  std::deque<std::vector<uint8_t>> rx_;
};

void Air::Advance(uint32_t now)
{
  for (size_t i = 0; i < flights.size();)
  {
    if (static_cast<int32_t>(flights[i].end_us - now) > 0)
    {
      ++i;
      continue;
    }
    if (!flights[i].collided)
    {
      for (AirRadio* radio : radios_)
      {
        radio->Deliver(flights[i], now);
      }
    }
    flights.erase(flights.begin() + static_cast<std::ptrdiff_t>(i));
  }
}

// 命令第二个字节是所属的对，远端执行时核对 / The second byte of a command names its pair,
// which the remote checks when it executes it.
class PairExec
{
 public:
  using Forwarder = bool (*)(void*, Transport, const uint8_t*, uint16_t, uint8_t*,
                             uint16_t, uint16_t&);
  explicit PairExec(uint8_t marker) : marker_(marker) {}
  void SetForwarder(Forwarder f, void* ctx)
  {
    forwarder_ = f;
    ctx_ = ctx;
  }
  std::vector<uint8_t> Usb(const std::vector<uint8_t>& req)
  {
    uint8_t resp[kMaxMessage];
    uint16_t out = 0;
    if (!forwarder_(ctx_, Transport::USB, req.data(), static_cast<uint16_t>(req.size()),
                    resp, sizeof(resp), out))
    {
      ++local;
      return {};
    }
    return {resp, resp + out};
  }
  void ExecuteHere(Transport, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    if (len >= 2U && req[1] != marker_)
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
  void ControlHere(const uint8_t* req, uint16_t, uint8_t* resp, uint16_t cap,
                   uint16_t& out)
  {
    out = Control::StatusOnly(req[0], Control::kStatusUnsupported, resp, cap);
  }
  void LinkLost(Transport) {}

  uint32_t local = 0;
  uint32_t foreign = 0;

 private:
  uint8_t marker_;
  Forwarder forwarder_ = nullptr;
  void* ctx_ = nullptr;
};

class NoStore final : public Store
{
 public:
  bool Save(const Settings&) override { return true; }
};

using PairLink = RadioLink<AirRadio, PairExec>;

// 所有对都觉得最安静的点，和每对自己次好的点：相同的扫描结果会让几个主机一起换到同一个点。
// / The point every pair finds quietest, and each pair's own second best: identical scan
// results would make the hosts move to the same point together.
constexpr uint8_t kSharedChannel = 20;
constexpr uint8_t SecondChannel(uint8_t index)
{
  return static_cast<uint8_t>(40U + 20U * index);
}

// 一对探针和它们的计数 / A pair of probes and its counters.
struct AirPair
{
  AirPair(Air& air, uint8_t index, Rate rate, const std::function<void()>& drive)
      : marker(static_cast<uint8_t>(0x40U + index)),
        radio_a(air),
        radio_b(air),
        exec_a(marker),
        exec_b(marker)
  {
    std::memset(uid_a, 0x10 + index, sizeof(uid_a));
    std::memset(uid_b, 0x80 + index, sizeof(uid_b));
    a = std::make_unique<PairLink>(radio_a, exec_a, store, uid_a);
    b = std::make_unique<PairLink>(radio_b, exec_b, store, uid_b);
    radio_a.on_poll = drive;
    radio_a.noise = [index](uint32_t hz) -> int16_t
    {
      if (hz == ChannelHz(kSharedChannel))
      {
        return -200;
      }
      return hz == ChannelHz(SecondChannel(index)) ? -190 : -150;
    };
    Settings settings;
    settings.paired = 1;
    settings.link_id = LinkId(uid_a, uid_b);
    settings.rate = rate;
    a->Begin(settings, false);
    b->Begin(settings, false);
  }

  std::vector<uint8_t> Command(uint8_t id, uint16_t size) const
  {
    std::vector<uint8_t> c(size);
    c[0] = id;
    for (uint16_t i = 1; i < size; ++i)
    {
      c[i] = static_cast<uint8_t>(i * 13U);
    }
    c[1] = marker;
    return c;
  }

  // 发一条命令，核对应答。 / Send one command and check the answer.
  void Send(const std::vector<uint8_t>& c)
  {
    const auto answer = exec_a.Usb(c);
    ++total;
    if (answer.size() == 2U && answer[1] == Byte(DapStatus::ERROR))
    {
      ++failed;  // 转发失败，上位机看得到 / a failed forward the PC sees
      return;
    }
    bool same = answer.size() == c.size() && answer[0] == c[0];
    for (size_t k = 1; same && k < c.size(); ++k)
    {
      same = answer[k] == c[c.size() - k];
    }
    if (!same)
    {
      ++wrong;
    }
  }

  // 这一轮的命令。正在阻塞或这一轮发过时什么都不做。 / This round's command. Nothing when
  // it is blocked already or sent one this round.
  void TakeTurn()
  {
    if (busy || turn_taken)
    {
      return;
    }
    busy = true;
    turn_taken = true;
    static constexpr uint16_t kSizes[] = {16, 64, 1024};
    Send(connect_next ? Command(Byte(DapCmd::CONNECT), 2)
                      : Command(Byte(DapCmd::TRANSFER), kSizes[total % 3U]));
    connect_next = false;
    busy = false;
  }

  StatsAnswer Stats()
  {
    const uint8_t request[] = {Op::kStats};
    uint8_t resp[sizeof(StatsAnswer)];
    uint16_t out = 0;
    CHECK(a->HandleControl(request, sizeof(request), resp, sizeof(resp), out));
    StatsAnswer answer;
    std::memcpy(&answer, resp, sizeof(answer));
    return answer;
  }

  uint8_t marker;
  uint8_t uid_a[kUidSize] = {};
  uint8_t uid_b[kUidSize] = {};
  AirRadio radio_a, radio_b;
  PairExec exec_a, exec_b;
  NoStore store;
  std::unique_ptr<PairLink> a, b;
  bool connect_next = false;
  bool busy = false;
  bool turn_taken = false;
  uint32_t total = 0;
  uint32_t failed = 0;
  uint32_t wrong = 0;
};

class Fleet
{
 public:
  Fleet(size_t count, Rate rate) : rate_(rate)
  {
    RadioTest::VirtualClock::StartBeforeWrap(RadioTest::VirtualClock::kLeadUs);
    for (size_t i = 0; i < count; ++i)
    {
      pairs_.push_back(std::make_unique<AirPair>(air_, static_cast<uint8_t>(i), rate,
                                                 [this] { Drive(); }));
    }
  }

  // 各对单独建立第一个会话（扫描），然后都回到 F。 / Each pair opens its first session on
  // its own (the scan), and then all are back on F.
  void OpenOneByOne()
  {
    for (auto& pair : pairs_)
    {
      pair->Send(pair->Command(Byte(DapCmd::CONNECT), 2));
      pair->Send(pair->Command(Byte(DapCmd::DISCONNECT), 2));
      CHECK(pair->radio_a.Frequency() == ChannelHz(kControlChannel));
    }
  }

  // 所有主机同时在 F 上建立会话。 / All hosts open their sessions on F at once.
  void OpenTogether()
  {
    for (auto& pair : pairs_)
    {
      pair->connect_next = true;
    }
    Round(0);
    for (auto& pair : pairs_)
    {
      CHECK_EQ(pair->exec_a.local, 0U);
      CHECK_EQ(pair->failed, 0U);
      CHECK(pair->radio_a.Frequency() == ChannelHz(kSharedChannel));
    }
  }

  // 长短命令交替，直到没有两对重叠，再稳定 kSettledRounds 轮；分开后不再有干扰，所以这段
  // 时间一次重发都没有。 / Short and long commands in turn until no two pairs overlap and
  // that held for kSettledRounds rounds; once apart nothing interferes, so there is not a
  // single resend in that time.
  void RunUntilApart()
  {
    uint32_t apart = 0;
    for (uint32_t round = 0; round < kLimitRounds && apart < kSettledRounds; ++round)
    {
      Round(round);
      if (!Apart())
      {
        apart = 0;
        continue;
      }
      if (apart == 0U)
      {
        before_.clear();
        for (auto& pair : pairs_)
        {
          before_.push_back(pair->Stats());
        }
      }
      ++apart;
    }
    CHECK_EQ(apart, kSettledRounds);
    CHECK_EQ(before_.size(), pairs_.size());
    for (size_t i = 0; i < pairs_.size() && i < before_.size(); ++i)
    {
      AirPair& pair = *pairs_[i];
      const StatsAnswer stats = pair.Stats();
      std::printf(
          "    pair %zu: %u commands, %u failed, %u resends, %u bad, channel %u\n", i,
          pair.total, pair.failed, stats.retries, pair.radio_a.bad + pair.radio_b.bad,
          stats.channel);
      CHECK_EQ(stats.retries, before_[i].retries);
      CHECK_EQ(stats.failures, before_[i].failures);
      // 串台是硬错误：命令到了别的对的目标，或者应答对不上。 / Crosstalk is a hard error:
      // a command reaching another pair's target, or an answer that does not match.
      CHECK_EQ(pair.exec_a.foreign, 0U);
      CHECK_EQ(pair.exec_b.foreign, 0U);
      CHECK_EQ(pair.wrong, 0U);
      // 低速率不比地址，别的对的包到得了接收机、CRC 错；高速率到不了。 / The low rate
      // does not compare addresses, so another pair's packets reach the receiver with a
      // CRC error; at the high rate they do not reach it.
      if (rate_ == Rate::kLow)
      {
        CHECK(pair.radio_a.bad + pair.radio_b.bad > 0U);
      }
      else
      {
        CHECK_EQ(pair.radio_a.bad + pair.radio_b.bad, 0U);
      }
    }
  }

 private:
  static constexpr uint32_t kSettledRounds = 50;
  static constexpr uint32_t kLimitRounds = 2000;

  void Round(uint32_t round)
  {
    together_ = true;
    for (auto& pair : pairs_)
    {
      pair->turn_taken = false;
    }
    for (size_t i = 0; i < pairs_.size(); ++i)
    {
      pairs_[(round + i) % pairs_.size()]->TakeTurn();
    }
    together_ = false;
  }

  void Drive()
  {
    for (auto& pair : pairs_)
    {
      (void)pair->b->Service();
    }
    if (!together_)
    {
      return;
    }
    for (auto& pair : pairs_)
    {
      pair->TakeTurn();
    }
  }

  bool Apart()
  {
    for (size_t i = 0; i < pairs_.size(); ++i)
    {
      const uint32_t hz = pairs_[i]->radio_a.Frequency();
      if (hz == ChannelHz(kControlChannel))
      {
        return false;
      }
      for (size_t k = i + 1; k < pairs_.size(); ++k)
      {
        if (Overlap(pairs_[k]->radio_a.Frequency(), hz))
        {
          return false;
        }
      }
    }
    return true;
  }

  Rate rate_;
  bool together_ = false;
  Air air_;
  std::vector<std::unique_ptr<AirPair>> pairs_;
  std::vector<StatsAnswer> before_;
};

void RunPairs(size_t count, Rate rate)
{
  Fleet fleet(count, rate);
  fleet.OpenOneByOne();
  fleet.OpenTogether();
  fleet.RunUntilApart();
}
}  // namespace

TEST(radio_two_pairs_gfsk) { RunPairs(2, Rate::kHigh); }

TEST(radio_two_pairs_flrc) { RunPairs(2, Rate::kLow); }

TEST(radio_four_pairs_gfsk) { RunPairs(4, Rate::kHigh); }
