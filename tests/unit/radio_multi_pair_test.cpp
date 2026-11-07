// SPDX-License-Identifier: Apache-2.0
// 几对探针共用内存里的一段“空中”。判据：docs/protocol.md 第 8 节：几对共用 F，不同的对靠
// CRC 初值和链路地址分开，变差的信道被换掉。这里不模拟近距离阻塞和接收灵敏度，带宽外完全
// 不干扰。
//
// 单线程模拟：主机阻塞在一次转发里时，由它的 Poll 带动别的探针，别的主机的命令就嵌套在这
// 里跑，它们的包和外层的包在空中重叠。每轮每个主机一条命令，每轮换一个主机在最外层。第一
// 次建立会话要扫描 0.6 s，这段时间外层不动，所以各对先单独建立一次。
//
// Several pairs of probes share an in-memory "air". Oracle: docs/protocol.md section 8:
// the pairs share F, the CRC seed and the link address keep them apart, and a channel
// that turned bad is left. Blocking at short range and receiver sensitivity are not
// modelled, and there is no interference beyond the bandwidth.
//
// A single-thread simulation: a host blocked in a forward drives the other probes from
// its Poll, so the other hosts' commands run nested there and their packets overlap the
// outer ones on air. Every host sends one command per round, and each round another host
// is the outermost. The first session scans for 0.6 s, during which the outer frames
// stand still, so each pair opens one session on its own first.
#include <cstring>
#include <memory>
#include <vector>

#include "check.hpp"
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;
using namespace RadioTest;

namespace
{
// 所有对都觉得最安静的点，和每对自己次好的点：扫描结果都一样时，几个主机会一起换到同一个
// 点 / The point every pair finds quietest, and each pair's own second best: with
// identical scan results the hosts would move to the same point together.
constexpr uint8_t kSharedChannel = 20;
constexpr uint8_t SecondChannel(uint8_t index)
{
  return static_cast<uint8_t>(40U + 20U * index);
}

// 一对探针和它们的计数 / A pair of probes and its counters.
struct FleetPair : Pair
{
  FleetPair(Air& shared, const uint8_t* a_id, const uint8_t* b_id, uint8_t index,
            Rate rate)
      : Pair(shared, a_id, b_id, rate), marker(static_cast<uint8_t>(0x40U + index))
  {
    exec_a.marker = marker;
    exec_b.marker = marker;
    radio_a.noise = QuietAt(kSharedChannel, SecondChannel(index));
  }

  std::vector<uint8_t> Marked(uint8_t id, uint16_t size) const
  {
    auto c = Command(id, size);
    c[1] = marker;
    return c;
  }

  // 发一条命令，核对应答。 / Send one command and check the answer.
  void Forward(const std::vector<uint8_t>& c)
  {
    const auto answer = exec_a.Usb(c);
    ++total;
    if (answer.size() == 2U && answer[1] == Byte(DapStatus::ERROR))
    {
      ++failed;  // 转发失败，上位机看得到 / a failed forward the PC sees
    }
    else if (answer != Expected(c))
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
    Forward(connect_next ? Marked(Byte(DapCmd::CONNECT), 2)
                         : Marked(Byte(DapCmd::TRANSFER), kSizes[total % 3U]));
    connect_next = false;
    busy = false;
  }

  uint8_t marker;
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
  Fleet(size_t count, Rate rate)
  {
    VirtualClock::StartBeforeWrap(VirtualClock::kLeadUs);
    for (size_t i = 0; i < count; ++i)
    {
      uint8_t uid_a[kUidSize];
      uint8_t uid_b[kUidSize];
      std::memset(uid_a, static_cast<int>(0x10U + i), sizeof(uid_a));
      std::memset(uid_b, static_cast<int>(0x80U + i), sizeof(uid_b));
      pairs_.push_back(
          std::make_unique<FleetPair>(air_, uid_a, uid_b, static_cast<uint8_t>(i), rate));
      pairs_.back()->radio_a.on_poll = [this] { Drive(); };
    }
  }

