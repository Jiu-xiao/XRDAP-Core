// SPDX-License-Identifier: Apache-2.0
// 经 RadioLink 的会话和分片串：一对探针，射频是内存里的假收发器。判据：radio_link.hpp 的
// 角色和会话规则，以及 docs/protocol.md 第 8.2 节 / Sessions and bursts through
// RadioLink: a pair of probes over fake transceivers in memory. Oracle: the role and
// session rules of radio_link.hpp and docs/protocol.md section 8.2.
#include <initializer_list>
#include <vector>

#include "check.hpp"
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;
using namespace RadioTest;

namespace
{
// 命令的所有分片，序号由测试给出 / All fragments of a command, with a given sequence.
std::vector<std::vector<uint8_t>> Burst(const std::vector<uint8_t>& c, uint8_t sequence)
{
  const uint8_t fragment = 255 - kHeader;
  const uint16_t total = static_cast<uint16_t>(c.size());
  std::vector<std::vector<uint8_t>> out;
  uint8_t packet[255];
  for (uint8_t i = 0; i < FragmentCount(total, fragment); ++i)
  {
    const uint8_t n =
        BuildFragment(kCommand, sequence, c.data(), total, fragment, i, packet);
    out.emplace_back(packet, packet + n);
  }
  return out;
}

// 主机的射频发一包，等它在空中走完 / The host's radio sends a packet and it finishes its
// time on air.
void Deliver(Pair& p, const std::vector<uint8_t>& packet)
{
  while (p.radio_a.Send(packet.data(), static_cast<uint8_t>(packet.size())) ==
         LibXR::ErrorCode::BUSY)
  {
  }
  while (p.radio_a.Sending())
  {
  }
}

// 让远端把在发的应答发完：5 片加上等下一片的 3 ms / Lets the remote finish the answer it
// is sending: 5 fragments plus the 3 ms it waits for a next one.
void Serve(Pair& p) { RunFor(p.b, 10'000); }
}  // namespace

// AUTO：DAP_Connect 找到对方后整个会话走无线，长短命令都逐字节到达；DAP_Disconnect 结束会
// 话，远端的连接作废 / AUTO: once DAP_Connect reaches the peer the whole session goes
// over the radio and commands of any size arrive byte for byte; DAP_Disconnect ends it
// and the remote's connection lapses.
TEST(radio_link_session)
{
  for (Rate m : {Rate::kHigh, Rate::kLow})
  {
    Pair p(true, m);
    CHECK(p.a.CurrentRole() == Role::REMOTE && p.b.CurrentRole() == Role::REMOTE);
    const auto info = Command(Byte(DapCmd::INFO), 2);
    CHECK(p.exec_a.Usb(info) == std::vector<uint8_t>{0xEE});  // DAP_Info 本地 / local
    const auto connect = Command(Byte(DapCmd::CONNECT), 2);
    CHECK(p.exec_a.Usb(connect) == Expected(connect));
    CHECK(p.a.CurrentRole() == Role::HOST && p.b.RemoteActive());
    const uint32_t lost = p.exec_b.lost;
    for (uint16_t size : {3, 250, 1024})
    {
      const auto c = Command(Byte(DapCmd::TRANSFER), size);
      CHECK(p.exec_a.Usb(c) == Expected(c));
    }
    const auto disconnect = Command(Byte(DapCmd::DISCONNECT), 1);
    CHECK(p.exec_a.Usb(disconnect) == Expected(disconnect));
    CHECK(p.a.CurrentRole() == Role::REMOTE && !p.b.RemoteActive());
    CHECK_EQ(p.exec_b.lost, lost + 1U);
    CHECK_EQ(p.exec_a.local, 1U);
  }
}

// 丢包时主机重发；远端用缓存应答重复的命令，同一命令只执行一次 / With packets lost the
// host resends; the remote answers a repeated command from its cache and executes each
// command once.
TEST(radio_link_resend)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  const uint32_t before = p.exec_b.executed;
  // 远端第一次应答的分片全部丢掉 / Every fragment of the remote's first answer is lost.
  int lost = 0;
  p.air.lose = [&lost](const Flight& f) { return f.data[0] == kAnswer && lost++ < 5; };
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK_EQ(p.exec_b.executed, before + 1U);
  CHECK_EQ(ReadStats(p.a).retries, 1U);
  CHECK_EQ(ReadStats(p.b).duplicates, 1U);
}

// 重发在中途拼齐命令时，远端等这一串发完才执行和应答：主机发射时听不到 / A resend that
// completes a command halfway: the remote executes and answers only after the burst,
// since the host cannot hear while it transmits.
TEST(radio_link_answer_after_burst)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  Serve(p);
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  const auto f = Burst(c, 200);
  CHECK_EQ(f.size(), 5U);
  const uint32_t executed = p.exec_b.executed;
  const uint32_t sent = p.radio_b.sent;
  for (size_t i : {0, 2, 3, 4})  // 第 1 片丢了 / fragment 1 lost
  {
    Deliver(p, f[i]);
  }
  (void)p.b.Service();
  CHECK_EQ(p.exec_b.executed, executed);
  Deliver(p, f[0]);
  Deliver(p, f[1]);  // 这里拼齐 / completes here
  size_t next = 2;
  p.radio_b.on_poll = [&]
  {
    CHECK_EQ(p.exec_b.executed, executed);
    CHECK_EQ(p.radio_b.sent, sent);
    if (next < f.size() &&
        p.radio_a.Send(f[next].data(), static_cast<uint8_t>(f[next].size())) ==
            LibXR::ErrorCode::OK)
    {
      ++next;
    }
  };
  (void)p.b.Service();
  p.radio_b.on_poll = nullptr;
  CHECK_EQ(next, f.size());
  CHECK_EQ(p.exec_b.executed, executed + 1U);
  Serve(p);
  CHECK(p.exec_b.last == c);
  CHECK_EQ(p.radio_b.sent, sent + 5U);
}

