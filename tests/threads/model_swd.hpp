// SPDX-License-Identifier: Apache-2.0
#pragma once

// ADIv5 的 SWD 模型：两个 MEM-AP，读和写都是 posted，可以注入失步、故障和校验错。读出的值
// 由地址算出，写进的值记在表里。
//   posted 读：AP 读回的是上一次的结果，最后一个结果要读 RDBUFF。
//   posted 写：DRW 写一律回 OK，失败要到下一次 AP 访问或 RDBUFF 读才以 FAULT 出现；
//   CTRL/STAT 的读不等 AP 事务，所以看不到它（V305-2）。
//
// An ADIv5 SWD model: two MEM-APs, reads and writes both posted, with injectable desync,
// faults and parity errors. A read value is computed from its address; written values are
// kept in a table.
//   Posted read: an AP read returns the previous result; the last result takes an RDBUFF
//   read.
//   Posted write: a DRW write always answers OK; a failure only shows up as FAULT on the
//   next AP access or RDBUFF read. A CTRL/STAT read does not wait for the AP transaction,
//   so it cannot see the failure (V305-2).

#include <cstdint>
#include <map>
#include <vector>

#include "../unit/check.hpp"
#include "libxr.hpp"
#include "xrdap/vendor/mem_ap.hpp"

namespace
{
using LibXR::ErrorCode;
namespace P = LibXR::Debug::SwdProtocol;

class ModelSwd final : public LibXR::Debug::Swd
{
 public:
  /// 写这个地址的 posted 写会失败 / A posted write to this address fails.
  static constexpr uint32_t kFaultAddr = 0x20001000U;

  /// 从 AP 的 addr 读出的值 / The value read at addr on an AP.
  static uint32_t Value(unsigned ap, uint32_t addr) { return addr ^ (0x12340000U + ap); }

  // 注入 / Injections.
  /// 线复位之前一律 NO_ACK / NO_ACK until a line reset.
  bool desynced = false;
  /// 目标不在，一律 NO_ACK / No target: always NO_ACK.
  bool missing = false;
  /// 第 n 次 DRW 读回 FAULT / DRW read number n answers FAULT.
  unsigned fail_drw = 0;
  /// 第 n 次 DRW 读校验错 / DRW read number n has a parity error.
  unsigned parity_drw = 0;
  /// RDBUFF 读校验错 / RDBUFF reads have a parity error.
  bool parity_rdbuff = false;
  /// 读出的值再加上它 / Added to every value read.
  uint32_t value_bias = 0U;
  /// CTRL/STAT 的粘滞位，ABORT 清除 / Sticky bits in CTRL/STAT, cleared by ABORT.
  uint32_t ctrl_stat = 0U;
  /// AP 的 IDR，默认是 MEM-AP、AHB3 / The APs' IDR; a MEM-AP of type AHB3 by default.
  uint32_t idr = (8U << 13) | 1U;
  uint32_t csw[2] = {0x23000002U, 0x23000002U};

  // 观察 / Observations.
  unsigned resets = 0;
  unsigned enters = 0;
  unsigned memory_reads = 0;
  unsigned csw_reads = 0;
  unsigned tar_writes = 0;
  unsigned drw_attempts = 0;
  std::vector<uint32_t> addresses;

  /// 写到 addr 的值，没写过是 0 / The value written at addr, 0 when none was.
  uint32_t Written(uint32_t addr) const
  {
    const auto it = written_.find(addr);
    return it == written_.end() ? 0U : it->second;
  }

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
    select_ = 0;
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
    if (resp.ack == P::Ack::OK)
    {
      if (req.port == P::Port::DP)
      {
        DpAccess(req, resp);
      }
      else
      {
        ApAccess(req, resp);
      }
    }
    return ErrorCode::OK;
  }

 private:
  // A[3:2]
  static constexpr uint8_t kDpAbort = 0U;
  static constexpr uint8_t kDpCtrlStat = 1U;
  static constexpr uint8_t kDpSelect = 2U;
  static constexpr uint8_t kDpRdbuff = 3U;
  static constexpr uint8_t kApCsw = 0U;
  static constexpr uint8_t kApTar = 1U;
  static constexpr uint8_t kApDrw = 3U;
  static constexpr uint32_t kAbortClearAll = 0x1EU;

  // 结算上一次 posted 写：失败了这次访问就回 FAULT / Settle the last posted write: when
  // it failed, this access answers FAULT.
  bool Settle(P::Response& resp)
  {
    const bool failed = write_failed_;
    write_failed_ = false;
    if (failed)
    {
      resp.ack = P::Ack::FAULT;
    }
    return !failed;
  }

  void DpAccess(const P::Request& req, P::Response& resp)
  {
    if (!req.rnw)
    {
      if (req.addr2b == kDpAbort && (req.wdata & kAbortClearAll) == kAbortClearAll)
      {
        ctrl_stat = 0U;
      }
      else if (req.addr2b == kDpSelect)
      {
        select_ = req.wdata;
      }
      return;
    }
    if (req.addr2b == kDpRdbuff)
    {
      if (Settle(resp))
      {
        resp.rdata = posted_;
        resp.parity_ok = !parity_rdbuff;
      }
      return;
    }
    resp.rdata = req.addr2b == kDpCtrlStat ? ctrl_stat : 0x2BA01477U;
  }

  void ApAccess(const P::Request& req, P::Response& resp)
  {
    if (!Settle(resp))
    {
      return;
    }
    const uint32_t ap = select_ >> 24;
    const uint32_t bank = select_ & 0xF0U;
    CHECK(ap < 2U);
    CHECK(bank == 0U || bank == 0xF0U);
    if (ap >= 2U)
    {
      resp.ack = P::Ack::FAULT;
      return;
    }
    if (bank == 0xF0U)
    {
      CHECK(req.rnw && req.addr2b == kApDrw);
      resp.rdata = posted_;
      posted_ = idr;
    }
    else if (req.rnw)
    {
      ApRead(ap, req, resp);
    }
    else
    {
      ApWrite(ap, req);
    }
  }

  void ApRead(uint32_t ap, const P::Request& req, P::Response& resp)
  {
    if (req.addr2b == kApDrw && ++drw_attempts == fail_drw)
    {
      resp.ack = P::Ack::FAULT;
      return;
    }
    resp.rdata = posted_;
    if (req.addr2b == kApCsw)
    {
      ++csw_reads;
      posted_ = csw[ap];
    }
    else if (req.addr2b == kApDrw)
    {
      ++memory_reads;
      addresses.push_back(tar_[ap]);
      posted_ = Value(ap, tar_[ap]) + value_bias;
      if ((csw[ap] & 0x30U) == 0x10U)
      {
        tar_[ap] = (tar_[ap] & ~1023U) | ((tar_[ap] + 4U) & 1023U);
      }
      if (drw_attempts == parity_drw)
      {
        resp.parity_ok = false;
      }
    }
  }

  void ApWrite(uint32_t ap, const P::Request& req)
  {
    if (req.addr2b == kApCsw)
    {
      csw[ap] = req.wdata;
    }
    else if (req.addr2b == kApTar)
    {
      ++tar_writes;
      tar_[ap] = req.wdata;
    }
    else if (tar_[ap] == kFaultAddr)
    {
      write_failed_ = true;
    }
    else
    {
      written_[tar_[ap]] = req.wdata;
    }
  }

  uint32_t select_ = 0;
  uint32_t posted_ = 0;
  uint32_t tar_[2] = {};
  bool write_failed_ = false;
  std::map<uint32_t, uint32_t> written_;
};
}  // namespace
