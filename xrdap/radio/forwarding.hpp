// SPDX-License-Identifier: Apache-2.0
#pragma once

// 主机探针经无线转发 DAP 命令时要知道的事。纯逻辑。
//
// ExecutionUs()：远端执行一条命令最多用多久（命令自己的等待，或按转发出去的 SWD 时钟算
// 的 SWD 事务，再乘 2 留余量），至多 3 s；主机把它加到等应答的时间上。
// SettingsReplay：工具在 DAP_Connect 之前发的 SWJ_Clock、TransferConfigure 和
// SWD_Configure 在本地执行，因为那时还不知道会话走哪条路；每种保留最后一条，会话走无线时
// 重发给远端。
//
// What the host probe needs to know about DAP commands to forward them over the radio.
// Pure logic.
//
// ExecutionUs(): how long the remote may take to execute a command (its waits, or its
// SWD transactions at the forwarded SWD clock, twice over for margin), 3 s at most; the
// host adds it to the time it waits for the answer.
// SettingsReplay: SWJ_Clock, TransferConfigure and SWD_Configure that a tool sends before
// DAP_Connect run locally, as the session is not decided yet; the last of each is kept
// and sent to the remote when the session goes over the radio.

#include <cstdint>

#include "xrdap/protocol/dap_cmd.hpp"

namespace XrDap::Radio
{

/// @brief req 是不是这条 DAP 命令 / Whether `req` is this DAP command.
inline bool IsCommand(const uint8_t* req, uint16_t len, DapCmd id)
{
  return len != 0U && req[0] == Byte(id);
}

/// 还没转发过 SWJ_Clock 时假定的 SWD 时钟 / SWD clock assumed before any SWJ_Clock was
/// forwarded.
inline constexpr uint32_t kDefaultSwdHz = 500'000;

/// @brief 远端执行这条命令最多用多久，µs，至多 3 s / How long the remote may take to
///        execute this command, µs, 3 s at most.
uint32_t ExecutionUs(const uint8_t* req, uint16_t len, uint32_t swd_hz);

/// @brief SWJ_Clock 命令设的时钟；别的命令或时钟为 0 时返回 0 / The clock an SWJ_Clock
///        command sets; 0 for other commands or a zero clock.
uint32_t SwjClockHz(const uint8_t* req, uint16_t len);

/// @brief 会话开始前的设置命令，会话走无线时重发 / Setting commands before a session,
///        replayed when it goes over the radio.
class SettingsReplay
{
 public:
  /// 保留的命令种数 / Kinds of command kept.
  static constexpr uint8_t kSettings = 3;

  /// @brief 看一条命令，是设置命令就保留 / Look at a command and keep it if it is a
  ///        setting.
  void Note(const uint8_t* req, uint16_t len);

  /// @brief 第 i 种保留的命令；没有时 Size(i) 为 0 / The kept command of kind i;
  ///        Size(i) is 0 when none was noted.
  const uint8_t* Data(uint8_t i) const { return items_[i].data; }
  uint8_t Size(uint8_t i) const { return items_[i].size; }

 private:
  struct Item
  {
    DapCmd id;
    uint8_t size;
    uint8_t data[8];
  };
  Item items_[kSettings] = {{DapCmd::SWJ_CLOCK, 0, {}},
                            {DapCmd::TRANSFER_CONFIGURE, 0, {}},
                            {DapCmd::SWD_CONFIGURE, 0, {}}};
};

}  // namespace XrDap::Radio
