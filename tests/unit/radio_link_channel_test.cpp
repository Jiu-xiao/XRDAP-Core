// SPDX-License-Identifier: Apache-2.0
// RadioLink 的信道：一对探针，射频是内存里的假收发器。判据：channel_picker.hpp 和
// docs/protocol.md 第 8.4 节 / The channels of RadioLink: a pair of probes over fake
// transceivers in memory. Oracle: channel_picker.hpp and docs/protocol.md section 8.4.
#include <vector>

#include "check.hpp"
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;
using namespace RadioTest;

// 会话在 F 上建立，主机扫描后两端换到最安静的点；首次扫描在 DAP_Connect 里，要短于工具的
// USB 超时（OpenOCD 1 s） / The session opens on F, the host scans and both move to the
// quietest point; the first scan runs inside DAP_Connect and must stay below the tools'
// USB timeout (OpenOCD: 1 s).
TEST(radio_link_opens_on_f_and_takes_the_quietest_point)
{
  Pair p(true);
  CHECK(p.radio_a.Frequency() == ChannelHz(93));
  CHECK(p.radio_b.Frequency() == ChannelHz(93));
  p.radio_a.noise = QuietAt(150);
  const uint32_t begin = NowUs();
  const auto connect = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_a.Usb(connect) == Expected(connect));
  CHECK(NowUs() - begin < 1'000'000U);
  CHECK(p.radio_a.Frequency() == ChannelHz(150));
  CHECK(p.radio_b.Frequency() == ChannelHz(150));
}

// 统计记下了这个会话：两条转发的命令，都一次成功 / The statistics record the session: two
// forwarded commands, both right the first time.
TEST(radio_link_stats_count_a_session)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(150);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  const StatsAnswer stats = ReadStats(p.a);
  CHECK_EQ(stats.channel, 150U);
  CHECK_EQ(stats.commands, 2U);
  CHECK_EQ(stats.retries, 0U);
  CHECK_EQ(stats.failures, 0U);
}

// 会话结束时两端都回 F / Both return to F when the session ends.
TEST(radio_link_disconnect_returns_both_to_f)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(150);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(150));
  (void)p.exec_a.Usb(Command(Byte(DapCmd::DISCONNECT), 1));
  CHECK(p.radio_a.Frequency() == ChannelHz(93));
  CHECK(p.radio_b.Frequency() == ChannelHz(93));
}

// 第二个会话用第一次的扫描结果，不再扫描 / A second session uses the results of the first
// scan and does not scan again.
TEST(radio_link_second_session_does_not_scan)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(150);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  (void)p.exec_a.Usb(Command(Byte(DapCmd::DISCONNECT), 1));
  const uint32_t reads = p.radio_a.rssi_reads;
  CHECK(reads > 0U);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(150));
  CHECK(p.radio_b.Frequency() == ChannelHz(150));
  CHECK_EQ(p.radio_a.rssi_reads, reads);
}

// CHANNEL_ACK 丢了：对方已换过去，主机每发 4 次 CHANNEL 就到目标信道上 PING 一次，不用等
// 100 ms 换信道失败 / The CHANNEL_ACK is lost: the peer has moved, and after every 4
// sends of CHANNEL the host pings on the target channel, without waiting for the change
// to fail after 100 ms.
TEST(radio_link_channel_ack_lost)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(20);
  bool lost = false;
  uint32_t lost_at = 0;
  p.air.lose = [&](const Flight& f)
  {
    if (f.data[0] != kChannelAck || lost)
    {
      return false;
    }
    lost = true;
    lost_at = NowUs();
    return true;
  };
  const auto c = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(lost);
  CHECK(NowUs() - lost_at < 100'000U);
  CHECK(p.radio_a.Frequency() == ChannelHz(20));
  CHECK(p.radio_b.Frequency() == ChannelHz(20));
  CHECK_EQ(ReadStats(p.a).failures, 0U);
}

// CHANNEL 一直到不了对方：100 ms 后主机放弃这个点，再用 30 ms 看对方是不是已经过去，然后
// 留在 F 上，会话照常。这个点被拉黑，下个会话换下一个最好的点 / CHANNEL never reaches the
// peer: after 100 ms the host gives the point up, spends 30 ms looking for the peer
// there, and stays on F, where the session works. The point is banned, so the next
// session takes the next best one.
TEST(radio_link_channel_change_gives_up)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(150, 120);
  uint32_t first = 0;
  bool sent = false;
  p.air.lose = [&](const Flight& f)
  {
    if (f.data[0] != kChannel)
    {
      return false;
    }
    if (!sent)
    {
      first = NowUs();
      sent = true;
    }
    return true;
  };
  const auto connect = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_a.Usb(connect) == Expected(connect));
  const uint32_t waited = NowUs() - first;
  CHECK(sent && waited >= 130'000U && waited < 140'000U);
  CHECK(p.radio_a.Frequency() == ChannelHz(93));
  CHECK(p.radio_b.Frequency() == ChannelHz(93));
  p.air.lose = nullptr;
  (void)p.exec_a.Usb(Command(Byte(DapCmd::DISCONNECT), 1));
  CHECK(p.exec_a.Usb(connect) == Expected(connect));
  CHECK(p.radio_a.Frequency() == ChannelHz(120));
  CHECK(p.radio_b.Frequency() == ChannelHz(120));
}

