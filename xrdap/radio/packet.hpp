// SPDX-License-Identifier: Apache-2.0
#pragma once

// 两块探针之间无线链路的包格式（docs/protocol.md 第 8 节）。纯逻辑，不碰硬件。
//
// 包：6 字节包头，后接最多 fragment 字节载荷，fragment = 最大射频包长 - 6（GFSK 249，
// FLRC 121）。
//   [0] 类型  [1] 序号  [2] 分片序号  [3] 分片数  [4..5] 消息总长，小端
// 类型：
//   1 COMMAND  DAP 命令，主机 -> 远端
//   2 ANSWER   它的应答，远端 -> 主机，序号与命令相同
//   3 PING     主机 -> 远端；载荷：会话号，u16 小端
//   4 PONG     远端 -> 主机；序号相同，载荷：远端现在持有的会话号
//   5 HELLO    配对（identity.hpp；自有布局，没有包头）
//   6 CONTROL  控制请求（op + 载荷），主机 -> 远端；像 COMMAND 一样以同序号的 ANSWER
//              应答，经过同样的会话规则
//   7 CHANNEL  主机 -> 远端，换数据信道；载荷：会话号 u16 小端，信道索引 u8
//              （channel.hpp）
//   8 CHANNEL_ACK  远端 -> 主机；序号相同，载荷同 CHANNEL；发完它远端就换信道
// 总长 total（至多 kMaxMessage）的消息分成 count = max(1, ceil(total / fragment)) 片；
// 第 i 片载有 [i * fragment, min(total, (i + 1) * fragment)) 字节。接收方只接受同一
// (序号, 片数, 总长) 的所有分片都到齐、且每片恰好是应有长度的消息，其余丢弃。
//
// Packet format of the radio link between two probes (docs/protocol.md section 8). Pure
// logic, no hardware.
//
// Packet: a 6-byte header, then up to `fragment` payload bytes, where fragment =
// largest radio packet - 6 (249 bytes in GFSK, 121 in FLRC).
//   [0] kind  [1] sequence  [2] fragment index  [3] fragment count
//   [4..5] message size (total), little endian
// Kinds:
//   1 COMMAND  a DAP command, host -> remote
//   2 ANSWER   its answer, remote -> host, same sequence as the command
//   3 PING     host -> remote; payload: session id, u16 little endian
//   4 PONG     remote -> host; same sequence, payload: the session id it now holds
//   5 HELLO    pairing (identity.hpp; its own layout, no header)
//   6 CONTROL  a control request (op + payload), host -> remote; answered by an ANSWER
//              with the same sequence like a COMMAND, under the same session rules
//   7 CHANNEL  host -> remote, change the data channel; payload: session id u16 little
//              endian, channel index u8 (channel.hpp)
//   8 CHANNEL_ACK  remote -> host; same sequence and payload as the CHANNEL; the remote
//              changes channel once it is sent
// A message of `total` bytes (at most kMaxMessage) travels as count = max(1,
// ceil(total / fragment)) fragments; fragment i carries bytes [i * fragment,
// min(total, (i + 1) * fragment)). A receiver accepts a message only when every fragment
// of one (sequence, count, total) arrived and each fragment has exactly its length;
// anything else is dropped.

#include <cstdint>

