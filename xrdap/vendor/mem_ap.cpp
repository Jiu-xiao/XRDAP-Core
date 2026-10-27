// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/mem_ap.hpp"

#include <cstring>

namespace XrDap::Vendor
{
namespace
{
using Ack = LibXR::Debug::SwdProtocol::Ack;
using DpReadReg = LibXR::Debug::SwdProtocol::DpReadReg;

// MEM-AP 寄存器（A[3:2]）/ MEM-AP registers (A[3:2]).
constexpr uint8_t kApCsw = 0x00U;
constexpr uint8_t kApTar = 0x01U;
constexpr uint8_t kApDrw = 0x03U;
// CSW 的 Size（位 2:0）和 AddrInc（位 5:4）/ CSW Size (bits 2:0) and AddrInc (bits 5:4).
constexpr uint32_t kCswSizeAndInc = 0x37U;
// AHB-AP 的 CSW Prot（位 30:24）里要置的位：位 24 HPROT0 数据访问，位 25 HPROT1 特权，
// 位 29 调试主机，与 OpenOCD 的 CSW_AHB_DEFAULT 相同；缓存、缓冲等其余位保持 AP 原值 /
// Bits to set in the AHB-AP CSW Prot field (bits 30:24): bit 24 HPROT0 data access,
// bit 25 HPROT1 privileged, bit 29 debug master, as in OpenOCD's CSW_AHB_DEFAULT; the
// cache, buffer and other bits keep the AP's value.
constexpr uint32_t kCswAhbProt = 0x23000000U;
constexpr uint32_t kCswHprot1 = 1U << 25;
// AP 的 IDR 在 bank 0xF 的 0xFC：类别在位 16:13（8 为 MEM-AP），类型在位 3:0（1 AHB3、
// 5 AHB5、8 带增强 HPROT 的 AHB5）/ The AP IDR is at 0xFC in bank 0xF: class in bits
// 16:13 (8 is MEM-AP), type in bits 3:0 (1 AHB3, 5 AHB5, 8 AHB5 with enhanced HPROT).
constexpr uint32_t kSelectBankIdr = 0xF0U;
constexpr uint8_t kApIdr = 0x03U;
// CTRL/STAT 的 STICKYORUN、STICKYERR、WDATAERR / CTRL/STAT STICKYORUN, STICKYERR,
// WDATAERR.
constexpr uint32_t kCtrlStatErrors = (1U << 1) | (1U << 5) | (1U << 7);
// ABORT 的 STKCMPCLR、STKERRCLR、WDERRCLR、ORUNERRCLR / ABORT STKCMPCLR, STKERRCLR,
// WDERRCLR, ORUNERRCLR.
constexpr uint32_t kAbortClearAll = 0x1EU;

uint8_t SizeCode(uint8_t size) { return (size == 1U) ? 0U : (size == 2U) ? 1U : 2U; }
}  // namespace

// ack 按引用传入：实参 swd_.Xxx(ack) 和 ack 的求值顺序没有规定，传值时不开优化会先读到调用前的
// NO_ACK（V305-2）。
// ack is taken by reference: the evaluation order of `swd_.Xxx(ack)` and `ack` is unspecified,
// and passed by value an unoptimised build reads the NO_ACK from before the call (V305-2).
bool MemAp::TxnOk(LibXR::ErrorCode ec, const Ack& ack)
{
  if (ec == LibXR::ErrorCode::OK && ack == Ack::OK)
  {
    return true;
  }
  last_ack_.store(static_cast<uint32_t>(ack));
  last_error_.store(static_cast<uint32_t>(ec));
  failed_transactions_.fetch_add(1U);
  return false;
}

bool MemAp::Resync()
{
  ResetCache();
  resyncs_.fetch_add(1U);
  swd_.InvalidateSelectCache();
  // Re-enter SWD as well as resetting the line. A malformed request can leave the
  // target's debug port in the other wire protocol; a bare line reset does not
  // switch it back. EnterSwd() never touches the target CPU run/halt state.
  if (swd_.EnterSwd() != LibXR::ErrorCode::OK)
  {
    return false;
  }
  Ack ack = Ack::NO_ACK;
  // Clear sticky DP errors before probing IDCODE. Use the transaction wrappers so a
  // transient WAIT does not turn a recoverable link into a failed sample forever.
  if (!TxnOk(swd_.WriteAbortTxn(kAbortClearAll, ack), ack))
  {
    return false;
  }
  uint32_t id = 0U;
  if (!TxnOk(swd_.DpReadTxn(DpReadReg::IDCODE, id, ack), ack))
  {
    return false;
  }
  return true;
}

void MemAp::ResetCache(bool reuse_csw)
{
  const uint32_t failures = failed_transactions_.load(std::memory_order_relaxed);
  reuse_csw = reuse_csw && failures == cache_failure_count_;
  cache_failure_count_ = failures;
  uint8_t kept = 0U;
  if (reuse_csw)
  {
    for (uint8_t i = 0U; i < ap_count_; ++i)
    {
      if (ap_[i].valid)
      {
        ap_[kept] = ap_[i];
        ap_[kept++].written = false;
      }
    }
  }
  for (size_t i = kept; i < kMaxAps; ++i)
  {
    ap_[i] = ApState{};
  }
  ap_count_ = kept;
}

MemAp::ApState* MemAp::EnsureAp(uint8_t ap)
{
  for (size_t i = 0; i < ap_count_; ++i)
  {
    if (ap_[i].ap == ap)
    {
      return ap_[i].valid ? &ap_[i] : nullptr;
    }
  }
  // 表满时复用最后一格，代价是每次重新读 CSW。
  ApState* st = (ap_count_ >= kMaxAps) ? &ap_[kMaxAps - 1U] : &ap_[ap_count_++];
  st->ap = ap;
  st->valid = ReadCsw(ap, st->base_csw);
  st->written = false;
  return st->valid ? st : nullptr;
}

// 刚重新连接，或上一次访问被打断（例如采样被停掉）之后，第一次 AP 访问偶尔会因 DP 里
// 残留的粘滞错误失败，所以失败时清掉再试一次。
// Right after a reconnect, or after an access was cut short (for example sampling being
// stopped), the first AP access sometimes fails on a sticky error left in the DP, so a
// failure clears it and tries once more.
//
// AHB-AP 上电后的 Prot 可能是非特权访问，这时访问 PPB（CPUID、DHCSR、AIRCR）会出错，
// 所以 AHB-AP 一律改成特权数据访问，与 OpenOCD 的 CSW_AHB_DEFAULT 相同。别的 AP
// 类型这几位 含义不同（AXI-AP 是 ARPROT 和缓存属性），保持原值。已经是特权访问时不再读
// IDR，这一位 写过一次就留在 AP 里。 On an AHB-AP the Prot field after power-up may say
// unprivileged, and accesses to the PPB (CPUID, DHCSR, AIRCR) then fail, so an AHB-AP
// always gets privileged data access, as in OpenOCD. Other AP types use these bits
// differently (ARPROT and cache attributes on an AXI-AP) and keep them. The IDR is not
// read once the CSW says privileged; after one write the bit stays in the AP.
bool MemAp::ReadCsw(uint8_t ap, uint32_t& csw)
{
  const uint32_t select = static_cast<uint32_t>(ap) << 24;
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    Ack ack = Ack::NO_ACK;
    // 事务先执行、再按它的 ack 判定，见 TxnOk 的注释 / Run the transfer first, judge its
    // ack after; see the comment on TxnOk.
    const auto STEP = [&ack](LibXR::ErrorCode ec) { return TxnOk(ec, ack); };

    bool ok =
        STEP(swd_.SetSelectCached(select, ack)) && STEP(swd_.ApReadTxn(kApCsw, csw, ack));
    if (ok && (csw & kCswHprot1) != 0U)
    {
      return true;
    }
    if (ok)
    {
      uint32_t idr = 0U;
      ok = STEP(swd_.SetSelectCached(select | kSelectBankIdr, ack)) &&
           STEP(swd_.ApReadTxn(kApIdr, idr, ack)) &&
           STEP(swd_.SetSelectCached(select, ack));
      if (ok)
      {
        const uint32_t type = idr & 0xFU;
        const bool mem_ap = ((idr >> 13) & 0xFU) == 8U;
        if (mem_ap && (type == 1U || type == 5U || type == 8U))
        {
          csw |= kCswAhbProt;
        }
        return true;
      }
    }
    ClearSticky();
    swd_.InvalidateSelectCache();
  }
  return false;
}

