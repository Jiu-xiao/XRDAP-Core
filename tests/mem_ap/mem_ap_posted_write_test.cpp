// SPDX-License-Identifier: Apache-2.0
// MEM-AP 的写（V305-2）：写是 posted 的，总线故障可能要到下一次 AP 访问才回来，而 DP
// CTRL/STAT 的读并不等 AP 事务。期望：故障报在写它的那个条目上（0x1F），下一个条目照常写
// 进去（0x00 且值落地）。CSW 的 Prot（docs/protocol.md 3.6）在同一条路上。
//
// MEM-AP writes (V305-2): a write is posted, so a bus fault may only come back on the
// next AP access, while a DP CTRL/STAT read does not wait for the AP transaction. The
// failure must be reported on the item that wrote it (0x1F) and the next item must still
// be written (0x00 with the value in memory). The CSW's Prot (docs/protocol.md 3.6) is on
// the same path.
#include <cstdint>

#include "../threads/model_swd.hpp"
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/protocol.hpp"

namespace
{
using XrDap::Vendor::Item;
using XrDap::Vendor::MemAp;

// 条目状态（docs/protocol.md 3.2）/ Item status (docs/protocol.md 3.2).
constexpr uint8_t kItemOk = 0x00U;
constexpr uint8_t kItemSwdFailed = 0x1FU;

Item WordAt(uint32_t addr)
{
  Item it;
  it.size = 4U;
  it.base = addr;
  return it;
}

struct Target
{
  ModelSwd swd;
  MemAp mem{swd};
  MemAp::Access access{mem};
};
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

// AHB-AP 的 CSW 没有特权位时，读 IDR 后补上，读完把 SELECT 改回 bank 0，值才写得进去 / An
// AHB-AP whose CSW lacks the privileged bit gets it after the IDR read, and SELECT goes
// back to bank 0, or the value would not land.
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
