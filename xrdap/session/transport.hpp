// SPDX-License-Identifier: Apache-2.0
#pragma once

// 命令从哪条链路来。只是标签：探针一次只使用一条链路由使用者保证，Core 不替它们仲裁。
//
// Which link a command came from. Only a label: that the probe is used over one link at a
// time is up to the user; Core does not arbitrate between links.

#include <cstdint>

namespace XrDap
{

/// 驱动探针的链路 / Link that drives the probe.
enum class Transport : uint8_t
{
  NONE = 0,    ///< 没有 / None.
  USB = 1,     ///< USB 接口 / USB interface.
  SERIAL = 2,  ///< 串口链路 / Serial link.
  TCP = 3,     ///< TCP 连接 / TCP connection.
  RADIO = 4,   ///< 无线对端（远端探针上）/ Radio peer (on the remote probe).
  CONSOLE = 5,  ///< 命令行（只发控制请求）/ Console (control requests only).
};

}  // namespace XrDap
