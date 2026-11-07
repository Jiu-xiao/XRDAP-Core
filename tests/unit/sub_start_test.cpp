// SPDX-License-Identifier: Apache-2.0
// SUB_START 请求检查。判据：用户的决定（不估计耗时，SUB_SET 并入开始命令）和
// docs/protocol.md 3.2、3.4 的状态规则 / SUB_START request check. Oracles: a user
// decision (no time estimate, SUB_SET merged into the start command) and the status
// rules of docs/protocol.md 3.2 and 3.4.
#include <vector>

#include "check.hpp"
#include "vendor_item.hpp"
#include "xrdap/vendor/protocol.hpp"

using namespace XrDap::Vendor;

namespace
{
// 文档 3.2 的整体状态 / The overall statuses of 3.2.
constexpr uint8_t kOk = 0x00U;
constexpr uint8_t kBadArgument = 0x01U;
constexpr uint8_t kNoResource = 0x03U;

// 一个采样条目：条目后面跟 div、phase / One sampling item: the item, then div and phase.
void AddItem(std::vector<uint8_t>& r, uint8_t ap, uint8_t size, uint16_t div = 1,
             uint16_t phase = 0)
{
  PutItem(r, size, ap);
  r.insert(r.end(), {static_cast<uint8_t>(div), static_cast<uint8_t>(div >> 8),
                     static_cast<uint8_t>(phase), static_cast<uint8_t>(phase >> 8)});
}

std::vector<uint8_t> Head(uint32_t period, uint8_t flags, uint16_t count)
{
  return {0x83,
          static_cast<uint8_t>(period),
          static_cast<uint8_t>(period >> 8),
          static_cast<uint8_t>(period >> 16),
          static_cast<uint8_t>(period >> 24),
          flags,
          static_cast<uint8_t>(count),
          static_cast<uint8_t>(count >> 8)};
}

// count 个 size 字节条目，都在 AP 0 上 / count items of size bytes, all on AP 0.
std::vector<uint8_t> SameItems(uint16_t count, uint8_t size)
{
  auto r = Head(1000, 0, count);
  for (uint16_t i = 0; i < count; ++i)
  {
    AddItem(r, 0, size);
  }
  return r;
}

uint8_t Parse(const std::vector<uint8_t>& r, SubStartRequest& out)
{
  return ParseSubStart(r.data(), static_cast<uint16_t>(r.size()), out);
}
}  // namespace

TEST(ValidRequest)
{
  auto r = Head(1000, 0x01, 2);
  AddItem(r, 0, 4);
  AddItem(r, 0, 2, 4, 3);
  SubStartRequest req;
  CHECK_EQ(Parse(r, req), kOk);
  CHECK_EQ(req.period_us, 1000U);
  CHECK(req.stop_on_full);
  CHECK(!req.compact);
  CHECK_EQ(req.items.count, 2U);
  std::vector<SubItem> items;
  req.items.ForEach([&items](const SubItem& sub) { items.push_back(sub); });
  CHECK_EQ(items.size(), 2U);
  CHECK(items.size() == 2U && items[0].item.size == 4U && items[0].div == 1U &&
        items[0].phase == 0U);
  CHECK(items.size() == 2U && items[1].item.size == 2U && items[1].div == 4U &&
        items[1].phase == 3U);
}

// flags bit1 选紧凑帧（docs/protocol.md 3.4、3.5）/ flags bit1 selects compact frames
// (docs/protocol.md 3.4, 3.5).
TEST(CompactFlag)
{
  auto r = Head(1000, 0x02, 1);
  AddItem(r, 0, 4);
  SubStartRequest req;
  CHECK_EQ(Parse(r, req), kOk);
  CHECK(req.compact);
  CHECK(!req.stop_on_full);
}

TEST(ReservedFlagBitsAreArgumentErrors)
{
  SubStartRequest req;
  for (int bit = 2; bit < 8; ++bit)
  {
    auto r = Head(1000, static_cast<uint8_t>(1U << bit), 1);
    AddItem(r, 0, 4);
    CHECK_EQ(Parse(r, req), kBadArgument);
  }
}

