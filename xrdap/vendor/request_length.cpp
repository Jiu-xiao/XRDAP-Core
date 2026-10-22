// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/request_length.hpp"

#include "xrdap/vendor/vendor_protocol.hpp"

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

// 0x86：查询 86 00；保存 86 01 ssid_len ssid pw_len pw；清除 86 02 A5（3.7 节）/ 0x86:
// query 86 00; save 86 01 ssid_len ssid pw_len pw; clear 86 02 A5 (section 3.7).
bool WifiLength(const uint8_t* req, uint16_t avail, uint16_t& len)
{
  if (avail < 2U)
  {
    return false;
  }
  uint32_t need = 0U;
  switch (req[1])
  {
    case kWifiOpQuery:
      need = 2U;
      break;
    case kWifiOpClear:
      need = 3U;
      break;
    case kWifiOpSave:
      if (avail < 3U)
      {
        return false;
      }
      need = 3U + req[2];
      if (need >= avail)
      {
        return false;
      }
      need += 1U + req[need];
      break;
    default:
      return false;
  }
  if (need > avail)
  {
    return false;
  }
  len = static_cast<uint16_t>(need);
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
    case kWifiCmd:
      return WifiLength(req, avail, len);
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
