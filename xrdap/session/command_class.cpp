// SPDX-License-Identifier: Apache-2.0
#include "xrdap/session/command_class.hpp"

#include "xrdap/protocol/dap_ids.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

namespace XrDap
{
namespace
{
// 不看操作字节 / The operation byte does not matter.
constexpr int16_t kAnyOp = -1;

struct FreeCommand
{
  uint8_t cmd;
  int16_t op;  ///< req[1]，或 kAnyOp / req[1], or kAnyOp.
};

// 不需要拥有探针的命令；表外的都要 / Commands that do not need to own the probe; every
// command not listed does.
constexpr FreeCommand kFree[] = {
    // 标准命令：探针信息、状态灯 / Standard: probe information, the status light.
    {Dap::kInfo, kAnyOp},
    {Dap::kHostStatus, kAnyOp},
    // 容器：里面的命令各自归类 / Containers: the commands inside are classed one by one.
    {Dap::kQueueCommands, kAnyOp},
    {Dap::kExecuteCommands, kAnyOp},
    // 厂商 INFO：能力和探针时间 / Vendor INFO: capabilities and probe time.
    {static_cast<uint8_t>(Vendor::Cmd::INFO), kAnyOp},
    // 0x86 查询 Wi-Fi 状态（XRDAP-ESP）/ 0x86 Wi-Fi status query (XRDAP-ESP).
    {Vendor::kWifiCmd, Vendor::kWifiOpQuery},
    // 0x9F 0x13 读射频设置、配对和角色（XRDAP-CH32V307）/ 0x9F 0x13 reads the radio
    // settings, pairing and role (XRDAP-CH32V307).
    {0x9FU, 0x13},
};
}  // namespace

CommandClass ClassifyCommand(const uint8_t* req, uint16_t len)
{
  if (req == nullptr || len < 1U)
  {
    return CommandClass::OWNER;
  }
  for (const FreeCommand& f : kFree)
  {
    if (f.cmd != req[0])
    {
      continue;
    }
    if (f.op == kAnyOp || (len >= 2U && req[1] == static_cast<uint8_t>(f.op)))
    {
      return CommandClass::FREE;
    }
  }
  return CommandClass::OWNER;
}

}  // namespace XrDap
