// SPDX-License-Identifier: Apache-2.0
#pragma once

// 不是主人的传输发来、又要拥有探针的命令怎么回（docs/protocol.md 3.1；哪些命令要拥有
// 探针见 command_class.hpp）。标准命令回 [命令号, DAP_ERROR]，厂商范围（0x80 到 0x9F）
// 的命令回 [命令号, kStatusBusy]。响应空间只剩 1 字节时只回 0xFF，没有空间时不回。
//
// How a command that needs to own the probe is answered when it comes from a transport
// that does not (docs/protocol.md 3.1; which commands need to own it is in
// command_class.hpp). A standard command gets [command, DAP_ERROR], a command in the
// vendor range (0x80 to 0x9F) gets [command, kStatusBusy]. With only 1 byte of response
// space the reply is 0xFF alone; with none there is no reply.

#include <cstdint>

namespace XrDap
{

/// 厂商命令的“忙”状态 / Vendor command status "busy".
inline constexpr uint8_t kStatusBusy = 0x05U;

/// @brief 写“忙”回复 / Write the busy reply.
/// @note 纯函数，任何上下文 / Pure function, any context.
void WriteBusyReply(uint8_t cmd, uint8_t* resp, uint16_t cap, uint16_t& out);

}  // namespace XrDap
