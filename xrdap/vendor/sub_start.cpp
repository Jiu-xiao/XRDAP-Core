// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/sub_start.hpp"

namespace XrDap::Vendor
{
namespace
{
// 请求头：命令号 + 周期 u32 + 标志 u8 + 条目数 u16 / Request head: command + period u32
// + flags u8 + item count u16.
constexpr uint16_t kHeadLen = 8U;
}  // namespace

uint8_t ParseSubStart(const uint8_t* req, uint16_t len, SubStartRequest& out)
{
  if (len < kHeadLen)
  {
    return kStatusArg;
  }
  const uint32_t period = Get32(&req[1]);
  const uint8_t flags = req[5];
  const uint16_t count = Get16(&req[6]);
  if (period == 0U || (flags & 0xFEU) != 0U || count == 0U)
  {
    return kStatusArg;
  }
  if (count > kMaxItems)
  {
    return kStatusResource;
  }

  const uint8_t* p = req + kHeadLen;
  const uint8_t* end = req + len;
  uint8_t aps_seen[kMaxAps];
  uint8_t ap_count = 0U;
  uint32_t values = 0U;
  for (uint16_t i = 0; i < count; ++i)
  {
    Item it;
    SubItem sub;
    if (!ParseItem(p, end, it, &sub) || !SizeValid(it.size) || sub.div == 0U ||
        sub.phase >= sub.div)
    {
      return kStatusArg;
    }
    values += it.size;
    bool found = false;
    for (uint8_t j = 0; j < ap_count; ++j)
    {
      found = found || (aps_seen[j] == it.ap);
    }
    if (!found)
    {
      if (ap_count >= kMaxAps)
      {
        return kStatusResource;
      }
      aps_seen[ap_count++] = it.ap;
    }
  }
  if (p != end)
  {
    return kStatusArg;
  }
  // 所有条目都到期时的一帧，加上缓冲里 2 字节的长度头 / One frame with every item due,
  // plus its 2-byte length head in the ring.
  const uint32_t max_frame = 10U + (count + 7U) / 8U + values;
  if (max_frame + 2U > kRingBytes)
  {
    return kStatusResource;
  }
  out.period_us = period;
  out.stop_on_full = (flags & 1U) != 0U;
  out.items = SubItemList{req + kHeadLen, end, count};
  return kStatusOk;
}

}  // namespace XrDap::Vendor
