// SPDX-License-Identifier: Apache-2.0
// 回复空间。判据：用户决定 B8（统一检查剩余空间，不够回 0x03，剩 1 字节时不能越界）、
// docs/protocol.md 3.2 的回复长度和 3.8 的字节示例、tests/SPEC.md VND-02 的 355 字节 /
// Reply space. Oracles: user decision B8 (one check of the remaining space, 0x03 when
// short, no overrun with 1 byte left), the reply sizes of docs/protocol.md 3.2 with the
// byte examples of 3.8, and the 355 bytes of tests/SPEC.md VND-02.
#include <vector>

#include "check.hpp"
#include "xrdap/vendor/protocol.hpp"

using XrDap::Vendor::RequiredReply;
using XrDap::Vendor::ReserveReply;

namespace
{
using Bytes = std::vector<uint8_t>;

// docs/protocol.md 3.8 “READ，一个条目”，回复 81 00 01 0100 00 78563412 /
// docs/protocol.md 3.8 "READ, one item", answered by 81 00 01 0100 00 78563412.
const Bytes kReadExample = {0x81, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00,
                            0x00, 0x20, 0x01, 0x08, 0x00, 0x04, 0x00};

// READ 的一个条目，不解引用 / One READ item without dereference.
void PutItem(Bytes& r, uint8_t size)
{
  const uint8_t item[] = {0x00, 0x01, 0x00, size, 0x00, 0x00,
                          0x00, 0x20, 0x00, 0x00, 0x00};
  r.insert(r.end(), item, item + sizeof(item));
}

uint16_t Required(const Bytes& r)
{
  return RequiredReply(r.data(), static_cast<uint16_t>(r.size()));
}

// count 个 size 字节条目的 READ / A READ of count items of size bytes.
Bytes Read(uint8_t count, uint8_t size)
{
  Bytes r = {0x81, count};
  for (uint8_t i = 0; i < count; ++i)
  {
    PutItem(r, size);
  }
  return r;
}
}  // namespace

TEST(ReadReplySizeCountsEveryItem)
{
  CHECK_EQ(Required(kReadExample), 10U);
  // 3 + Σ(3 + size) = 3 + 7 + 11 / 3 + the sum of (3 + size).
  Bytes r = {0x81, 0x02};
  PutItem(r, 4);
  PutItem(r, 8);
  CHECK_EQ(Required(r), 21U);
}

TEST(LargestReadNeeds355Bytes) { CHECK_EQ(Required(Read(32, 8)), 355U); }

TEST(WriteReplySizeCountsEveryItem)
{
  // 3.8 “WRITE，一个条目”的条目放两次，回复 3 + 3 × 2 / The item of 3.8 "WRITE, one item"
  // twice, answered by 3 + 3 x 2.
  const Bytes r = {0x82, 0x02, 0x00, 0x02, 0x00, 0x02, 0x10, 0x00, 0x00, 0x20,
                   0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x02, 0x00, 0x02, 0x10,
                   0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x34, 0x12};
  CHECK_EQ(Required(r), 9U);
}

TEST(FixedReplySizes)
{
  CHECK_EQ(Required({0x80}), 22U);
  CHECK_EQ(Required({0x83}), 2U);
  CHECK_EQ(Required({0x84, 0x00}), 2U);
  CHECK_EQ(Required({0x85, 0x00, 0x04}), 24U);
}

// 整体状态不为 0 时响应只有两个字节 / A non-zero overall status makes a two-byte reply.
TEST(MalformedRequestNeedsStatusOnly)
{
  Bytes truncated = kReadExample;
  truncated.pop_back();
  CHECK_EQ(Required(truncated), 2U);
  CHECK_EQ(Required({0x81, 0x00}), 2U);  // 计数为 0 / count 0
  CHECK_EQ(Required(Read(33, 4)), 2U);   // 超过 32 个 / above 32 items
  CHECK_EQ(Required(Read(1, 3)), 2U);    // size 不合法 / bad size
  Bytes trailing = Read(1, 4);
  trailing.push_back(0x00);
  CHECK_EQ(Required(trailing), 2U);
}

TEST(OneByteLeftWritesOnlyFF)
{
  uint8_t resp[4] = {0xAA, 0xAA, 0xAA, 0xAA};
  uint16_t out = 9;
  CHECK(!ReserveReply(0x81, 10, resp, 1, out));
  CHECK_EQ(out, 1U);
  CHECK_EQ(resp[0], 0xFFU);
  CHECK_EQ(resp[1], 0xAAU);
}

TEST(ShortSpaceAnswersResource)
{
  // 从正好 2 字节到差 1 字节 / From exactly 2 bytes up to 1 byte short.
  for (const uint16_t cap : {2U, 5U, 9U})
  {
    uint8_t resp[4] = {};
    uint16_t out = 0;
    CHECK(!ReserveReply(0x81, 10, resp, cap, out));
    CHECK_EQ(out, 2U);
    CHECK_EQ(resp[0], 0x81U);
    CHECK_EQ(resp[1], 0x03U);
  }
}

TEST(EnoughSpaceRunsAndWritesNothing)
{
  uint8_t resp[4] = {0xAA, 0xAA, 0xAA, 0xAA};
  uint16_t out = 0;
  CHECK(ReserveReply(0x81, 10, resp, 10, out));
  CHECK_EQ(out, 0U);
  CHECK_EQ(resp[0], 0xAAU);
}

TEST(NoSpaceNoReply)
{
  uint8_t resp[1] = {0xAA};
  uint16_t out = 5;
  CHECK(!ReserveReply(0x81, 2, resp, 0, out));
  CHECK_EQ(out, 0U);
  CHECK_EQ(resp[0], 0xAAU);
}
