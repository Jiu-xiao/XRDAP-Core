// SPDX-License-Identifier: Apache-2.0
// Exercise real MemAp/Sampler (single-loop policy) code against an ADIv5 posted-read model. A framing
// failure must remain visible in the failed frame and recover on a later tick.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "libxr_system.hpp"
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
class ModelSwd final : public LibXR::Debug::Swd
{
 public:
  bool desynced = false;
  bool missing = false;
  unsigned resets = 0;
  unsigned enters = 0;
  unsigned memory_reads = 0;
  unsigned csw_reads = 0;
  uint32_t value_bias = 0U;
  uint32_t select = 0;
  uint32_t posted = 0;
  uint32_t csw[2] = {0x23000002U, 0x23000002U};
  uint32_t tar[2] = {};
  unsigned tar_writes = 0;
  unsigned drw_attempts = 0;
  unsigned fail_drw = 0;
  unsigned parity_drw = 0;
  bool parity_rdbuff = false;
  std::vector<uint32_t> addresses;

  ErrorCode SetClockHz(uint32_t) override { return ErrorCode::OK; }
  void Close() override {}
  ErrorCode EnterSwd() override
  {
    ++enters;
    return LineReset();
  }
  ErrorCode LineReset() override
  {
    ++resets;
    desynced = false;
    select = 0;
    InvalidateSelectCache();
    return ErrorCode::OK;
  }
  void IdleClocks(uint32_t) override {}
  ErrorCode SeqWriteBits(uint32_t, const uint8_t*) override { return ErrorCode::OK; }
  ErrorCode SeqReadBits(uint32_t, uint8_t*) override { return ErrorCode::OK; }
  ErrorCode Transfer(const P::Request& req, P::Response& resp) override
  {
    resp = {};
    resp.ack = (desynced || missing) ? P::Ack::NO_ACK : P::Ack::OK;
    if (resp.ack != P::Ack::OK) return ErrorCode::OK;
    if (req.port == P::Port::DP)
    {
      if (req.rnw)
      {
        resp.rdata = req.addr2b == 0 ? 0x2BA01477U : posted;
        if (req.addr2b == 3 && parity_rdbuff) resp.parity_ok = false;
      }
      else if (req.addr2b == 2)
        select = req.wdata;
      return ErrorCode::OK;
    }
    const unsigned ap = select >> 24;
    CHECK_RECOVERY(ap < 2);
    CHECK_RECOVERY((select & 0xF0U) == 0U);
    if (req.rnw)
    {
      if (req.addr2b == 3 && ++drw_attempts == fail_drw)
      {
        resp.ack = P::Ack::FAULT;
        return ErrorCode::OK;
      }
      resp.rdata = posted;
      if (req.addr2b == 0)
      {
        ++csw_reads;
        posted = csw[ap];
      }
      else if (req.addr2b == 3)
      {
        ++memory_reads;
        addresses.push_back(tar[ap]);
        posted = (tar[ap] ^ (0x12340000U + ap)) + value_bias;
        if ((csw[ap] & 0x30U) == 0x10U)
          tar[ap] = (tar[ap] & ~1023U) | ((tar[ap] + 4U) & 1023U);
        if (drw_attempts == parity_drw) resp.parity_ok = false;
      }
    }
    else if (req.addr2b == 0)
      csw[ap] = req.wdata;
    else if (req.addr2b == 1)
    {
      ++tar_writes;
      tar[ap] = req.wdata;
    }
    else
      CHECK_RECOVERY(false);  // These tests never write target memory.
    return ErrorCode::OK;
  }
};

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
  MemAp::Lock lock(mem);
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
  LoopSampler sampler(mem, +[](void*, uint32_t) { return true; }, nullptr);
  Timer timer;
  // AP=0, id=1, u32 at 0x08000000, no dereference, div=1, phase=0.
  const uint8_t entry[] = {0, 1, 0, 4, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0};
  CHECK_RECOVERY(sampler.Start({entry, entry + sizeof(entry), 1}, 10000, false, 1U) ==
                 kStatusOk);
  uint8_t frame[32] = {};
  auto take = [&]()
  {
    const auto got = sampler.TakeFrames(frame, sizeof(frame));
    CHECK_RECOVERY(got.frames == 1 && got.bytes == 15);
    CHECK_RECOVERY(got.skipped == 0 && got.dropped == 0);
  };
  sampler.Poll(timer);
  take();
  CHECK_RECOVERY(Get32(frame) == 0 && frame[10] == 0);
  swd.desynced = true;
  sampler.OnTickIsr();
  sampler.Poll(timer);
  take();
  CHECK_RECOVERY(Get32(frame) == 1 && frame[10] == 1);  // Keep evidence of the error.
  CHECK_RECOVERY(swd.resets == 0);
  sampler.OnTickIsr();
  sampler.Poll(timer);
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
    sampler.Poll(timer);
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
    MemAp::Lock lock(mem);
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
    LoopSampler sampler(mem, +[](void*, uint32_t) { return true; }, nullptr);
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
    sampler.Poll(timer);
    uint8_t frame[64] = {};
    auto got = sampler.TakeFrames(frame, sizeof(frame));
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
    sampler.CommandBegin();
    sampler.CommandReadOnly();
    sampler.OnTickIsr(4U);  // Ticks 1..3 were missed; only frame 4 is produced.
    sampler.Poll(timer);
    sampler.CommandEnd();
    got = sampler.TakeFrames(frame, sizeof(frame));
    CHECK_RECOVERY(got.frames == 1U && Get32(frame) == 4U);
    CHECK_RECOVERY(got.skipped == 3U && got.dropped == 0U);
    sampler.OnTickIsr(0U);
    sampler.Poll(timer);
    CHECK_RECOVERY(sampler.TakeFrames(frame, sizeof(frame)).frames == 0U);
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
  vendor.CommandBegin();
  CHECK_RECOVERY(vendor.OnCommand(connect, sizeof(connect), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::PASS);
  vendor.CommandEnd();
  Item item;
  item.ap = 0U;
  item.base = 0x08000000U;
  item.size = 4U;
  const auto entries = Entries({item});
  std::vector<uint8_t> start = {0x83U, 0xA0U, 0x86U, 0x01U, 0U, 0U, 1U, 0U};
  start.insert(start.end(), entries.begin(), entries.end());
  vendor.CommandBegin();
  CHECK_RECOVERY(vendor.OnCommand(start.data(), start.size(), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk);
  vendor.CommandEnd();
  vendor.Poll(timer);
  const uint8_t info[] = {0x00U, 0x01U};
  vendor.CommandBegin();
  CHECK_RECOVERY(vendor.OnCommand(info, sizeof(info), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::PASS);
  vendor.OnTickIsr();
  vendor.Poll(timer);
  vendor.OnTickIsr();
  vendor.Poll(timer);  // Still sampling with the read-only command turn open.
  vendor.CommandEnd();
  // Tick 0 plus the two ticks above: three frames, none skipped, none dropped.
  const uint8_t read[] = {0x85U, 0xFFU, 0x00U};
  vendor.CommandBegin();
  CHECK_RECOVERY(vendor.OnCommand(read, sizeof(read), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::HANDLED);
  vendor.CommandEnd();
  CHECK_RECOVERY(response[1] == kStatusOk);
  CHECK_RECOVERY(Get32(response + 14) == 0U);  // dropped
  CHECK_RECOVERY(Get32(response + 18) == 0U);  // skipped
  CHECK_RECOVERY(Get16(response + 22) == 3U);  // frames
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen) == 0U);
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen + 15U) == 1U);
  CHECK_RECOVERY(Get32(response + kSubReadHeaderLen + 30U) == 2U);
  const uint8_t stop[] = {0x84U, 0U};
  vendor.CommandBegin();
  CHECK_RECOVERY(vendor.OnCommand(stop, sizeof(stop), response, sizeof(response),
                                  length) == XrDap::DapVendor::Result::HANDLED);
  vendor.CommandEnd();
}

