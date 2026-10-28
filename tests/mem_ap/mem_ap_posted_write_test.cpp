// SPDX-License-Identifier: Apache-2.0

// V305-2：MEM-AP 的写是 posted 的，总线故障可能要到下一次 AP 访问才回来，而 DP CTRL/STAT
// 的读并不等 AP 事务。这份测试用一个假 SWD 把这件事照实模拟出来：DRW 写一律回 OK 并记下
// "已提交"，成功与否要等到下一次 AP 访问或 DP RDBUFF 读才以 FAULT 出现；CTRL/STAT 的读
// 永远只回当前的粘滞位，不结算。期望：故障报在写它的那个条目上（0x1F），下一个条目照常写
// 进去（0x00 且值落地）。
//
// V305-2: a MEM-AP write is posted, so a bus fault may only come back on the next AP
// access, while a DP CTRL/STAT read does not wait for the AP transaction. This test
// models that with a fake SWD: a DRW write always answers OK and records "submitted";
// success or failure only shows up as FAULT on the next AP access or DP RDBUFF read, and
// a CTRL/STAT read only returns the sticky bits as they are now. The failure must be
// reported on the item that wrote it (0x1F) and the next item must still be written (0x00
// with the value in memory).

#include <cstdint>
#include <cstdio>
#include <map>

#include "../unit/check.hpp"
#include "debug/swd.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/protocol.hpp"

namespace
{
using Ack = LibXR::Debug::SwdProtocol::Ack;
using Port = LibXR::Debug::SwdProtocol::Port;
using Req = LibXR::Debug::SwdProtocol::Request;
using Resp = LibXR::Debug::SwdProtocol::Response;

// AP 寄存器（A[3:2]）与 DP 寄存器 / AP and DP registers (A[3:2]).
constexpr uint8_t kApCsw = 0x0U;
constexpr uint8_t kApTar = 0x1U;
constexpr uint8_t kApDrw = 0x3U;
constexpr uint8_t kDpAbort = 0x0U;
constexpr uint8_t kDpCtrlStat = 0x1U;
constexpr uint8_t kDpSelect = 0x2U;
constexpr uint8_t kDpRdbuff = 0x3U;

// CTRL/STAT 的 STICKYERR / CTRL/STAT STICKYERR.
constexpr uint32_t kStickyErr = 1U << 5;
// ABORT 的四个清除位 / The four clear bits of ABORT.
constexpr uint32_t kAbortClearAll = 0x1EU;
// MEM-AP、AHB3 的 IDR：类别在位 16:13 为 8，类型在位 3:0 为 1 / IDR of a MEM-AP of class
// 8 (bits 16:13) and type 1 (AHB3).
constexpr uint32_t kAhbApIdr = (8U << 13) | 1U;
// CSW 的 Size 字段 / CSW Size field.
constexpr uint32_t kCswSize = 0x7U;

/// 假 SWD：一个 AHB-AP 加一块内存，post 写的故障晚一拍回来 / Fake SWD: one AHB-AP with
/// a memory, where a posted write's fault comes back one transaction late.
class FakeMemApSwd final : public LibXR::Debug::Swd
{
 public:
  /// 写这个地址会在下一次 AP 访问时回 FAULT / A write here faults on the next AP access.
  static constexpr uint32_t kFaultAddr = 0x20001000U;

  /// 读一个内存字（调试用）/ Read a memory word (for the checks).
  uint32_t Peek(uint32_t addr) const
  {
    const auto IT = mem_.find(addr & ~3U);
    return (IT == mem_.end()) ? 0U : IT->second;
  }

  /// 迟到的 FAULT 次数 / Number of late FAULTs delivered.
  uint32_t late_faults = 0U;
  /// RDBUFF 读的次数 / Number of DP RDBUFF reads.
  uint32_t rdbuff_reads = 0U;

  // ---- libxr 的 SWD 接口 / libxr's SWD interface ----

  LibXR::ErrorCode SetClockHz(uint32_t) override { return LibXR::ErrorCode::OK; }
  void Close() override {}
  LibXR::ErrorCode LineReset() override { return LibXR::ErrorCode::OK; }
  LibXR::ErrorCode EnterSwd() override { return LibXR::ErrorCode::OK; }
  void IdleClocks(uint32_t) override {}
  LibXR::ErrorCode SeqWriteBits(uint32_t, const uint8_t*) override
  {
    return LibXR::ErrorCode::OK;
  }
  LibXR::ErrorCode SeqReadBits(uint32_t, uint8_t*) override
  {
    return LibXR::ErrorCode::OK;
  }

