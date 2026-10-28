// SPDX-License-Identifier: Apache-2.0
#pragma once

// 串口链路的帧（docs/protocol.md 第 1 节，与 XRDAP-Host 的串口传输一致）。帧头和尾是
// libxr Topic 的打包格式（Topic::PackedDataHeader、PACKET_PREFIX、PACKET_VERSION，CRC 用
// LibXR::CRC8、CRC32），这里只有 payload 和流式重同步：
//   0x5A + data_len(3) + topic_crc32(4) + timestamp_us(6) + version 1 + header_crc8 +
//   payload + crc8，payload = n(2) + DAP 包(n)。
// 请求的 topic 是 dap_req，响应的是 dap_resp；控制请求是 ctrl_req，响应是 ctrl_resp。帧头
// CRC8 或尾 CRC8 错、版本或 topic 不对、长度不合法时丢掉当前候选帧，从缓冲里下一个 0x5A
// 重新开始。这里只拼帧、校验、打包，不碰串口，也不执行命令。
//
// Frames of the serial link (docs/protocol.md section 1, matching the serial transport
// of XRDAP-Host). The header and trailer are libxr's Topic packing format
// (Topic::PackedDataHeader, PACKET_PREFIX, PACKET_VERSION, CRCs from LibXR::CRC8 and
// CRC32); only the payload and the streaming resync live here:
//   0x5A + data_len(3) + topic_crc32(4) + timestamp_us(6) + version 1 + header_crc8 +
//   payload + crc8, with payload = n(2) + DAP packet(n).
// Requests use topic dap_req, responses dap_resp; control requests use ctrl_req and
// responses ctrl_resp. A wrong header or trailing CRC8, a wrong version or topic, or a
// bad length drops the current candidate and parsing restarts at the next 0x5A in the
// buffer. This only assembles, checks and packs frames; it touches no UART and runs no
// command.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "crc.hpp"
#include "topic.hpp"

namespace XrDap::Serial
{

/// 帧前缀 / Frame prefix.
inline constexpr uint8_t kPrefix = LibXR::Topic::PACKET_PREFIX;
/// 帧版本 / Frame version.
inline constexpr uint8_t kVersion = LibXR::Topic::PACKET_VERSION;
/// 帧头字节数 / Header bytes.
inline constexpr size_t kHeaderSize = sizeof(LibXR::Topic::PackedDataHeader);
/// USB 全速 bulk 端点的包长 / Packet size of a USB full-speed bulk endpoint.
inline constexpr size_t kUsbPacket = 64U;
/// 补的字节 / The pad byte.
inline constexpr uint8_t kPad = 0x00U;
/// 请求 topic 的名字 / Name of the request topic.
inline constexpr char kRequestTopicName[] = "dap_req";
/// 响应 topic 的名字 / Name of the response topic.
inline constexpr char kResponseTopicName[] = "dap_resp";
/// 控制请求 topic 的名字:探针自己的设置,不是 DAP 命令 / Name of the control request
/// topic: the probe's own settings, not a DAP command.
inline constexpr char kControlRequestTopicName[] = "ctrl_req";
/// 控制响应 topic 的名字 / Name of the control response topic.
inline constexpr char kControlResponseTopicName[] = "ctrl_resp";

/// topic 名字的 CRC32 键，与 libxr Topic 的算法相同 / The CRC32 key of a topic name, the
/// same as libxr's Topic computes.
inline uint32_t TopicKey(const char* name)
{
  return LibXR::CRC32::Calculate(name, std::strlen(name));
}

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