// 写 CSW：保留原值里 Size 和 AddrInc 以外的位，关闭自增。
bool MemAp::SetSize(ApState& st, uint8_t size, bool increment)
{
  if (!st.valid)
  {
    return false;
  }
  Ack ack = Ack::NO_ACK;
  if (!TxnOk(swd_.SetSelectCached(static_cast<uint32_t>(st.ap) << 24, ack), ack))
  {
    st.written = false;
    return false;
  }
  const uint32_t csw =
      (st.base_csw & ~kCswSizeAndInc) | SizeCode(size) | (increment ? 0x10U : 0U);
  if (st.written && st.written_csw == csw)
  {
    return true;
  }
  if (!TxnOk(swd_.ApWriteTxn(kApCsw, csw, ack), ack))
  {
    st.written = false;
    ClearSticky();
    return false;
  }
  st.written = true;
  st.written_csw = csw;
  return true;
}

uint16_t MemAp::ReadWords(ApState& ap, uint32_t addr, uint16_t count, uint8_t* out)
{
  if (count == 0U || (addr & 3U) != 0U || count > (1024U - (addr & 1023U)) / 4U ||
      !SetSize(ap, 4U, true))
  {
    return 0U;
  }
  Ack ack = Ack::NO_ACK;
  uint32_t value = 0U;
  uint16_t confirmed = 0U;
  if (TxnOk(swd_.ApWriteTxn(kApTar, addr, ack), ack) &&
      TxnOk(swd_.ApReadPostedTxn(kApDrw, value, ack), ack))
  {
    // The first AP read launches word 0; each later AP read returns the previous
    // word and launches the next one. Only RDBUFF drains the final word.
    for (uint16_t i = 1U; i < count; ++i)
    {
      if (!TxnOk(swd_.ApReadPostedTxn(kApDrw, value, ack), ack))
      {
        ap.written = false;
        ClearSticky();
        return confirmed;
      }
      std::memcpy(out + confirmed * 4U, &value, 4U);
      ++confirmed;
    }
    if (TxnOk(swd_.DpReadRdbuffTxn(value, ack), ack))
    {
      std::memcpy(out + confirmed * 4U, &value, 4U);
      return static_cast<uint16_t>(confirmed + 1U);
    }
  }
  ap.written = false;
  ClearSticky();
  return confirmed;
}

