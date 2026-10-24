// SPDX-License-Identifier: Apache-2.0
// 条目解析。判据：docs/protocol.md 3.3 的条目布局（11 + 2n 字节，n 不超过 8，偏移是有
// 符号 16 位）/ Item parsing. Oracle: the item layout of docs/protocol.md 3.3 (11 + 2n
// bytes, n at most 8, offsets are signed 16-bit).
#include <vector>

#include "check.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

using namespace XrDap::Vendor;

TEST(ItemWithPointerChain)
{
  // ap 1、id 0x1234、size 2、base 0x20001000、n 2（+4、-8）、final +6 / ap 1, id 0x1234,
  // size 2, base 0x20001000, n 2 (+4, -8), final +6.
  const uint8_t b[] = {0x01, 0x34, 0x12, 0x02, 0x00, 0x10, 0x00, 0x20,
                       0x02, 0x04, 0x00, 0xF8, 0xFF, 0x06, 0x00};
  const uint8_t* p = b;
  Item it;
  CHECK(ParseItem(p, b + sizeof(b), it, nullptr));
  CHECK(p == b + sizeof(b));
  CHECK_EQ(it.ap, 1U);
  CHECK_EQ(it.id, 0x1234U);
  CHECK_EQ(it.size, 2U);
  CHECK_EQ(it.base, 0x20001000U);
  CHECK_EQ(it.n, 2U);
  CHECK_EQ(it.deref[0], 4);
  CHECK_EQ(it.deref[1], -8);
  CHECK_EQ(it.final_off, 6);
}

TEST(SamplingItemCarriesDivAndPhase)
{
  const uint8_t b[] = {0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x20,
                       0x00, 0x00, 0x00, 0x05, 0x00, 0x03, 0x00};
  const uint8_t* p = b;
  Item it;
  SubItem sub;
  CHECK(ParseItem(p, b + sizeof(b), it, &sub));
  CHECK_EQ(sub.div, 5U);
  CHECK_EQ(sub.phase, 3U);
  CHECK_EQ(sub.item.base, 0x20000000U);
}

TEST(TruncatedOrTooDeepIsRejected)
{
  const uint8_t b[] = {0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00};
  for (size_t n = 0; n < sizeof(b); ++n)
  {
    const uint8_t* p = b;
    Item it;
    CHECK(!ParseItem(p, b + n, it, nullptr));
  }
  uint8_t deep[11 + 2 * 9] = {0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x20, 9};
  const uint8_t* p = deep;
  Item it;
  CHECK(!ParseItem(p, deep + sizeof(deep), it, nullptr));
}

TEST(AccessSizes)
{
  CHECK(SizeValid(1) && SizeValid(2) && SizeValid(4) && SizeValid(8));
  CHECK(!SizeValid(0) && !SizeValid(3) && !SizeValid(16));
}