  /// @brief 送入收到的字节；每拼出一个合法请求就调用一次 on_request(data, len, control)，
  ///        control 为真表示它是控制请求 / Feed received bytes; on_request(data, len,
  ///        control) is called for every valid request, control true for a control
  ///        request.
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
        // 还没有候选帧：在输入里找前缀，跳过前面的字节 / No candidate yet: find the
        // prefix in the input and skip the bytes before it.
        const void* hit = std::memchr(data + index, kPrefix, len - index);
        if (hit == nullptr)
        {
          return handed;
        }
        const size_t at = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data);
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
                             uint8_t* out, bool control = false)
  {
    if (len > MaxPacket)
    {
      len = MaxPacket;
    }
    const uint32_t payload = 2U + len;
    LibXR::Topic::PackedDataHeader header;
    header.prefix = kPrefix;
    header.SetDataLen(payload);
    header.topic_name_crc32 =
        TopicKey(control ? kControlResponseTopicName : kResponseTopicName);
    header.SetTimestamp(LibXR::MicrosecondTimestamp(timestamp_us));
    header.version = kVersion;
    header.pack_header_crc8 =
        LibXR::CRC8::Calculate(&header, kHeaderSize - sizeof(header.pack_header_crc8));
    std::memcpy(out, &header, kHeaderSize);
    out[kHeaderSize] = static_cast<uint8_t>(len);
    out[kHeaderSize + 1U] = static_cast<uint8_t>(len >> 8);
    if (len != 0U)
    {
      std::memcpy(out + kHeaderSize + 2U, dap, len);
    }
    const size_t body = kHeaderSize + payload;
    out[body] = LibXR::CRC8::Calculate(out, body);
    return body + 1U;
  }

  /// 发送缓冲至少要的字节数：一帧加一个补的字节 / Bytes a send buffer needs at least: a
  /// frame plus one pad byte.
  static constexpr size_t kTxCapacity = kFrameCapacity + 1U;

  /// @brief 帧长为 64 的整数倍时在后面补一个 0x00。经 USB 转串口（CP210x、CH343、
  ///        USB Serial/JTAG）送到主机时，刚好填满全速 bulk 包的数据要等下一个短包才
  ///        交给主机程序；补的字节让最后一个包变短。接收端丢掉 0x5A 之前的字节 / Append
  ///        one 0x00 after a frame whose length is a multiple of 64. Over a USB-serial
  ///        bridge (CP210x, CH343, USB Serial/JTAG) data that fills full-speed bulk
  ///        packets exactly reaches the host program only with the next short packet;
  ///        the pad makes the last packet short. Receivers drop bytes before 0x5A.
  /// @param out 至少 kTxCapacity 字节，前 len 字节是一帧 / At least kTxCapacity bytes,
  ///        the first len of them one frame.
  /// @return 要发出的字节数 / Bytes to send.
  /// @note 纯函数，任何上下文 / Pure function, any context.
  static size_t AppendPad(uint8_t* out, size_t len)
  {
    if (len % kUsbPacket != 0U)
    {
      return len;
    }
    out[len] = kPad;
    return len + 1U;
  }

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
      if (!LibXR::CRC8::Verify(frame_, total_))
      {
        DropFront(1U);
        continue;
      }
      const uint8_t* payload = frame_ + kHeaderSize;
      const uint32_t n = payload[0] | (static_cast<uint32_t>(payload[1]) << 8);
      if (n <= MaxPacket && n + 2U <= payload_len_)
      {
        ++handed;
        on_request(payload + 2, static_cast<uint16_t>(n), control_frame_);
      }
      const size_t rest = fill_ - total_;
      std::memmove(frame_, frame_ + total_, rest);
      fill_ = rest;
      total_ = 0U;
    }
  }

  bool AcceptHeader()
  {
    LibXR::Topic::PackedDataHeader header;
    std::memcpy(&header, frame_, kHeaderSize);
    if (!LibXR::CRC8::Verify(frame_, kHeaderSize))
    {
      return false;
    }
    const uint32_t len = header.GetDataLen();
    const bool control = header.topic_name_crc32 == control_topic_;
    if (header.version != kVersion ||
        (header.topic_name_crc32 != request_topic_ && !control) || len < 2U ||
        len > kPayloadCapacity)
    {
      return false;
    }
    control_frame_ = control;
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
    std::memmove(frame_, frame_ + next, fill_ - next);
    fill_ -= next;
    total_ = 0U;
  }

  uint8_t frame_[kFrameCapacity] = {};
  size_t fill_ = 0U;
  size_t total_ = 0U;
  uint32_t payload_len_ = 0U;
  uint32_t request_topic_ = TopicKey(kRequestTopicName);
  uint32_t control_topic_ = TopicKey(kControlRequestTopicName);
  bool control_frame_ = false;
};

}  // namespace XrDap::Serial