bool MemAp::ReadWord(uint32_t addr, uint32_t& val)
{
  Ack ack = Ack::NO_ACK;
  const LibXR::ErrorCode TAR = swd_.ApWriteTxn(kApTar, addr, ack);
  if (!TxnOk(TAR, ack))
  {
    ClearSticky();
    return false;
  }
  const LibXR::ErrorCode DRW = swd_.ApReadTxn(kApDrw, val, ack);
  if (!TxnOk(DRW, ack))
  {
    ClearSticky();
    return false;
  }
  return true;
}

// 写是 posted 的，DP 的 CTRL/STAT 读不会等 AP 事务完成：总线故障来晚时这一次读看不到，
// 于是故障被算到下一个条目上（TAR 写回 FAULT），最后一条恢复写也就发不出去。所以先读一次
// DP RDBUFF 把这次写等到底——它在写完成前只回 WAIT，写失败时回 FAULT——再看 CTRL/STAT 的
// 粘滞错误位。
// The write is posted and a DP CTRL/STAT read does not wait for the AP transaction: a bus
// fault that arrives after that read is missed and lands on the next item (the TAR write
// returns FAULT), so the final recovery write never happens. Read DP RDBUFF first, which
// is held off with WAIT until the posted write completes and answers FAULT when it
// failed, then look at the sticky error bits in CTRL/STAT.
bool MemAp::CheckCtrlStat()
{
  Ack ack = Ack::NO_ACK;
  uint32_t rb = 0U;
  const LibXR::ErrorCode DRAIN = swd_.DpReadRdbuffTxn(rb, ack);
  if (!TxnOk(DRAIN, ack))
  {
    ClearSticky();
    return false;
  }
  uint32_t cs = 0U;
  const LibXR::ErrorCode CS = swd_.DpReadTxn(DpReadReg::CTRL_STAT, cs, ack);
  if (!TxnOk(CS, ack) || (cs & kCtrlStatErrors) != 0U)
  {
    ClearSticky();
    return false;
  }
  return true;
}

bool MemAp::WriteWord(uint32_t addr, uint32_t val)
{
  Ack ack = Ack::NO_ACK;
  const LibXR::ErrorCode TAR = swd_.ApWriteTxn(kApTar, addr, ack);
  if (!TxnOk(TAR, ack))
  {
    ClearSticky();
    return false;
  }
  const LibXR::ErrorCode DRW = swd_.ApWriteTxn(kApDrw, val, ack);
  if (!TxnOk(DRW, ack))
  {
    ClearSticky();
    return false;
  }
  return CheckCtrlStat();
}

// 不清掉粘滞错误，后面的访问会连续失败。
void MemAp::ClearSticky()
{
  Ack ack = Ack::NO_ACK;
  (void)swd_.WriteAbortTxn(kAbortClearAll, ack);
}

