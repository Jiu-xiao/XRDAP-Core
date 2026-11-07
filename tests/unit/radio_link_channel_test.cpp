// SPDX-License-Identifier: Apache-2.0
// RadioLink 的信道：一对探针，射频是内存里的假收发器。判据：channel_picker.hpp 和
// docs/protocol.md 第 8.4 节 / The channels of RadioLink: a pair of probes over fake
// transceivers in memory. Oracle: channel_picker.hpp and docs/protocol.md section 8.4.
#include <cstring>
#include <functional>
#include <vector>

#include "check.hpp"
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;
using namespace RadioTest;

// 信道：会话在 F 上建立，主机扫描后两端换到最安静的点；会话结束时都回 F；第二个会话不再
// 扫描 / Channels: the session opens on F, the host scans and both move to the quietest
// point; both return to F when the session ends; a second session does not scan.
TEST(radio_link_channel)
{
  Pair p(true);
  CHECK(p.radio_a.Frequency() == ChannelHz(kControlChannel));
  p.radio_a.noise = QuietAt(150);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(150));
  CHECK(p.radio_b.Frequency() == ChannelHz(150));
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  std::vector<uint8_t> stats;
  CHECK_EQ(ControlStatus(p.a, {Op::kStats}, &stats), Status::kOk);
  StatsAnswer answer;
  CHECK_EQ(stats.size(), sizeof(answer));
  std::memcpy(&answer, stats.data(), sizeof(answer));
  CHECK(answer.count == 13 && answer.channel == 150U);
  CHECK(answer.commands >= 2U && answer.failures == 0U);
  CHECK(p.exec_a.Usb(Command(Byte(DapCmd::DISCONNECT), 1)) ==
        Expected(Command(Byte(DapCmd::DISCONNECT), 1)));
  (void)p.b.Service();
  CHECK(p.radio_a.Frequency() == ChannelHz(kControlChannel));
  CHECK(p.radio_b.Frequency() == ChannelHz(kControlChannel));
  // 再连一次不再扫描 / A second session does not scan again.
  const uint32_t reads = p.radio_a.rssi_reads;
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_b.Frequency() == ChannelHz(150));
  CHECK_EQ(p.radio_a.rssi_reads, reads);
}

// CHANNEL_ACK 丢了：对方已换过去，主机到目标信道上 PING 找到它 / The CHANNEL_ACK is
// lost: the peer has moved, and the host finds it with a PING on the target channel.
TEST(radio_link_channel_ack_lost)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(20);
  const uint32_t first = p.radio_b.sent;
  p.air.lose = [&](const Flight& f)
  { return f.from == &p.radio_b && p.radio_b.sent == first + 2; };  // PONG, 然后 ACK
  const auto c = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(p.radio_a.Frequency() == ChannelHz(20) && p.radio_b.Frequency() == ChannelHz(20));
}

// 数据信道被堵死：主机拉黑它、回 F；远端沉默超时后回 F，会话在另一个点上恢复 / The data
// channel is jammed: the host bans it and returns to F; the remote returns to F after
// its silence timeout, and the session recovers on another point.
TEST(radio_link_channel_jammed)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(150, 120);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_b.Frequency() == ChannelHz(150));
  p.air.lose = [](const Flight& f) { return f.params.frequency_hz == ChannelHz(150); };
  const auto c = Command(Byte(DapCmd::TRANSFER), 64);
  const uint32_t begin = NowUs();
  bool ok = false;
  while (!ok && NowUs() - begin < 8'000'000U)
  {
    ok = p.exec_a.Usb(c) == Expected(c);
  }
  CHECK(ok);
  CHECK(p.radio_a.Frequency() != ChannelHz(150));
  CHECK(p.radio_a.Frequency() != ChannelHz(kControlChannel));
  CHECK(p.radio_a.Frequency() == p.radio_b.Frequency());
}

// 对方已换信道（ACK 到了）而新信道上一阵 PING 不通：主机在那个信道上再找到它，不等它的
// 沉默超时 / The peer has moved (the ACK arrived) but PINGs on the new channel fail for a
// while: the host finds it there again instead of waiting for its silence timeout.
TEST(radio_link_channel_lost_after_ack)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(30);
  uint32_t first = 0;
  bool reached = false;
  p.air.lose = [&](const Flight& f)
  {
    if (f.from != &p.radio_b || f.params.frequency_hz != ChannelHz(30))
    {
      return false;
    }
    if (!reached)
    {
      first = NowUs();
      reached = true;
    }
    return NowUs() - first < 35'000U;
  };
  const auto c = Command(Byte(DapCmd::CONNECT), 2);
  const uint32_t begin = NowUs();
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(NowUs() - begin < 2'000'000U);
  CHECK(reached);
  CHECK(p.radio_a.Frequency() == p.radio_b.Frequency());
}

// 锁定的信道上接收机聋了：复位射频，留在这个信道 / A deaf receiver on a locked channel:
// the radio is reset and stays on the channel.
TEST(radio_link_deaf_locked)
{
  Pair p(true);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 40}, nullptr), Status::kOk);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(40));
  const uint32_t starts = p.radio_a.starts;
  p.air.lose = [&p](const Flight& f) { return f.from == &p.radio_b; };
  (void)p.exec_a.Usb(Command(Byte(DapCmd::TRANSFER), 16));
  CHECK(p.radio_a.starts > starts);
  CHECK(p.radio_a.Frequency() == ChannelHz(40));
}

// 远端在数据信道上 2.5 s 没听到主机就回 F；主机下一条命令在 F 上重新建立 / The remote
// returns to F after 2.5 s on a data channel without hearing the host; the host's next
// command opens the session on F again.
TEST(radio_link_remote_silence)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(40);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_b.Frequency() == ChannelHz(40));
  RunFor(p.b, 2'700'000);
  CHECK(p.radio_b.Frequency() == ChannelHz(kControlChannel));
  const auto c = Command(Byte(DapCmd::TRANSFER), 16);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(p.radio_b.Frequency() == ChannelHz(40));
}

// 锁定信道：保存，下个会话直接用它、不扫描；越界的信道和扫描参数被拒 / A locked
// channel: stored, the next session uses it without scanning; channels and scan
// arguments out of range are refused.
TEST(radio_link_channel_lock)
{
  Pair p(true);
  std::vector<uint8_t> answer;
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 40}, &answer), Status::kOk);
  CHECK(p.store_a.saved.channel_lock == 41U && answer.size() == sizeof(ChannelAnswer));
  CHECK_EQ(answer[3], 40);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, kChannels}, nullptr),
           Status::kBadRequest);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 2, 1}, nullptr), Status::kBadRequest);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(40) && p.radio_b.Frequency() == ChannelHz(40));
  CHECK_EQ(p.radio_a.rssi_reads, 0U);  // 锁定时不扫描 / no scan while locked
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 0, 0}, nullptr), Status::kOk);
  CHECK_EQ(p.store_a.saved.channel_lock, 0U);
}
