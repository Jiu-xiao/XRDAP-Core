// SPDX-License-Identifier: Apache-2.0
// MEM-AP 经 SWD 的读写（docs/protocol.md 3.6）。写是 posted 的，总线故障可能要到下一次
// AP 访问才回来，而 DP CTRL/STAT 的读并不等 AP 事务：故障报在写它的那个条目
// 上，下一个条目照常写进去。突发读里，每个字要等后面一次读才确认。
//
// MEM-AP reads and writes over SWD (docs/protocol.md 3.6). A write is posted, so a bus
// fault may only come back on the next AP access, while a DP CTRL/STAT read does not wait
// for the AP transaction: the failure is reported on the item that wrote it and
// the next item is still written. In a burst read, each word is confirmed by the read
// after it.
#include "xrdap/vendor/mem_ap.hpp"

#include <cstdint>

#include "fixture.hpp"
#include "xrdap/vendor/protocol.hpp"

namespace
{
using XrDap::Vendor::Get32;
using XrDap::Vendor::Item;
using XrDap::Vendor::MemAp;
using XrDapTest::ModelSwd;
using XrDapTest::WordAt;

// 条目状态（docs/protocol.md 3.2）/ Item status (docs/protocol.md 3.2).
constexpr uint8_t kItemOk = 0x00U;
constexpr uint8_t kItemSwdFailed = 0x1FU;

constexpr uint32_t kAddr = 0x08000000U;

struct Target
{
  ModelSwd swd;
  MemAp mem{swd};
  MemAp::Access access{mem};

  uint32_t Read(uint32_t addr, uint8_t ap = 0U)
  {
    uint8_t bytes[4] = {};
    CHECK_EQ(mem.Read(WordAt(addr, ap), bytes), kItemOk);
    return Get32(bytes);
  }
};

// 突发读用的 AP 状态：和采样一样，不经过命令的 CSW 缓存 / The AP state of a burst read:
// like sampling's, apart from the command's CSW cache.
struct BurstTarget : Target
{
  MemAp::ApState ap;
  uint8_t values[12] = {};

  BurstTarget()
  {
    ap.ap = 0U;
    ap.valid = true;
    ap.base_csw = 0x23000002U;
  }
  uint16_t ReadThree() { return mem.ReadWords(ap, kAddr, 3U, values); }
};

using Injection = void (*)(ModelSwd&);
void NoFault(ModelSwd&) {}
void FaultOnFirstRead(ModelSwd& swd) { swd.fail_drw = 1U; }
void BadParityOnRdbuff(ModelSwd& swd) { swd.parity_rdbuff = true; }

// 确认的字数对，确认的值对，每个地址只读一次 / The right number of words is confirmed,
// their values are right, and each address is read once.
void CheckBurst(Injection inject, unsigned confirmed)
{
  BurstTarget t;
  inject(t.swd);
  CHECK_EQ(t.ReadThree(), confirmed);
  CHECK_EQ(t.swd.tar_writes, 1U);
  CHECK(t.swd.drw_attempts <= 3U);
  for (unsigned i = 0U; i < confirmed; ++i)
  {
    CHECK_EQ(Get32(t.values + i * 4U), ModelSwd::Value(0U, kAddr + i * 4U));
  }
  for (unsigned i = 0U; i < t.swd.addresses.size(); ++i)
  {
    CHECK_EQ(t.swd.addresses[i], kAddr + i * 4U);
  }
}

// 一条命令读过 CSW，一次突发之后的下一条命令又读了几次 / A command read the CSW; how many
// reads there are in all after a burst and the next command.
unsigned CswReadsAfterBurst(Injection inject)
{
  BurstTarget t;
  t.mem.ResetCache();
  CHECK(t.mem.EnsureAp(0U) != nullptr);
  inject(t.swd);
  (void)t.ReadThree();
  t.swd.parity_rdbuff = false;
  t.mem.ResetCache(true);
  CHECK(t.mem.EnsureAp(0U) != nullptr);
  return t.swd.csw_reads;
}

}  // namespace

// 控制组：普通写要成功且值落地 / Control: a plain write succeeds and lands.
TEST(WriteToGoodAddressSucceeds)
{
  Target t;
  const uint8_t value[4] = {0x2A, 0x00, 0x00, 0x00};
  CHECK_EQ(t.mem.Write(WordAt(0x20000000U), value), kItemOk);
  CHECK_EQ(t.swd.Written(0x20000000U), 0x0000002AU);
}

// 写到会出总线故障的地址，故障必须报在这个条目上，下一个条目要照常写进去 / Writing to an
// address that faults must report it on that item and leave the next item working.
TEST(WriteFaultIsReportedOnTheItemThatWroteIt)
{
  Target t;
  const uint8_t v1[4] = {0x11, 0x11, 0x11, 0x11};
  CHECK_EQ(t.mem.Write(WordAt(ModelSwd::kFaultAddr), v1), kItemSwdFailed);
  const uint8_t v2[4] = {0x22, 0x22, 0x22, 0x22};
  CHECK_EQ(t.mem.Write(WordAt(0x20000000U), v2), kItemOk);
  CHECK_EQ(t.swd.Written(0x20000000U), 0x22222222U);
}

