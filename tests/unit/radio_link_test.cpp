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
#include "radio_fakes.hpp"

using namespace XrDap;
using namespace XrDap::Radio;

namespace
{
// 接口同 xrobot-org/SX1281 的假收发器；Send 立即投到对方的接收队列 / Fake transceiver
// with the interface of xrobot-org/SX1281; Send delivers to the peer's receive queue at
// once.
class FakeRadio
{
 public:
  LibXR::ErrorCode Start(const LinkParams& config)
  {
    config_ = config;
    rx_.clear();
    ++starts;
    return LibXR::ErrorCode::OK;
  }
  uint8_t MaxPayload() const { return config_.rate == Rate::kLow ? 127 : 255; }
  void PrepareSend() {}
  LibXR::ErrorCode Tune(uint32_t frequency_hz)
  {
    config_.frequency_hz = frequency_hz;
    return LibXR::ErrorCode::OK;
  }
  int16_t RssiInstant()
  {
    ++rssi_reads;
    return noise ? noise(config_.frequency_hz) : -200;
  }
  uint32_t TxPackets() const { return sent; }
  uint32_t TxErrors() const { return 0; }
  uint32_t RxPackets() const { return received; }
  uint32_t RxErrors() const { return 0; }
  uint32_t Rearms() const { return 0; }
  uint32_t Restarts() const { return starts; }
  uint32_t Frequency() const { return config_.frequency_hz; }
  LibXR::ErrorCode Send(const uint8_t* data, uint8_t size)
  {
    if (sending_)
    {
      if (on_air_ > 0U)
      {
        --on_air_;
        return LibXR::ErrorCode::BUSY;
      }
      sending_ = false;  // 上一包发完，不回接收 / previous one done, no receive between
    }
    ++sent;
    // 双方身份（含频率）相同、信道没被堵时才收得到 / Delivered only when both use the
    // same identity (frequency included) and the channel is not jammed.
    if (peer != nullptr && (drop == nullptr || !drop(sent)) &&
        (jam == nullptr || !jam(config_.frequency_hz)) && SameIdentity(*peer))
    {
      peer->rx_.emplace_back(data, data + size);
    }
    sending_ = air > 0U;
    on_air_ = air;
    return LibXR::ErrorCode::OK;
  }
  bool Sending() const { return sending_; }
  // 主机等应答时让对方跑一轮 / Lets the peer run a round while the host waits.
  void Poll()
  {
    if (sending_)
    {
      if (on_air_ > 0U)
      {
        --on_air_;
      }
      else
      {
        sending_ = false;
        ++receive_entries;
      }
    }
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
    ++received;
    std::memcpy(out, packet.data(), packet.size());
    return static_cast<int>(packet.size());
  }

  bool SameIdentity(const FakeRadio& other) const
  {
    return other.config_.rate == config_.rate &&
           other.config_.frequency_hz == config_.frequency_hz &&
           other.config_.crc_seed == config_.crc_seed &&
           std::memcmp(other.config_.address, config_.address, 4) == 0;
  }

  FakeRadio* peer = nullptr;
  std::function<void()> on_poll;
  std::function<bool(uint32_t)> drop;  // 第 n 次发送丢掉时为 true / true drops send n
  // 在这个频率上发的包丢掉时为 true / true loses packets sent on this frequency
  std::function<bool(uint32_t)> jam;
  // 某个频率上的 RSSI，0.5 dBm；没设时处处安静 / RSSI on a frequency, 0.5 dBm; quiet
  // everywhere when unset
  std::function<int16_t(uint32_t)> noise;
  uint32_t received = 0;
  uint32_t sent = 0;
  uint32_t starts = 0;
  uint32_t rssi_reads = 0;
  // 每包在空中停留的 Send / Poll 调用次数，0 为立即发完 / Send / Poll calls each packet
  // stays on air, 0 for at once.
  uint32_t air = 0;
  uint32_t receive_entries = 0;  // 发完转回接收的次数 / Returns to receive after sending

 private:
  bool sending_ = false;
  uint32_t on_air_ = 0;
  LinkParams config_{};
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
    last.assign(req, req + len);
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
  std::vector<uint8_t> last;  // 上一条执行的命令 / the last command executed
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
  Link a{radio_a, exec_a, store_a, kUidA};
  Link b{radio_b, exec_b, store_b, kUidB};

