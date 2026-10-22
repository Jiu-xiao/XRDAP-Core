// SPDX-License-Identifier: Apache-2.0
#pragma once

// 厂商命令请求的长度（docs/protocol.md 3.2）。DAP_ExecuteCommands 和 DAP_QueueCommands
// 把多条命令首尾相接放在一个包里，没有长度字段，CMSIS-DAP 要求每条命令的长度能从它自己
// 的内容得出。标准命令由 libxr 计算，0x80 到 0x86 由这里计算，libxr 经长度钩子来问。
//
// Request length of vendor commands (docs/protocol.md 3.2). DAP_ExecuteCommands and
// DAP_QueueCommands place several commands back to back in one packet with no length
// field; CMSIS-DAP requires every command's length to follow from its own content.
// libxr works out standard commands, this works out 0x80 to 0x86, and libxr asks through
// its length hook.

#include <cstdint>

namespace XrDap::Vendor
{

/// @brief 算出 req 开头这条厂商命令的请求字节数 / Work out the request bytes of the
///        vendor command at the start of req.
/// @param avail req 里可用的字节数 / Bytes available in req.
/// @param len 成功时为这条命令的字节数，不超过 avail / On success, the bytes of this
///        command, at most avail.
/// @return 命令号不是 0x80 到 0x86、内容不完整或 0x86 的操作未知时 false / False when the
///         command is not 0x80 to 0x86, its content is incomplete, or the 0x86 operation
///         is unknown.
/// @note 纯函数，任何上下文 / Pure function, any context.
bool RequestLength(const uint8_t* req, uint16_t avail, uint16_t& len);

}  // namespace XrDap::Vendor
