// SPDX-License-Identifier: Apache-2.0
#include "xrdap/session/command_class.hpp"

#include <cstring>

#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/vendor/vendor_protocol.hpp"

namespace XrDap
{
namespace
{
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
};
}  // namespace

CommandClass ClassifyCommand(const uint8_t* req, uint16_t len,
                             const FreeCommand* platform_free, size_t platform_free_count)
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
  for (size_t i = 0U; i < platform_free_count; ++i)
  {
    const FreeCommand& f = platform_free[i];
    if (len >= f.len && std::memcmp(req, f.prefix, f.len) == 0)
    {
      return CommandClass::FREE;
    }
  }
  return CommandClass::OWNER;
}

}  // namespace XrDap
