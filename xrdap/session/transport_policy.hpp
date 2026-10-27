// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针有几条传输是编译期就知道的事，平台在 xrdap_port.hpp 里用 Port::TransportPolicy 选：
//   MultiTransport   USB、串口、TCP、无线里有两条以上：命令要排队，要拥有探针的命令先问
//                    会话主人，不是主人的回“忙”（现状）。
//   SingleTransport  只有 USB 一条（例如 F103）：没有谁可以抢，所以没有排队、没有命令分类、
//                    没有“忙”，Executor::Execute / EndSession / TouchSession /
//                    SetFreeCommands 不存在（调用它们是编译错误）。会话仍然有：每条命令
//                    都由 USB 准入，没有会话时开始一个，USB 总线复位或 DAP_Disconnect
//                    结束它，采样和 DAP_Connect 状态记在它的编号上。
//
// Whether the probe has several transports is known at compile time; the platform picks
// with Port::TransportPolicy in xrdap_port.hpp:
//   MultiTransport   two or more of USB, serial, TCP, radio: commands queue, a command
//                    that needs to own the probe asks the session owner first, and a
//                    transport that is not the owner is answered busy (as before).
//   SingleTransport  USB only (for example the F103): nobody can compete, so there is no
//                    queue, no command classification and no busy reply, and
//                    Executor::Execute / EndSession / TouchSession / SetFreeCommands do
//                    not exist (calling them does not compile). The session remains:
//                    USB admits every command and starts a session when there is none, a
//                    USB bus reset or DAP_Disconnect ends it, and sampling and the
//                    DAP_Connect state are tied to its number.

namespace XrDap::Session
{

/// 两条以上传输 / Two or more transports.
class MultiTransport
{
 public:
  static constexpr bool kMulti = true;
};

/// 只有 USB 一条传输 / USB is the only transport.
class SingleTransport
{
 public:
  static constexpr bool kMulti = false;
};

}  // namespace XrDap::Session
