// SPDX-License-Identifier: Apache-2.0
// 厂商命令处理器，请求和回复按 docs/protocol.md 第 3 节的字节：采样期间放行哪些命令、
// 紧凑帧、CSW 的复用，以及上位机回来时的 DAP_Connect。
//
// The vendor command handler, with requests and replies as the bytes of docs/protocol.md
// section 3: which commands pass during sampling, compact frames, CSW reuse, and a
// DAP_Connect from a host that came back.
#include "xrdap/vendor/dap_vendor.hpp"

#include <cstdint>
#include <vector>

#include "fixture.hpp"
#include "test_clock.hpp"

namespace
{
using Result = XrDap::DapVendor::Result;
using XrDap::Transport;
using XrDap::Vendor::Get32;
using XrDap::Vendor::Get48;
using XrDap::Vendor::Item;
using XrDapTest::kSubReadHead;
using XrDapTest::ModelSwd;
using XrDapTest::NullTimer;
using XrDapTest::SubReadHead;
using XrDapTest::WordAt;

constexpr uint32_t kAddr = 0x08000000U;
constexpr uint32_t kPeriodUs = 100000U;
constexpr uint8_t kCompact = 0x02U;  // SUB_START flags bit1

// 探针的厂商命令一侧和发命令的上位机 / The vendor command side of a probe and the host
// sending commands to it.
struct Host
{
  ModelSwd swd;
  XrDap::DapVendor vendor{swd};
  NullTimer timer;
  uint8_t reply[256] = {};
  uint16_t length = 0U;

  Host()
  {
    vendor.SetCurrentLink(Transport::USB);
    CHECK_EQ(Send({0x02U, 0x01U}), Result::PASS);  // DAP_Connect
  }

  Result Send(const std::vector<uint8_t>& request)
  {
    return vendor.OnCommand(request.data(), static_cast<uint16_t>(request.size()), reply,
                            sizeof(reply), length);
  }

  void Start(uint8_t flags, const std::vector<Item>& items,
             const std::vector<uint16_t>& divs = {})
  {
    CHECK_EQ(Send(XrDapTest::SubStart(kPeriodUs, flags, items, divs)), Result::HANDLED);
    CHECK_EQ(reply[1], 0x00U);
  }

  void StartOneWord() { Start(0U, {WordAt(kAddr)}); }

  void Stop() { CHECK_EQ(Send({0x84U, 0x00U}), Result::HANDLED); }

  void SampleNow() { vendor.Sample(timer); }

  void Tick(uint32_t elapsed = 1U)
  {
    vendor.OnTickIsr(elapsed);
    SampleNow();
  }

  SubReadHead SubRead(uint8_t max_bytes)
  {
    CHECK_EQ(Send({0x85U, max_bytes, 0x00U}), Result::HANDLED);
    return XrDapTest::ParseSubReadHead(reply);
  }

  // READ 一个条目，回复：81 状态 个数，id(2) 条目状态 值 / READ of one item; reply: 81
  // status count, id(2) item_status value.
  uint32_t Read()
  {
    std::vector<uint8_t> request = {0x81U, 1U};
    XrDapTest::AppendItem(request, WordAt(kAddr), 1U);
    CHECK_EQ(Send(request), Result::HANDLED);
    CHECK_EQ(reply[1], 0x00U);
    CHECK_EQ(reply[5], 0x00U);
    return Get32(reply + 6);
  }
};

// 紧凑帧的一次运行：条目 a 每拍到期，b 在偶数拍到期。节拍 0（a、b）；节拍 1（a）；节拍 2
// （a、b，都失败）；节拍 5（a，跳过 3 和 4）。三个时间间隔是 7、7、300 µs / A compact
// run: item a is due every tick, b on even ticks. Tick 0 (a, b); tick 1 (a); tick 2 (a,
// b, both failed); tick 5 (a, skipping 3 and 4). The time gaps are 7, 7 and 300 µs.
struct CompactRun : Host
{
  const Item a = WordAt(kAddr);
  const Item b = WordAt(kAddr + 4U);

