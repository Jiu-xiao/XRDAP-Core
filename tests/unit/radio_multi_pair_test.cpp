// SPDX-License-Identifier: Apache-2.0
// 几对探针同时工作，共用内存里的一段“空中”：频率相差不到一个带宽、时间重叠的包都
// 丢掉，包要发射一段真实时间，RSSI 看得到带宽内别的对的发射。每对一个线程。这里不模拟
// 近距离阻塞和接收灵敏度，带宽外完全不干扰。判据：docs/protocol.md 第 8 节：不同的对靠
// CRC 初值（GFSK 还有同步字）分开，变差的信道被换掉。
// Several pairs of probes at once, sharing an in-memory "air": packets less than one
// bandwidth apart that overlap in time are lost, a packet takes real time on air, and the
// RSSI sees the other pairs' transmissions within the bandwidth. One thread per pair.
// Blocking at short range and receiver sensitivity are not modelled, and there is no
// interference beyond the bandwidth. Oracle: docs/protocol.md section 8: the CRC seed
// (and in GFSK the sync word) keeps pairs apart, and a channel that turned bad is left.
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "check.hpp"
#include "radio_test_time.hpp"
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

  // 结束了的包送给同频、不在发射的收发器。调用时持锁。 / Packets that ended reach the
  // transceivers on their frequency that are not transmitting. Called with the lock held.
  void Advance(uint32_t now);

  std::mutex lock;
  std::vector<Flight> flights;

 private:
  std::vector<AirRadio*> radios_;
};

// 接口同 xrobot-org/SX1281 的假收发器 / Fake transceiver with the interface of
// xrobot-org/SX1281.
class AirRadio
{
 public:
  enum class Modulation : uint8_t
  {
    LORA,
    GFSK,
    FLRC
  };
  struct Config
  {
    Modulation modulation = Modulation::LORA;
    uint32_t frequency_hz = 0;
    int8_t tx_power_dbm = 0;
    uint8_t sync_word[4] = {};
    uint16_t crc_seed = 0;
  };

  explicit AirRadio(Air& air) : air_(air) { air_.Join(this); }

  LibXR::ErrorCode Start(const Config& config)
  {
    std::lock_guard<std::mutex> hold(air_.lock);
    config_ = config;
    rx_.clear();
    sending_ = false;
    return LibXR::ErrorCode::OK;
  }
  uint8_t MaxPayload() const
  {
    return config_.modulation == Modulation::FLRC ? 127 : 255;
  }
  void PrepareSend() {}
  LibXR::ErrorCode Tune(uint32_t frequency_hz)
  {
    std::lock_guard<std::mutex> hold(air_.lock);
    config_.frequency_hz = frequency_hz;
    rx_.clear();
    return LibXR::ErrorCode::OK;
  }
  int16_t RssiInstant()
  {
    std::lock_guard<std::mutex> hold(air_.lock);
    const uint32_t now = Now();
    air_.Advance(now);
    for (const auto& flight : air_.flights)
    {
      if (Overlap(flight.hz, config_.frequency_hz))
      {
        return -100;  // -50 dBm
      }
    }
    return -200;  // -100 dBm
  }
  uint32_t TxPackets() const { return 0; }
  uint32_t TxErrors() const { return 0; }
  uint32_t RxPackets() const { return 0; }
  uint32_t RxErrors() const { return 0; }
  uint32_t Rearms() const { return 0; }
  uint32_t Restarts() const { return 0; }
  uint32_t Frequency()
  {
    std::lock_guard<std::mutex> hold(air_.lock);
    return config_.frequency_hz;
  }

  LibXR::ErrorCode Send(const uint8_t* data, uint8_t size)
  {
    std::lock_guard<std::mutex> hold(air_.lock);
    const uint32_t now = Now();
    air_.Advance(now);
    if (sending_ && static_cast<int32_t>(tx_end_us_ - now) > 0)
    {
      return LibXR::ErrorCode::BUSY;
    }
    // 2 Mb/s 或 1.3 Mb/s，加前导码和包头 / 2 Mb/s or 1.3 Mb/s, plus preamble and header.
    const uint32_t us_per_byte = config_.modulation == Modulation::FLRC ? 7U : 4U;
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
      std::lock_guard<std::mutex> hold(air_.lock);
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
    std::lock_guard<std::mutex> hold(air_.lock);
    air_.Advance(Now());
    if (rx_.empty())
    {
      return 0;
    }
    const auto packet = rx_.front();
    rx_.pop_front();
    if (packet.empty())
    {
      return -1;  // 坏包 / a bad packet
    }
    std::memcpy(out, packet.data(), packet.size());
    return static_cast<int>(packet.size());
  }

