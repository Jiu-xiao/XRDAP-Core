// SPDX-License-Identifier: Apache-2.0
// 无线链路的纯逻辑。判据：packet.hpp、session.hpp、identity.hpp、forwarding.hpp、
// channel.hpp 里的规范和 docs/protocol.md 第 8 节 / Pure logic of the radio link. Oracle:
// the specifications in packet.hpp, session.hpp, identity.hpp, forwarding.hpp and
// channel.hpp, and docs/protocol.md section 8.
#include <cstring>
#include <vector>

#include "check.hpp"
#include "xrdap/radio/channel.hpp"
#include "xrdap/radio/forwarding.hpp"
#include "xrdap/radio/identity.hpp"
#include "xrdap/radio/packet.hpp"
#include "xrdap/radio/session.hpp"

using namespace XrDap::Radio;
using Result = Reassembler::Result;

namespace
{
constexpr uint8_t kGfsk = 255 - kHeader;  // 分片长度 / fragment sizes
constexpr uint8_t kFlrc = 127 - kHeader;

std::vector<uint8_t> Message(uint16_t size, uint8_t salt)
{
  std::vector<uint8_t> m(size);
  for (uint16_t i = 0; i < size; ++i)
  {
    m[i] = static_cast<uint8_t>(i * 7U + salt);
  }
  return m;
}

std::vector<std::vector<uint8_t>> Fragments(const std::vector<uint8_t>& m, uint8_t kind,
                                            uint8_t sequence, uint8_t fragment)
{
  std::vector<std::vector<uint8_t>> out;
  const uint16_t total = static_cast<uint16_t>(m.size());
  for (uint8_t i = 0; i < FragmentCount(total, fragment); ++i)
  {
    std::vector<uint8_t> p(kHeader + fragment);
    p.resize(BuildFragment(kind, sequence, m.data(), total, fragment, i, p.data()));
    out.push_back(p);
  }
  return out;
}

Result Add(Reassembler& r, const std::vector<uint8_t>& p, uint8_t fragment)
{
  return r.Add(p.data(), static_cast<int>(p.size()), fragment);
}
}  // namespace

TEST(radio_fragment_count)
{
  CHECK_EQ(FragmentCount(0, kGfsk), 1);
  CHECK_EQ(FragmentCount(kGfsk, kGfsk), 1);
  CHECK_EQ(FragmentCount(kGfsk + 1, kGfsk), 2);
  CHECK_EQ(FragmentCount(kMaxMessage, kGfsk), 5);
  CHECK_EQ(FragmentCount(kMaxMessage, kFlrc), 9);
  CHECK_EQ(kMaxFragments, 9);
}

// 消息逐字节还原，分片顺序不限；COMMAND 和 CONTROL 同样分片 / A message comes back byte
// for byte in any fragment order; COMMAND and CONTROL fragment alike.
TEST(radio_round_trip)
{
  for (uint8_t kind : {kCommand, kControl})
  {
    for (uint8_t fragment : {kGfsk, kFlrc})
    {
      for (uint16_t size : {0, 1, 121, 122, 249, 250, 1024})
      {
        const auto m = Message(size, 3);
        const auto frags = Fragments(m, kind, 9, fragment);
        for (bool reversed : {false, true})
        {
          uint8_t buffer[kMaxMessage];
          Reassembler r(buffer, sizeof(buffer));
          Result last = Result::INCOMPLETE;
          for (size_t k = 0; k < frags.size(); ++k)
          {
            CHECK_EQ(frags[k][0], kind);
            last = Add(r, frags[reversed ? frags.size() - 1 - k : k], fragment);
          }
          CHECK(last == Result::COMPLETE);
          CHECK_EQ(r.Size(), size);
          CHECK_EQ(r.Sequence(), 9);
          CHECK(size == 0 || std::memcmp(buffer, m.data(), size) == 0);
        }
      }
    }
  }
}

