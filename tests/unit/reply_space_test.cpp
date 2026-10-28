// SPDX-License-Identifier: Apache-2.0
// 回复空间。判据：用户决定 B8（统一检查剩余空间，不够回 0x03，剩 1 字节时不能越界）、
// docs/protocol.md 3.2 和 3.4 的回复格式 / Reply space. Oracles: user decision B8 (one
// check of the remaining space, 0x03 when short, no overrun with 1 byte left) and the
// reply formats of docs/protocol.md 3.2 and 3.4.
#include "check.hpp"
#include "xrdap/vendor/protocol.hpp"

using XrDap::Vendor::RequiredReply;
using XrDap::Vendor::ReserveReply;

namespace
{
// READ 一个条目：ap 0、id 1、size 4、base 0x20000000、不解引用、final 0
// (docs/protocol.md 3.8 “READ，一个条目”) / READ with one item: ap 0, id 1, size 4,
// base 0x20000000, no dereference, final 0 (docs/protocol.md 3.8 "READ, one item").
const uint8_t kRead1[] = {0x81, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00,
                          0x00, 0x00, 0x20, 0x00, 0x00, 0x00};
}  // namespace

TEST(ReadReplySizeCountsEveryItem)
{
  // 3 字节头 + 每条目 id(2) + 状态(1) + 值(size) / 3-byte head + per item id(2) +
  // status(1) + value(size).
  CHECK_EQ(RequiredReply(kRead1, sizeof(kRead1)), 3U + 3U + 4U);
}

TEST(WriteReplySize)
{
  // WRITE 一个条目 4 字节值 / WRITE, one item with a 4-byte value.
  const uint8_t req[] = {0x82, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
                         0x20, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12};
  CHECK_EQ(RequiredReply(req, sizeof(req)), 3U + 3U);
}

TEST(MalformedRequestNeedsStatusOnly)
{
  CHECK_EQ(RequiredReply(kRead1, 5), 2U);  // 截断的条目 / truncated item
  const uint8_t ctrl[] = {0x84, 0x00};
  CHECK_EQ(RequiredReply(ctrl, sizeof(ctrl)), 2U);
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
  uint8_t resp[4] = {};
  uint16_t out = 0;
  CHECK(!ReserveReply(0x81, 10, resp, 9, out));
  CHECK_EQ(out, 2U);
  CHECK_EQ(resp[0], 0x81U);
  CHECK_EQ(resp[1], 0x03U);
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