// 写之后查到粘滞错误：这个条目失败，ABORT 清掉错误，下一个条目不受影响（docs/protocol.md
// 3.6）/ A sticky error found after a write: this item fails, ABORT clears the error, and
// the next item is not affected (docs/protocol.md 3.6).
TEST(StickyErrorAfterAWriteIsReportedAndCleared)
{
  Target t;
  const uint8_t value[4] = {0x33, 0x33, 0x33, 0x33};
  t.swd.ctrl_stat = 1U << 7;  // WDATAERR
  CHECK_EQ(t.mem.Write(WordAt(0x20000000U), value), kItemSwdFailed);
  CHECK_EQ(t.swd.ctrl_stat, 0U);
  CHECK_EQ(t.mem.Write(WordAt(0x20000004U), value), kItemOk);
}

// AHB-AP 的 CSW 没有特权位时，读 IDR 后补上，之后的写照常落地（docs/protocol.md 3.6）/ An
// AHB-AP whose CSW lacks the privileged bit gets it after the IDR read, and the write
// after that lands as usual (docs/protocol.md 3.6).
TEST(UnprivilegedAhbApGetsPrivilegedProt)
{
  Target t;
  t.swd.csw[0] = 0x00000002U;
  const uint8_t value[4] = {0x44, 0x44, 0x44, 0x44};
  CHECK_EQ(t.mem.Write(WordAt(0x20000000U), value), kItemOk);
  CHECK_EQ(t.swd.csw[0] & 0x23000000U, 0x23000000U);
  CHECK_EQ(t.swd.Written(0x20000000U), 0x44444444U);
}

TEST(OtherApTypesKeepTheirProt)
{
  Target t;
  t.swd.csw[0] = 0x00000002U;
  t.swd.idr = (8U << 13) | 2U;  // APB-AP
  const uint8_t value[4] = {0x55, 0x55, 0x55, 0x55};
  CHECK_EQ(t.mem.Write(WordAt(0x20000000U), value), kItemOk);
  CHECK_EQ(t.swd.csw[0] & 0x7F000000U, 0U);
}

// 第三次读复用 AP 0 的状态，只读两次 CSW；没有失败也就没有恢复 / The third read reuses AP
// 0's state, so the CSW is read twice; nothing failed, so nothing is recovered.
TEST(ReadsReachTheSelectedAp)
{
  Target t;
  CHECK_EQ(t.Read(kAddr, 0U), ModelSwd::Value(0U, kAddr));
  CHECK_EQ(t.Read(kAddr, 1U), ModelSwd::Value(1U, kAddr));
  CHECK_EQ(t.Read(kAddr, 0U), ModelSwd::Value(0U, kAddr));
  CHECK_EQ(t.swd.csw_reads, 2U);
  CHECK_EQ(t.swd.resets, 0U);
  CHECK_EQ(t.swd.enters, 0U);
}

TEST(BurstWithoutAFaultConfirmsEveryWord) { CheckBurst(NoFault, 3U); }

TEST(BurstFaultOnTheFirstReadConfirmsNothing) { CheckBurst(FaultOnFirstRead, 0U); }

// 第二次读才带回第一个字，它失败了第一个字也就没有确认 / The second read is what brings
// the first word back, so when it fails that word is not confirmed.
TEST(BurstFaultOnTheSecondReadConfirmsNothing)
{
  CheckBurst([](ModelSwd& swd) { swd.fail_drw = 2U; }, 0U);
}

TEST(BurstFaultOnTheThirdReadConfirmsOneWord)
{
  CheckBurst([](ModelSwd& swd) { swd.fail_drw = 3U; }, 1U);
}

TEST(BurstBadParityOnTheFirstReadConfirmsNothing)
{
  CheckBurst([](ModelSwd& swd) { swd.parity_drw = 1U; }, 0U);
}

TEST(BurstBadParityOnTheSecondReadConfirmsNothing)
{
  CheckBurst([](ModelSwd& swd) { swd.parity_drw = 2U; }, 0U);
}

// 最后一个字只有 RDBUFF 带回来 / Only RDBUFF brings the last word back.
TEST(BurstBadParityOnRdbuffConfirmsTwoWords) { CheckBurst(BadParityOnRdbuff, 2U); }

TEST(CleanBurstKeepsTheCswForReuse) { CHECK_EQ(CswReadsAfterBurst(NoFault), 1U); }

TEST(FaultInABurstForbidsCswReuse) { CHECK_EQ(CswReadsAfterBurst(FaultOnFirstRead), 2U); }

TEST(BadParityInABurstForbidsCswReuse)
{
  CHECK_EQ(CswReadsAfterBurst(BadParityOnRdbuff), 2U);
}

