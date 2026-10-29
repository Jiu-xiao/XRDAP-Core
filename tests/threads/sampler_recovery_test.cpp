// SPDX-License-Identifier: Apache-2.0 Exercise real MemAp/Sampler (single-loop policy)
// code against an ADIv5 posted-read model. A framing failure must remain visible in the
// failed frame and recover on a later tick.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "libxr_system.hpp"
#include "model_swd.hpp"
#include "xrdap/vendor/dap_vendor.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/sampler.hpp"

using namespace XrDap::Vendor;
using LibXR::ErrorCode;
namespace P = LibXR::Debug::SwdProtocol;

#define CHECK_RECOVERY(x)                                  \
  do                                                       \
  {                                                        \
    if (!(x))                                              \
    {                                                      \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
      std::exit(1);                                        \
    }                                                      \
  } while (0)

namespace
{
struct Timer : TickTimer
{
  void Start(uint32_t) override {}
  void Stop() override {}
};

uint32_t Read(MemAp& mem, uint8_t ap)
{
  Item it;
  it.ap = ap;
  it.size = 4;
  it.base = 0x08000000U;
  uint8_t bytes[4] = {};
  CHECK_RECOVERY(mem.Read(it, bytes) == kItemOk);
  return Get32(bytes);
}

void ApSelection()
{
  ModelSwd swd;
  MemAp mem(swd);
  MemAp::Access access(mem);
  CHECK_RECOVERY(Read(mem, 0) == (0x08000000U ^ 0x12340000U));
  CHECK_RECOVERY(Read(mem, 1) == (0x08000000U ^ 0x12340001U));
  CHECK_RECOVERY(Read(mem, 0) == (0x08000000U ^ 0x12340000U));
  // Each AP's CSW is read once; the third read reuses the cached AP 0 state, and
  // nothing failed or needed recovery.
  CHECK_RECOVERY(swd.csw_reads == 2U);
  CHECK_RECOVERY(swd.resets == 0U && swd.enters == 0U);
}

void SamplerRecovery()
{
  ModelSwd swd;
  MemAp mem(swd);
  Sampler sampler(mem, +[](void*, uint32_t) { return true; }, nullptr);
  Timer timer;
  // AP=0, id=1, u32 at 0x08000000, no dereference, div=1, phase=0.
  const uint8_t entry[] = {0, 1, 0, 4, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0};
  CHECK_RECOVERY(sampler.Start({entry, entry + sizeof(entry), 1}, 10000, false, 1U) ==
                 kStatusOk);
  uint8_t frame[32] = {};
  auto take = [&]()
  {
    const auto got = sampler.TakeFrames(frame, sizeof(frame), false);
    CHECK_RECOVERY(got.frames == 1 && got.bytes == 15);
    CHECK_RECOVERY(got.skipped == 0 && got.dropped == 0);
  };
  sampler.Sample(timer);
  take();
  CHECK_RECOVERY(Get32(frame) == 0 && frame[10] == 0);
  swd.desynced = true;
  sampler.OnTickIsr();
  sampler.Sample(timer);
  take();
  CHECK_RECOVERY(Get32(frame) == 1 && frame[10] == 1);  // Keep evidence of the error.
  CHECK_RECOVERY(swd.resets == 0);
  sampler.OnTickIsr();
  sampler.Sample(timer);
  take();
  CHECK_RECOVERY(Get32(frame) == 2 && frame[10] == 0);
  CHECK_RECOVERY(Get32(frame + 11) == (0x08000000U ^ 0x12340000U));
  CHECK_RECOVERY(swd.resets == 1);
  CHECK_RECOVERY(swd.enters == 1);
  CHECK_RECOVERY(swd.memory_reads == 2);  // No hidden re-read of the failed frame.

  swd.missing = true;
  for (int i = 0; i < 5; ++i)
  {
    sampler.OnTickIsr();
    sampler.Sample(timer);
    take();
    CHECK_RECOVERY(frame[10] == 1);
  }
  CHECK_RECOVERY(swd.resets == 1);  // Cooldown prevents per-frame recovery storms.
  sampler.Stop();
}

void BurstReadAndFailure()
{
  for (unsigned fault = 0U; fault < 7U; ++fault)
  {
    ModelSwd swd;
    MemAp mem(swd);
    MemAp::Access access(mem);
    MemAp::ApState ap;
    ap.ap = 0U;
    ap.valid = true;
    ap.base_csw = 0x23000002U;
    // Prime the command-level AP cache so CSW reuse after the burst can be observed.
    mem.ResetCache();
    CHECK_RECOVERY(mem.EnsureAp(0U) != nullptr);
    CHECK_RECOVERY(swd.csw_reads == 1U);
    if (fault >= 1U && fault <= 3U) swd.fail_drw = fault;
    if (fault == 4U) swd.parity_drw = 2U;
    if (fault == 5U) swd.parity_rdbuff = true;
    if (fault == 6U) swd.parity_drw = 1U;
    uint8_t values[12] = {};
    const uint16_t confirmed = mem.ReadWords(ap, 0x08000000U, 3U, values);
    const unsigned expected[] = {3U, 0U, 0U, 1U, 0U, 2U, 0U};
    CHECK_RECOVERY(confirmed == expected[fault]);
    CHECK_RECOVERY(swd.tar_writes == 1U);
    CHECK_RECOVERY(swd.drw_attempts <= 3U);
    for (unsigned i = 0; i < confirmed; ++i)
      CHECK_RECOVERY(Get32(values + i * 4U) == ((0x08000000U + i * 4U) ^ 0x12340000U));
    for (unsigned i = 0; i < swd.addresses.size(); ++i)
      CHECK_RECOVERY(swd.addresses[i] == 0x08000000U + i * 4U);
    swd.parity_rdbuff = false;
    // A failed transaction (and only a failure) forbids reusing the cached CSW: the
    // next command re-reads it from hardware. This replaces the old failure counter.
    mem.ResetCache(true);
    CHECK_RECOVERY(mem.EnsureAp(0U) != nullptr);
    CHECK_RECOVERY(swd.csw_reads == (fault ? 2U : 1U));
    // A subsequent single read restores non-incrementing CSW and succeeds.
    Item it;
    it.ap = 0U;
    it.size = 4U;
    it.base = 0x08000010U;
    CHECK_RECOVERY(mem.ReadItem(ap, it, values) == kItemOk);
    CHECK_RECOVERY(Get32(values) == (it.base ^ 0x12340000U));
    CHECK_RECOVERY((swd.csw[0] & 0x30U) == 0U);
    // Reject an unsafe TAR wrap without touching the target.
    const auto reads = swd.memory_reads;
    CHECK_RECOVERY(mem.ReadWords(ap, 0x080003FCU, 2U, values) == 0U);
    CHECK_RECOVERY(mem.ReadWords(ap, 0x08000001U, 2U, values) == 0U);
    CHECK_RECOVERY(swd.memory_reads == reads);
  }
}

std::vector<uint8_t> Entries(const std::vector<Item>& items)
{
  std::vector<uint8_t> bytes;
  uint16_t id = 0U;
  for (const auto& it : items)
  {
    bytes.push_back(it.ap);
    bytes.push_back(static_cast<uint8_t>(++id));
    bytes.push_back(0U);
    bytes.push_back(it.size);
    const auto start = bytes.size();
    bytes.resize(start + 4U);
    Put32(bytes.data() + start, it.base);
    bytes.push_back(it.n);
    for (unsigned i = 0; i < it.n; ++i)
    {
      const auto offset = bytes.size();
      bytes.resize(offset + 2U);
      Put16(bytes.data() + offset, static_cast<uint16_t>(it.deref[i]));
    }
    const auto end = bytes.size();
    bytes.resize(end + 6U);
    Put16(bytes.data() + end, static_cast<uint16_t>(it.final_off));
    Put16(bytes.data() + end + 2U, 1U);
    Put16(bytes.data() + end + 4U, 0U);
  }
  return bytes;
}

void SamplerMergedTicksAndBatches()
{
  for (unsigned kind = 0U; kind < 8U; ++kind)
  {
    ModelSwd swd;
    MemAp mem(swd);
    Sampler sampler(mem, +[](void*, uint32_t) { return true; }, nullptr);
    Timer timer;
    Item a;
    a.ap = 0U;
    a.size = 4U;
    a.base = kind == 1U ? 0x080003FCU : 0x08000000U;
    Item b = a;
    b.base += kind == 2U ? 8U : 4U;
    if (kind == 3U) b.ap = 1U;
    if (kind == 4U) b.size = 2U;
    if (kind == 5U) b.n = 1U;
    if (kind == 6U) swd.fail_drw = 2U;
    const auto bytes = Entries({a, b});
    CHECK_RECOVERY(sampler.Start({bytes.data(), bytes.data() + bytes.size(), 2}, 100000U,
                                 false, 1U) == kStatusOk);
    // Inject failure after CSW reads during start, at the next two-word frame.
    if (kind == 6U) swd.fail_drw = swd.drw_attempts + 2U;
    if (kind == 7U) swd.parity_rdbuff = true;
    sampler.Sample(timer);
    uint8_t frame[64] = {};
    auto got = sampler.TakeFrames(frame, sizeof(frame), false);
    CHECK_RECOVERY(got.frames == 1U && Get32(frame) == 0U);
    // Observe bus traffic: contiguous words need one TAR write; wrap, a gap,
    // another AP or a halfword need two. The pointer case adds a third read.
    const unsigned expected_tar_writes[] = {1U, 2U, 2U, 2U, 2U, 3U, 1U, 1U};
    CHECK_RECOVERY(swd.tar_writes == expected_tar_writes[kind]);
    if (kind == 6U)
    {
      CHECK_RECOVERY(frame[10] == 3U);         // Neither posted word was confirmed.
      CHECK_RECOVERY(swd.memory_reads == 1U);  // Failed address was not replayed.
    }
    else if (kind == 7U)
      CHECK_RECOVERY(frame[10] == 2U);
    else
      CHECK_RECOVERY(frame[10] == 0U);
    if (kind <= 3U)
    {
      CHECK_RECOVERY(Get32(frame + 11U) == (a.base ^ 0x12340000U));
      CHECK_RECOVERY(Get32(frame + 15U) == (b.base ^ (0x12340000U + b.ap)));
    }
    swd.parity_rdbuff = false;
    sampler.OnTickIsr(4U);  // Ticks 1..3 were missed; only frame 4 is produced.
    sampler.Sample(timer);
    got = sampler.TakeFrames(frame, sizeof(frame), false);
    CHECK_RECOVERY(got.frames == 1U && Get32(frame) == 4U);
    CHECK_RECOVERY(got.skipped == 3U && got.dropped == 0U);
    sampler.OnTickIsr(0U);
    sampler.Sample(timer);
    CHECK_RECOVERY(sampler.TakeFrames(frame, sizeof(frame), false).frames == 0U);
    sampler.Stop();
  }
}

void StandardInfoDoesNotParkSampler()
{
  ModelSwd swd;
  XrDap::DapVendor vendor(swd);
  vendor.SetCurrentLink(XrDap::Transport::USB);
  Timer timer;
  uint8_t response[128] = {};
  uint16_t length = 0U;
  const uint8_t connect[] = {0x02U, 0x01U};
  CHECK_RECOVERY(vendor.OnCommand(connect, sizeof(connect), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::PASS);
  Item item;
  item.ap = 0U;
  item.base = 0x08000000U;
  item.size = 4U;
  const auto entries = Entries({item});
  std::vector<uint8_t> start = {0x83U, 0xA0U, 0x86U, 0x01U, 0U, 0U, 1U, 0U};
  start.insert(start.end(), entries.begin(), entries.end());
  CHECK_RECOVERY(vendor.OnCommand(start.data(), start.size(), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk);
  vendor.Sample(timer);
  const uint8_t info[] = {0x00U, 0x01U};
  CHECK_RECOVERY(vendor.OnCommand(info, sizeof(info), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::PASS);
  vendor.OnTickIsr();
  vendor.Sample(timer);
  vendor.OnTickIsr();
  vendor.Sample(timer);  // Still sampling after a standard command.
  // Tick 0 plus the two ticks above: three frames, none skipped, none dropped.
  const uint8_t read[] = {0x85U, 0xFFU, 0x00U};
  CHECK_RECOVERY(vendor.OnCommand(read, sizeof(read), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk);
  CHECK_RECOVERY(Get32(response + 14) == 0U);  // dropped
  CHECK_RECOVERY(Get32(response + 18) == 0U);  // skipped
  CHECK_RECOVERY(Get16(response + 22) == 3U);  // frames
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen) == 0U);
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen + 15U) == 1U);
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen + 30U) == 2U);
  const uint8_t stop[] = {0x84U, 0U};
  CHECK_RECOVERY(vendor.OnCommand(stop, sizeof(stop), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::HANDLED);
}

