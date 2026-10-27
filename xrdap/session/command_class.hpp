// SPDX-License-Identifier: Apache-2.0
#pragma once

// 命令要不要拥有探针（docs/protocol.md 3.1，用户决定 B2 及其扩展）。不碰目标、SWD 和
// 采样的命令——只读的状态和设置查询、状态灯——任何传输都能执行，不成为主人，也不让
// 别的传输变忙；其余命令要拥有探针，不是主人时回“忙”。DAP_ExecuteCommands、
// DAP_QueueCommands 只是容器，里面的每条命令各自归类。所有归类都在这一张表里。
//
// Whether a command needs to own the probe (docs/protocol.md 3.1, user decision B2 and
// its extension). Commands that touch neither the target, SWD nor sampling (read-only
// status and settings queries, the status light) run from any transport, never make it
// the owner and never make another transport busy; every other command needs to own the
// probe and gets busy otherwise. DAP_ExecuteCommands and DAP_QueueCommands are only
// containers, and each command inside them is classed on its own. Every class lives in
// this one table.

#include <cstdint>

namespace XrDap
{

/// 命令的归类 / Class of a command.
enum class CommandClass : uint8_t
{
  FREE,   ///< 不需要拥有探针 / Does not need to own the probe.
  OWNER,  ///< 要拥有探针 / Needs to own the probe.
};

/// @brief 按命令号（以及需要时的操作字节）归类 / Class a command by its command byte
///        (and its operation byte where that matters).
/// @note 纯函数，任何上下文 / Pure function, any context.
CommandClass ClassifyCommand(const uint8_t* req, uint16_t len);

}  // namespace XrDap