void ReuseCswKeepsDataFreshAndInvalidatesOnFailure()
{
  ModelSwd swd;
  MemAp mem(swd);
  MemAp::Lock lock(mem);
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
    vendor.CommandBegin();
    const auto handled = vendor.OnCommand(request.data(), request.size(), response,
                                          sizeof(response), length);
    vendor.CommandEnd();
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
  vendor.Poll(timer);
  CHECK_RECOVERY(read_value() == original && swd.csw_reads == 3U);
  swd.value_bias = 19U;
  vendor.OnTickIsr();
  vendor.Poll(timer);
  CHECK_RECOVERY(read_value() == original + 19U && swd.csw_reads == 3U);

  CHECK_RECOVERY(command({0x84U, 0U}) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(read_value() == original + 19U && swd.csw_reads == 4U);
  // A new sampling run cannot inherit the preceding command's CSW cache.
  CHECK_RECOVERY(command(start) == XrDap::DapVendor::Result::HANDLED);
  CHECK_RECOVERY(response[1] == kStatusOk && swd.csw_reads == 5U);
  CHECK_RECOVERY(read_value() == original + 19U && swd.csw_reads == 5U);
  CHECK_RECOVERY(command({0x84U, 0U}) == XrDap::DapVendor::Result::HANDLED);
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
  ReuseCswKeepsDataFreshAndInvalidatesOnFailure();
  OnlyActiveSamplingReusesCommandCsw();
  std::puts("AP selection and sampler framing recovery passed");
}
