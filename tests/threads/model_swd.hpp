// SPDX-License-Identifier: Apache-2.0
#pragma once

// ADIv5 的 SWD 模型：两个 AP，带 posted 读，可以注入失步和故障。采样和执行器的线程测试共
// 用 / An ADIv5 SWD model: two APs with posted reads, with injectable desync and faults.
// Shared by the sampling and the executor thread tests.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "libxr.hpp"
#include "xrdap/vendor/mem_ap.hpp"

#define MODEL_SWD_CHECK(x)                                 \
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
using LibXR::ErrorCode;
namespace P = LibXR::Debug::SwdProtocol;

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
    MODEL_SWD_CHECK(ap < 2);
    MODEL_SWD_CHECK((select & 0xF0U) == 0U);
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
      MODEL_SWD_CHECK(false);  // These tests never write target memory.
    return ErrorCode::OK;
  }
};
}  // namespace
