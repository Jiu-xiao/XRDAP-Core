// SPDX-License-Identifier: Apache-2.0
// RadioLink 的控制口操作和转发：一对探针，射频是内存里的假收发器。判据：settings.hpp 的操
// 作和 docs/protocol.md 第 8.3 节 / The control port operations and forwarding of
// RadioLink: a pair of probes over fake transceivers in memory. Oracle: the operations of
// settings.hpp and docs/protocol.md section 8.3.
#include <cstring>
#include <vector>

#include "check.hpp"
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;
using namespace RadioTest;

namespace
{
template <typename PairT>
void AnswerControlOnB(PairT& p)
{
  p.exec_b.control =
      [&p](const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap, uint16_t& out)
  { return p.b.HandleControl(req, len, resp, cap, out); };
}

// 对方写 Flash 时不运行也收不到包 / While the peer writes flash it neither runs nor
// receives.
void HoldPeerWhileSaving(PairWith<SlowStore>& p, uint32_t save_us)
{
  p.store_b.save_us = save_us;
  AnswerControlOnB(p);
  p.radio_a.on_poll = [&p]
  {
    if (!p.store_b.Busy())
    {
      (void)p.b.Service();
    }
  };
  p.air.lose = [&p](const Flight& f) { return f.from == &p.radio_a && p.store_b.Busy(); };
}
}  // namespace

// 配对中只能读，0x11 和 0x13 之外的操作要等配对结束 / While pairing, operations can only
// read: all but 0x11 and 0x13 have to wait for its end.
TEST(radio_link_control_while_pairing)
{
  Pair p(true);
  p.a.RequestPairing();
  p.radio_a.on_poll = nullptr;
  (void)p.a.Service();
  CHECK(p.a.Pairing() == PairState::RUNNING);
  CHECK_EQ(ControlStatus(p.a, {Op::kSetRole, 0}, nullptr), Status::kBusy);
  CHECK_EQ(ControlStatus(p.a, {Op::kSettings}, nullptr), Status::kOk);
  CHECK_EQ(ControlStatus(p.a, {Op::kStats}, nullptr), Status::kOk);
}

// 读对方的设置：0x30 + 请求，应答是对方的应答。两块探针记的对方 UID 不同，看得出应答来自
// 哪一块 / The peer's settings: 0x30 + request, answered with the peer's answer. The two
// probes hold different peer UIDs, which shows where the answer comes from.
TEST(radio_link_forward_answers_for_the_peer)
{
  Pair p(true);
  AnswerControlOnB(p);
  Settings settings;
  settings.paired = 1;
  settings.link_id = LinkId(kUidA, kUidB);
  std::memcpy(settings.peer_uid, kUidA, kUidSize);
  p.b.Begin(settings, false);
  std::vector<uint8_t> answer;
  CHECK_EQ(ControlStatus(p.a, {Op::kForward, Op::kSettings}, &answer), Status::kOk);
  CHECK_EQ(answer[0], Op::kSettings);
  CHECK(answer.size() == 24U && std::memcmp(answer.data() + 12, kUidA, kUidSize) == 0);
}

// 不转发转发请求，也不用无线 / A forward of a forward is refused, without using the
// radio.
TEST(radio_link_forward_refuses_a_forward)
{
  Pair p(true);
  std::vector<uint8_t> answer;
  CHECK_EQ(ControlStatus(p.a, {Op::kForward, Op::kForward, Op::kSettings}, &answer),
           Status::kBadRequest);
  CHECK_EQ(answer.size(), 2U);
  CHECK_EQ(p.radio_a.sent, 0U);
}

// 角色设为 LOCAL：保存并立即生效，无线停用 / Role setting LOCAL: stored and in effect at
// once, the radio stops.
TEST(radio_link_role_setting_takes_effect_at_once)
{
  Pair p(true);
  CHECK_EQ(ControlStatus(p.a, {Op::kSetRole, static_cast<uint8_t>(RoleSetting::LOCAL)},
                         nullptr),
           Status::kOk);
  CHECK(p.store_a.saved.role == RoleSetting::LOCAL);
  CHECK(p.a.CurrentRole() == Role::LOCAL);
}

// 保存失败：状态 2，设置不生效 / A failed save: status 2, and the setting does not apply.
TEST(radio_link_failed_save_changes_nothing)
{
  Pair p(true);
  p.store_a.ok = false;
  CHECK_EQ(ControlStatus(p.a, {Op::kSetRole, static_cast<uint8_t>(RoleSetting::LOCAL)},
                         nullptr),
           Status::kError);
  CHECK(p.a.CurrentRole() == Role::REMOTE);
}

