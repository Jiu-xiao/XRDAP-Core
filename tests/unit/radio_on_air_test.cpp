// SPDX-License-Identifier: Apache-2.0
// 不能变的字节：已经配对的探针要继续互通，已有的工具要继续读得懂。字面值是单独取得的，不
// 是从被测的代码抄的，改代码时不要跟着改。包和应答的布局见 docs/protocol.md 第 8.1、8.3
// 节；身份的算法文档没有写，这里只固定结果。
//
// Bytes that must not change: probes already paired have to keep working together, and
// existing tools have to keep reading the answers. The literals were obtained separately,
// not copied from the code under test, and must not follow changes to it. The layout of
// packets and answers is in docs/protocol.md sections 8.1 and 8.3; the documents do not
// give the identity's algorithm, so only its results are pinned.
#include <cstring>
#include <vector>

#include "check.hpp"
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;
using namespace RadioTest;

namespace
{
using Bytes = std::vector<uint8_t>;

const uint8_t kOwnUid[kUidSize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
const uint8_t kPeerUid[kUidSize] = {12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};

bool IdentityIs(const Identity& identity, uint16_t crc_seed, const Bytes& address)
{
  return identity.crc_seed == crc_seed &&
         std::memcmp(identity.address, address.data(), 4) == 0;
}

Bytes Answer(Link& link, const Bytes& request)
{
  uint8_t resp[128];
  uint16_t out = 0;
  CHECK(link.HandleControl(request.data(), static_cast<uint16_t>(request.size()), resp,
                           sizeof(resp), out));
  return {resp, resp + out};
}

// 消息内容就是字节的序号 / The message is its own byte offsets.
Bytes CountingMessage()
{
  Bytes message(300);
  for (size_t i = 0; i < message.size(); ++i)
  {
    message[i] = static_cast<uint8_t>(i);
  }
  return message;
}
}  // namespace

TEST(radio_on_air_default_identity)
{
  CHECK(IdentityIs(DefaultIdentity(), 0xFFFF, {'X', 'R', 'D', 'P'}));
}

// 地址要避开相邻位变化太少或太多的散列值，再算一次：这条路也是身份的一部分。后四个链路号
// 的第一个散列值分别变化 9、10、22、23 次，正好在界限两边 / The address avoids a hash
// with too few or too many bit changes and hashes again; that path is part of the
// identity as well. The first hash of the last four link ids has 9, 10, 22 and 23
// changes, either side of the limits.
TEST(radio_on_air_pair_identity)
{
  CHECK(IdentityIs(PairIdentity(0x12345678), 0x444C, {0xA3, 0x64, 0x97, 0x85}));
  CHECK(IdentityIs(PairIdentity(0x00000025), 0x0025, {0x41, 0x93, 0x0F, 0x8C}));
  CHECK(IdentityIs(PairIdentity(0x0000001B), 0x001B, {0x1F, 0xE8, 0x1D, 0xBE}));
  CHECK(IdentityIs(PairIdentity(0x00000057), 0x0057, {0x52, 0xC5, 0x64, 0xB2}));
  CHECK(IdentityIs(PairIdentity(0x000000C0), 0x00C0, {0x53, 0x31, 0xC1, 0x40}));
}

// 两块探针算出同一个链路号，与顺序无关 / Both probes compute the same link id, whatever
// the order.
TEST(radio_on_air_link_id)
{
  CHECK_EQ(LinkId(kOwnUid, kPeerUid), 0x1146B4B5U);
  CHECK_EQ(LinkId(kPeerUid, kOwnUid), 0x1146B4B5U);
}

TEST(radio_on_air_fragment)
{
  const Bytes message = CountingMessage();
  uint8_t out[255];

  // 高速率 300 字节的第 0 片：满 255 字节 / Fragment 0 of 300 bytes at the high rate: all
  // 255 bytes.
  uint8_t size = BuildFragment(kCommand, 0x5A, message.data(), 300, 249, 0, out);
  CHECK_EQ(size, 255);
  CHECK((Bytes{out, out + 10} ==
         Bytes{0x01, 0x5A, 0x00, 0x02, 0x2C, 0x01, 0x00, 0x01, 0x02, 0x03}));

  size = BuildFragment(kCommand, 0x5A, message.data(), 300, 249, 1, out);
  CHECK((Bytes{out, out + size} ==
         Bytes{0x01, 0x5A, 0x01, 0x02, 0x2C, 0x01, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE,
               0xFF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
               0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
               0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22,
               0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B}));

  // 低速率应答的第 1 片 / Fragment 1 of an answer at the low rate.
  size = BuildFragment(kAnswer, 0xA5, message.data(), 130, 121, 1, out);
  CHECK((Bytes{out, out + size} == Bytes{0x02, 0xA5, 0x01, 0x02, 0x82, 0x00, 0x79, 0x7A,
                                         0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81}));

  // 空消息也是一片 / An empty message is one fragment too.
  size = BuildFragment(kControl, 0x01, message.data(), 0, 249, 0, out);
  CHECK((Bytes{out, out + size} == Bytes{0x06, 0x01, 0x00, 0x01, 0x00, 0x00}));
}

TEST(radio_on_air_ping)
{
  uint8_t out[16];
  uint8_t size = BuildPing(kPing, 0x12, 0xBEEF, out);
  CHECK(
      (Bytes{out, out + size} == Bytes{0x03, 0x12, 0x00, 0x01, 0x02, 0x00, 0xEF, 0xBE}));
  size = BuildPing(kPong, 0x34, 0x0102, out);
  CHECK(
      (Bytes{out, out + size} == Bytes{0x04, 0x34, 0x00, 0x01, 0x02, 0x00, 0x02, 0x01}));
}

TEST(radio_on_air_channel)
{
  uint8_t out[16];
  uint8_t size = BuildChannel(kChannel, 0x56, 0xBEEF, 150, out);
  CHECK((Bytes{out, out + size} ==
         Bytes{0x07, 0x56, 0x00, 0x01, 0x03, 0x00, 0xEF, 0xBE, 0x96}));
  size = BuildChannel(kChannelAck, 0x78, 0x0102, 20, out);
  CHECK((Bytes{out, out + size} ==
         Bytes{0x08, 0x78, 0x00, 0x01, 0x03, 0x00, 0x02, 0x01, 0x14}));
}

TEST(radio_on_air_hello)
{
  uint8_t out[kHelloSize];
  uint8_t size = BuildHello(kOwnUid, nullptr, out);
  CHECK(
      (Bytes{out, out + size} ==
       Bytes{0x05, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
  size = BuildHello(kOwnUid, kPeerUid, out);
  CHECK(
      (Bytes{out, out + size} ==
       Bytes{0x05, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
             0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
}

// 0x13 的应答：角色设置、速率、已配对、安全模式、当前角色、配对状态、链路号、对方 UID /
// The answer of 0x13: role setting, rate, paired, safe mode, current role, pairing state,
// link id, the peer's UID.
TEST(radio_control_settings_layout)
{
  Pair p(true);
  Settings settings;
  settings.role = RoleSetting::HOST;
  settings.rate = Rate::kLow;
  settings.paired = 1;
  settings.link_id = 0x11223344;
  std::memcpy(settings.peer_uid, kPeerUid, kUidSize);
  p.a.Begin(settings, false);
  CHECK((Answer(p.a, {Op::kSettings}) ==
         Bytes{0x13, 0x00, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00, 0x44, 0x33, 0x22, 0x11,
               0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
  p.a.Begin(settings, true);  // 安全模式 / safe mode
  CHECK((Answer(p.a, {Op::kSettings}) ==
         Bytes{0x13, 0x00, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11,
               0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
}

// 0x11 的应答：每个计数都不同，才看得出它们有没有串位 / The answer of 0x11: every counter
// differs, or a swap would not show.
TEST(radio_control_stats_layout)
{
  StatsAnswer answer{};
  answer.op = 0x11;
  answer.status = 0x22;
  answer.count = 0x33;
  answer.radio_tx = 0x01010101;
  answer.radio_rx = 0x02020202;
  answer.crc_errors = 0x03030303;
  answer.tx_timeouts = 0x04040404;
  answer.commands = 0x05050505;
  answer.retries = 0x06060606;
  answer.failures = 0x07070707;
  answer.duplicates = 0x08080808;
  answer.last_rtt_us = 0x09090909;
  answer.max_rtt_us = 0x0A0A0A0A;
  answer.channel = 0x0B0B0B0B;
  answer.rearms = 0x0C0C0C0C;
  answer.restarts = 0x0D0D0D0D;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&answer);
  CHECK((Bytes{bytes, bytes + sizeof(answer)} ==
         Bytes{0x11, 0x22, 0x33, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
               0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05,
               0x05, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07, 0x08, 0x08,
               0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B,
               0x0B, 0x0B, 0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D}));
}

// 收发器的计数各进各的位置 / Each of the transceiver's counters lands in its own place.
TEST(radio_control_stats_from_radio)
{
  Pair p(true);
  p.radio_a.sent = 0x11121314;
  p.radio_a.tx_errors = 0x21222324;
  p.radio_a.received = 0x31323334;
  p.radio_a.bad = 0x41424344;
  p.radio_a.rearms = 0x51525354;
  p.radio_a.starts = 0x61626364;
  CHECK((Answer(p.a, {Op::kStats}) ==
         Bytes{0x11, 0x00, 0x0D, 0x14, 0x13, 0x12, 0x11, 0x34, 0x33, 0x32, 0x31,
               0x44, 0x43, 0x42, 0x41, 0x24, 0x23, 0x22, 0x21, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5D,
               0x00, 0x00, 0x00, 0x54, 0x53, 0x52, 0x51, 0x64, 0x63, 0x62, 0x61}));
}

TEST(radio_control_channel_layout)
{
  Pair p(true);
  CHECK((Answer(p.a, {Op::kChannel}) == Bytes{0x19, 0x00, 0x5D, 0xFF}));
}
