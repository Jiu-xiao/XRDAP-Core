// SPDX-License-Identifier: Apache-2.0
// 两块探针经 RadioLink 通信，射频是内存里的一对假收发器。判据：radio_link.hpp 的角色和
// 会话规则、settings.hpp 的操作，以及 docs/protocol.md 第 8 节 / Two probes talking
// through RadioLink over a pair of in-memory fake transceivers. Oracle: the role and
// session rules of radio_link.hpp, the operations of settings.hpp and docs/protocol.md
// section 8.
#include "xrdap/radio/radio_link.hpp"

#include <cstring>
#include <deque>
#include <functional>
#include <vector>

#include "check.hpp"
#include "linux_timebase.hpp"

using namespace XrDap;
using namespace XrDap::Radio;

namespace
{
LibXR::LinuxTimebase& Time()
{
  static LibXR::LinuxTimebase timebase;
  return timebase;
}

// 接口同 xrobot-org/SX1281 的假收发器；Send 立即投到对方的接收队列 / Fake transceiver
// with the interface of xrobot-org/SX1281; Send delivers to the peer's receive queue at
// once.
class FakeRadio
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

  LibXR::ErrorCode Start(const Config& config)
  {
    config_ = config;
    rx_.clear();
    ++starts;
    return LibXR::ErrorCode::OK;
  }
  uint8_t MaxPayload() const
  {
    return config_.modulation == Modulation::FLRC ? 127 : 255;
  }
  void PrepareSend() {}
  LibXR::ErrorCode Send(const uint8_t* data, uint8_t size)
  {
    ++sent;
    // 双方身份相同时才收得到 / Delivered only when both use the same identity.
    if (peer != nullptr && (drop == nullptr || !drop(sent)) && SameIdentity(*peer))
    {
      peer->rx_.emplace_back(data, data + size);
    }
    return LibXR::ErrorCode::OK;
  }
  bool Sending() const { return false; }
  // 主机等应答时让对方跑一轮 / Lets the peer run a round while the host waits.
  void Poll()
  {
    if (on_poll)
    {
      on_poll();
    }
  }
  int Receive(uint8_t* out)
  {
    if (rx_.empty())
    {
      return 0;
    }
    const auto packet = rx_.front();
    rx_.pop_front();
    std::memcpy(out, packet.data(), packet.size());
    return static_cast<int>(packet.size());
  }

  bool SameIdentity(const FakeRadio& other) const
  {
    return other.config_.modulation == config_.modulation &&
           other.config_.frequency_hz == config_.frequency_hz &&
           other.config_.crc_seed == config_.crc_seed &&
           std::memcmp(other.config_.sync_word, config_.sync_word, 4) == 0;
  }

  FakeRadio* peer = nullptr;
  std::function<void()> on_poll;
  std::function<bool(uint32_t)> drop;  // 第 n 次发送丢掉时为 true / true drops send n
  uint32_t sent = 0;
  uint32_t starts = 0;

 private:
  Config config_{};
  std::deque<std::vector<uint8_t>> rx_;
};

// 执行器只留 RadioLink 用到的部分 / Executor with only what RadioLink uses.
class FakeExec
{
 public:
  using Forwarder = bool (*)(void*, Transport, const uint8_t*, uint16_t, uint8_t*,
                             uint16_t, uint16_t&);
  void SetForwarder(Forwarder f, void* ctx)
  {
    forwarder_ = f;
    ctx_ = ctx;
  }
  // USB 来一条命令：转发钩子不处理时在本地执行 / A command from USB: runs locally when
  // the forwarder hook does not take it.
  std::vector<uint8_t> Usb(const std::vector<uint8_t>& req)
  {
    uint8_t resp[kMaxMessage];
    uint16_t out = 0;
    if (!forwarder_(ctx_, Transport::USB, req.data(), static_cast<uint16_t>(req.size()),
                    resp, sizeof(resp), out))
    {
      ++local;
      return {0xEE};
    }
    return {resp, resp + out};
  }
  // 远端执行：应答 = 命令号 + 命令倒序 / Remote execution: answer = command id + the
  // command reversed.
  void ExecuteHere(Transport t, const uint8_t* req, uint16_t len, uint8_t* resp,
                   uint16_t cap, uint16_t& out)
  {
    uint16_t forwarded = 0;
    CHECK(!forwarder_(ctx_, t, req, len, resp, cap, forwarded));  // RADIO 不转发
    ++executed;
    resp[0] = req[0];
    for (uint16_t i = 1; i < len && i < cap; ++i)
    {
      resp[i] = req[len - i];
    }
    out = len;
  }
  void ControlHere(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                   uint16_t& out)
  {
    if (control == nullptr || !control(req, len, resp, cap, out))
    {
      out = Control::StatusOnly(req[0], Control::kStatusUnsupported, resp, cap);
    }
  }
  void LinkLost(Transport t)
  {
    CHECK(t == Transport::RADIO);
    ++lost;
  }