// 紧凑帧（docs/protocol.md 3.5）：解码出的节拍号、失败位和值与采到的一致；每个回复的第 1
// 帧都是原帧；max_bytes 按编码后的字节算 / Compact frames (docs/protocol.md 3.5): the
// decoded ticks, failure bits and values match what was sampled; the first frame of every
// reply is a plain frame; max_bytes counts encoded bytes.
struct Decoded
{
  uint32_t tick;
  uint64_t stamp;
  uint8_t bitmap;
  std::vector<uint32_t> values;
};

uint64_t GetVarint(const uint8_t*& p)
{
  uint64_t v = 0U;
  for (int shift = 0;; shift += 7)
  {
    const uint8_t b = *p++;
    v |= static_cast<uint64_t>(b & 0x7FU) << shift;
    if ((b & 0x80U) == 0U) return v;
  }
}

// 条目 a 每拍到期，b 在偶数拍到期 / Item a is due every tick, b on even ticks.
std::vector<Decoded> DecodeCompact(const uint8_t* p, uint16_t frames)
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
    for (unsigned k = 0; k < due; ++k, p += 4) d.values.push_back(Get32(p));
    out.push_back(d);
  }
  return out;
}

void CompactSubRead()
{
  ModelSwd swd;
  XrDap::DapVendor vendor(swd);
  vendor.SetCurrentLink(XrDap::Transport::USB);
  Timer timer;
  uint8_t response[256] = {};
  uint16_t length = 0U;
  auto command = [&](const std::vector<uint8_t>& request)
  {
    return vendor.OnCommand(request.data(), request.size(), response, sizeof(response),
                            length);
  };
  CHECK_RECOVERY(command({0x80U}) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY((Get32(response + 3) & kFeatureCompactFrames) != 0U);
  CHECK_RECOVERY(command({0x02U, 0x01U}) == XrDap::DapVendor::Result::PASS);
  Item a;
  a.ap = 0U;
  a.size = 4U;
  a.base = 0x08000000U;
  Item b = a;
  b.base += 4U;
  auto entries = Entries({a, b});
  entries[26] = 2U;  // b: div 2
  std::vector<uint8_t> start = {0x83U, 0xE8U, 0x03U, 0U, 0U, 0x02U, 2U, 0U};
  start.insert(start.end(), entries.begin(), entries.end());
  CHECK_RECOVERY(command(start) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk);
  vendor.Sample(timer);  // tick 0: a, b
  vendor.OnTickIsr();
  vendor.Sample(timer);  // tick 1: a
  swd.missing = true;
  vendor.OnTickIsr();
  vendor.Sample(timer);  // tick 2: a, b, both failed
  swd.missing = false;
  vendor.OnTickIsr(3U);
  vendor.Sample(timer);  // tick 5: a (3 and 4 skipped)

  // 只放得下第 1 帧（原帧 19 字节）/ Room for the first frame only (19 plain bytes).
  CHECK_RECOVERY(command({0x85U, 19U, 0U}) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(Get16(response + 22) == 1U && length == kSubReadHeaderLen + 19U);
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen) == 0U);
  const uint32_t a_value = a.base ^ 0x12340000U;
  const uint32_t b_value = b.base ^ 0x12340000U;
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen + 11U) == a_value);
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen + 15U) == b_value);

  // 其余三帧：第 1 帧（节拍 1）又是原帧 / The other three: the first (tick 1) is plain
  // again.
  CHECK_RECOVERY(command({0x85U, 0xFFU, 0U}) == XrDap::DapVendor::Result::HANDLED);
  const uint16_t frames = Get16(response + 22);
  CHECK_RECOVERY(frames == 3U && Get32(response + 10) == 0U);
  CHECK_RECOVERY(Get32(response + 18) == 2U);  // skipped: ticks 3, 4
  const auto got = DecodeCompact(response + kSubReadHeaderLen, frames);
  // 解码用掉的字节正好是回复里的帧字节 / Decoding consumes exactly the reply's frame
  // bytes.
  const uint8_t* end = response + kSubReadHeaderLen;
  for (uint16_t i = 0; i < frames; ++i)
  {
    if (i == 0U)
    {
      end += 15U;
      continue;
    }
    const uint64_t h = GetVarint(end);
    (void)GetVarint(end);
    end += ((h & 1U) != 0U ? 1U : 0U) + 4U * ((got[i].tick % 2U == 0U) ? 2U : 1U);
  }
  CHECK_RECOVERY(end == response + length);
  CHECK_RECOVERY(got[0].tick == 1U && got[0].bitmap == 0U && got[0].values[0] == a_value);
  CHECK_RECOVERY(got[1].tick == 2U && got[1].bitmap == 3U);
  CHECK_RECOVERY(got[1].values[0] == 0U && got[1].values[1] == 0U);
  CHECK_RECOVERY(got[2].tick == 5U && got[2].bitmap == 0U && got[2].values[0] == a_value);
  CHECK_RECOVERY(got[0].stamp <= got[1].stamp && got[1].stamp <= got[2].stamp);
  CHECK_RECOVERY(command({0x84U, 0U}) == XrDap::DapVendor::Result::HANDLED);

  // 下一次 SUB_START 不带 bit1：回到原帧 / The next SUB_START without bit1: plain frames
  // again.
  start[5] = 0U;
  CHECK_RECOVERY(command(start) == XrDap::DapVendor::Result::HANDLED);
  vendor.Sample(timer);
  vendor.OnTickIsr();
  vendor.Sample(timer);
  CHECK_RECOVERY(command({0x85U, 0xFFU, 0U}) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(Get16(response + 22) == 2U && length == kSubReadHeaderLen + 19U + 15U);
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen + 19U) == 1U);
  CHECK_RECOVERY(command({0x84U, 0U}) == XrDap::DapVendor::Result::HANDLED);
}

