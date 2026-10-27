// SPDX-License-Identifier: Apache-2.0
#include "xrdap/session/command_class.hpp"

#include <cstring>

#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

namespace XrDap
{
namespace
{
// 一条不需要拥有探针的命令：请求以这几个字节开头 / A command that needs no ownership:
// the request starts with these bytes.
struct FreeCommand
{
  uint8_t len;  ///< 要比较的字节数 / Bytes to compare.
  uint8_t prefix[4];
};

// 不需要拥有探针的命令；表外的都要 / Commands that do not need to own the probe; every
// command not listed does.
constexpr FreeCommand kFree[] = {
    // 标准命令：探针信息、状态灯 / Standard: probe information, the status light.
    {1, {Byte(DapCmd::INFO)}},
    {1, {Byte(DapCmd::HOST_STATUS)}},
    // 容器：里面的命令各自归类 / Containers: the commands inside are classed one by one.
    {1, {Byte(DapCmd::QUEUE_COMMANDS)}},
    {1, {Byte(DapCmd::EXECUTE_COMMANDS)}},
    // 厂商 INFO：能力和探针时间 / Vendor INFO: capabilities and probe time.
    {1, {static_cast<uint8_t>(Vendor::Cmd::INFO)}},
    // 0x86 查询 Wi-Fi 状态（XRDAP-ESP）/ 0x86 Wi-Fi status query (XRDAP-ESP).
    {2, {Vendor::kWifiCmd, Vendor::kWifiOpQuery}},
    // 0x9F 0x13 读本机的射频设置、配对和角色（XRDAP-CH32V307）。0x9F 0x30 包着 0x13 读
    // 对端时要用无线链路，那条链路可能正承载另一条传输的会话，所以要拥有探针 / 0x9F
    // 0x13 reads this probe's radio settings, pairing and role (XRDAP-CH32V307). 0x9F
    // 0x30
    // wrapping 0x13 to read the peer uses the radio link, which may be carrying another
    // transport's session, so it needs ownership.
    {2, {0x9FU, 0x13U}},
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
    if (len >= f.len && std::memcmp(req, f.prefix, f.len) == 0)
    {
      return CommandClass::FREE;
    }
  }
  return CommandClass::OWNER;
}

}  // namespace XrDap
