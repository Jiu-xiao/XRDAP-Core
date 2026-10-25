// SPDX-License-Identifier: Apache-2.0
// Exercise real MemAp/Sampler code against an ADIv5 posted-read model. A framing
// failure must remain visible in the failed frame and recover on a later tick.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "libxr_system.hpp"
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
  uint32_t select = 0;
  uint32_t posted = 0;
  uint32_t csw[2] = {0x23000002U, 0x23000002U};
  uint32_t tar[2] = {};

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
        resp.rdata = req.addr2b == 0 ? 0x2BA01477U : posted;
      else if (req.addr2b == 2)
        select = req.wdata;
      return ErrorCode::OK;
    }
    const unsigned ap = select >> 24;
    CHECK_RECOVERY(ap < 2);
    CHECK_RECOVERY((select & 0xF0U) == 0U);
    if (req.rnw)
    {
      resp.rdata = posted;
      if (req.addr2b == 0)
        posted = csw[ap];
      else if (req.addr2b == 3)
      {
        ++memory_reads;
        posted = tar[ap] ^ (0x12340000U + ap);
      }
    }
    else if (req.addr2b == 0)
      csw[ap] = req.wdata;
    else if (req.addr2b == 1)
      tar[ap] = req.wdata;
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
  CHECK_RECOVERY(mem.Stats().failed_transactions == 0U);
}

void SamplerRecovery()
{
  ModelSwd swd;
  MemAp mem(swd);
  Sampler sampler(mem, +[](void*) { return true; }, nullptr);
  Timer timer;
  // AP=0, id=1, u32 at 0x08000000, no dereference, div=1, phase=0.
  const uint8_t entry[] = {0, 1, 0, 4, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0};
  CHECK_RECOVERY(sampler.Start({entry, entry + sizeof(entry), 1}, 10000, false) ==
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
  CHECK_RECOVERY(mem.Stats().last_ack == 0U && mem.Stats().failed_transactions > 0U);
  CHECK_RECOVERY(swd.resets == 0);
  sampler.OnTickIsr();
  sampler.Poll(timer);
  take();
  CHECK_RECOVERY(Get32(frame) == 2 && frame[10] == 0);
  CHECK_RECOVERY(Get32(frame + 11) == (0x08000000U ^ 0x12340000U));
  CHECK_RECOVERY(swd.resets == 1 && mem.Stats().resyncs == 1);
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
}  // namespace

int main()
{
  LibXR::PlatformInit();
  ApSelection();
  SamplerRecovery();
  std::puts("AP selection and sampler framing recovery passed");
}