void ReuseCswKeepsDataFreshAndInvalidatesOnFailure()
{
  ModelSwd swd;
  MemAp mem(swd);
  MemAp::Access access(mem);
  mem.ResetCache();
  const uint32_t initial = Read(mem, 0U);
  CHECK_RECOVERY(swd.csw_reads == 1U);
  swd.value_bias = 7U;
  mem.ResetCache(true);
  CHECK_RECOVERY(Read(mem, 0U) == initial + 7U);
  CHECK_RECOVERY(swd.csw_reads == 1U && swd.memory_reads == 2U);

  // A sampling burst changes hardware AddrInc behind the command's AP state.
  MemAp::ApState sampling;
  sampling.ap = 0U;
  sampling.base_csw = 0x23000002U;
  sampling.valid = true;
  uint8_t values[8] = {};
  CHECK_RECOVERY(mem.ReadWords(sampling, 0x08000000U, 2U, values) == 2U);
  CHECK_RECOVERY((swd.csw[0] & 0x30U) == 0x10U);
  mem.ResetCache(true);
  CHECK_RECOVERY(Read(mem, 0U) == initial + 7U);
  CHECK_RECOVERY((swd.csw[0] & 0x37U) == 2U && swd.csw_reads == 1U);

  CHECK_RECOVERY(Read(mem, 1U) == (0x08000000U ^ 0x12340001U) + 7U);
  CHECK_RECOVERY(swd.csw_reads == 2U);
  mem.ResetCache(true);
  CHECK_RECOVERY(Read(mem, 0U) == initial + 7U);
  CHECK_RECOVERY(Read(mem, 1U) == (0x08000000U ^ 0x12340001U) + 7U);
  CHECK_RECOVERY(swd.csw_reads == 2U);

  Item it;
  it.ap = 0U;
  it.base = 0x08000000U;
  it.size = 4U;
  swd.fail_drw = swd.drw_attempts + 1U;
  const unsigned before_reads = swd.memory_reads;
  CHECK_RECOVERY(mem.Read(it, values) == kItemSwdLast);
  CHECK_RECOVERY(swd.memory_reads == before_reads);  // No failed read is replayed.
  mem.ResetCache(true);
  CHECK_RECOVERY(Read(mem, 0U) == initial + 7U);
  CHECK_RECOVERY(swd.csw_reads == 3U);

  CHECK_RECOVERY(mem.Resync());
  mem.ResetCache(true);
  CHECK_RECOVERY(Read(mem, 0U) == initial + 7U);
  CHECK_RECOVERY(swd.csw_reads == 4U);

  // Failed CSW discovery must not poison future commands.
  swd.missing = true;
  mem.ResetCache();
  CHECK_RECOVERY(mem.Read(it, values) == kItemSwdLast);
  swd.missing = false;
  mem.ResetCache(true);
  CHECK_RECOVERY(Read(mem, 0U) == initial + 7U);
}