// 与总长对不上的分片丢弃，不会拼成消息 / Fragments that do not cover the total exactly
// are dropped, never completed.
TEST(radio_reassembly_rejects)
{
  uint8_t buffer[kMaxMessage];
  Reassembler r(buffer, sizeof(buffer));
  uint8_t p[255] = {kCommand, 1, 0, 1, 500 & 0xFF, 500 >> 8};
  CHECK(r.Add(p, kHeader + 10, kGfsk) ==
        Result::REJECTED);  // 声称 500 只带 10 / claims 500, carries 10
  p[3] = 2;
  p[4] = 100;
  p[5] = 0;
  CHECK(r.Add(p, kHeader + 100, kGfsk) ==
        Result::REJECTED);  // 一片装得下却说两片 / two for one
  p[4] = 300 & 0xFF;
  p[5] = 300 >> 8;
  CHECK(r.Add(p, kHeader + 100, kGfsk) ==
        Result::REJECTED);  // 非末片太短 / short non-last
  p[2] = 2;
  CHECK(r.Add(p, kHeader + 51, kGfsk) == Result::REJECTED);  // 序号超出 / index out
  Reassembler small(buffer, 100);
  p[2] = 0;
  p[3] = 1;
  p[4] = 101;
  p[5] = 0;
  CHECK(small.Add(p, kHeader + 101, kGfsk) ==
        Result::REJECTED);                        // 超过缓冲 / above buffer
  CHECK(r.Add(p, 3, kGfsk) == Result::REJECTED);  // 短于包头 / short header
}

// 提前拼齐的重发留下的尾巴丢弃，同序号只从第 0 片重新开始 / The tail of a resend that
// completed early is dropped; the same sequence starts again only with fragment 0.
TEST(radio_reassembly_resend_tail)
{
  uint8_t buffer[kMaxMessage];
  Reassembler r(buffer, sizeof(buffer));
  const auto a = Fragments(Message(1021, 4), kCommand, 7, kGfsk);
  for (size_t i : {0, 1, 3, 4})
  {
    CHECK(Add(r, a[i], kGfsk) == Result::INCOMPLETE);  // 第 2 片丢了 / fragment 2 lost
  }
  CHECK(Add(r, a[0], kGfsk) == Result::INCOMPLETE);
  CHECK(Add(r, a[1], kGfsk) == Result::INCOMPLETE);
  CHECK(Add(r, a[2], kGfsk) == Result::COMPLETE);
  CHECK(Add(r, a[3], kGfsk) == Result::DUPLICATE);
  CHECK(Add(r, a[4], kGfsk) == Result::DUPLICATE);
  for (size_t i = 0; i < 4; ++i)
  {
    CHECK(Add(r, a[i], kGfsk) == Result::INCOMPLETE);
  }
  CHECK(Add(r, a[4], kGfsk) == Result::COMPLETE);
  // 另一条消息的分片丢掉拼了一半的消息 / Another message drops the half-assembled one.
  const auto b = Fragments(Message(500, 5), kCommand, 8, kGfsk);
  const auto c = Fragments(Message(500, 6), kCommand, 9, kGfsk);
  CHECK(Add(r, b[0], kGfsk) == Result::INCOMPLETE);
  CHECK(Add(r, c[1], kGfsk) == Result::INCOMPLETE);
  CHECK(Add(r, b[1], kGfsk) == Result::INCOMPLETE);
  CHECK(Add(r, b[2], kGfsk) == Result::INCOMPLETE);
  CHECK(Add(r, b[0], kGfsk) == Result::COMPLETE);
}

TEST(radio_ping)
{
  uint8_t p[16];
  const uint8_t n = BuildPing(kPing, 4, 0xBEEF, p);
  CHECK_EQ(n, kHeader + kPingPayload);
  uint16_t session = 0;
  CHECK(ParsePing(p, n, session) && session == 0xBEEF);
  CHECK(!ParsePing(p, n - 1, session));
}