  CompactRun()
  {
    Start(kCompact, {a, b}, {1U, 2U});
    SampleNow();
    XrDapTest::AdvanceClockUs(7U);
    Tick();
    XrDapTest::AdvanceClockUs(7U);
    swd.missing = true;
    Tick();
    swd.missing = false;
    XrDapTest::AdvanceClockUs(300U);
    Tick(3U);
  }
};

struct Decoded
{
  uint32_t tick;
  uint64_t stamp;
  uint8_t bitmap;
  uint32_t values[2];
};

uint64_t GetVarint(const uint8_t*& p)
{
  uint64_t v = 0U;
  for (int shift = 0;; shift += 7)
  {
    const uint8_t b = *p++;
    v |= static_cast<uint64_t>(b & 0x7FU) << shift;
    if ((b & 0x80U) == 0U)
    {
      return v;
    }
  }
}

// 按 docs/protocol.md 3.5 解一个紧凑回复，p 停在帧后面。到期的条目：奇数拍 1 个，偶数拍 2
// 个 / Decode one compact reply as docs/protocol.md 3.5 says, leaving p after the frames.
// Items due: one on odd ticks, two on even ticks.
std::vector<Decoded> DecodeCompact(const uint8_t*& p, uint16_t frames)
{
  std::vector<Decoded> out;
  for (uint16_t i = 0; i < frames; ++i)
  {
    Decoded d{};
    if (i == 0U)
    {
      d.tick = Get32(p);
      d.stamp = Get48(p + 4);
      d.bitmap = p[10];
      p += 11;
    }
    else
    {
      const uint64_t h = GetVarint(p);
      d.tick = out.back().tick + static_cast<uint32_t>(h >> 1);
      d.stamp = out.back().stamp + GetVarint(p);
      d.bitmap = (h & 1U) != 0U ? *p++ : 0U;
    }
    const unsigned due = d.tick % 2U == 0U ? 2U : 1U;
    for (unsigned k = 0; k < due; ++k, p += 4)
    {
      d.values[k] = Get32(p);
    }
    out.push_back(d);
  }
  return out;
}
}  // namespace

TEST(StandardInfoDoesNotParkSampler)
{
  Host h;
  h.StartOneWord();
  h.SampleNow();
  CHECK_EQ(h.Send({0x00U, 0x01U}), Result::PASS);  // DAP_Info
  h.Tick();
  h.Tick();
  const SubReadHead head = h.SubRead(0xFFU);
  CHECK_EQ(head.status, 0x00U);
  CHECK_EQ(head.dropped, 0U);
  CHECK_EQ(head.skipped, 0U);
  CHECK_EQ(head.frames, 3U);
  CHECK_EQ(Get32(h.reply + kSubReadHead), 0U);
  CHECK_EQ(Get32(h.reply + kSubReadHead + 15U), 1U);
  CHECK_EQ(Get32(h.reply + kSubReadHead + 30U), 2U);
}

TEST(InfoReportsCompactFrames)
{
  Host h;
  CHECK_EQ(h.Send({0x80U}), Result::HANDLED);
  CHECK((Get32(h.reply + 3) & (1U << 6)) != 0U);
}

// 回复的第 1 帧总是原帧，每个回复自己就解得开：节拍 0（19 字节），节拍 1（15 字节）/ The
// first frame of a reply is always plain, so every reply decodes on its own: tick 0 (19
// bytes), tick 1 (15 bytes).
TEST(EveryCompactReplyStartsWithAPlainFrame)
{
  CompactRun r;
  SubReadHead head = r.SubRead(19U);
  CHECK_EQ(head.frames, 1U);
  CHECK_EQ(r.length, kSubReadHead + 19U);
  CHECK_EQ(Get32(r.reply + kSubReadHead), 0U);
  CHECK_EQ(Get32(r.reply + kSubReadHead + 11U), ModelSwd::Value(0U, r.a.base));
  CHECK_EQ(Get32(r.reply + kSubReadHead + 15U), ModelSwd::Value(0U, r.b.base));
  head = r.SubRead(15U);
  CHECK_EQ(head.frames, 1U);
  CHECK_EQ(r.length, kSubReadHead + 15U);
  CHECK_EQ(Get32(r.reply + kSubReadHead), 1U);
  CHECK_EQ(Get32(r.reply + kSubReadHead + 11U), ModelSwd::Value(0U, r.a.base));
}

