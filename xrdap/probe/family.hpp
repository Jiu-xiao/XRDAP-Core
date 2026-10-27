// SPDX-License-Identifier: Apache-2.0
#pragma once

// XR-DAP 全系列共用的 USB 标识。所有探针都用 XRUSB 登记的 VID:PID 1D50:6199，靠序列号
// 区分设备。Windows 按 VID、PID 和 bcdDevice 缓存设备描述符，所以各板的 bcdDevice 互不
// 相同，都列在这里。
//
// USB identity shared by the whole XR-DAP family. Every probe uses the XRUSB-registered
// VID:PID 1D50:6199 and is told apart by its serial. Windows caches device descriptors by
// VID, PID and bcdDevice, so each board has its own bcdDevice, all listed here.

#include <cstdint>

namespace XrDap::Family
{

/// USB 厂商号 / USB vendor id.
inline constexpr uint16_t kUsbVid = 0x1D50U;
/// USB 产品号 / USB product id.
inline constexpr uint16_t kUsbPid = 0x6199U;

/// ESP32-S3 的 USB 设备 / The ESP32-S3 USB device.
inline constexpr uint16_t kBcdEsp32S3 = 0x0100U;
/// ESP 探针经 USB/IP 提供的虚拟设备 / The virtual device ESP probes serve over USB/IP.
inline constexpr uint16_t kBcdUsbip = 0x0200U;
/// CH32V307 / CH32V307.
inline constexpr uint16_t kBcdCh32V307 = 0x0300U;
/// STM32F103（STLINK-V2 板）/ STM32F103 (STLINK-V2 board).
inline constexpr uint16_t kBcdF103 = 0x0400U;
/// ESP32-S2 的 USB 设备（描述符布局与 S3 相同）/ The ESP32-S2 USB device (same
/// descriptor layout as the S3).
inline constexpr uint16_t kBcdEsp32S2 = 0x0500U;
/// CH32V305 板（CH32V305-LINKE）/ The CH32V305 board (CH32V305-LINKE).
inline constexpr uint16_t kBcdCh32V305 = 0x0600U;

}  // namespace XrDap::Family