TEST(SingleReadAfterABurstTurnsAddrIncOff)
{
  BurstTarget t;
  (void)t.ReadThree();
  uint8_t value[4] = {};
  CHECK_EQ(t.mem.ReadItem(t.ap, WordAt(kAddr + 16U), value), kItemOk);
  CHECK_EQ(Get32(value), ModelSwd::Value(0U, kAddr + 16U));
  CHECK_EQ(t.swd.csw[0] & 0x30U, 0U);
}

// TAR 的自增只保证在 1 KiB 之内（ADIv5）：跨过去的突发不碰目标 / TAR increment only holds
// within 1 KiB (ADIv5): a burst across that boundary does not touch the target.
TEST(BurstRefusesToCrossThe1KiBWindow)
{
  BurstTarget t;
  CHECK_EQ(t.mem.ReadWords(t.ap, kAddr + 0x3FCU, 2U, t.values), 0U);
  CHECK_EQ(t.swd.memory_reads.load(), 0U);
}

TEST(BurstRefusesAnUnalignedAddress)
{
  BurstTarget t;
  CHECK_EQ(t.mem.ReadWords(t.ap, kAddr + 1U, 2U, t.values), 0U);
  CHECK_EQ(t.swd.memory_reads.load(), 0U);
}

// 复用的是 CSW，不是目标数据 / What is reused is the CSW, not target data.
TEST(ReusedCswStillReadsFreshData)
{
  Target t;
  t.mem.ResetCache();
  CHECK_EQ(t.Read(kAddr), ModelSwd::Value(0U, kAddr));
  t.swd.value_bias = 7U;
  t.mem.ResetCache(true);
  CHECK_EQ(t.Read(kAddr), ModelSwd::Value(0U, kAddr) + 7U);
  CHECK_EQ(t.swd.csw_reads, 1U);
  CHECK_EQ(t.swd.memory_reads.load(), 2U);
}

// 一次采样突发在命令的 AP 状态背后打开了 AddrInc，复用时要重新写 / A sampling burst turns
// AddrInc on behind the command's AP state, so reuse must program it again.
TEST(CswReuseStillProgramsTheAccessMode)
{
  BurstTarget t;
  t.mem.ResetCache();
  (void)t.Read(kAddr);
  CHECK_EQ(t.mem.ReadWords(t.ap, kAddr, 2U, t.values), 2U);
  CHECK_EQ(t.swd.csw[0] & 0x30U, 0x10U);
  t.mem.ResetCache(true);
  CHECK_EQ(t.Read(kAddr), ModelSwd::Value(0U, kAddr));
  CHECK_EQ(t.swd.csw[0] & 0x37U, 2U);
  CHECK_EQ(t.swd.csw_reads, 1U);
}

TEST(CswIsReusedForEachAp)
{
  Target t;
  t.mem.ResetCache();
  (void)t.Read(kAddr, 0U);
  (void)t.Read(kAddr, 1U);
  CHECK_EQ(t.swd.csw_reads, 2U);
  t.mem.ResetCache(true);
  CHECK_EQ(t.Read(kAddr, 0U), ModelSwd::Value(0U, kAddr));
  CHECK_EQ(t.Read(kAddr, 1U), ModelSwd::Value(1U, kAddr));
  CHECK_EQ(t.swd.csw_reads, 2U);
}

// 失败的读不重放，下一条命令重新读 CSW / A failed read is not replayed, and the next
// command reads the CSW again.
TEST(FailedReadForbidsCswReuse)
{
  Target t;
  t.mem.ResetCache();
  (void)t.Read(kAddr);
  t.swd.fail_drw = t.swd.drw_attempts + 1U;
  const unsigned reads = t.swd.memory_reads;
  uint8_t value[4] = {};
  CHECK_EQ(t.mem.Read(WordAt(kAddr), value), kItemSwdFailed);
  CHECK_EQ(t.swd.memory_reads.load(), reads);
  t.mem.ResetCache(true);
  CHECK_EQ(t.Read(kAddr), ModelSwd::Value(0U, kAddr));
  CHECK_EQ(t.swd.csw_reads, 2U);
}

TEST(ResyncForbidsCswReuse)
{
  Target t;
  t.mem.ResetCache();
  (void)t.Read(kAddr);
  CHECK(t.mem.Resync());
  t.mem.ResetCache(true);
  CHECK_EQ(t.Read(kAddr), ModelSwd::Value(0U, kAddr));
  CHECK_EQ(t.swd.csw_reads, 2U);
}

TEST(FailedCswDiscoveryDoesNotPoisonLaterCommands)
{
  Target t;
  t.swd.missing = true;
  t.mem.ResetCache();
  uint8_t value[4] = {};
  CHECK_EQ(t.mem.Read(WordAt(kAddr), value), kItemSwdFailed);
  t.swd.missing = false;
  t.mem.ResetCache(true);
  CHECK_EQ(t.Read(kAddr), ModelSwd::Value(0U, kAddr));
}