namespace XrDap::Radio
{

/// 包类型 / Packet kind.
enum Kind : uint8_t
{
  kCommand = 1,
  kAnswer = 2,
  kPing = 3,
  kPong = 4,
  kHello = 5,
  kControl = 6,
  kChannel = 7,
  kChannelAck = 8,
};

/// 包头字节数 / Header size.
inline constexpr uint8_t kHeader = 6;
/// 一条消息（请求或应答）的最大长度 / Largest message (request or answer).
inline constexpr uint16_t kMaxMessage = 1024;
/// 一条消息最多的分片数（FLRC 的 127 字节包）/ Most fragments of one message (127-byte
/// FLRC packets).
inline constexpr uint8_t kMaxFragments =
    (kMaxMessage + (127 - kHeader) - 1) / (127 - kHeader);
static_assert(kMaxFragments <= 16, "the fragment mask has 16 bits");
/// PING、PONG 的载荷字节数 / Payload size of PING and PONG.
inline constexpr uint8_t kPingPayload = 2;
/// CHANNEL、CHANNEL_ACK 的载荷字节数 / Payload size of CHANNEL and CHANNEL_ACK.
inline constexpr uint8_t kChannelPayload = 3;

/// @brief total 字节的消息分成几片 / Number of fragments of a `total`-byte message.
uint8_t FragmentCount(uint16_t total, uint8_t fragment);

/// @brief 写出消息的第 index 片，返回包长 / Write fragment `index` of a message and
///        return the packet size.
/// @param out 至少 kHeader + fragment 字节 / At least kHeader + fragment bytes.
uint8_t BuildFragment(uint8_t kind, uint8_t sequence, const uint8_t* message,
                      uint16_t total, uint8_t fragment, uint8_t index, uint8_t* out);

/// @brief 写出 PING 或 PONG，返回包长 / Write a PING or PONG and return its size.
uint8_t BuildPing(uint8_t kind, uint8_t sequence, uint16_t session, uint8_t* out);

/// @brief 解析 PING 或 PONG 的会话号 / Parse the session id of a PING or PONG.
bool ParsePing(const uint8_t* packet, int size, uint16_t& session);

/// @brief 写出 CHANNEL 或 CHANNEL_ACK，返回包长 / Write a CHANNEL or CHANNEL_ACK and
///        return its size.
uint8_t BuildChannel(uint8_t kind, uint8_t sequence, uint16_t session, uint8_t channel,
                     uint8_t* out);

/// @brief 解析 CHANNEL 或 CHANNEL_ACK / Parse a CHANNEL or CHANNEL_ACK.
bool ParseChannel(const uint8_t* packet, int size, uint16_t& session, uint8_t& channel);

/// @brief 把分片拼回消息 / Reassembles fragments into a message.
class Reassembler
{
 public:
  /// 一片的处理结果 / Outcome of one fragment.
  enum class Result : uint8_t
  {
    INCOMPLETE,  ///< 还缺分片 / fragments still missing
    COMPLETE,    ///< 消息齐了，Size() 为长度 / the message is complete, see Size()
    REJECTED,   ///< 分片与规范不符，已丢弃 / the fragment breaks the spec and was dropped
    DUPLICATE,  ///< 刚拼齐的消息的分片，已丢弃 / a fragment of the message just
                ///< completed, dropped
  };

  /// @param buffer 消息缓冲，capacity 字节 / Message buffer of `capacity` bytes.
  constexpr Reassembler(uint8_t* buffer, uint16_t capacity)
      : buffer_(buffer), capacity_(capacity)
  {
  }

  /// 丢掉拼了一半的消息 / Drop a partly assembled message.
  void Reset()
  {
    mask_ = 0;
    done_ = false;
  }

  /// @brief 收下一片 / Take one fragment.
  /// @param packet 完整的包（含包头）/ The whole packet including the header.
  /// @param fragment 当前调制下的分片长度 / Fragment size of the current modulation.
  /// @note 序号、片数或总长与在拼的消息不同时，先丢掉在拼的消息。消息拼齐后，同一消息
  ///       要从第 0 片重新开始，其余分片作为 DUPLICATE 丢弃 / A different sequence, count
  ///       or total first drops the message being assembled. After a message completes,
  ///       the same message starts again only with fragment 0; its other fragments are
  ///       dropped as DUPLICATE.
  Result Add(const uint8_t* packet, int size, uint8_t fragment);

  /// 拼齐的消息长度 / Size of the completed message.
  uint16_t Size() const { return total_; }
  /// 拼齐的消息序号 / Sequence of the completed message.
  uint8_t Sequence() const { return sequence_; }

 private:
  uint8_t* buffer_;
  uint16_t capacity_;
  uint16_t mask_ = 0;
  uint16_t total_ = 0;
  uint8_t sequence_ = 0;
  uint8_t count_ = 0;
  bool done_ = false;  // 上一个消息刚拼齐 / the last message just completed
};

}  // namespace XrDap::Radio
