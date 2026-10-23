// SPDX-License-Identifier: Apache-2.0
// 命令要不要拥有探针。判据：用户决定 B2 及其扩展（2026-10-24，“只读的状态和设置查询
// 可以做，甚至 led 都可以改”）和 docs/protocol.md 3.1：不碰目标、SWD 和采样的命令不需要
// 拥有探针；设置写入、Wi-Fi 保存和清除、采样、SWD、SWJ 和一切碰目标的命令要拥有探针。
//
// Whether a command needs to own the probe. Oracles: user decision B2 and its extension
// (2026-10-24: read-only status and settings queries may run, even the LED may change)
// and docs/protocol.md 3.1: commands that touch neither the target, SWD nor sampling do
// not need to own the probe; settings writes, Wi-Fi save and clear, sampling, SWD, SWJ
// and everything that touches the target do.
#include "xrdap/session/command_class.hpp"

#include <initializer_list>
#include <vector>

#include "check.hpp"

using XrDap::ClassifyCommand;
using XrDap::CommandClass;

namespace
{
CommandClass Class(std::initializer_list<uint8_t> bytes)
{
  const std::vector<uint8_t> v(bytes);
  return ClassifyCommand(v.data(), static_cast<uint16_t>(v.size()));
}
}  // namespace

TEST(ProbeInformationIsFree)
{
  CHECK(Class({0x00, 0x01}) == CommandClass::FREE);  // DAP_Info
  CHECK(Class({0x80}) == CommandClass::FREE);        // 厂商 INFO / vendor INFO
}

TEST(StatusLightIsFree)
{
  CHECK(Class({0x01, 0x00, 0x01}) == CommandClass::FREE);  // DAP_HostStatus
}

TEST(ContainersAreFreeTheirCommandsAreClassedAlone)
{
  CHECK(Class({0x7E, 0x01, 0x00, 0x01}) == CommandClass::FREE);
  CHECK(Class({0x7F, 0x01, 0x02, 0x01}) == CommandClass::FREE);
}

TEST(ReadOnlySettingsQueriesAreFree)
{
  CHECK(Class({0x86, 0x00}) == CommandClass::FREE);  // Wi-Fi 状态 / Wi-Fi status
  CHECK(Class({0x9F, 0x13}) == CommandClass::FREE);  // 射频设置 / radio settings
}

TEST(SettingsWritesNeedTheOwner)
{
  CHECK(Class({0x86, 0x01, 0x00, 0x00}) == CommandClass::OWNER);  // Wi-Fi 保存 / save
  CHECK(Class({0x86, 0x02, 0xA5}) == CommandClass::OWNER);        // Wi-Fi 清除 / clear
  CHECK(Class({0x86}) == CommandClass::OWNER);  // 没有操作字节 / no operation byte
  for (uint8_t op : {0x10, 0x14, 0x15, 0x16, 0x20, 0x30})
  {
    CHECK(Class({0x9F, op, 0x00}) == CommandClass::OWNER);
  }
  CHECK(Class({0x9F}) == CommandClass::OWNER);
}

TEST(TargetSwdAndSamplingNeedTheOwner)
{
  CHECK(Class({0x02, 0x01}) == CommandClass::OWNER);              // DAP_Connect
  CHECK(Class({0x03}) == CommandClass::OWNER);                    // DAP_Disconnect
  CHECK(Class({0x05, 0x00, 0x01, 0x02}) == CommandClass::OWNER);  // DAP_Transfer
  CHECK(Class({0x06}) == CommandClass::OWNER);                    // DAP_TransferBlock
  CHECK(Class({0x0A}) == CommandClass::OWNER);                    // DAP_ResetTarget
  CHECK(Class({0x10}) == CommandClass::OWNER);                    // DAP_SWJ_Pins
  CHECK(Class({0x11}) == CommandClass::OWNER);                    // DAP_SWJ_Clock
  CHECK(Class({0x12}) == CommandClass::OWNER);                    // DAP_SWJ_Sequence
  CHECK(Class({0x81}) == CommandClass::OWNER);                    // READ
  CHECK(Class({0x82}) == CommandClass::OWNER);                    // WRITE
  CHECK(Class({0x83}) == CommandClass::OWNER);                    // SUB_START
  CHECK(Class({0x84, 0x00}) == CommandClass::OWNER);              // SUB_CTRL
  CHECK(Class({0x85, 0x00, 0x04}) == CommandClass::OWNER);        // SUB_READ
  CHECK(ClassifyCommand(nullptr, 0U) == CommandClass::OWNER);
}