// 数据信道被堵死：主机拉黑它、回 F；远端沉默超时后回 F，会话在另一个点上恢复 / The data
// channel is jammed: the host bans it and returns to F; the remote returns to F after its
// silence timeout, and the session recovers on another point.
TEST(radio_link_channel_jammed)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(150, 120);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_b.Frequency() == ChannelHz(150));
  p.air.lose = [](const Flight& f) { return f.params.frequency_hz == ChannelHz(150); };
  const auto c = Command(Byte(DapCmd::TRANSFER), 64);
  bool ok = false;
  for (int attempt = 0; attempt < 10 && !ok; ++attempt)
  {
    ok = p.exec_a.Usb(c) == Expected(c);
  }
  CHECK(ok);
  CHECK(p.radio_a.Frequency() != ChannelHz(150));
  CHECK(p.radio_a.Frequency() != ChannelHz(93));
  CHECK(p.radio_a.Frequency() == p.radio_b.Frequency());
}

// 对方已换信道（ACK 到了）而新信道上 PING 不通的时间比确认用的 30 ms 长：主机在那个信道上
// 再找到它，不等它的沉默超时 / The peer has moved (the ACK arrived) but PINGs on the new
// channel fail for longer than the 30 ms of the confirmation: the host finds it there
// again instead of waiting for its silence timeout.
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
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(reached);
  CHECK(p.radio_a.Frequency() == p.radio_b.Frequency());
}

// 锁定的信道上连发 3 次都没收到：复位射频一次，留在这个信道 / Nothing heard after 3 sends
// on a locked channel: the radio is reset once and stays on the channel.
TEST(radio_link_deaf_locked)
{
  Pair p(true);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 40}, nullptr), Status::kOk);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(40));
  const uint32_t starts = p.radio_a.starts;
  p.air.lose = [&p](const Flight& f) { return f.from == &p.radio_b; };
  (void)p.exec_a.Usb(Command(Byte(DapCmd::TRANSFER), 16));
  CHECK_EQ(p.radio_a.starts, starts + 1U);
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
  RunFor(p.b, 2'400'000);
  CHECK(p.radio_b.Frequency() == ChannelHz(40));
  RunFor(p.b, 200'000);
  CHECK(p.radio_b.Frequency() == ChannelHz(93));
  const auto c = Command(Byte(DapCmd::TRANSFER), 16);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(p.radio_b.Frequency() == ChannelHz(40));
}

// 锁定信道：保存为索引 + 1，下次选信道时才生效 / A locked channel is stored as its index
// + 1 and takes effect at the next choice of channel.
TEST(radio_link_channel_lock_is_stored)
{
  Pair p(true);
  std::vector<uint8_t> answer;
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 40}, &answer), Status::kOk);
  CHECK((answer == std::vector<uint8_t>{0x19, 0x00, 93, 40}));
  CHECK_EQ(p.store_a.saved.channel_lock, 41U);
}

// 越界的信道和不认识的 mode 被拒，什么都不保存 / A channel out of range and an unknown
// mode are refused and nothing is stored.
TEST(radio_link_channel_lock_refuses_bad_arguments)
{
  Pair p(true);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 160}, nullptr), Status::kBadRequest);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 2, 1}, nullptr), Status::kBadRequest);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1}, nullptr), Status::kBadRequest);
  CHECK_EQ(p.store_a.saves, 0U);
}

// 下个会话直接用锁定的信道，不扫描 / The next session uses the locked channel without
// scanning.
TEST(radio_link_locked_channel_needs_no_scan)
{
  Pair p(true);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 40}, nullptr), Status::kOk);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(40));
  CHECK(p.radio_b.Frequency() == ChannelHz(40));
  CHECK_EQ(p.radio_a.rssi_reads, 0U);
}

// mode 0 回到自动，应答里锁定的信道是 0xFF / Mode 0 goes back to automatic, and the
// answer shows 0xFF as the locked channel.
TEST(radio_link_channel_unlock)
{
  Pair p(true);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 40}, nullptr), Status::kOk);
  std::vector<uint8_t> answer;
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 0, 0}, &answer), Status::kOk);
  CHECK((answer == std::vector<uint8_t>{0x19, 0x00, 93, 0xFF}));
  CHECK_EQ(p.store_a.saved.channel_lock, 0U);
}

// 同时找对方的几对不会一直撞在一起：PING 的等待在 3 ms 之上加一段至多约 1 ms 的偏移，随时
// 间和 CRC 初值变化 / Pairs looking for their peers at the same time do not collide again
// and again: the wait of a PING adds to 3 ms an offset of up to about 1 ms that changes
// with the time and the CRC seed.
TEST(radio_ping_wait_differs)
{
  Air air;
  FakeRadio radio_x(air);
  FakeRadio radio_y(air);
  RadioPort<FakeRadio> x(radio_x);
  RadioPort<FakeRadio> y(radio_y);
  x.Start(Rate::kHigh, PairIdentity(1));
  y.Start(Rate::kHigh, PairIdentity(2));
  VirtualClock::StartBeforeWrap(VirtualClock::kLeadUs);
  const uint32_t first = x.PingWaitUs();
  VirtualClock::StartBeforeWrap(VirtualClock::kLeadUs);
  CHECK(y.PingWaitUs() != first);  // 同一时刻，另一个 CRC 初值 / same time, other seed
  bool changed = false;
  for (int i = 0; i < 100; ++i)
  {
    const uint32_t wait = x.PingWaitUs();
    CHECK(wait >= 3'000U && wait < 4'100U);
    changed = changed || wait != first;
  }
  CHECK(changed);
}