TEST(CompactFramesDecodeToWhatWasSampled)
{
  CompactRun r;
  (void)r.SubRead(19U);  // 节拍 0 / Tick 0.
  const SubReadHead head = r.SubRead(0xFFU);
  CHECK_EQ(head.frames, 3U);
  CHECK_EQ(head.left, 0U);
  CHECK_EQ(head.skipped, 2U);
  const uint8_t* p = r.reply + kSubReadHead;
  const auto got = DecodeCompact(p, head.frames);
  CHECK_EQ(p - r.reply, r.length);  // 解码正好用完回复 / Decoding uses the whole reply.
  if (got.size() != 3U)
  {
    return;
  }
  const uint32_t a_value = ModelSwd::Value(0U, r.a.base);
  CHECK_EQ(got[0].tick, 1U);
  CHECK_EQ(got[0].bitmap, 0U);
  CHECK_EQ(got[0].values[0], a_value);
  CHECK_EQ(got[1].tick, 2U);
  CHECK_EQ(got[1].bitmap, 3U);
  CHECK_EQ(got[1].values[0], 0U);
  CHECK_EQ(got[1].values[1], 0U);
  CHECK_EQ(got[1].stamp - got[0].stamp, 7U);
  CHECK_EQ(got[2].tick, 5U);
  CHECK_EQ(got[2].bitmap, 0U);
  CHECK_EQ(got[2].values[0], a_value);
  CHECK_EQ(got[2].stamp - got[1].stamp, 300U);
}

TEST(StartWithoutTheCompactFlagGoesBackToPlainFrames)
{
  CompactRun r;
  r.Stop();
  r.Start(0U, {r.a, r.b}, {1U, 2U});
  r.SampleNow();
  r.Tick();
  const SubReadHead head = r.SubRead(0xFFU);
  CHECK_EQ(head.frames, 2U);
  CHECK_EQ(r.length, kSubReadHead + 19U + 15U);
  CHECK_EQ(Get32(r.reply + kSubReadHead + 19U), 1U);
}

// CSW 的读法（docs/protocol.md 3.6）/ How the CSW is read (docs/protocol.md 3.6).
TEST(ReadsOutsideSamplingReadTheCswEachTime)
{
  Host h;
  const uint32_t first = h.Read();
  CHECK_EQ(h.Read(), first);
  CHECK_EQ(h.swd.csw_reads, 2U);
}

TEST(ReadsDuringSamplingReuseTheCsw)
{
  Host h;
  h.StartOneWord();
  CHECK_EQ(h.swd.csw_reads, 1U);
  h.SampleNow();
  (void)h.Read();
  (void)h.Read();
  CHECK_EQ(h.swd.csw_reads, 1U);
}

TEST(ReadsDuringSamplingReturnFreshData)
{
  Host h;
  h.StartOneWord();
  h.SampleNow();
  CHECK_EQ(h.Read(), ModelSwd::Value(0U, kAddr));
  h.swd.value_bias = 19U;
  h.Tick();
  CHECK_EQ(h.Read(), ModelSwd::Value(0U, kAddr) + 19U);
}

TEST(StoppingSamplingEndsTheCswReuse)
{
  Host h;
  h.StartOneWord();
  h.SampleNow();
  h.Stop();
  (void)h.Read();
  CHECK_EQ(h.swd.csw_reads, 2U);
}

TEST(ANewSamplingRunReadsTheCswAgain)
{
  Host h;
  (void)h.Read();
  h.StartOneWord();
  CHECK_EQ(h.swd.csw_reads, 2U);
  (void)h.Read();
  CHECK_EQ(h.swd.csw_reads, 2U);
  h.Stop();
  h.StartOneWord();
  CHECK_EQ(h.swd.csw_reads, 3U);
}

// 采样中上位机被强杀、没发 DAP_Disconnect：同一条链路上新的 DAP_Connect 结束旧的采样
// 并重新连接；别的链路的 DAP_Connect 仍被拒绝（docs/protocol.md 3.1）/ The host is killed
// while sampling and sends no DAP_Disconnect: a new DAP_Connect on the same link ends the
// old sampling and connects afresh; one on another link is still refused
// (docs/protocol.md 3.1).
TEST(ConnectFromAnotherLinkIsRefusedWhileSampling)
{
  Host h;
  h.StartOneWord();
  h.vendor.SetCurrentLink(Transport::TCP);
  CHECK_EQ(h.Send({0x02U, 0x01U}), Result::REFUSED);
  CHECK_EQ(h.length, 1U);
  CHECK_EQ(h.reply[0], 0xFFU);
  CHECK(h.vendor.Sampling());
}

TEST(ConnectFromTheSamplingLinkEndsSampling)
{
  Host h;
  h.StartOneWord();
  CHECK_EQ(h.Send({0x02U, 0x01U}), Result::PASS);
  CHECK(!h.vendor.Sampling());
  CHECK_EQ(h.vendor.ConnectedLink(), Transport::USB);
}

TEST(SamplingStartsAgainAfterTheHostCameBack)
{
  Host h;
  h.StartOneWord();
  CHECK_EQ(h.Send({0x02U, 0x01U}), Result::PASS);
  h.StartOneWord();
  CHECK(h.vendor.Sampling());
}
