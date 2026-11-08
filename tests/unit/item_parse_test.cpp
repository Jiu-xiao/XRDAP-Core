// SPDX-License-Identifier: Apache-2.0
// 条目解析。判据：docs/protocol.md 3.3 的条目布局（11 + 2n 字节，n 不超过 8，偏移是有
// 符号 16 位）/ Item parsing. Oracle: the item layout of docs/protocol.md 3.3 (11 + 2n
// bytes, n at most 8, offsets are signed 16-bit).
#include <vector>

#include "check.hpp"
#include "xrdap/vendor/protocol.hpp"

using namespace XrDap::Vendor;

namespace
{
// ap 1、id 0x1234、size 2、base 0x20001000、n 2（+4、-8）、final +6 / ap 1, id 0x1234,
// size 2, base 0x20001000, n 2 (+4, -8), final +6.
const uint8_t kChainItem[] = {0x01, 0x34, 0x12, 0x02, 0x00, 0x10, 0x00, 0x20,
                              0x02, 0x04, 0x00, 0xF8, 0xFF, 0x06, 0x00};
// ap 0、id 1、size 4、base 0x20000000、n 0、final 0、div 5、phase 3 / ap 0, id 1, size 4,
// base 0x20000000, n 0, final 0, div 5, phase 3.
const uint8_t kSamplingItem[] = {0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x20,
                                 0x00, 0x00, 0x00, 0x05, 0x00, 0x03, 0x00};

// 解引用 n 次的条目，第 i 级偏移是 i + 1，final 是 0x7F / An item with n dereferences,
// the offset at level i is i + 1, final is 0x7F.
std::vector<uint8_t> ItemOfDepth(uint8_t n)
{
  std::vector<uint8_t> b = {0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x20, n};
  for (uint8_t i = 0; i < n; ++i)
  {
    b.push_back(static_cast<uint8_t>(i + 1U));
    b.push_back(0x00);
  }
  b.push_back(0x7F);
  b.push_back(0x00);
  return b;
}
}  // namespace

TEST(ItemWithPointerChain)
{
  const uint8_t* p = kChainItem;
  Item it;
  CHECK(ParseItem(p, kChainItem + sizeof(kChainItem), it, nullptr));
  CHECK(p == kChainItem + sizeof(kChainItem));
  CHECK_EQ(it.ap, 1U);
  CHECK_EQ(it.id, 0x1234U);
  CHECK_EQ(it.size, 2U);
  CHECK_EQ(it.base, 0x20001000U);
  CHECK_EQ(it.n, 2U);
  CHECK_EQ(it.deref[0], 4);
  CHECK_EQ(it.deref[1], -8);
  CHECK_EQ(it.final_off, 6);
}

TEST(TruncatedItemIsRejected)
{
  for (size_t n = 0; n < sizeof(kChainItem); ++n)
  {
    const uint8_t* p = kChainItem;
    Item it;
    CHECK(!ParseItem(p, kChainItem + n, it, nullptr));
  }
}

TEST(TruncatedSamplingItemIsRejected)
{
  for (size_t n = 0; n < sizeof(kSamplingItem); ++n)
  {
    const uint8_t* p = kSamplingItem;
    Item it;
    SubItem sub;
    CHECK(!ParseItem(p, kSamplingItem + n, it, &sub));
  }
}

TEST(DepthEightIsTheLimit)
{
  const auto eight = ItemOfDepth(8);
  const uint8_t* p = eight.data();
  Item it;
  CHECK(ParseItem(p, eight.data() + eight.size(), it, nullptr));
  CHECK(p == eight.data() + eight.size());
  CHECK_EQ(it.n, 8U);
  CHECK_EQ(it.deref[7], 8);
  CHECK_EQ(it.final_off, 0x7F);

  const auto nine = ItemOfDepth(9);
  p = nine.data();
  CHECK(!ParseItem(p, nine.data() + nine.size(), it, nullptr));
}

TEST(AccessSizes)
{
  CHECK(SizeValid(1) && SizeValid(2) && SizeValid(4) && SizeValid(8));
  CHECK(!SizeValid(0) && !SizeValid(3) && !SizeValid(16));
}