// 别的 op 不归这里，留给平台（0x17 引导槽信息）/ Other ops are not handled here and are
// left to the platform (0x17, boot slot information).
TEST(radio_link_other_ops_left_to_the_platform)
{
  Pair p(true);
  const uint8_t other[] = {0x17};
  uint8_t resp[64];
  uint16_t out = 0;
  CHECK(!p.a.HandleControl(other, sizeof(other), resp, sizeof(resp), out));
}

// 没有 DAP 会话时转发一条请求：两块探针都回到 F，下一条转发和 DAP_Connect 都走无线，不会
// 悄悄落到本地目标上 / A forward with no DAP session: both probes return to F, and the
// next forward and DAP_Connect go over the radio instead of silently reaching the local
// target.
TEST(radio_link_forward_outside_session)
{
  Pair p(true);
  AnswerControlOnB(p);
  std::vector<uint8_t> answer;
  CHECK_EQ(ControlStatus(p.a, {Op::kForward, Op::kSettings}, &answer), Status::kOk);
  (void)p.a.Service();
  (void)p.b.Service();
  CHECK(p.radio_a.Frequency() == ChannelHz(kControlChannel));
  CHECK(p.radio_b.Frequency() == ChannelHz(kControlChannel));
  CHECK_EQ(ControlStatus(p.a, {Op::kForward, Op::kSettings}, &answer), Status::kOk);
  const uint32_t executed = p.exec_b.executed;
  const auto connect = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_a.Usb(connect) == Expected(connect));
  CHECK_EQ(p.exec_a.local, 0U);
  CHECK_EQ(p.exec_b.executed, executed + 1U);
  CHECK(p.a.CurrentRole() == Role::HOST);
}

// 转发已经把两块探针带到数据信道，对方的应答却都回不来：电脑收到状态 1，没有无线 DAP 会
// 话，所以两块探针都回到 F / The forward has taken both probes to a data channel, but
// none of the peer's answers come back: the PC gets status 1 and, with no wireless DAP
// session, both probes return to F.
TEST(radio_link_forward_no_answer)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(40, 120);
  bool on_data_channel = false;
  p.air.lose = [&on_data_channel](const Flight& f)
  {
    if (f.data[0] != kAnswer)
    {
      return false;
    }
    on_data_channel = f.params.frequency_hz != ChannelHz(93);
    return true;
  };
  CHECK_EQ(ControlStatus(p.a, {Op::kForward, Op::kSettings}, nullptr), Status::kTimeout);
  CHECK(on_data_channel);
  CHECK(p.radio_a.Frequency() == ChannelHz(93));
  CHECK(p.radio_b.Frequency() == ChannelHz(93));
  CHECK(p.a.CurrentRole() == Role::REMOTE);
}

// 无线 DAP 会话中的转发不结束会话，也不换信道 / A forward during a wireless DAP session
// neither ends the session nor changes channel.
TEST(radio_link_forward_in_session)
{
  Pair p(true);
  AnswerControlOnB(p);
  p.radio_a.noise = QuietAt(40);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_a.Frequency() == ChannelHz(40));
  CHECK_EQ(ControlStatus(p.a, {Op::kForward, Op::kSettings}, nullptr), Status::kOk);
  CHECK(p.radio_a.Frequency() == ChannelHz(40) && p.radio_b.Frequency() == ChannelHz(40));
  CHECK(p.a.CurrentRole() == Role::HOST);
  const auto c = Command(Byte(DapCmd::TRANSFER), 16);
  CHECK(p.exec_a.Usb(c) == Expected(c));
}

// 对方写 Flash 的时间：比“听不到”规则的 60 ms 长，又在控制请求第一次等待留给它的 250 ms
// 之内，所以主机这段时间既不该重发，也不该换信道 / How long the peer writes flash: longer
// than the 60 ms of the deaf rule, and within the 250 ms the first wait of a control
// request leaves for it, so that the host should neither resend nor change channel
// meanwhile.
TEST(radio_link_forward_slow_save)
{
  const std::vector<std::vector<uint8_t>> requests = {
      {Op::kForward, Op::kSetRate, 1},
      {Op::kForward, Op::kSetRole, static_cast<uint8_t>(RoleSetting::LOCAL)},
      {Op::kForward, Op::kSetRole, static_cast<uint8_t>(RoleSetting::AUTO)},
      {Op::kForward, Op::kChannel, 1, 10},
      {Op::kForward, Op::kUnpair},
  };
  for (const auto& request : requests)
  {
    PairWith<SlowStore> p(true);
    HoldPeerWhileSaving(p, 240'000);
    CHECK_EQ(ControlStatus(p.a, request, nullptr), Status::kOk);
    CHECK_EQ(p.store_b.saves, 1U);
    CHECK_EQ(ReadStats(p.a).retries, 0U);
    CHECK(p.radio_a.Frequency() == ChannelHz(93));
  }
}
