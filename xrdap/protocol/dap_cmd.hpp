// SPDX-License-Identifier: Apache-2.0
#pragma once

// Core 用到的 CMSIS-DAP 命令号和状态值取自 libxr 的 CMSIS-DAP v2 定义，这里只补一个取字节
// 的函数，不再重复定义数值。
//
// The CMSIS-DAP command ids and status values Core uses come from libxr's CMSIS-DAP v2
// definitions; this header only adds a function that yields the byte and defines no value
// again.

#include <cstdint>

// STM32 的设备头文件把 USB 定义成寄存器指针的宏，会破坏命名空间 LibXR::USB；这里暂时取消
// / The STM32 device headers define USB as a register-pointer macro, which breaks the
// namespace LibXR::USB; it is lifted for the lines that name it.
#pragma push_macro("USB")
#undef USB

#include "daplink_v2_def.hpp"

namespace XrDap
{

/// CMSIS-DAP 命令号 / CMSIS-DAP command id.
using DapCmd = LibXR::USB::DapLinkV2Def::CommandId;
/// CMSIS-DAP 命令状态 / CMSIS-DAP command status.
using DapStatus = LibXR::USB::DapLinkV2Def::Status;

#pragma pop_macro("USB")

/// 命令号的字节值 / The byte of a command id.
constexpr uint8_t Byte(DapCmd id) { return static_cast<uint8_t>(id); }
/// 命令状态的字节值 / The byte of a command status.
constexpr uint8_t Byte(DapStatus status) { return static_cast<uint8_t>(status); }

}  // namespace XrDap