  // 空中送来一包。调用时持锁。FLRC 的同步字各对相同，别的对的包收得到但 CRC 错；GFSK
  // 的同步 字不同，收不到。 / A packet from the air. Called with the lock held. In FLRC
  // every pair has the same sync word, so another pair's packet arrives with a CRC error;
  // in GFSK the sync words differ and it is not received at all.
  void Deliver(const Air::Flight& flight, uint32_t now)
  {
    if (flight.from == this || flight.hz != config_.frequency_hz ||
        (sending_ && static_cast<int32_t>(tx_end_us_ - now) > 0) ||
        flight.from->config_.modulation != config_.modulation ||
        std::memcmp(flight.from->config_.sync_word, config_.sync_word, 4) != 0)
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

 private:
  Air& air_;
  Config config_{};
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
      local.fetch_add(1);
      return {};
    }
    return {resp, resp + out};
  }
  void ExecuteHere(Transport, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    if (len >= 2U && req[1] != marker_)
    {
      foreign.fetch_add(1);
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

  std::atomic<uint32_t> local{0};
  std::atomic<uint32_t> foreign{0};

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

// 一对探针和它们的计数 / A pair of probes and its counters.
struct AirPair
{
  AirPair(Air& air, uint8_t index, Modulation modulation)
      : marker(static_cast<uint8_t>(0x40U + index)),
        radio_a(air),
        radio_b(air),
        exec_a(marker),
        exec_b(marker)
  {
    std::memset(uid_a, 0x10 + index, sizeof(uid_a));
    std::memset(uid_b, 0x80 + index, sizeof(uid_b));
    a = std::make_unique<PairLink>(radio_a, exec_a, store, uid_a, AirRadio::Config{});
    b = std::make_unique<PairLink>(radio_b, exec_b, store, uid_b, AirRadio::Config{});
    radio_a.on_poll = [this] { (void)b->Service(); };
    Settings settings;
    settings.paired = 1;
    settings.link_id = LinkId(uid_a, uid_b);
    settings.modulation = modulation;
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

  // 这个线程跑一对：连接后不停地发长短命令，核对每个应答 / One pair on this thread:
  // connect, then keep sending short and long commands and check every answer.
  void Run(const std::atomic<bool>& stop)
  {
    (void)exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
    static constexpr uint16_t kSizes[] = {16, 64, 1024};
    for (uint32_t i = 0; !stop.load(); ++i)
    {
      const auto c = Command(Byte(DapCmd::TRANSFER), kSizes[i % 3U]);
      const auto answer = exec_a.Usb(c);
      total.fetch_add(1);
      if (answer.size() == 2U && answer[1] == Byte(DapStatus::ERROR))
      {
        failed.fetch_add(1);  // 转发失败，上位机看得到 / a failed forward the PC sees
        continue;
      }
      bool same = answer.size() == c.size() && answer[0] == c[0];
      for (size_t k = 1; same && k < c.size(); ++k)
      {
        same = answer[k] == c[c.size() - k];
      }
      if (!same)
      {
        wrong.fetch_add(1);
      }
    }
    hz.store(radio_a.Frequency());
    (void)exec_a.Usb(Command(Byte(DapCmd::DISCONNECT), 2));
    (void)b->Service();
  }

  uint8_t marker;
  uint8_t uid_a[kUidSize] = {};
  uint8_t uid_b[kUidSize] = {};
  AirRadio radio_a, radio_b;
  PairExec exec_a, exec_b;
  NoStore store;
  std::unique_ptr<PairLink> a, b;
  std::atomic<uint32_t> total{0};
  std::atomic<uint32_t> failed{0};
  std::atomic<uint32_t> wrong{0};
  std::atomic<uint32_t> hz{0};
};

// 几对同时跑到互不重叠、又稳定了一阵，或者到 limit_ms。 / Run the pairs together until no
// two overlap and that held for a while, or until limit_ms.
void RunPairs(size_t count, Modulation modulation, uint32_t limit_ms)
{
  (void)RadioTestTime();
  Air air;
  std::vector<std::unique_ptr<AirPair>> pairs;
  for (size_t i = 0; i < count; ++i)
  {
    pairs.push_back(std::make_unique<AirPair>(air, static_cast<uint8_t>(i), modulation));
  }
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (auto& pair : pairs)
  {
    threads.emplace_back([&pair, &stop] { pair->Run(stop); });
  }

  const auto distinct = [&pairs]
  {
    for (size_t i = 0; i < pairs.size(); ++i)
    {
      const uint32_t hz = pairs[i]->radio_a.Frequency();
      if (hz == ChannelHz(kControlChannel))
      {
        return false;
      }
      for (size_t k = i + 1; k < pairs.size(); ++k)
      {
        if (Overlap(pairs[k]->radio_a.Frequency(), hz))
        {
          return false;
        }
      }
    }
    return true;
  };
  uint32_t held_ms = 0;
  for (uint32_t ms = 0; ms < limit_ms && held_ms < 1500U; ms += 50U)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    held_ms = distinct() ? held_ms + 50U : 0U;
  }
  stop.store(true);
  for (auto& thread : threads)
  {
    thread.join();
  }

  for (size_t i = 0; i < pairs.size(); ++i)
  {
    const AirPair& pair = *pairs[i];
    std::printf("    pair %zu: %u commands, %u failed, channel %.1f MHz\n", i,
                pair.total.load(), pair.failed.load(),
                static_cast<double>(pair.hz.load()) / 1e6);
    // 串台是硬错误：命令到了别的对的目标，或者应答对不上。 / Crosstalk is a hard error: a
    // command reaching another pair's target, or an answer that does not match.
    CHECK_EQ(pair.exec_a.foreign.load(), 0U);
    CHECK_EQ(pair.exec_b.foreign.load(), 0U);
    CHECK_EQ(pair.wrong.load(), 0U);
    CHECK_EQ(pair.exec_a.local.load(), 0U);
    CHECK(pair.total.load() > 0U);
    // 抢信道时可能超时一两条，绝大多数要成功。 / A contested channel may time out a
    // command or two; nearly all must succeed.
    CHECK(pair.failed.load() * 20U <= pair.total.load());
    CHECK(pair.hz.load() != ChannelHz(kControlChannel));
    for (size_t k = i + 1; k < pairs.size(); ++k)
    {
      CHECK(!Overlap(pairs[k]->hz.load(), pair.hz.load()));
    }
  }
}
}  // namespace

TEST(radio_two_pairs_gfsk) { RunPairs(2, Modulation::GFSK, 20000); }

TEST(radio_two_pairs_flrc) { RunPairs(2, Modulation::FLRC, 20000); }

TEST(radio_four_pairs_gfsk) { RunPairs(4, Modulation::GFSK, 30000); }