// 第一串丢了第 1 片；重发拼齐了命令，这一串却只到了 resend 这几片：远端等 3 ms 没有下一片
// 才执行和应答 / The first burst lost fragment 1; a resend completes the command, but
// only the fragments in `resend` of it arrive: the remote executes and answers only after
// 3 ms without a next one.
void CheckAnswerAfterShortResend(std::initializer_list<size_t> resend)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  Serve(p);
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  const auto f = Burst(c, 200);
  const uint32_t executed = p.exec_b.executed;
  for (size_t i : {0, 2, 3, 4})
  {
    Deliver(p, f[i]);
  }
  (void)p.b.Service();
  CHECK_EQ(p.exec_b.executed, executed);
  for (size_t i : resend)
  {
    Deliver(p, f[i]);
  }
  VirtualClock::StartBeforeWrap(1'000);  // 这 3 ms 跨过回绕 / These 3 ms cross the wrap.
  const uint32_t begin = NowUs();
  (void)p.b.Service();
  const uint32_t waited = NowUs() - begin;
  CHECK_EQ(p.exec_b.executed, executed + 1U);
  CHECK(p.exec_b.last == c);
  CHECK(waited >= 3'000U && waited < 3'500U);
}

TEST(radio_link_answer_when_resend_stops_short) { CheckAnswerAfterShortResend({0, 1}); }

TEST(radio_link_answer_when_last_fragment_lost)
{
  CheckAnswerAfterShortResend({0, 1, 2, 3});
}

