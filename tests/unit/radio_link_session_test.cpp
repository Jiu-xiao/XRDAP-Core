// SPDX-License-Identifier: Apache-2.0
// 经 RadioLink 的会话和分片串：一对探针，射频是内存里的假收发器。判据：radio_link.hpp 的
// 角色和会话规则，以及 docs/protocol.md 第 8.2 节 / Sessions and bursts through
// RadioLink: a pair of probes over fake transceivers in memory. Oracle: the role and
// session rules of radio_link.hpp and docs/protocol.md section 8.2.
#include <cstring>
#include <functional>
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

// AUTO：DAP_Connect 找到对方后整个会话走无线，长短命令都逐字节到达；DAP_Disconnect
// 结束会话，远端的连接作废 / AUTO: once DAP_Connect reaches the peer the whole session
// goes over the radio and commands of any size arrive byte for byte; DAP_Disconnect ends
// it and the remote's connection lapses.
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
    for (uint16_t size : {3, 250, 1024})
    {
      const auto c = Command(Byte(DapCmd::TRANSFER), size);
      CHECK(p.exec_a.Usb(c) == Expected(c));
    }
    const auto disconnect = Command(Byte(DapCmd::DISCONNECT), 1);
    CHECK(p.exec_a.Usb(disconnect) == Expected(disconnect));
    (void)p.b.Service();
    CHECK(p.a.CurrentRole() == Role::REMOTE && !p.b.RemoteActive());
    CHECK(p.exec_b.lost >= 1U);
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
  p.air.lose = [&](const Flight& f)
  { return f.from == &p.radio_b && f.data[0] == kAnswer && lost++ < 5; };
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK_EQ(p.exec_b.executed, before + 1U);
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

// 最后一片丢了也会应答，只是多等一会 / With the last fragment lost the answer still
// comes, a little later.
TEST(radio_link_answer_when_last_fragment_lost)
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
  Deliver(p, f[0]);
  Deliver(p, f[1]);
  (void)p.b.Service();
  CHECK_EQ(p.exec_b.executed, executed + 1U);
  CHECK(p.exec_b.last == c);
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
  CHECK(p.radio_a.sent - sent_a > 1U && p.radio_b.sent - sent_b > 1U);
  CHECK_EQ(p.radio_a.receive_entries - entries_a, 1U);
  CHECK_EQ(p.radio_b.receive_entries - entries_b, 1U);
}

// 空闲结束（H3）后探针重新服务无线：对方能把它当远端用 / After the session ended on idle
// (H3) the probe serves the radio again: its peer can use it as a remote.
TEST(radio_link_idle_end_serves_peer)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.a.CurrentRole() == Role::HOST);
  RunFor(p.a, 5'200'000);
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
