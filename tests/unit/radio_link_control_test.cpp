// SPDX-License-Identifier: Apache-2.0
// RadioLink 的控制口操作和转发：一对探针，射频是内存里的假收发器。判据：settings.hpp 的
// 操作和 docs/protocol.md 第 8.3 节 / The control port operations and forwarding of
// RadioLink: a pair of probes over fake transceivers in memory. Oracle: the operations of
// settings.hpp and docs/protocol.md section 8.3.
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

// 配对中只能读 / While pairing, operations can only read.
TEST(radio_link_control_while_pairing)
{
  Pair p(true);
  p.a.RequestPairing();
  p.radio_a.on_poll = nullptr;
  (void)p.a.Service();
  CHECK(p.a.Pairing() == PairState::RUNNING);
  CHECK_EQ(ControlStatus(p.a, {Op::kSetRole, 0}, nullptr), Status::kBusy);
  CHECK_EQ(ControlStatus(p.a, {Op::kSettings}, nullptr), Status::kOk);
}

// 控制口：读设置的布局，保存后生效，转发给对方 / Control port: the settings layout, a
// stored setting taking effect, forwarding to the peer.
TEST(radio_link_control)
{
  Pair p(true);
  p.exec_b.control =
      [&p](const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap, uint16_t& out)
  { return p.b.HandleControl(req, len, resp, cap, out); };
  uint8_t resp[64];
  uint16_t out = 0;
  const uint8_t read[] = {Op::kSettings};
  CHECK(p.a.HandleControl(read, sizeof(read), resp, sizeof(resp), out));
  CHECK_EQ(out, sizeof(SettingsAnswer));
  SettingsAnswer answer;
  std::memcpy(&answer, resp, sizeof(answer));
  const uint32_t id = LinkId(kUidA, kUidB);
  CHECK(answer.op == Op::kSettings && answer.status == Status::kOk);
  CHECK(answer.paired == 1 && answer.role == static_cast<uint8_t>(Role::REMOTE));
  CHECK(answer.link_id[0] == static_cast<uint8_t>(id) &&
        answer.link_id[3] == static_cast<uint8_t>(id >> 24));

  // 对方读设置：0x30 + 请求，应答为对方的应答 / The peer's settings: 0x30 + request,
  // answered with the peer's answer.
  const uint8_t forward[] = {Op::kForward, Op::kSettings};
  CHECK(p.a.HandleControl(forward, sizeof(forward), resp, sizeof(resp), out));
  CHECK(out == sizeof(SettingsAnswer) && resp[0] == Op::kSettings && resp[1] == 0);
  const uint8_t nested[] = {Op::kForward, Op::kForward, Op::kSettings};
  CHECK(p.a.HandleControl(nested, sizeof(nested), resp, sizeof(resp), out));
  CHECK(out == 2 && resp[1] == Status::kBadRequest);

  // 角色设为 LOCAL：保存并立即生效，无线停用 / Role setting LOCAL: stored and in effect
  // at once, the radio stops.
  const uint8_t local[] = {Op::kSetRole, static_cast<uint8_t>(RoleSetting::LOCAL)};
  CHECK(p.a.HandleControl(local, sizeof(local), resp, sizeof(resp), out));
  CHECK(out == 2 && resp[1] == Status::kOk);
  CHECK(p.store_a.saved.role == RoleSetting::LOCAL && p.a.CurrentRole() == Role::LOCAL);
  p.store_a.ok = false;
  CHECK(p.a.HandleControl(local, sizeof(local), resp, sizeof(resp), out));
  CHECK(resp[1] == Status::kError);
  const uint8_t other[] = {0x17};
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

// 对方不应答时也回到 F / Back on F when the peer does not answer either.
TEST(radio_link_forward_no_answer)
{
  Pair p(true);
  p.air.lose = [&p](const Flight& f) { return f.from == &p.radio_a; };
  CHECK_EQ(ControlStatus(p.a, {Op::kForward, Op::kSettings}, nullptr), Status::kTimeout);
  CHECK(p.radio_a.Frequency() == ChannelHz(kControlChannel));
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

// 对方写 Flash 比“听不到”规则的时限长时：请求只执行一次、不重发、不换信道，电脑收到成功
// / A save longer than the deaf rule's limit: the request runs once, with no resend and
// no channel change, and the PC gets success.
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
    HoldPeerWhileSaving(p, 150'000);
    CHECK_EQ(ControlStatus(p.a, request, nullptr), Status::kOk);
    CHECK_EQ(p.store_b.saves, 1U);
    std::vector<uint8_t> stats;
    CHECK_EQ(ControlStatus(p.a, {Op::kStats}, &stats), Status::kOk);
    StatsAnswer answer;
    std::memcpy(&answer, stats.data(), sizeof(answer));
    CHECK_EQ(answer.retries, 0U);
    CHECK(p.radio_a.Frequency() == ChannelHz(kControlChannel));
  }
}