  std::function<bool(const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t&)> control;
  uint32_t executed = 0;
  uint32_t local = 0;
  uint32_t lost = 0;

 private:
  Forwarder forwarder_ = nullptr;
  void* ctx_ = nullptr;
};

class FakeStore final : public Store
{
 public:
  bool Save(const Settings& s) override
  {
    saved = s;
    ++saves;
    return ok;
  }
  Settings saved{};
  uint32_t saves = 0;
  bool ok = true;
};

using Link = RadioLink<FakeRadio, FakeExec>;

const uint8_t kUidA[kUidSize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
const uint8_t kUidB[kUidSize] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

// 一对探针：a 接电脑，b 接目标 / A pair: a on the PC, b on the target.
struct Pair
{
  FakeRadio radio_a, radio_b;
  FakeExec exec_a, exec_b;
  FakeStore store_a, store_b;
  Link a{radio_a, exec_a, store_a, kUidA, {}};
  Link b{radio_b, exec_b, store_b, kUidB, {}};

  explicit Pair(bool paired, Modulation modulation = Modulation::GFSK)
  {
    (void)Time();
    radio_a.peer = &radio_b;
    radio_b.peer = &radio_a;
    // 只有主机在等应答时阻塞；远端不阻塞，不必反过来驱动主机 / Only the host blocks,
    // waiting for answers; the remote never blocks and need not drive the host.
    radio_a.on_poll = [this] { (void)b.Service(); };
    Settings s;
    s.paired = paired ? 1 : 0;
    s.link_id = paired ? LinkId(kUidA, kUidB) : 0;
    s.modulation = modulation;
    a.Begin(s, false);
    b.Begin(s, false);
  }
};

std::vector<uint8_t> Command(uint8_t id, uint16_t size)
{
  std::vector<uint8_t> c(size);
  c[0] = id;
  for (uint16_t i = 1; i < size; ++i)
  {
    c[i] = static_cast<uint8_t>(i * 13U);
  }
  return c;
}

std::vector<uint8_t> Expected(const std::vector<uint8_t>& c)
{
  std::vector<uint8_t> e(c.size());
  e[0] = c[0];
  for (size_t i = 1; i < c.size(); ++i)
  {
    e[i] = c[c.size() - i];
  }
  return e;
}
}  // namespace

// AUTO：DAP_Connect 找到对方后整个会话走无线，长短命令都逐字节到达；DAP_Disconnect
// 结束会话，远端的连接作废 / AUTO: once DAP_Connect reaches the peer the whole session
// goes over the radio and commands of any size arrive byte for byte; DAP_Disconnect ends
// it and the remote's connection lapses.
TEST(radio_link_session)
{
  for (Modulation m : {Modulation::GFSK, Modulation::FLRC})
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
  const uint32_t first = p.radio_b.sent;
  p.radio_b.drop = [first](uint32_t n) { return n > first && n <= first + 5; };
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK_EQ(p.exec_b.executed, before + 1U);
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

// 配对：两块未配对的探针得到同一个链路号并保存 / Pairing: two unpaired probes end with
// the same link id, stored.
TEST(radio_link_pairing)
{
  Pair p(false);
  p.radio_a.on_poll = nullptr;
  p.a.RequestPairing();
  p.b.RequestPairing();
  const uint32_t begin = static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds());
  while ((p.a.Pairing() == PairState::IDLE || p.a.Pairing() == PairState::RUNNING ||
          p.b.Pairing() == PairState::IDLE || p.b.Pairing() == PairState::RUNNING) &&
         static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds()) - begin < 15'000'000U)
  {
    (void)p.a.Service();
    (void)p.b.Service();
  }
  CHECK(p.a.Pairing() == PairState::DONE && p.b.Pairing() == PairState::DONE);
  CHECK(p.store_a.saved.paired == 1 && p.store_b.saved.paired == 1);
  CHECK(p.store_a.saved.link_id == LinkId(kUidA, kUidB));
  CHECK(p.store_b.saved.link_id == p.store_a.saved.link_id);
  CHECK(std::memcmp(p.store_a.saved.peer_uid, kUidB, kUidSize) == 0);
  CHECK(p.a.CurrentRole() == Role::REMOTE);  // 配对后设置生效 / settings applied
}
