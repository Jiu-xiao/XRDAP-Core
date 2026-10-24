// SPDX-License-Identifier: Apache-2.0
#pragma once

// 厂商命令的回复空间检查，所有命令共用一处（docs/protocol.md 3.2）。执行前先算出这条
// 命令的回复要多少字节：空间不到 2 字节时只回一个 0xFF，不够整条回复时回
// [命令号, 0x03]，都不执行命令，也不写出给定的空间。DAP_ExecuteCommands 里排在后面的
// 命令剩下的空间可能只有 1 字节。
//
// Reply space check for vendor commands, one place for all of them (docs/protocol.md
// 3.2). Before a command runs, the bytes its reply needs are worked out: with less than
// 2 bytes of space the reply is 0xFF alone, with less than the whole reply it is
// [command, 0x03]; the command does not run in either case and nothing is written past
// the given space. A command late in a DAP_ExecuteCommands batch may have only 1 byte
// left.

#include <cstdint>

namespace XrDap::Vendor
{

/// @brief 命令 0x80 到 0x85 的回复最多要多少字节；请求本身不合法时为 2（只回状态）/
///        Bytes the reply of command 0x80 to 0x85 needs at most; 2 (status only) when
///        the request itself is malformed.
/// @note 纯函数，任何上下文 / Pure function, any context.
uint16_t RequiredReply(const uint8_t* req, uint16_t len);

/// @brief 检查回复空间，不够时写好回复 / Check the reply space and write the reply when
///        it is short.
/// @param need 这条命令的回复要的字节数，至少 2 / Bytes this command's reply needs, at
///        least 2.
/// @return 空间够、可以执行时 true / True when there is room and the command may run.
/// @note 纯函数，任何上下文 / Pure function, any context.
bool ReserveReply(uint8_t cmd, uint16_t need, uint8_t* resp, uint16_t cap, uint16_t& out);

}  // namespace XrDap::Vendor
