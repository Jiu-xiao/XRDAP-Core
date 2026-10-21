// SPDX-License-Identifier: Apache-2.0
#pragma once

// 不是主人的传输发来的命令怎么回（docs/protocol.md 3.1）。DAP_Info 照常执行；
// DAP_ExecuteCommands、DAP_QueueCommands 只是容器，里面的每条命令各自检查。标准命令回
// [命令号, DAP_ERROR]，厂商范围（0x80 到 0x9F）的命令回 [命令号, kStatusBusy]。响应空间
// 只剩 1 字节时只回 0xFF，没有空间时不回。
//
// How commands from a transport that does not own the probe are answered
// (docs/protocol.md 3.1). DAP_Info runs as usual; DAP_ExecuteCommands and
// DAP_QueueCommands are only containers, and each command inside them is checked on its
// own. A standard command gets [command, DAP_ERROR], a command in the vendor range (0x80
// to 0x9F) gets [command, kStatusBusy]. With only 1 byte of response space the reply is
// 0xFF alone; with none there is no reply.

#include <cstdint>

namespace XrDap
{

/// 厂商命令的“忙”状态 / Vendor command status "busy".
inline constexpr uint8_t kStatusBusy = 0x05U;

/// @brief 这条命令不拥有探针也能执行 / Whether this command runs without owning the
///        probe.
/// @note 纯函数，任何上下文 / Pure function, any context.
bool RunsWithoutOwner(uint8_t cmd);

/// @brief 写“忙”回复 / Write the busy reply.
/// @note 纯函数，任何上下文 / Pure function, any context.
void WriteBusyReply(uint8_t cmd, uint8_t* resp, uint16_t cap, uint16_t& out);

}  // namespace XrDap