TEST(BadFieldsAreArgumentErrors)
{
  SubStartRequest req;
  auto zero_period = Head(0, 0, 1);
  AddItem(zero_period, 0, 4);
  CHECK_EQ(Parse(zero_period, req), kBadArgument);
  CHECK_EQ(Parse(Head(1000, 0, 0), req), kBadArgument);  // 计数为 0 / count 0
  auto bad_size = Head(1000, 0, 1);
  AddItem(bad_size, 0, 3);
  CHECK_EQ(Parse(bad_size, req), kBadArgument);
  auto zero_div = Head(1000, 0, 1);
  AddItem(zero_div, 0, 4, 0, 0);
  CHECK_EQ(Parse(zero_div, req), kBadArgument);
  auto phase_ge_div = Head(1000, 0, 1);
  AddItem(phase_ge_div, 0, 4, 2, 2);
  CHECK_EQ(Parse(phase_ge_div, req), kBadArgument);
}

TEST(LengthNotMatchingTheCountIsAnArgumentError)
{
  SubStartRequest req;
  auto trailing = Head(1000, 0, 1);
  AddItem(trailing, 0, 4);
  trailing.push_back(0);
  CHECK_EQ(Parse(trailing, req), kBadArgument);
  auto missing = Head(1000, 0, 2);
  AddItem(missing, 0, 4);
  CHECK_EQ(Parse(missing, req), kBadArgument);
  const auto head = Head(1000, 0, 1);
  const std::vector<uint8_t> short_head(head.begin(), head.begin() + 7);
  CHECK_EQ(Parse(short_head, req), kBadArgument);
}

TEST(ItemLimitIsThirtyTwo)
{
  SubStartRequest req;
  CHECK_EQ(Parse(SameItems(32, 4), req), kOk);
  CHECK_EQ(Parse(SameItems(33, 4), req), kNoResource);
}

TEST(ApLimitIsEight)
{
  SubStartRequest req;
  auto eight = Head(1000, 0, 8);
  for (uint8_t ap = 0; ap < 8U; ++ap)
  {
    AddItem(eight, ap, 1);
  }
  CHECK_EQ(Parse(eight, req), kOk);
  auto nine = Head(1000, 0, 9);
  for (uint8_t ap = 0; ap < 9U; ++ap)
  {
    AddItem(nine, ap, 1);
  }
  CHECK_EQ(Parse(nine, req), kNoResource);
}

TEST(RepeatedApIsCountedOnce)
{
  // 9 个条目，8 个不同的 AP / 9 items on 8 different APs.
  SubStartRequest req;
  auto r = Head(1000, 0, 9);
  for (uint8_t i = 0; i < 9U; ++i)
  {
    AddItem(r, i % 8U, 1);
  }
  CHECK_EQ(Parse(r, req), kOk);
}

TEST(FrameMustFitTheRingWithItsLengthHead)
{
  // 测试用缓冲 256 字节。30 个 8 字节条目：头 10 + 位图 4 + 值 240 + 长度头 2 =
  // 256，放得下； 再加 1 个 1 字节条目多出 1 字节 / The test ring is 256 bytes. 30 items
  // of 8 bytes: head 10 + bitmap 4 + values 240 + length head 2 = 256, which fits; one
  // more item of 1 byte is 1 byte too many.
  SubStartRequest req;
  auto fits = SameItems(30, 8);
  CHECK_EQ(Parse(fits, req), kOk);
  auto above = Head(1000, 0, 31);
  above.insert(above.end(), fits.begin() + 8, fits.end());
  AddItem(above, 0, 1);
  CHECK_EQ(Parse(above, req), kNoResource);
}

TEST(ShortPeriodIsAccepted)
{
  // 周期 1 us 也接受，能否跟上运行时看 / Even a 1 us period is accepted; keeping up is
  // judged at run time.
  SubStartRequest req;
  auto r = Head(1, 0, 1);
  AddItem(r, 0, 4);
  CHECK_EQ(Parse(r, req), kOk);
}