bool MemAp::Resolve(const Item& it, uint32_t& addr, uint8_t& st)
{
  uint32_t cur = it.base;
  for (uint8_t i = 0; i < it.n; ++i)
  {
    const uint32_t a = cur + static_cast<uint32_t>(static_cast<int32_t>(it.deref[i]));
    if ((a & 3U) != 0U)
    {
      st = static_cast<uint8_t>(kItemAlignChain + i);
      return false;
    }
    uint32_t p = 0U;
    if (!ReadWord(a, p))
    {
      st = static_cast<uint8_t>(kItemSwdChain + i);
      return false;
    }
    if (p == 0U)
    {
      st = static_cast<uint8_t>(kItemNull + i);
      return false;
    }
    cur = p;
  }
  const uint32_t a = cur + static_cast<uint32_t>(static_cast<int32_t>(it.final_off));
  const uint8_t align = (it.size == 8U) ? 4U : it.size;
  if ((a & (align - 1U)) != 0U)
  {
    st = kItemAlignLast;
    return false;
  }
  addr = a;
  st = kItemOk;
  return true;
}

// 1、2 字节在 DRW 的对应字节通道里；8 字节是两次 32 位读，不是原子的。
uint8_t MemAp::ReadValue(ApState& ap, uint8_t size, uint32_t addr, uint8_t* out)
{
  if (!SetSize(ap, (size == 8U) ? 4U : size))
  {
    return kItemSwdLast;
  }
  uint32_t v = 0U;
  if (!ReadWord(addr, v))
  {
    return kItemSwdLast;
  }
  if (size == 8U)
  {
    uint32_t v2 = 0U;
    if (!ReadWord(addr + 4U, v2))
    {
      return kItemSwdLast;
    }
    std::memcpy(out, &v, 4);
    std::memcpy(out + 4, &v2, 4);
    return kItemOk;
  }
  if (size == 4U)
  {
    std::memcpy(out, &v, 4);
    return kItemOk;
  }
  const uint32_t shifted = v >> ((addr & 3U) * 8U);
  if (size == 1U)
  {
    out[0] = static_cast<uint8_t>(shifted);
  }
  else
  {
    const uint16_t h = static_cast<uint16_t>(shifted);
    std::memcpy(out, &h, 2);
  }
  return kItemOk;
}

// 解引用前 CSW 必须是 32 位访问：上一个条目可能把它改成了 1 或 2 字节。
uint8_t MemAp::ReadItem(ApState& ap, const Item& it, uint8_t* out)
{
  if (it.n != 0U && !SetSize(ap, 4U))
  {
    return kItemSwdLast;
  }
  uint32_t addr = 0U;
  uint8_t st = kItemOk;
  if (!Resolve(it, addr, st))
  {
    return st;
  }
  return ReadValue(ap, it.size, addr, out);
}

uint8_t MemAp::Read(const Item& it, uint8_t* out)
{
  ApState* ap = EnsureAp(it.ap);
  return (ap == nullptr) ? kItemSwdLast : ReadItem(*ap, it, out);
}

uint8_t MemAp::Write(const Item& it, const uint8_t* value)
{
  ApState* ap = EnsureAp(it.ap);
  if (ap == nullptr || (it.n != 0U && !SetSize(*ap, 4U)))
  {
    return kItemSwdLast;
  }
  uint32_t addr = 0U;
  uint8_t st = kItemOk;
  if (!Resolve(it, addr, st))
  {
    return st;
  }
  if (it.size == 8U)
  {
    uint32_t lo = 0U;
    uint32_t hi = 0U;
    std::memcpy(&lo, value, 4);
    std::memcpy(&hi, value + 4, 4);
    if (!SetSize(*ap, 4U) || !WriteWord(addr, lo) || !WriteWord(addr + 4U, hi))
    {
      return kItemSwdLast;
    }
    return kItemOk;
  }
  if (!SetSize(*ap, it.size))
  {
    return kItemSwdLast;
  }
  uint32_t word = 0U;
  const uint32_t lane = (addr & 3U) * 8U;
  if (it.size == 4U)
  {
    std::memcpy(&word, value, 4);
  }
  else if (it.size == 2U)
  {
    uint16_t h = 0U;
    std::memcpy(&h, value, 2);
    word = static_cast<uint32_t>(h) << lane;
  }
  else
  {
    word = static_cast<uint32_t>(value[0]) << lane;
  }
  return WriteWord(addr, word) ? kItemOk : kItemSwdLast;
}

}  // namespace XrDap::Vendor