void OnlyActiveSamplingReusesCommandCsw()
{
  ModelSwd swd;
  XrDap::DapVendor vendor(swd);
  vendor.SetCurrentLink(XrDap::Transport::USB);
  Timer timer;
  uint8_t response[128] = {};
  uint16_t length = 0U;
  auto command = [&](const std::vector<uint8_t>& request)
  {
    const auto handled = vendor.OnCommand(request.data(), request.size(), response,
                                          sizeof(response), length);
    return handled;
  };
  CHECK_RECOVERY(command({0x02U, 0x01U}) == XrDap::DapVendor::Result::PASS);
  Item it;
  it.ap = 0U;
  it.base = 0x08000000U;
  it.size = 4U;
  const auto encoded = Entries({it});
  std::vector<uint8_t> read = {0x81U, 1U};
  read.insert(read.end(), encoded.begin(), encoded.begin() + 11U);
  auto read_value = [&]()
  {
    CHECK_RECOVERY(command(read) == XrDap::DapVendor::Result::HANDLED);
    CHECK_RECOVERY(response[1] == kStatusOk && response[5] == kItemOk);
    return Get32(response + 6U);
  };
  const auto original = read_value();
  CHECK_RECOVERY(read_value() == original && swd.csw_reads == 2U);

  std::vector<uint8_t> start = {0x83U, 0xA0U, 0x86U, 0x01U, 0U, 0U, 1U, 0U};
  start.insert(start.end(), encoded.begin(), encoded.end());
  CHECK_RECOVERY(command(start) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk && swd.csw_reads == 3U);
  vendor.Sample(timer);
  CHECK_RECOVERY(read_value() == original && swd.csw_reads == 3U);
  swd.value_bias = 19U;
  vendor.OnTickIsr();
  vendor.Sample(timer);
  CHECK_RECOVERY(read_value() == original + 19U && swd.csw_reads == 3U);

  CHECK_RECOVERY(command({0x84U, 0U}) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(read_value() == original + 19U && swd.csw_reads == 4U);
  // A new sampling run cannot inherit the preceding command's CSW cache.
  CHECK_RECOVERY(command(start) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk && swd.csw_reads == 5U);
  CHECK_RECOVERY(read_value() == original + 19U && swd.csw_reads == 5U);
  CHECK_RECOVERY(command({0x84U, 0U}) == XrDap::DapVendor::Result::HANDLED);
}

void ConnectFromTheSamplingLinkRestartsIt()
{
  // 采样中上位机被强杀、没发 DAP_Disconnect：同一条链路上新的 DAP_Connect 结束旧的采样并
  // 重新连接；别的链路的 DAP_Connect 仍被拒绝 / The host is killed while sampling and
  // sends no DAP_Disconnect: a new DAP_Connect on the same link ends the old sampling and
  // connects afresh; a DAP_Connect on another link is still refused.
  ModelSwd swd;
  XrDap::DapVendor vendor(swd);
  Timer timer;
  uint8_t response[128] = {};
  uint16_t length = 0U;
  auto command = [&](XrDap::Transport link, const std::vector<uint8_t>& request)
  {
    vendor.SetCurrentLink(link);
    const auto handled = vendor.OnCommand(request.data(), request.size(), response,
                                          sizeof(response), length);
    return handled;
  };
  CHECK_RECOVERY(command(XrDap::Transport::USB, {0x02U, 0x01U}) ==
                 XrDap::DapVendor::Result::PASS);
  Item it;
  it.ap = 0U;
  it.base = 0x08000000U;
  it.size = 4U;
  const auto encoded = Entries({it});
  std::vector<uint8_t> start = {0x83U, 0xA0U, 0x86U, 0x01U, 0U, 0U, 1U, 0U};
  start.insert(start.end(), encoded.begin(), encoded.end());
  CHECK_RECOVERY(command(XrDap::Transport::USB, start) ==
                 XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(vendor.Sampling());
  // 别的链路：被拒绝，采样不受影响 / Another link: refused, sampling unaffected
  CHECK_RECOVERY(command(XrDap::Transport::TCP, {0x02U, 0x01U}) ==
                 XrDap::DapVendor::Result::REFUSED);
  CHECK_RECOVERY(vendor.Sampling());
  // 同一条链路：旧采样结束，新连接成立 / The same link: old sampling ends, new connection
  CHECK_RECOVERY(command(XrDap::Transport::USB, {0x02U, 0x01U}) ==
                 XrDap::DapVendor::Result::PASS);
  CHECK_RECOVERY(!vendor.Sampling());
  CHECK_RECOVERY(vendor.ConnectedLink() == XrDap::Transport::USB);
  CHECK_RECOVERY(command(XrDap::Transport::USB, start) ==
                 XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk);
  CHECK_RECOVERY(command(XrDap::Transport::USB, {0x84U, 0U}) ==
                 XrDap::DapVendor::Result::HANDLED);
}
}  // namespace

int main()
{
  LibXR::PlatformInit();
  ApSelection();
  SamplerRecovery();
  BurstReadAndFailure();
  SamplerMergedTicksAndBatches();
  StandardInfoDoesNotParkSampler();
  CompactSubRead();
  ReuseCswKeepsDataFreshAndInvalidatesOnFailure();
  OnlyActiveSamplingReusesCommandCsw();
  ConnectFromTheSamplingLinkRestartsIt();
  std::puts("AP selection and sampler framing recovery passed");
}