// 已拼齐的命令再来重发：哪片到了都用缓存再答一次，不再执行 / A resend of a completed
// command: whichever fragment arrives gets the cached answer again, without executing.
TEST(radio_link_resend_without_first_fragment)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  Serve(p);
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  const auto f = Burst(c, 200);
  for (const auto& packet : f)
  {
    Deliver(p, packet);
  }
  Serve(p);
  const uint32_t executed = p.exec_b.executed;
  const uint32_t sent = p.radio_b.sent;
  for (size_t i : {1, 2, 3, 4})  // 第 0 片丢了 / fragment 0 lost
  {
    Deliver(p, f[i]);
  }
  Serve(p);
  CHECK_EQ(p.radio_b.sent, sent + 5U);
  Deliver(p, f[3]);  // 只到了一片 / only one arrived
  Serve(p);
  CHECK_EQ(p.radio_b.sent, sent + 10U);
  CHECK_EQ(p.exec_b.executed, executed);
}

// 等这一串结束时来了别的消息：已拼齐的命令不被覆盖 / Another message while waiting for
// the end of the burst does not overwrite the completed command.
TEST(radio_link_other_message_during_burst)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  Serve(p);
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  const auto f = Burst(c, 200);
  const auto g = Burst(Command(Byte(DapCmd::TRANSFER_BLOCK), 1024), 201);
  for (size_t i : {0, 2, 3, 4})
  {
    Deliver(p, f[i]);
  }
  (void)p.b.Service();
  Deliver(p, f[0]);
  Deliver(p, f[1]);
  Deliver(p, g[0]);
  Serve(p);
  CHECK(p.exec_b.last == c);
}

// 多包的命令和应答连续发出，整条消息发完才转回接收一次 / The packets of a long command
// and of its answer go out back to back, returning to receive once per message.
TEST(radio_link_back_to_back)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  Serve(p);
  const uint32_t sent_a = p.radio_a.sent;
  const uint32_t sent_b = p.radio_b.sent;
  const uint32_t entries_a = p.radio_a.receive_entries;
  const uint32_t entries_b = p.radio_b.receive_entries;
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  RunFor(p.b, 2'000);
  CHECK_EQ(p.radio_a.sent - sent_a, 5U);
  CHECK_EQ(p.radio_b.sent - sent_b, 5U);
  CHECK_EQ(p.radio_a.receive_entries - entries_a, 1U);
  CHECK_EQ(p.radio_b.receive_entries - entries_b, 1U);
}

// 电脑 5 s 没有命令时会话结束（H3），探针重新服务无线：对方能把它当远端用 / After 5 s
// without a command from the PC the session ends (H3) and the probe serves the radio
// again: its peer can use it as a remote.
TEST(radio_link_idle_end_serves_peer)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.a.CurrentRole() == Role::HOST);
  RunFor(p.a, 4'900'000);
  CHECK(p.a.CurrentRole() == Role::HOST);
  RunFor(p.a, 200'000);
  CHECK(p.a.CurrentRole() == Role::REMOTE);
  CHECK(p.radio_a.Frequency() == ChannelHz(kControlChannel));
  p.radio_a.on_poll = nullptr;
  p.radio_b.on_poll = [&p] { (void)p.a.Service(); };
  const auto c = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_b.Usb(c) == Expected(c));
  CHECK(p.b.CurrentRole() == Role::HOST && p.a.RemoteActive());
}

// 未配对的探针不用无线：命令在本地执行，对方不执行无线命令 / An unpaired probe does not
// use the radio: commands run locally and the peer executes nothing from the radio.
TEST(radio_link_unpaired)
{
  Pair p(false);
  CHECK(p.a.CurrentRole() == Role::LOCAL && !p.a.Service());
  CHECK(p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2)) == std::vector<uint8_t>{0xEE});
  CHECK_EQ(p.exec_b.executed, 0U);
}

// 探针正在当远端发应答时，它自己的 USB 上来了 DAP_Connect：这一包发完才回 F，因为 Tune 只
// 能在没有发射时调用（FakeRadio 核对）/ A probe is sending an answer as a remote when
// DAP_Connect comes from its own USB: it returns to F only when the packet is out, since
// Tune may be called only when not sending (FakeRadio checks that).
TEST(radio_link_connect_while_answering)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(40);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  Serve(p);
  for (const auto& packet : Burst(Command(Byte(DapCmd::TRANSFER), 1024), 200))
  {
    Deliver(p, packet);
  }
  for (int i = 0; i < 2 && !p.radio_b.Sending(); ++i)
  {
    (void)p.b.Service();
  }
  CHECK(p.radio_b.Sending());
  (void)p.exec_b.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_b.Frequency() == ChannelHz(93));
}

