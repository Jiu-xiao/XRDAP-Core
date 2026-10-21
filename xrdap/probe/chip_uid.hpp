// SPDX-License-Identifier: Apache-2.0
#pragma once

// STM32F1 和 CH32V3 的 96 位芯片唯一标识，两者地址相同（STM32F1 参考手册“Device
// electronic signature”，CH32V3 参考手册“电子签名”）。它是这两种探针序列号的标识部分。
//
// The 96-bit unique id of the STM32F1 and the CH32V3, at the same address on both
// (STM32F1 reference manual "Device electronic signature", CH32V3 reference manual
// "electronic signature"). It is the id part of these probes' serials.

#include <cstdint>

namespace XrDap
{

/// 唯一标识的字节数 / Bytes of the unique id.
inline constexpr uint32_t kF1ChipUidBytes = 12U;

/// 唯一标识（只读，可在任何上下文读）/ The unique id (read-only, readable from any
/// context).
inline const uint8_t* F1ChipUid()
{
  return reinterpret_cast<const uint8_t*>(0x1FFFF7E8U);
}

}  // namespace XrDap
