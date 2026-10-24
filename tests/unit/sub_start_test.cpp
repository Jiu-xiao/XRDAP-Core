// SPDX-License-Identifier: Apache-2.0
// SUB_START 请求检查。判据：用户决定 B9（不估计耗时，SUB_SET 并入开始命令）和
// docs/protocol.md 3.2、3.4 的状态规则 / SUB_START request check. Oracles: user
// decision B9 (no time estimate, SUB_SET merged into the start command) and the status
// rules of docs/protocol.md 3.2 and 3.4.
#include "xrdap/vendor/sub_start.hpp"

#include <vector>

#include "check.hpp"

using namespace XrDap::Vendor;

namespace
{
// 一个采样条目：ap、id、size、base、无解引用、final 0、div、phase / One sampling item:
// ap, id, size, base, no dereference, final 0, div, phase.
void AddItem(std::vector<uint8_t>& r, uint8_t ap, uint8_t size, uint16_t div = 1,
             uint16_t phase = 0)
{
  const uint8_t item[] = {ap,
                          0x01,
                          0x00,
                          size,
                          0x00,
                          0x00,
                          0x00,
                          0x20,
                          0x00,
                          0x00,
                          0x00,
                          static_cast<uint8_t>(div),
                          static_cast<uint8_t>(div >> 8),
                          static_cast<uint8_t>(phase),
                          static_cast<uint8_t>(phase >> 8)};
  r.insert(r.end(), item, item + sizeof(item));
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
  CHECK_EQ(Parse(r, req), kStatusOk);
  CHECK_EQ(req.period_us, 1000U);
  CHECK(req.stop_on_full);
  CHECK_EQ(req.items.count, 2U);
}

TEST(BadFieldsAreArgumentErrors)
{
  SubStartRequest req;
  auto zero_period = Head(0, 0, 1);
  AddItem(zero_period, 0, 4);
  CHECK_EQ(Parse(zero_period, req), kStatusArg);
  auto reserved_flag = Head(1000, 0x02, 1);
  AddItem(reserved_flag, 0, 4);
  CHECK_EQ(Parse(reserved_flag, req), kStatusArg);
  CHECK_EQ(Parse(Head(1000, 0, 0), req), kStatusArg);  // 计数为 0 / count 0
  auto bad_size = Head(1000, 0, 1);
  AddItem(bad_size, 0, 3);
  CHECK_EQ(Parse(bad_size, req), kStatusArg);
  auto zero_div = Head(1000, 0, 1);
  AddItem(zero_div, 0, 4, 0, 0);
  CHECK_EQ(Parse(zero_div, req), kStatusArg);
  auto phase_ge_div = Head(1000, 0, 1);
  AddItem(phase_ge_div, 0, 4, 2, 2);
  CHECK_EQ(Parse(phase_ge_div, req), kStatusArg);
  auto trailing = Head(1000, 0, 1);
  AddItem(trailing, 0, 4);
  trailing.push_back(0);
  CHECK_EQ(Parse(trailing, req), kStatusArg);
  auto short_head = Head(1000, 0, 1);
  short_head.resize(7);
  CHECK_EQ(Parse(short_head, req), kStatusArg);
}

TEST(TooManyItemsIsResource)
{
  SubStartRequest req;
  auto r = Head(1000, 0, kMaxItems + 1U);
  for (uint16_t i = 0; i <= kMaxItems; ++i)
  {
    AddItem(r, 0, 4);
  }
  CHECK_EQ(Parse(r, req), kStatusResource);
}

TEST(TooManyApsIsResource)
{
  SubStartRequest req;
  auto r = Head(1000, 0, kMaxAps + 1U);
  for (uint8_t ap = 0; ap <= kMaxAps; ++ap)
  {
    AddItem(r, ap, 1);
  }
  CHECK_EQ(Parse(r, req), kStatusResource);
}

TEST(FrameLargerThanRingIsResource)
{
  // 测试用缓冲 256 字节：32 个 8 字节条目一帧 10 + 4 + 256 字节 / The test ring is 256
  // bytes: 32 items of 8 bytes make a frame of 10 + 4 + 256 bytes.
  SubStartRequest req;
  auto r = Head(1000, 0, kMaxItems);
  for (uint16_t i = 0; i < kMaxItems; ++i)
  {
    AddItem(r, 0, 8);
  }
  CHECK_EQ(Parse(r, req), kStatusResource);
}

TEST(NoTimeEstimateRejectsShortPeriods)
{
  // B9：周期 1 us 也接受，能否跟上运行时看 / B9: even a 1 us period is accepted;
  // keeping up is judged at run time.
  SubStartRequest req;
  auto r = Head(1, 0, 1);
  AddItem(r, 0, 4);
  CHECK_EQ(Parse(r, req), kStatusOk);
}