  LibXR::ErrorCode Transfer(const Req& req, Resp& resp) override
  {
    resp.rdata = 0U;
    resp.parity_ok = true;
    resp.ack = Ack::OK;
    const LibXR::ErrorCode EC =
        (req.port == Port::DP) ? DpAccess(req, resp) : ApAccess(req, resp);
    if (trace)
    {
      std::fprintf(stderr, "  swd %s %s a%u w=%08X -> ack=%u d=%08X\n",
                   (req.port == Port::DP) ? "DP" : "AP", req.rnw ? "r" : "w",
                   static_cast<unsigned>(req.addr2b), req.wdata,
                   static_cast<unsigned>(resp.ack), resp.rdata);
    }
    return EC;
  }

  /// 打印每一笔传输（排查用）/ Print every transfer (for debugging).
  bool trace = false;

 private:
  /// 结算上一次 posted 写：成功即无事，失败回 FAULT / Settle the last posted write:
  /// nothing to do on success, FAULT when it failed.
  bool Settle(Ack& ack)
  {
    if (!pending_.valid)
    {
      return true;
    }
    pending_.valid = false;
    if (!pending_.fault)
    {
      return true;
    }
    ctrl_stat_ |= kStickyErr;
    ++late_faults;
    ack = Ack::FAULT;
    return false;
  }

  LibXR::ErrorCode DpAccess(const Req& req, Resp& resp)
  {
    if (req.rnw)
    {
      switch (req.addr2b)
      {
        case kDpRdbuff:
          // RDBUFF 会把 AP 事务等到底：写没完成就先 WAIT，失败时回 FAULT。这里直接给出
          // 结果 / RDBUFF waits for the AP transaction: FAULT when the write failed.
          ++rdbuff_reads;
          (void)Settle(resp.ack);
          return LibXR::ErrorCode::OK;
        case kDpCtrlStat:
          // 关键：CTRL/STAT 不等 AP 事务，所以晚到的故障在这里看不见 /
          // CTRL/STAT does not wait for the AP transaction, so a late fault is invisible.
          resp.rdata = ctrl_stat_;
          return LibXR::ErrorCode::OK;
        case kDpSelect:
          resp.rdata = select_;
          return LibXR::ErrorCode::OK;
        default:
          resp.rdata = 0x2BA01477U;  // IDCODE
          return LibXR::ErrorCode::OK;
      }
    }

    switch (req.addr2b)
    {
      case kDpAbort:
        if ((req.wdata & kAbortClearAll) != 0U)
        {
          ctrl_stat_ &= ~(kStickyErr | (1U << 1) | (1U << 7));
        }
        return LibXR::ErrorCode::OK;
      case kDpCtrlStat:
        ctrl_stat_ = req.wdata;
        return LibXR::ErrorCode::OK;
      case kDpSelect:
        select_ = req.wdata;
        return LibXR::ErrorCode::OK;
      default:
        return LibXR::ErrorCode::OK;
    }
  }

  LibXR::ErrorCode ApAccess(const Req& req, Resp& resp)
  {
    // 任何 AP 访问都会把上一次 post 写等到底，失败时这次访问本身回 FAULT /
    // Every AP access waits for the last posted write and faults when it failed.
    if (!Settle(resp.ack))
    {
      return LibXR::ErrorCode::OK;
    }

    if ((select_ & 0xF0U) == 0xF0U)
    {
      // bank 0xF 的 0xFC 是 AP IDR / AP IDR at 0xFC in bank 0xF.
      if (req.rnw)
      {
        resp.rdata = kAhbApIdr;
      }
      return LibXR::ErrorCode::OK;
    }

    switch (req.addr2b)
    {
      case kApCsw:
        if (req.rnw)
        {
          resp.rdata = csw_;
        }
        else
        {
          csw_ = req.wdata;
        }
        return LibXR::ErrorCode::OK;
      case kApTar:
        if (req.rnw)
        {
          resp.rdata = tar_;
        }
        else
        {
          tar_ = req.wdata;
        }
        return LibXR::ErrorCode::OK;
      case kApDrw:
        if (req.rnw)
        {
          resp.rdata = Read(tar_);
          return LibXR::ErrorCode::OK;
        }
        // 写是 posted 的：这里一律回 OK，只记下来等下一次 AP 访问 / RDBUFF 读 /
        // The write is posted: always OK here, only recorded for the next access.
        if (tar_ == kFaultAddr)
        {
          pending_.valid = true;
          pending_.fault = true;
        }
        else
        {
          Write(tar_, req.wdata);
          pending_.valid = true;
          pending_.fault = false;
        }
        return LibXR::ErrorCode::OK;
      default:
        return LibXR::ErrorCode::OK;
    }
  }

