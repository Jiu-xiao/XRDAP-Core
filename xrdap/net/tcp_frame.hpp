// SPDX-License-Identifier: Apache-2.0
#pragma once

// TCP 帧（docs/protocol.md 第 2 节）：8 字节头加 DAP 包，与 OpenOCD 的 cmsis-dap
// backend tcp 一致。只编码和解码帧头，不碰套接字，任何上下文都能用，电脑上可测。
//   "DAP\0"(4) + 长度(2，小端) + 类型(1) + 保留(1，填 0) + 数据(长度)
//
// TCP frame (docs/protocol.md section 2): an 8-byte header plus a DAP packet, as in
// OpenOCD's cmsis-dap backend tcp. Only the header is encoded and decoded; there are no
// sockets, so it works in any context and is host-testable.
//   "DAP\0"(4) + length(2, little-endian) + type(1) + reserved(1, zero) + data(length)

#include <cstddef>
#include <cstdint>

namespace XrDap::Net
{

/// 帧类型 / Frame type.
enum class FrameType : uint8_t
{
  REQUEST = 1,           ///< DAP 请求 / DAP request.
  RESPONSE = 2,          ///< DAP 响应 / DAP response.
  CONTROL_REQUEST = 3,   ///< 控制请求（3.7.1）/ Control request (3.7.1).
  CONTROL_RESPONSE = 4,  ///< 控制应答（3.7.1）/ Control answer (3.7.1).
};

/// 帧头字节数 / Header bytes.
inline constexpr size_t kHeaderLen = 8U;

/// 帧头里的字段位置 / Field positions in the header.
inline constexpr size_t kMagicLen = 4U;
inline constexpr size_t kLengthAt = kMagicLen;
inline constexpr size_t kTypeAt = kLengthAt + 2U;
inline constexpr size_t kReservedAt = kTypeAt + 1U;
static_assert(kReservedAt + 1U == kHeaderLen, "header layout");

/// 帧头开头的签名 "DAP\0" / Signature at the start of the header, "DAP\0".
inline constexpr uint8_t kMagic[kMagicLen] = {'D', 'A', 'P', 0U};

/// 解码出的帧头 / A decoded header.
struct Header
{
  FrameType type;   ///< 帧类型 / Frame type.
  uint16_t length;  ///< 后面数据的字节数 / Bytes of data that follow.
};

/// @brief 写出帧头 / Write a header.
/// @param out 至少 kHeaderLen 字节 / At least kHeaderLen bytes.
/// @param length 后面数据的字节数，1 到对方接受的上限 / Bytes of data that follow, from 1
///        up to what the peer accepts.
/// @note 纯函数，任何上下文 / Pure function, any context.
inline void EncodeHeader(uint8_t* out, FrameType type, uint16_t length)
{
  for (size_t i = 0; i < kMagicLen; ++i)
  {
    out[i] = kMagic[i];
  }
  out[kLengthAt] = static_cast<uint8_t>(length & 0xFFU);
  out[kLengthAt + 1U] = static_cast<uint8_t>(length >> 8);
  out[kTypeAt] = static_cast<uint8_t>(type);
  out[kReservedAt] = 0U;
}

/// @brief 探针端解码收到的帧头：只收请求（DAP 请求和控制请求）/ Probe side: decode a
///        received header, requests only (DAP request and control request).
/// @param hdr 收到的 kHeaderLen 字节 / The received kHeaderLen bytes.
/// @param max_length 数据长度的上限（DAP_Info 报告的包长）/ Limit of the data length
///        (the packet size DAP_Info reports).
/// @param out 成功时的帧头 / The header on success.
/// @return 签名正确、类型是请求、长度在 1 到 max_length 之间时 true；false 时探针关闭
///         连接 / True when the signature is right, the type is a request and the length
///         is within 1 to max_length; on false the probe closes the connection.
/// @note 保留字节不检查。纯函数，任何上下文 / The reserved byte is not checked. Pure
///       function, any context.
inline bool DecodeRequestHeader(const uint8_t* hdr, uint16_t max_length, Header& out)
{
  for (size_t i = 0; i < kMagicLen; ++i)
  {
    if (hdr[i] != kMagic[i])
    {
      return false;
    }
  }
  const uint8_t type = hdr[kTypeAt];
  const auto length = static_cast<uint16_t>(hdr[kLengthAt] | (hdr[kLengthAt + 1U] << 8));
  if ((type != static_cast<uint8_t>(FrameType::REQUEST) &&
       type != static_cast<uint8_t>(FrameType::CONTROL_REQUEST)) ||
      length == 0U || length > max_length)
  {
    return false;
  }
  out = Header{static_cast<FrameType>(type), length};
  return true;
}

/// @brief 请求类型对应的响应类型 / The response type that answers a request type.
/// @note 纯函数，任何上下文 / Pure function, any context.
constexpr FrameType ResponseTypeOf(FrameType request)
{
  return request == FrameType::CONTROL_REQUEST ? FrameType::CONTROL_RESPONSE
                                               : FrameType::RESPONSE;
}

}  // namespace XrDap::Net
