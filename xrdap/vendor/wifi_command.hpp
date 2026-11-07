// SPDX-License-Identifier: Apache-2.0
#pragma once

// 厂商命令 0x86（Wi-Fi 配置，docs/protocol.md 3.7）的编解码：请求的长度和解析，查询应答
// 的编码。不碰设置、NVS 和重启，这些留给平台。控制口（3.7.1）用同一份载荷，只是没有
// 0x86 字节，应答第 1 字节是 op，所以解析从 op 开始，编码由调用者给第 1 个字节。
//   86 00                                  查询
//   86 01 ssid_len ssid pw_len password    保存
//   86 02 A5                               清除（A5 是确认字节）
//
// Codec of vendor command 0x86 (Wi-Fi configuration, docs/protocol.md 3.7): the length
// and parsing of a request and the encoding of the query answer. Settings, NVS and the
// restart are left to the platform. The control port (3.7.1) carries the same payload
// without the 0x86 byte and its answer starts with the op, so parsing starts at the op
// and the caller gives the first byte when encoding.
//   86 00                                  query
//   86 01 ssid_len ssid pw_len password    save
//   86 02 A5                               clear (A5 is the confirm byte)

#include <cstddef>
#include <cstdint>

#include "xrdap/vendor/protocol.hpp"

namespace XrDap::Vendor::WifiCommand
{

/// 命令字节 / Command byte.
inline constexpr uint8_t kCmd = 0x86U;

/// 操作 / Operation.
enum class Op : uint8_t
{
  QUERY = 0x00,  ///< 查询 / Query.
  SAVE = 0x01,   ///< 保存热点参数 / Save the credentials.
  CLEAR = 0x02,  ///< 清除热点参数 / Clear the credentials.
};

/// 清除必须带的确认字节 / Confirm byte a clear must carry.
inline constexpr uint8_t kClearConfirm = 0xA5U;

/// 热点名最长字节数 / Longest SSID in bytes.
inline constexpr size_t kSsidMax = 32U;
/// 密码最短字节数，开放网络（空密码）除外（WPA）/ Shortest password in bytes, except an
/// open network's empty one (WPA).
inline constexpr size_t kPasswordMin = 8U;
/// 密码最长字节数（WPA）/ Longest password in bytes (WPA).
inline constexpr size_t kPasswordMax = 63U;

/// 查询应答的标志位 / Flag bits of the query answer.
inline constexpr uint8_t kFlagStarted = 0x01U;          ///< Wi-Fi 已启动 / Wi-Fi started.
inline constexpr uint8_t kFlagConnected = 0x02U;        ///< 已拿到 IP / Got an IP.
inline constexpr uint8_t kFlagStorageRepaired = 0x04U;  ///< NVS 已重建 / NVS rebuilt.

/// 热点参数最近一次被谁清除；数值即应答里的字节 / Who last cleared the credentials; the
/// values are the byte in the answer.
enum class EraseSource : uint8_t
{
  NONE = 0,            ///< 没有或不知道 / None or unknown.
  BUTTON = 1,          ///< 板上按键 / Board button.
  CONSOLE = 2,         ///< 命令行 wifi clear / Console wifi clear.
  VENDOR_COMMAND = 3,  ///< 0x86 清除 / 0x86 clear.
  STORAGE_REPAIR = 4,  ///< NVS 修复 / NVS repair.
};

/// 这次启动的复位原因；数值即应答里的字节 / Reset reason of this boot; the values are the
/// byte in the answer.
enum class ResetReason : uint8_t
{
  UNKNOWN = 0,   ///< 不知道 / Unknown.
  POWER_ON = 1,  ///< 上电 / Power-on.
  SOFTWARE = 2,  ///< 软件重启 / Software restart.
  WATCHDOG = 3,  ///< 看门狗 / Watchdog.
  PANIC = 4,     ///< 程序异常 / Panic.
  BROWNOUT = 5,  ///< 欠压 / Brown-out.
  OTHER = 6,     ///< 其他 / Other.
};

/// 解析出的请求；热点名和密码指向请求缓冲，不以 0 结尾 / A parsed request; the SSID and
/// password point into the request buffer and are not NUL-terminated.
struct Request
{
  Op op;                 ///< 操作 / Operation.
  const char* ssid;      ///< SAVE 的热点名 / SAVE: the SSID.
  uint8_t ssid_len;      ///< 热点名字节数 / SSID bytes.
  const char* password;  ///< SAVE 的密码 / SAVE: the password.
  uint8_t password_len;  ///< 密码字节数，0 为开放网络 / Password bytes, 0 is open.
};

/// 解析结果 / Parse outcome.
enum class Parse : uint8_t
{
  OK,            ///< 请求合法 / The request is valid.
  UNKNOWN_OP,    ///< op 未定义 / The op is undefined.
  BAD_ARGUMENT,  ///< 长度与 op 不符、热点名或密码长度不合法、清除缺确认字节 / The length
                 ///< does not fit the op, the SSID or password length is invalid, or a
                 ///< clear lacks its confirm byte.
};

/// @brief DAP_ExecuteCommands / DAP_QueueCommands 里一条 0x86 请求的字节数；签名与
///        DapVendor::ExtraLength 相同 / Request bytes of one 0x86 command inside
///        DAP_ExecuteCommands / DAP_QueueCommands; the signature matches
///        DapVendor::ExtraLength.
/// @param req 以 0x86 开头 / Starts with 0x86.
/// @return 内容不完整或 op 未定义时 false / False when the content is incomplete or the
/// op
///         is undefined.
/// @note 纯解析，任何上下文 / Pure parsing, any context.
inline bool RequestLength(const uint8_t* req, uint16_t avail, uint16_t& len)
{
  if (avail < 2U || req[0] != kCmd)
  {
    return false;
  }
  uint32_t need = 0U;
  switch (static_cast<Op>(req[1]))
  {
    case Op::QUERY:
      need = 2U;
      break;
    case Op::CLEAR:
      need = 3U;
      break;
    case Op::SAVE:
      // 86 01 ssid_len ssid pw_len password
      if (avail < 3U)
      {
        return false;
      }
      need = 3U + req[2];
      if (need >= avail)
      {
        return false;
      }
      need += 1U + req[need];
      break;
    default:
      return false;
  }
  if (need > avail)
  {
    return false;
  }
  len = static_cast<uint16_t>(need);
  return true;
}

/// @brief 解析一条请求，检查 docs/protocol.md 3.7 的全部规则 / Parse a request and check
///        every rule of docs/protocol.md 3.7.
/// @param body op 和载荷，不含 0x86 字节 / The op and payload, without the 0x86 byte.
/// @param len body 的字节数，要等于请求的实际长度 / Bytes of body; must equal the
///        request's actual length.
/// @param out 成功时的请求 / The request on success.
/// @note 纯解析，任何上下文 / Pure parsing, any context.
inline Parse ParseRequest(const uint8_t* body, uint16_t len, Request& out)
{
  if (len < 1U)
  {
    return Parse::BAD_ARGUMENT;
  }
  out = Request{static_cast<Op>(body[0]), nullptr, 0U, nullptr, 0U};
  switch (out.op)
  {
    case Op::QUERY:
      return len == 1U ? Parse::OK : Parse::BAD_ARGUMENT;
    case Op::CLEAR:
      return (len == 2U && body[1] == kClearConfirm) ? Parse::OK : Parse::BAD_ARGUMENT;
    case Op::SAVE:
    {
      // op ssid_len ssid pw_len password
      if (len < 3U)
      {
        return Parse::BAD_ARGUMENT;
      }
      const size_t ssid_len = body[1];
      if (len < 2U + ssid_len + 1U)
      {
        return Parse::BAD_ARGUMENT;
      }
      const size_t pw_len = body[2U + ssid_len];
      if (len != 3U + ssid_len + pw_len || ssid_len < 1U || ssid_len > kSsidMax ||
          (pw_len != 0U && (pw_len < kPasswordMin || pw_len > kPasswordMax)))
      {
        return Parse::BAD_ARGUMENT;
      }
      out.ssid = reinterpret_cast<const char*>(&body[2]);
      out.ssid_len = static_cast<uint8_t>(ssid_len);
      out.password = reinterpret_cast<const char*>(&body[3U + ssid_len]);
      out.password_len = static_cast<uint8_t>(pw_len);
      return Parse::OK;
    }
    default:
      return Parse::UNKNOWN_OP;
  }
}

/// 查询应答的内容 / Content of the query answer.
struct Query
{
  /// Wi-Fi 已启动 / Wi-Fi started.
  bool started;
  /// 已拿到 IP / Got an IP.
  bool connected;
  /// 这次启动时 NVS 已损坏并重建 / NVS was damaged and rebuilt at this boot.
  bool storage_repaired;
  /// IPv4，网络字节序，未连接为 0 / IPv4 in network order, 0 when not connected.
  uint8_t ip[4];
  /// dBm，未连接为 0 / dBm, 0 when not connected.
  int8_t rssi;
  /// 已保存的热点名，不以 0 结尾 / The saved SSID, not NUL-terminated.
  const char* ssid;
  /// 热点名字节数，不超过 kSsidMax，没有为 0 / SSID bytes, at most kSsidMax, 0 for none.
  uint8_t ssid_len;
  /// 最近一次清除来源 / Who last cleared.
  EraseSource last_erase;
  /// 这次启动的复位原因 / Reset reason of this boot.
  ResetReason reset_reason;
};

/// @brief 编码查询的成功应答；缓冲放不下时编码状态 0x03 / Encode the successful query
///        answer; when the buffer is too small, encode status 0x03 instead.
/// @param first 应答第 1 字节：DAP 上是 0x86，控制口上是 op / The first byte: 0x86 on
///        DAP, the op on the control port.
/// @return 应答字节数；cap 小于 2 时 0 / Answer bytes; 0 when cap is below 2.
/// @note 纯函数，任何上下文 / Pure function, any context.
inline uint16_t EncodeQuery(uint8_t first, const Query& q, uint8_t* resp, uint16_t cap)
{
  if (cap < 2U)
  {
    return 0U;
  }
  // first status flags ip(4) rssi ssid_len ssid erase reset
  const size_t total = 1U + 1U + 1U + sizeof(q.ip) + 1U + 1U + q.ssid_len + 1U + 1U;
  size_t i = 0U;
  resp[i++] = first;
  if (cap < total)
  {
    resp[i++] = kStatusResource;
    return static_cast<uint16_t>(i);
  }
  resp[i++] = kStatusOk;
  resp[i++] = static_cast<uint8_t>((q.started ? kFlagStarted : 0U) |
                                   (q.connected ? kFlagConnected : 0U) |
                                   (q.storage_repaired ? kFlagStorageRepaired : 0U));
  for (const uint8_t octet : q.ip)
  {
    resp[i++] = octet;
  }
  resp[i++] = static_cast<uint8_t>(q.rssi);
  resp[i++] = q.ssid_len;
  for (size_t k = 0; k < q.ssid_len; ++k)
  {
    resp[i++] = static_cast<uint8_t>(q.ssid[k]);
  }
  resp[i++] = static_cast<uint8_t>(q.last_erase);
  resp[i++] = static_cast<uint8_t>(q.reset_reason);
  return static_cast<uint16_t>(i);
}

}  // namespace XrDap::Vendor::WifiCommand
