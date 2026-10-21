// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/reply_space.hpp"

#include "xrdap/vendor/vendor_protocol.hpp"

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
    case Cmd::SUB_SET:
      return kSubSetReplyLen;
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