  explicit Pair(bool paired, Rate rate = Rate::kHigh)
  {
    RadioTest::VirtualClock::StartBeforeWrap(RadioTest::VirtualClock::kLeadUs);
    radio_a.peer = &radio_b;
    radio_b.peer = &radio_a;
    // 只有主机在等应答时阻塞；远端不阻塞，不必反过来驱动主机 / Only the host blocks,
    // waiting for answers; the remote never blocks and need not drive the host.
    radio_a.on_poll = [this] { (void)b.Service(); };
    Settings s;
    s.paired = paired ? 1 : 0;
    s.link_id = paired ? LinkId(kUidA, kUidB) : 0;
    s.rate = rate;
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

void Deliver(Pair& p, const std::vector<uint8_t>& packet)
{
  CHECK(p.radio_a.Send(packet.data(), static_cast<uint8_t>(packet.size())) ==
        LibXR::ErrorCode::OK);
}

// 让远端把在发的应答发完 / Lets the remote finish the answer it is sending.
void Serve(Pair& p)
{
  for (int i = 0; i < 100; ++i)
  {
    (void)p.b.Service();
  }
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
  const uint32_t first = p.radio_b.sent;
  p.radio_b.drop = [first](uint32_t n) { return n > first && n <= first + 5; };
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
    if (next < f.size())
    {
      Deliver(p, f[next++]);
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
  p.radio_a.air = 2;
  p.radio_b.air = 2;
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  const uint32_t sent_a = p.radio_a.sent;
  const uint32_t sent_b = p.radio_b.sent;
  const uint32_t entries_a = p.radio_a.receive_entries;
  const uint32_t entries_b = p.radio_b.receive_entries;
  const auto c = Command(Byte(DapCmd::TRANSFER), 1024);
  CHECK(p.exec_a.Usb(c) == Expected(c));
  (void)p.b.Service();
  CHECK(p.radio_a.sent - sent_a > 1U && p.radio_b.sent - sent_b > 1U);
  CHECK_EQ(p.radio_a.receive_entries - entries_a, 1U);
  CHECK_EQ(p.radio_b.receive_entries - entries_b, 1U);
}

// 一个点安静、其余忙碌的噪声 / Noise with one quiet point and the rest busy.
std::function<int16_t(uint32_t)> QuietAt(uint8_t quiet, uint8_t second = kNoChannel)
{
  return [quiet, second](uint32_t hz) -> int16_t
  {
    if (hz == ChannelHz(quiet))
    {
      return -200;
    }
    return hz == ChannelHz(second) ? -190 : -150;
  };
}

uint8_t ControlStatus(Link& link, std::vector<uint8_t> req, std::vector<uint8_t>* answer)
{
  uint8_t resp[128];
  uint16_t out = 0;
  CHECK(link.HandleControl(req.data(), static_cast<uint16_t>(req.size()), resp,
                           sizeof(resp), out));
  if (answer != nullptr)
  {
    answer->assign(resp, resp + out);
  }
  return out >= 2 ? resp[1] : 0xFF;
}

uint32_t Now() { return static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds()); }

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
  p.radio_b.drop = [first](uint32_t n) { return n == first + 2; };  // PONG, 然后 ACK
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
  auto jam = [](uint32_t hz) { return hz == ChannelHz(150); };
  p.radio_a.jam = jam;
  p.radio_b.jam = jam;
  const auto c = Command(Byte(DapCmd::TRANSFER), 64);
  const uint32_t begin = Now();
  bool ok = false;
  while (!ok && Now() - begin < 8'000'000U)
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
  p.radio_b.jam = [&first](uint32_t hz)
  {
    if (hz != ChannelHz(30))
    {
      return false;
    }
    if (first == 0U)
    {
      first = Now();
    }
    return Now() - first < 35'000U;
  };
  const auto c = Command(Byte(DapCmd::CONNECT), 2);
  const uint32_t begin = Now();
  CHECK(p.exec_a.Usb(c) == Expected(c));
  CHECK(Now() - begin < 2'000'000U);
  CHECK(first != 0U);
  CHECK(p.radio_a.Frequency() == p.radio_b.Frequency());
}

// 空闲结束（H3）后探针重新服务无线：对方能把它当远端用 / After the session ended on idle
// (H3) the probe serves the radio again: its peer can use it as a remote.
TEST(radio_link_idle_end_serves_peer)
{
  Pair p(true);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.a.CurrentRole() == Role::HOST);
  const uint32_t begin = Now();
  while (Now() - begin < 5'200'000U)
  {
    (void)p.a.Service();
  }
  CHECK(p.a.CurrentRole() == Role::REMOTE);
  CHECK(p.radio_a.Frequency() == ChannelHz(kControlChannel));
  p.radio_a.on_poll = nullptr;
  p.radio_b.on_poll = [&p] { (void)p.a.Service(); };
  const auto c = Command(Byte(DapCmd::CONNECT), 2);
  CHECK(p.exec_b.Usb(c) == Expected(c));
  CHECK(p.b.CurrentRole() == Role::HOST && p.a.RemoteActive());
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
  p.radio_b.drop = [](uint32_t) { return true; };
  (void)p.exec_a.Usb(Command(Byte(DapCmd::TRANSFER), 16));
  CHECK(p.radio_a.starts > starts);
  CHECK(p.radio_a.Frequency() == ChannelHz(40));
}

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

// 远端在数据信道上 2.5 s 没听到主机就回 F；主机下一条命令在 F 上重新建立 / The remote
// returns to F after 2.5 s on a data channel without hearing the host; the host's next
// command opens the session on F again.
TEST(radio_link_remote_silence)
{
  Pair p(true);
  p.radio_a.noise = QuietAt(40);
  (void)p.exec_a.Usb(Command(Byte(DapCmd::CONNECT), 2));
  CHECK(p.radio_b.Frequency() == ChannelHz(40));
  const uint32_t begin = Now();
  while (Now() - begin < 2'700'000U)
  {
    (void)p.b.Service();
  }
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

void AnswerControlOnB(Pair& p)
{
  p.exec_b.control =
      [&p](const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap, uint16_t& out)
  { return p.b.HandleControl(req, len, resp, cap, out); };
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
  p.radio_a.drop = [](uint32_t) { return true; };
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

// 保存设置要一段时间：Save 立即返回，这段时间里远端既不运行也收不到包，和它阻塞在写 Flash
// 上一样，主机却还能继续发 / A save that takes time: Save returns at once, and for that
// time the remote neither runs nor receives, as when it blocks writing flash, while the
// host can go on sending.
class SlowStore final : public Store
{
 public:
  explicit SlowStore(uint32_t ms) : ms_(ms) {}
  bool Save(const Settings& s) override
  {
    saved = s;
    ++saves;
    until_ = Now() + ms_ * 1000U;
    return true;
  }
  bool Busy() const { return saves != 0U && static_cast<int32_t>(until_ - Now()) > 0; }
  Settings saved{};
  uint32_t saves = 0;

 private:
  uint32_t ms_;
  uint32_t until_ = 0;
};

struct SlowSavePair
{
  FakeRadio radio_a, radio_b;
  FakeExec exec_a, exec_b;
  FakeStore store_a;
  SlowStore store_b;
  Link a{radio_a, exec_a, store_a, kUidA};
  Link b{radio_b, exec_b, store_b, kUidB};

  explicit SlowSavePair(uint32_t ms) : store_b(ms)
  {
    RadioTest::VirtualClock::StartBeforeWrap(RadioTest::VirtualClock::kLeadUs);
    radio_a.peer = &radio_b;
    radio_b.peer = &radio_a;
    radio_a.on_poll = [this]
    {
      if (!store_b.Busy())
      {
        (void)b.Service();
      }
    };
    radio_a.drop = [this](uint32_t) { return store_b.Busy(); };
    Settings s;
    s.paired = 1;
    s.link_id = LinkId(kUidA, kUidB);
    s.rate = Rate::kHigh;
    a.Begin(s, false);
    b.Begin(s, false);
    exec_b.control = [this](const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                            uint16_t& out)
    { return b.HandleControl(req, len, resp, cap, out); };
  }
};

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
    SlowSavePair p(150);
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