// R1..R5，时间按 32 位回绕 / R1..R5, with time wrapping at 32 bits.
TEST(radio_remote_lease)
{
  using Event = RemoteLease::Event;
  RemoteLease lease;
  CHECK(!lease.OnCommand(0));                // R5
  CHECK(lease.OnPing(0, 0) == Event::NONE);  // 没有会话 / no session
  CHECK(lease.OnPing(7, 10) == Event::REPLACED);
  CHECK(lease.OnPing(7, 20) == Event::NONE);  // 心跳 / heartbeat
  CHECK(lease.OnCommand(30));
  CHECK(lease.OnPing(8, 40) == Event::REPLACED);  // R2
  CHECK(lease.OnPing(0, 50) == Event::ENDED);     // R1
  CHECK(!lease.OnCommand(60));                    // R5
  CHECK(lease.OnPing(9, 1'000) == Event::REPLACED);
  CHECK(lease.OnCommand(2'000'000));  // R3 续租 / renews
  CHECK(lease.Tick(2'000'000 + kLeaseUs - 1) == Event::NONE);
  CHECK(lease.Tick(2'000'000 + kLeaseUs) == Event::ENDED);
  CHECK(lease.OnPing(10, 0xFFFFFF00U) == Event::REPLACED);
  CHECK(lease.Tick(0x100U) == Event::NONE);
}

// H1..H3；会话号跨 16 位回绕仍不为 0 / H1..H3; ids stay non-zero across the 16-bit wrap.
TEST(radio_host_session)
{
  using Action = HostSession::Action;
  HostSession host(0xFFFE);
  CHECK(host.OnCommand(0));  // H1
  const uint16_t first = host.Session();
  CHECK(first != 0 && !host.OnCommand(10));
  host.OnSent(10);
  CHECK(host.Tick(10 + kHeartbeatUs - 1) == Action::NONE);
  CHECK(host.Tick(10 + kHeartbeatUs) == Action::HEARTBEAT);  // H2
  CHECK(host.Tick(10 + kHostIdleUs) == Action::END);         // H3
  CHECK(host.OnCommand(20'000'000) && host.Session() != first);
  CHECK(host.OnDisconnect() && !host.OnDisconnect());
  bool nonzero = true;
  for (int i = 0; i < 70'000 && nonzero; ++i)
  {
    nonzero = host.OnCommand(30'000'000) && host.Session() != 0;
    (void)host.OnDisconnect();
  }
  CHECK(nonzero);
}

TEST(radio_pair_identity)
{
  const uint8_t a[kUidSize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const uint8_t b[kUidSize] = {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
  CHECK(LinkId(a, b) == LinkId(b, a) && LinkId(a, b) != 0);
  for (uint32_t id = 1; id < 5000; id = id * 3 + 1)
  {
    const Identity x = PairIdentity(id);
    CHECK_EQ(x.frequency_hz, ChannelHz(kControlChannel));
    const uint32_t word = static_cast<uint32_t>(x.sync_word[0]) << 24 |
                          static_cast<uint32_t>(x.sync_word[1]) << 16 |
                          static_cast<uint32_t>(x.sync_word[2]) << 8 | x.sync_word[3];
    const int transitions =
        __builtin_popcount(word ^ (word >> 1)) - static_cast<int>(word >> 31);
    CHECK(transitions >= 10 && transitions <= 22);
  }
  uint8_t hello[kHelloSize];
  CHECK_EQ(BuildHello(a, nullptr, hello), kHelloSize);
  CHECK(hello[0] == kHello && std::memcmp(hello + 1, a, kUidSize) == 0);
  CHECK(hello[1 + kUidSize] == 0 && hello[kHelloSize - 1] == 0);
}

TEST(radio_execution_us)
{
  const uint8_t transfer[] = {0x05, 0, 10};
  CHECK_EQ(ExecutionUs(transfer, sizeof(transfer), 1'000'000), 2U * 46U * 10U);
  const uint8_t block[] = {0x06, 0, 0x00, 0x01, 0x0F};  // 256 次 / 256 transactions
  CHECK_EQ(ExecutionUs(block, sizeof(block), 4'000'000), 2U * 46U * 256U / 4U);
  const uint8_t delay[] = {0x09, 0xE8, 0x03};
  CHECK_EQ(ExecutionUs(delay, sizeof(delay), 1'000'000), 1000U);
  const uint8_t pins[] = {0x10, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};  // 封顶 3 s / capped
  CHECK_EQ(ExecutionUs(pins, sizeof(pins), 1'000'000), 3'000'000U);
  const uint8_t clock[] = {0x11, 0x40, 0x42, 0x0F, 0x00};
  CHECK_EQ(SwjClockHz(clock, sizeof(clock)), 1'000'000U);
}

TEST(radio_settings_replay)
{
  SettingsReplay replay;
  const uint8_t clock1[] = {0x11, 1, 0, 0, 0};
  const uint8_t clock2[] = {0x11, 2, 0, 0, 0};
  const uint8_t config[] = {0x04, 0, 0x40, 0, 0, 0};
  const uint8_t transfer[] = {0x05, 0, 1, 0x02};
  replay.Note(clock1, sizeof(clock1));
  replay.Note(transfer, sizeof(transfer));
  replay.Note(clock2, sizeof(clock2));
  replay.Note(config, sizeof(config));
  CHECK(replay.Size(0) == sizeof(clock2) &&
        replay.Data(0)[1] == 2);  // 保留最后一条 / last kept
  CHECK_EQ(replay.Size(1), sizeof(config));
  CHECK_EQ(replay.Size(2), 0);
}

TEST(radio_channel_packet)
{
  uint8_t packet[16];
  CHECK_EQ(BuildChannel(kChannel, 7, 0x1234, 150, packet), kHeader + kChannelPayload);
  uint16_t session = 0;
  uint8_t channel = 0;
  CHECK(ParseChannel(packet, kHeader + kChannelPayload, session, channel));
  CHECK(packet[0] == kChannel && packet[1] == 7 && session == 0x1234 && channel == 150);
  CHECK(!ParseChannel(packet, kHeader + kChannelPayload - 1, session, channel));
}

// 频点：0.5 MHz 一格，2402.0–2481.5 MHz，越界落到 F；F 是 2448.5 MHz / Frequencies:
// 0.5 MHz steps over 2402.0–2481.5 MHz, out of range maps to F; F is 2448.5 MHz.
TEST(radio_channel_plan)
{
  CHECK_EQ(ChannelHz(0), 2'402'000'000U);
  CHECK_EQ(ChannelHz(kChannels - 1), 2'481'500'000U);
  CHECK_EQ(ChannelHz(kControlChannel), 2'448'500'000U);
  CHECK_EQ(ChannelHz(kChannels), ChannelHz(kControlChannel));
  CHECK_EQ(ChannelHz(kNoChannel), ChannelHz(kControlChannel));
  CHECK_EQ(DefaultIdentity().frequency_hz, ChannelHz(kControlChannel));
  CHECK_EQ(PairIdentity(0x12345678).frequency_hz, ChannelHz(kControlChannel));
}

// 测量与选点：忙碌低者胜，平均 RSSI 破平局；拉黑的点不选；没有测量时没有最好的点 /
// Measurement and choice: lower busy wins, lower mean breaks a tie; banned points are
// skipped; no measurement means no best point.
TEST(radio_channel_table)
{
  RssiMeter meter;
  CHECK_EQ(meter.Result().busy, 200);
  meter.Add(-200);  // -100 dBm
  meter.Add(-160);  // -80 dBm，忙 / busy
  CHECK(meter.Result().busy == 100 && meter.Result().mean_dbm == -90);

  ChannelTable table;
  CHECK(!table.Scanned() && table.Best(0) == kNoChannel);
  table.Record(10, {20, -90});
  table.Record(30, {5, -80});
  table.Record(31, {5, -95});
  table.Record(kChannels, {0, -100});  // 越界忽略 / out of range ignored
  CHECK(table.Scanned() && table.Measured(30) && !table.Measured(11));
  CHECK_EQ(table.Best(0), 31);
  table.Ban(31, 1000);
  CHECK(table.Banned(31, 1000) && table.Banned(28, 1000) && !table.Banned(27, 1000));
  CHECK_EQ(table.Best(1000), 10);  // 30 离得太近 / 30 is too close
  CHECK(!table.Banned(31, 1000 + kBanUs) && table.Best(1000 + kBanUs) == 31);
  for (uint8_t i = 0; i < 9; ++i)
  {
    table.Ban(static_cast<uint8_t>(i * 10U), 0);  // 8 项，最早的被挤掉 / 8 entries, the
                                                  // oldest drops out
  }
  CHECK(!table.Banned(0, 1) && table.Banned(80, 1));
  table.Merge(30, {2, -70});  // 留较差的 / the worse stays
  CHECK(table.Sample(30).busy == 5 && table.Sample(30).mean_dbm == -70);
  table.Merge(40, {7, -90});
  CHECK(table.Measured(40) && table.Sample(40).busy == 7);
  table.ClearSamples();
  CHECK(!table.Scanned() && table.Banned(80, 1));
}

TEST(radio_retry_window)
{
  RetryWindow window;
  for (uint16_t i = 0; i < RetryWindow::kWindow - 1U; ++i)
  {
    CHECK(!window.Add(i < RetryWindow::kLimit + 1U));
  }
  CHECK(window.Add(false));  // 11 次重发 / 11 retried
  for (uint16_t i = 0; i < RetryWindow::kWindow; ++i)
  {
    CHECK(!window.Add(i < RetryWindow::kLimit));  // 10 次不算差 / 10 is not bad
  }
}