namespace
{
// 一个没有丢包的短会话发了多少包。锁定信道省掉扫描 / How many packets a short session
// with nothing lost sends. A locked channel saves the scan.
struct Sent
{
  uint32_t host;
  uint32_t remote;
};

Sent RunShortSession(uint32_t lead_us)
{
  Pair p(true);
  CHECK_EQ(ControlStatus(p.a, {Op::kChannel, 1, 40}, nullptr), Status::kOk);
  VirtualClock::StartBeforeWrap(lead_us);
  const auto connect = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_a.Usb(connect) == Expected(connect));
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(p.exec_a.Usb(c) == Expected(c));
  const auto disconnect = Command(Byte(DapCmd::DISCONNECT), 1);
  CHECK(p.exec_a.Usb(disconnect) == Expected(disconnect));
  const StatsAnswer stats = ReadStats(p.a);
  CHECK_EQ(stats.retries, 0U);
  CHECK_EQ(stats.failures, 0U);
  CHECK(p.radio_a.Frequency() == ChannelHz(93));
  CHECK(p.radio_b.Frequency() == ChannelHz(93));
  return {p.radio_a.sent, p.radio_b.sent};
}
}  // namespace

// 命令执行的时间不算主机沉默。SWJ_Pins 等 3 s：执行了超过信道的沉默时限（2.5 s）或租期
// （3 s），会话还在，远端留在数据信道上，只执行一次 / The time a command executes is not
// the host's silence. SWJ_Pins waits for 3 s: after an execution beyond the channel's
// silence limit (2.5 s) or the lease (3 s) the session goes on, the remote stays on the
// data channel, and the command runs once.
static void CheckLongCommand(uint32_t execute_us)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(150);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  const uint32_t lost = p.exec_b.lost;
  const std::vector<uint8_t> pins = {0x10, 0, 0, 0xC0, 0xC6, 0x2D, 0x00};
  p.exec_b.execute_us = execute_us;
  CHECK(p.exec_a.Usb(pins) == Expected(pins));
  p.exec_b.execute_us = 0;
  CHECK_EQ(p.exec_b.executed, 2U);
  RunFor(p.b, 10'000);
  CHECK_EQ(p.exec_b.lost, lost);
  CHECK(p.radio_b.Frequency() == ChannelHz(150));
  const auto next = Command(Byte(DapCmd::TRANSFER), 8);
  CHECK(p.exec_a.Usb(next) == Expected(next));
  CHECK_EQ(p.exec_b.lost, lost);
  CHECK_EQ(p.exec_b.executed, 3U);
}

TEST(radio_link_command_longer_than_the_channel_silence) { CheckLongCommand(2'600'000); }

TEST(radio_link_command_longer_than_the_lease) { CheckLongCommand(3'200'000); }

// NowUs() 截成 32 位，约 71 分钟回绕一次：回绕点落在会话的任何地方，都不会多发一个包，因
// 为没有等待会提前结束。步长小于 3 ms 的等待 / NowUs() truncates to 32 bits and wraps
// about every 71 minutes: wherever the wrap falls in a session, not one more packet goes
// out, because no wait ends early. The step is below the wait of 3 ms.
TEST(radio_link_session_across_the_wrap)
{
  const Sent far = RunShortSession(VirtualClock::kLeadUs);
  for (uint32_t lead_us = 0; lead_us < 30'000U; lead_us += 250U)
  {
    const Sent sent = RunShortSession(lead_us);
    CHECK_EQ(sent.host, far.host);
    CHECK_EQ(sent.remote, far.remote);
  }
}
