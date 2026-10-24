// SPDX-License-Identifier: Apache-2.0
#pragma once

// 串口链路的帧（docs/protocol.md 第 1 节，与 XRDAP-Host 的串口传输一致）：
//   0x5A + data_len(3) + topic_crc32(4) + timestamp_us(6) + version 1 + header_crc8 +
//   payload + crc8，payload = n(2) + DAP 包(n)。
// 请求的 topic 是 dap_req，响应的是 dap_resp。帧头 CRC8 或尾 CRC8 错、版本或 topic 不对、
// 长度不合法时丢掉当前候选帧，从缓冲里下一个 0x5A 重新开始。这里只拼帧、校验、打包，
// 不碰串口，也不执行命令。
//
// Frames of the serial link (docs/protocol.md section 1, matching the serial transport
// of XRDAP-Host):
//   0x5A + data_len(3) + topic_crc32(4) + timestamp_us(6) + version 1 + header_crc8 +
//   payload + crc8, with payload = n(2) + DAP packet(n).
// Requests use topic dap_req, responses dap_resp. A wrong header or trailing CRC8, a
// wrong version or topic, or a bad length drops the current candidate and parsing
// restarts at the next 0x5A in the buffer. This only assembles, checks and packs frames;
// it touches no UART and runs no command.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace XrDap::Serial
{

/// 帧前缀 / Frame prefix.
inline constexpr uint8_t kPrefix = 0x5AU;
/// 帧版本 / Frame version.
inline constexpr uint8_t kVersion = 0x01U;
/// 帧头字节数 / Header bytes.
inline constexpr size_t kHeaderSize = 16U;
/// 请求 topic "dap_req" 的 CRC32 / CRC32 of the request topic "dap_req".
inline constexpr uint32_t kRequestTopic = 0xDC8309C5U;
/// 响应 topic "dap_resp" 的 CRC32 / CRC32 of the response topic "dap_resp".
inline constexpr uint32_t kResponseTopic = 0x89E1A688U;

/// @brief CRC8：反射多项式 0x8C，初值 0xFF / CRC8: reflected polynomial 0x8C, initial
///        value 0xFF.
/// @note 纯函数，任何上下文 / Pure function, any context.
uint8_t Crc8(const uint8_t* data, size_t len);

/// @brief CRC32：反射多项式 0xEDB88320，初值 0xFFFFFFFF，结果不取反 / CRC32: reflected
///        polynomial 0xEDB88320, initial value 0xFFFFFFFF, no final inversion.
/// @note 纯函数，任何上下文 / Pure function, any context.
uint32_t Crc32(const uint8_t* data, size_t len);

/// 帧解析的统计 / Frame parsing statistics.
struct FrameStats
{
  uint32_t frames = 0U;         ///< 交出的请求数 / Requests handed out.
  uint32_t crc_errors = 0U;     ///< 头或尾 CRC8 错 / Header or trailing CRC8 errors.
  uint32_t header_errors = 0U;  ///< 版本、topic、长度不对 / Bad version, topic, length.
  uint32_t dropped = 0U;        ///< 丢掉的字节和帧 / Dropped bytes and frames.
};

/// @brief 请求帧的拼接和响应帧的打包 / Request frame assembly and response packing.
/// @tparam MaxPacket DAP 包长上限 / Maximum DAP packet size.
/// @note 单线程使用（串口的接收上下文）/ Single-threaded (the serial receive context).
template <uint16_t MaxPacket>
class Framer
{
 public:
  static_assert(MaxPacket > 0U && MaxPacket <= 0xF000U, "MaxPacket out of range");

  /// 载荷最大字节数：n(2) + DAP 包 / Largest payload: n(2) + DAP packet.
  static constexpr size_t kPayloadCapacity = 2U + MaxPacket;
  /// 一帧的最大字节数 / Largest frame.
  static constexpr size_t kFrameCapacity = kHeaderSize + kPayloadCapacity + 1U;

  /// @brief 送入收到的字节；每拼出一个合法请求就调用一次 on_request(dap, len) / Feed
  ///        received bytes; on_request(dap, len) is called for every valid request.
  /// @return 这次交出的请求数 / Requests handed out by this call.
  template <typename Fn>
  uint32_t Feed(const uint8_t* data, size_t len, Fn&& on_request)
  {
    uint32_t handed = 0U;
    size_t index = 0U;
    while (index < len)
    {
      if (fill_ == 0U)
      {
        // 还没有候选帧：在输入里找前缀，跳过的字节算丢弃 / No candidate yet: find the
        // prefix in the input; skipped bytes count as dropped.
        const void* hit = std::memchr(data + index, kPrefix, len - index);
        if (hit == nullptr)
        {
          stats_.dropped += static_cast<uint32_t>(len - index);
          return handed;
        }
        const size_t at = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data);
        stats_.dropped += static_cast<uint32_t>(at - index);
        index = at;
      }
      const size_t target = (total_ != 0U) ? total_ : kHeaderSize;
      if (fill_ < target)
      {
        const size_t want = target - fill_;
        const size_t take = (want < len - index) ? want : len - index;
        std::memcpy(frame_ + fill_, data + index, take);
        fill_ += take;
        index += take;
      }
      handed += Advance(on_request);
    }
    return handed;
  }

  /// @brief 把一条响应打包成帧 / Pack one response into a frame.
  /// @param out 至少 kFrameCapacity 字节 / At least kFrameCapacity bytes.
  /// @return 帧的字节数 / Frame bytes.
  static size_t PackResponse(const uint8_t* dap, uint16_t len, uint64_t timestamp_us,
                             uint8_t* out)
  {
    if (len > MaxPacket)
    {
      len = MaxPacket;
    }
    const uint32_t payload = 2U + len;
    out[0] = kPrefix;
    out[1] = static_cast<uint8_t>(payload);
    out[2] = static_cast<uint8_t>(payload >> 8);
    out[3] = static_cast<uint8_t>(payload >> 16);
    for (int i = 0; i < 4; ++i)
    {
      out[4 + i] = static_cast<uint8_t>(kResponseTopic >> (8 * i));
    }
    for (int i = 0; i < 6; ++i)
    {
      out[8 + i] = static_cast<uint8_t>(timestamp_us >> (8 * i));
    }
    out[14] = kVersion;
    out[15] = Crc8(out, kHeaderSize - 1U);
    out[16] = static_cast<uint8_t>(len);
    out[17] = static_cast<uint8_t>(len >> 8);
    if (len != 0U)
    {
      std::memcpy(out + 18, dap, len);
    }
    const size_t body = kHeaderSize + payload;
    out[body] = Crc8(out, body);
    return body + 1U;
  }

  /// 统计 / Statistics.
  const FrameStats& Stats() const { return stats_; }
  /// 清零统计 / Clear the statistics.
  void ResetStats() { stats_ = FrameStats{}; }

 private:
  // 用缓冲里已有的字节把解析推进到不能再推进。重新同步后缓冲里可能留下不止一帧头的
  // 字节，所以这里按需丢掉开头的字节、处理完一帧后保留它后面的字节，缓冲从不写出界。
  // Advance the parser as far as the buffered bytes allow. After a resync the buffer may
  // hold more than a header, so leading bytes are dropped as needed and the bytes after a
  // finished frame are kept; the buffer is never written past its end.
  template <typename Fn>
  uint32_t Advance(Fn& on_request)
  {
    uint32_t handed = 0U;
    for (;;)
    {
      if (fill_ == 0U)
      {
        return handed;
      }
      if (frame_[0] != kPrefix)
      {
        DropFront(0U);
        continue;
      }
      if (total_ == 0U)
      {
        if (fill_ < kHeaderSize)
        {
          return handed;
        }
        if (!AcceptHeader())
        {
          DropFront(1U);
          continue;
        }
      }
      if (fill_ < total_)
      {
        return handed;
      }
      if (Crc8(frame_, total_ - 1U) != frame_[total_ - 1U])
      {
        ++stats_.crc_errors;
        DropFront(1U);
        continue;
      }
      const uint8_t* payload = frame_ + kHeaderSize;
      const uint32_t n = payload[0] | (static_cast<uint32_t>(payload[1]) << 8);
      if (n > MaxPacket || n + 2U > payload_len_)
      {
        ++stats_.dropped;
      }
      else
      {
        ++stats_.frames;
        ++handed;
        on_request(payload + 2, static_cast<uint16_t>(n));
      }
      const size_t rest = fill_ - total_;
      std::memmove(frame_, frame_ + total_, rest);
      fill_ = rest;
      total_ = 0U;
    }
  }

  bool AcceptHeader()
  {
    if (Crc8(frame_, kHeaderSize - 1U) != frame_[kHeaderSize - 1U])
    {
      ++stats_.crc_errors;
      return false;
    }
    const uint32_t topic = frame_[4] | (static_cast<uint32_t>(frame_[5]) << 8) |
                           (static_cast<uint32_t>(frame_[6]) << 16) |
                           (static_cast<uint32_t>(frame_[7]) << 24);
    const uint32_t len = frame_[1] | (static_cast<uint32_t>(frame_[2]) << 8) |
                         (static_cast<uint32_t>(frame_[3]) << 16);
    if (frame_[14] != kVersion || topic != kRequestTopic || len < 2U ||
        len > kPayloadCapacity)
    {
      ++stats_.header_errors;
      return false;
    }
    payload_len_ = len;
    total_ = kHeaderSize + len + 1U;
    return true;
  }

  // 丢掉开头 skip 字节，再丢到下一个前缀为止；放弃当前候选帧 / Drop `skip` leading bytes,
  // then up to the next prefix; the current candidate is abandoned.
  void DropFront(size_t skip)
  {
    size_t next = skip;
    while (next < fill_ && frame_[next] != kPrefix)
    {
      ++next;
    }
    stats_.dropped += static_cast<uint32_t>(next);
    std::memmove(frame_, frame_ + next, fill_ - next);
    fill_ -= next;
    total_ = 0U;
  }

  uint8_t frame_[kFrameCapacity] = {};
  size_t fill_ = 0U;
  size_t total_ = 0U;
  uint32_t payload_len_ = 0U;
  FrameStats stats_;
};

}  // namespace XrDap::Serial
