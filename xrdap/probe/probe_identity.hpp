// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针身份：DAP_Info 里的厂商、产品、序列号、版本。序列号 = 前缀 + 芯片唯一标识（ESP 为
// 出厂 MAC，STM32、CH32 为 12 字节 UID）的大写十六进制，与 USB 设备序列号相同，上位机靠它
// 在 USB、串口、TCP、射频上认出同一块探针。见 docs/protocol.md 第 4.1 节。
//
// Probe identity: vendor, product, serial and version for DAP_Info. Serial = prefix +
// the chip's unique id (the factory MAC on ESP, the 12-byte UID on STM32 and CH32) in
// upper-case hex, identical to the USB device serial, so host tools recognise one probe
// across USB, serial, TCP and radio. See docs/protocol.md section 4.1.

#include <cstddef>
#include <cstdint>

#include "xrdap/protocol/dap_cmd.hpp"

namespace XrDap
{

/// 厂商名 / Vendor name.
inline constexpr char kVendorName[] = "XRobot";
/// 序列号前缀 / Serial number prefix.
inline constexpr char kSerialPrefix[] = "XRobot-XRDAP-";
/// DAP_Info 报告的 CMSIS-DAP 协议版本 / CMSIS-DAP protocol version reported by DAP_Info.
inline constexpr char kProtocolVersion[] = "2.0.0";

/// 序列号缓冲需要的字节数（含结尾 0）/ Bytes a serial buffer needs (with the trailing 0).
constexpr size_t SerialBufferSize(size_t id_bytes)
{
  return sizeof(kSerialPrefix) + 2U * id_bytes;
}

/// @brief 写出序列号：前缀 + id 的大写十六进制 / Write the serial: prefix + id in
///        upper-case hex.
/// @param out 至少 SerialBufferSize(id_bytes) 字节 / At least SerialBufferSize(id_bytes)
///        bytes.
/// @note 不用 snprintf：64 KB 的探针放不下 printf 一族。纯函数，任何上下文 / No
///       snprintf: a 64 KB probe has no room for the printf family. Pure function, any
///       context.
void FormatSerial(const uint8_t* id, size_t id_bytes, char* out);

/// DAP_Info 里随探针变化的字符串，都需长期有效 / The DAP_Info strings that vary by probe;
/// all must stay valid.
struct ProbeIdentity
{
  /// 产品名，例如 "XR-DAP-S3" / Product name, e.g. "XR-DAP-S3".
  const char* product;
  /// 板子名，例如 "STLINK-V2"；与产品相同时传 product / Board name, e.g. "STLINK-V2";
  /// pass product when they are the same.
  const char* board;
  /// 序列号 / Serial.
  const char* serial;
  /// 探针固件版本 / Probe firmware version.
  const char* firmware;
};

/// @brief 把探针身份写入 DAP 实例的 DAP_Info 字符串 / Write the probe identity into the
///        DAP instance's DAP_Info strings.
/// @note 初始化时、命令开始执行之前调用 / Call at initialisation, before commands run.
template <typename DapT>
void SetProbeInfo(DapT& dap, const ProbeIdentity& id)
{
  dap.SetInfoStrings({
      kVendorName,            // 厂商 / vendor
      id.product,             // 产品 / product
      id.serial,              // 序列号 / serial
      kProtocolVersion,  // CMSIS-DAP 协议版本 / CMSIS-DAP protocol version
      nullptr,                // 目标芯片厂商（不固定）/ target device vendor (not fixed)
      nullptr,                // 目标芯片名 / target device name
      kVendorName,            // 板子厂商 / board vendor
      id.board,               // 板子名 / board name
      id.firmware,            // 探针固件版本 / probe firmware version
  });
}

}  // namespace XrDap
