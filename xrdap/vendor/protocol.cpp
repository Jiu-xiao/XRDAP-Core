// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/protocol.hpp"

// ===== Request length 请求长度 =====

namespace XrDap::Vendor
{
namespace
{
// READ、WRITE：命令号 + 条目数，然后是条目；WRITE 的每个条目后跟 size 字节的值。
// READ, WRITE: command + item count, then the items; each WRITE item is followed by its
// size bytes of value.
bool ItemsLength(const uint8_t* req, const uint8_t* end, bool write, uint16_t& len)
{
  if (end - req < 2)
  {
    return false;
  }
  const uint8_t* p = req + 2;
  for (uint8_t i = 0; i < req[1]; ++i)
  {
    Item it;
    if (!ParseItem(p, end, it, nullptr))
    {
      return false;
    }
    if (write)
    {
      if (end - p < it.size)
      {
        return false;
      }
      p += it.size;
    }
  }
  len = static_cast<uint16_t>(p - req);
  return true;
}

// SUB_START：8 字节头（命令号、周期、标志、条目数），然后是采样条目 / SUB_START: an
// 8-byte head (command, period, flags, item count), then the sampling items.
bool SubStartLength(const uint8_t* req, const uint8_t* end, uint16_t& len)
{
  if (end - req < 8)
  {
    return false;
  }
  const uint8_t* p = req + 8;
  const uint16_t count = Get16(&req[6]);
  for (uint16_t i = 0; i < count; ++i)
  {
    Item it;
    SubItem sub;
    if (!ParseItem(p, end, it, &sub))
    {
      return false;
    }
  }
  len = static_cast<uint16_t>(p - req);
  return true;
}
}  // namespace

bool RequestLength(const uint8_t* req, uint16_t avail, uint16_t& len)
{
  len = 0U;
  if (req == nullptr || avail < 1U)
  {
    return false;
  }
  const uint8_t* end = req + avail;
  uint16_t fixed = 0U;
  switch (req[0])
  {
    case static_cast<uint8_t>(Cmd::INFO):
      fixed = 1U;
      break;
    case static_cast<uint8_t>(Cmd::READ):
      return ItemsLength(req, end, false, len);
    case static_cast<uint8_t>(Cmd::WRITE):
      return ItemsLength(req, end, true, len);
    case static_cast<uint8_t>(Cmd::SUB_START):
      return SubStartLength(req, end, len);
    case static_cast<uint8_t>(Cmd::SUB_CTRL):
      fixed = 2U;
      break;
    case static_cast<uint8_t>(Cmd::SUB_READ):
      fixed = 3U;
      break;
    default:
      return false;
  }
  if (fixed > avail)
  {
    return false;
  }
  len = fixed;
  return true;
}

}  // namespace XrDap::Vendor

// ===== Reply space 回复空间 =====

namespace XrDap::Vendor
{
namespace
{
constexpr uint16_t kStatusOnly = 2U;

// READ：3 字节头，每个条目 id(2) + 状态(1) + 值。WRITE 的条目后面跟着值，回复里没有值。
// READ: a 3-byte head, then id(2) + status(1) + value per item. WRITE items are followed
// by their values in the request, and its reply carries no values.
uint16_t ItemReply(const uint8_t* req, uint16_t len, bool write)
{
  if (len < 2U || req[1] == 0U || req[1] > kMaxItems)
  {
    return kStatusOnly;
  }
  const uint8_t* p = req + 2;
  const uint8_t* end = req + len;
  uint32_t need = 3U;
  for (uint8_t i = 0; i < req[1]; ++i)
  {
    Item it;
    if (!ParseItem(p, end, it, nullptr) || !SizeValid(it.size))
    {
      return kStatusOnly;
    }
    if (write)
    {
      if (static_cast<size_t>(end - p) < it.size)
      {
        return kStatusOnly;
      }
      p += it.size;
      need += 3U;
    }
    else
    {
      need += 3U + it.size;
    }
  }
  return (p == end) ? static_cast<uint16_t>(need) : kStatusOnly;
}
}  // namespace

uint16_t RequiredReply(const uint8_t* req, uint16_t len)
{
  if (len < 1U)
  {
    return kStatusOnly;
  }
  switch (static_cast<Cmd>(req[0]))
  {
    case Cmd::INFO:
      return kInfoReplyLen;
    case Cmd::READ:
      return ItemReply(req, len, false);
    case Cmd::WRITE:
      return ItemReply(req, len, true);
    case Cmd::SUB_START:
    case Cmd::SUB_CTRL:
      return kStatusOnly;
    case Cmd::SUB_READ:
      return kSubReadHeaderLen;
  }
  return kStatusOnly;
}

bool ReserveReply(uint8_t cmd, uint16_t need, uint8_t* resp, uint16_t cap, uint16_t& out)
{
  if (cap >= need)
  {
    return true;
  }
  if (cap < 2U)
  {
    if (cap == 1U)
    {
      resp[0] = 0xFFU;
    }
    out = cap;
    return false;
  }
  resp[0] = cmd;
  resp[1] = kStatusResource;
  out = 2U;
  return false;
}

}  // namespace XrDap::Vendor

// ===== SUB_START checks SUB_START 检查 =====

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