  // 各对单独建立第一个会话（扫描），然后都回到 F。 / Each pair opens its first session on
  // its own (the scan), and then all are back on F.
  void OpenOneByOne()
  {
    for (auto& pair : pairs_)
    {
      pair->Forward(pair->Marked(Byte(DapCmd::CONNECT), 2));
      pair->Forward(pair->Marked(Byte(DapCmd::DISCONNECT), 2));
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
    const uint32_t collisions = air_.collisions;
    Round();
    CHECK(air_.collisions > collisions);
    for (auto& pair : pairs_)
    {
      CHECK_EQ(pair->exec_a.local, 0U);
      CHECK_EQ(pair->failed, 0U);
      CHECK(pair->radio_a.Frequency() == ChannelHz(kSharedChannel));
    }
  }

  // 长短命令交替，直到没有两对重叠 / Short and long commands in turn until no two pairs
  // overlap.
  void RunUntilApart()
  {
    while (round_ < kLimitRounds && !Apart())
    {
      Round();
    }
    CHECK(Apart());
  }

  // 分开以后没有干扰：不再有重发和失败，也不再换信道。 / Once apart nothing interferes:
  // no more resends or failures, and no more changes of channel.
  void CheckQuietOnceApart()
  {
    std::vector<StatsAnswer> before;
    for (auto& pair : pairs_)
    {
      before.push_back(ReadStats(pair->a));
    }
    for (int i = 0; i < 50; ++i)
    {
      Round();
    }
    for (size_t i = 0; i < pairs_.size(); ++i)
    {
      const StatsAnswer stats = ReadStats(pairs_[i]->a);
      CHECK_EQ(stats.retries, before[i].retries);
      CHECK_EQ(stats.failures, before[i].failures);
      CHECK_EQ(stats.channel, before[i].channel);
      std::printf("    pair %zu: %u commands, %u resends, %u bad, channel %u\n", i,
                  stats.commands, stats.retries,
                  pairs_[i]->radio_a.bad + pairs_[i]->radio_b.bad, stats.channel);
    }
  }

  // 串台是硬错误：命令到了别的对的目标，或者应答对不上。低速率不比地址，别的对的包到得了
  // 接收机、 CRC 错；高速率到不了。 / Crosstalk is a hard error: a command reaching
  // another pair's target, or an answer that does not match. The low rate does not
  // compare addresses, so another pair's packets reach the receiver with a CRC error; at
  // the high rate they do not reach it.
  void CheckIsolation(Rate rate)
  {
    for (auto& pair : pairs_)
    {
      CHECK_EQ(pair->exec_a.foreign, 0U);
      CHECK_EQ(pair->exec_b.foreign, 0U);
      CHECK_EQ(pair->wrong, 0U);
      const uint32_t bad = pair->radio_a.bad + pair->radio_b.bad;
      CHECK(rate == Rate::kLow ? bad > 0U : bad == 0U);
    }
  }

 private:
  static constexpr uint32_t kLimitRounds = 2000;

  void Round()
  {
    together_ = true;
    for (auto& pair : pairs_)
    {
      pair->turn_taken = false;
    }
    for (size_t i = 0; i < pairs_.size(); ++i)
    {
      pairs_[(round_ + i) % pairs_.size()]->TakeTurn();
    }
    together_ = false;
    ++round_;
  }

  // 主机先走：外层的包还在空中，这样才撞得上。 / The hosts first: the outer packet is
  // still on air, so that they can collide with it.
  void Drive()
  {
    if (together_)
    {
      for (auto& pair : pairs_)
      {
        pair->TakeTurn();
      }
    }
    for (auto& pair : pairs_)
    {
      (void)pair->b.Service();
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

  uint32_t round_ = 0;
  bool together_ = false;
  Air air_;
  std::vector<std::unique_ptr<FleetPair>> pairs_;
};

void RunPairs(size_t count, Rate rate)
{
  Fleet fleet(count, rate);
  fleet.OpenOneByOne();
  fleet.OpenTogether();
  fleet.RunUntilApart();
  fleet.CheckQuietOnceApart();
  fleet.CheckIsolation(rate);
}
}  // namespace

TEST(radio_two_pairs_high_rate) { RunPairs(2, Rate::kHigh); }

TEST(radio_two_pairs_low_rate) { RunPairs(2, Rate::kLow); }

TEST(radio_four_pairs_high_rate) { RunPairs(4, Rate::kHigh); }