  uint32_t& Word(uint32_t addr) { return mem_[addr & ~3U]; }

  uint32_t Read(uint32_t addr) const
  {
    const auto IT = mem_.find(addr & ~3U);
    const uint32_t W = (IT == mem_.end()) ? 0U : IT->second;
    if ((csw_ & kCswSize) == 0U)
    {
      return (W >> ((addr & 3U) * 8U)) & 0xFFU;
    }
    if ((csw_ & kCswSize) == 1U)
    {
      return (W >> ((addr & 2U) * 8U)) & 0xFFFFU;
    }
    return W;
  }

  void Write(uint32_t addr, uint32_t val)
  {
    if ((csw_ & kCswSize) == 0U)
    {
      const uint32_t shift = (addr & 3U) * 8U;
      Word(addr) = (Word(addr) & ~(0xFFU << shift)) | ((val & 0xFFU) << shift);
      return;
    }
    if ((csw_ & kCswSize) == 1U)
    {
      const uint32_t shift = (addr & 2U) * 8U;
      Word(addr) = (Word(addr) & ~(0xFFFFU << shift)) | ((val & 0xFFFFU) << shift);
      return;
    }
    Word(addr) = val;
  }

  struct Pending
  {
    bool valid = false;
    bool fault = false;
  };

  uint32_t select_ = 0U;
  uint32_t ctrl_stat_ = 0U;
  uint32_t csw_ = 0U;
  uint32_t tar_ = 0U;
  Pending pending_{};
  std::map<uint32_t, uint32_t> mem_;
};

XrDap::Vendor::Item MakeWordItem(uint32_t addr)
{
  XrDap::Vendor::Item it;
  it.ap = 0U;
  it.size = 4U;
  it.base = addr;
  it.n = 0U;
  it.final_off = 0;
  return it;
}

// 控制组：普通写要成功且值落地 / Control: a plain write succeeds and lands.
TEST(WriteToGoodAddressSucceeds)
{
  FakeMemApSwd swd;
  XrDap::Vendor::MemAp mem(swd);
  XrDap::Vendor::MemAp::Access access(mem);
  mem.ResetCache();

  const XrDap::Vendor::Item IT = MakeWordItem(0x20000000U);
  const uint8_t VALUE[4] = {0x2A, 0x00, 0x00, 0x00};
  CHECK_EQ(mem.Write(IT, VALUE), XrDap::Vendor::kItemOk);
  CHECK_EQ(swd.Peek(0x20000000U), 0x0000002AU);
}

// V305-2：写到会出总线故障的地址，故障必须报在这个条目上，下一个条目要照常写进去 /
// V305-2: writing to an address that faults must report it on that item and leave the
// next item working.
TEST(WriteFaultIsReportedOnTheItemThatWroteIt)
{
  FakeMemApSwd swd;
  XrDap::Vendor::MemAp mem(swd);
  XrDap::Vendor::MemAp::Access access(mem);
  mem.ResetCache();

  const XrDap::Vendor::Item BAD = MakeWordItem(FakeMemApSwd::kFaultAddr);
  const uint8_t V1[4] = {0x11, 0x11, 0x11, 0x11};
  CHECK_EQ(mem.Write(BAD, V1), XrDap::Vendor::kItemSwdLast);

  const XrDap::Vendor::Item GOOD = MakeWordItem(0x20000000U);
  const uint8_t V2[4] = {0x22, 0x22, 0x22, 0x22};
  CHECK_EQ(mem.Write(GOOD, V2), XrDap::Vendor::kItemOk);

  CHECK_EQ(swd.Peek(0x20000000U), 0x22222222U);
  CHECK_EQ(swd.Peek(FakeMemApSwd::kFaultAddr), 0U);
  CHECK_EQ(swd.late_faults, 1U);
}

}  // namespace

int main()
{
  int failed_cases = 0;
  for (const auto& c : XrDapTest::Cases())
  {
    const int before = XrDapTest::Failures();
    c.fn();
    const bool ok = XrDapTest::Failures() == before;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    failed_cases += ok ? 0 : 1;
  }
  std::printf("%zu cases, %d failed\n", XrDapTest::Cases().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
