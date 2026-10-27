// SPDX-License-Identifier: Apache-2.0
#include "xrdap/session/busy_reply.hpp"

#include "xrdap/protocol/dap_cmd.hpp"

namespace XrDap
{
namespace
{
// CMSIS-DAP 把 0x80 到 0x9F 留给厂商命令 / CMSIS-DAP reserves 0x80 to 0x9F for vendor
// commands.
constexpr uint8_t kVendorFirst = 0x80U;
constexpr uint8_t kVendorLast = 0x9FU;
}  // namespace

void WriteBusyReply(uint8_t cmd, uint8_t* resp, uint16_t cap, uint16_t& out)
{
  if (cap < 2U)
  {
    if (cap == 1U)
    {
      resp[0] = Byte(DapCmd::INVALID);
    }
    out = cap;
    return;
  }
  resp[0] = cmd;
  resp[1] = (cmd >= kVendorFirst && cmd <= kVendorLast) ? kStatusBusy : Byte(DapStatus::ERROR);
  out = 2U;
}

}  // namespace XrDap
