// SPDX-License-Identifier: Apache-2.0
#pragma once

// Core 用到的 CMSIS-DAP 命令号和应答值，按 CMSIS-DAP 规范（Command Overview、
// DAP_Info、DAP_HostStatus）。各固件也用这里的定义，不再各自重复。
//
// CMSIS-DAP command ids and reply values used by Core, from the CMSIS-DAP specification
// (Command Overview, DAP_Info, DAP_HostStatus). The firmwares use these definitions too
// instead of repeating them.

#include <cstdint>

namespace XrDap::Dap
{

/// DAP_Info / DAP_Info.
inline constexpr uint8_t kInfo = 0x00U;
/// DAP_HostStatus / DAP_HostStatus.
inline constexpr uint8_t kHostStatus = 0x01U;
/// DAP_Connect / DAP_Connect.
inline constexpr uint8_t kConnect = 0x02U;
/// DAP_Disconnect / DAP_Disconnect.
inline constexpr uint8_t kDisconnect = 0x03U;
/// DAP_TransferConfigure / DAP_TransferConfigure.
inline constexpr uint8_t kTransferConfigure = 0x04U;
/// DAP_Transfer / DAP_Transfer.
inline constexpr uint8_t kTransfer = 0x05U;
/// DAP_TransferBlock / DAP_TransferBlock.
inline constexpr uint8_t kTransferBlock = 0x06U;
/// DAP_TransferAbort（没有响应）/ DAP_TransferAbort (no response).
inline constexpr uint8_t kTransferAbort = 0x07U;
/// DAP_Delay / DAP_Delay.
inline constexpr uint8_t kDelay = 0x09U;
/// DAP_ResetTarget / DAP_ResetTarget.
inline constexpr uint8_t kResetTarget = 0x0AU;
/// DAP_SWJ_Pins / DAP_SWJ_Pins.
inline constexpr uint8_t kSwjPins = 0x10U;
/// DAP_SWJ_Clock / DAP_SWJ_Clock.
inline constexpr uint8_t kSwjClock = 0x11U;
/// DAP_SWD_Configure / DAP_SWD_Configure.
inline constexpr uint8_t kSwdConfigure = 0x13U;
/// DAP_QueueCommands / DAP_QueueCommands.
inline constexpr uint8_t kQueueCommands = 0x7EU;
/// DAP_ExecuteCommands / DAP_ExecuteCommands.
inline constexpr uint8_t kExecuteCommands = 0x7FU;

/// 命令状态 DAP_OK / Command status DAP_OK.
inline constexpr uint8_t kOk = 0x00U;
/// 命令状态 DAP_ERROR / Command status DAP_ERROR.
inline constexpr uint8_t kError = 0xFFU;
/// 不支持的命令只回这一个字节 / An unsupported command is answered with this one byte.
inline constexpr uint8_t kInvalid = 0xFFU;

/// DAP_HostStatus 的类型：已连接 / DAP_HostStatus type: connected.
inline constexpr uint8_t kHostStatusConnected = 0x00U;
/// DAP_HostStatus 的类型：运行中 / DAP_HostStatus type: running.
inline constexpr uint8_t kHostStatusRunning = 0x01U;

/// DAP_Info 报告的 CMSIS-DAP 协议版本 / CMSIS-DAP protocol version reported by DAP_Info.
inline constexpr char kProtocolVersion[] = "2.0.0";

}  // namespace XrDap::Dap
