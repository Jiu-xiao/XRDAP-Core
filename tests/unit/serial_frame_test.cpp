// SPDX-License-Identifier: Apache-2.0
// 串口帧。判据：docs/protocol.md 第 1 节（帧布局、CRC 参数、topic 的 CRC32、完整的
// DAP_Info 请求帧示例和丢帧规则）和 3.7.1（控制帧的 topic）。这里的帧都按文档逐字节拼，
// 不经过被测代码和 libxr 的 CRC / Serial frames. Oracle: docs/protocol.md section 1
// (layout, CRC parameters, the topic CRC32s, the complete DAP_Info request example and
// the drop rules) and 3.7.1 (the topics of control frames). Frames here are built byte by
// byte from the document, not through the code under test or libxr's CRC.
#include "xrdap/serial/serial_frame.hpp"

#include <vector>

#include "check.hpp"

using namespace XrDap::Serial;

namespace
{
using Bytes = std::vector<uint8_t>;
using Requests = std::vector<Bytes>;

constexpr uint32_t kDapReq = 0xDC8309C5U;
constexpr uint32_t kDapResp = 0x89E1A688U;
constexpr uint32_t kCtrlReq = 0x3D2538F2U;
constexpr uint32_t kCtrlResp = 0x31BDA5B6U;

// 文档第 1 节的例子：DAP_Info(0xFF) 请求，DAP 包 00 FF / The example of section 1:
// the DAP_Info(0xFF) request, DAP packet 00 FF.
const Bytes kInfoFrame = {0x5A, 0x04, 0x00, 0x00, 0xC5, 0x09, 0x83,
                          0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x01, 0xB1, 0x02, 0x00, 0x00, 0xFF, 0x32};
const Bytes kInfoPacket = {0x00, 0xFF};

// 文档第 1 节的 CRC8：反射多项式 0x8C，初值 0xFF / The CRC8 of section 1: reflected
// polynomial 0x8C, initial value 0xFF.
uint8_t Crc8(const Bytes& data)
{
  uint8_t crc = 0xFFU;
  for (const uint8_t byte : data)
  {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
    {
      crc = (crc & 1U) != 0U ? static_cast<uint8_t>((crc >> 1) ^ 0x8CU)
                             : static_cast<uint8_t>(crc >> 1);
    }
  }
  return crc;
}

// 按第 1 节打一帧，时间戳为 0；version 和帧头 CRC 可以给错，尾 CRC 总是对的 / Pack a
// frame by section 1 with timestamp 0; the version and the header CRC can be made wrong,
// the trailing CRC is always right.
Bytes PackFrame(uint32_t topic, const Bytes& payload, uint8_t version = 0x01U,
                uint8_t header_crc_flip = 0x00U)
{
  const auto len = static_cast<uint32_t>(payload.size());
  Bytes f = {0x5A,
             static_cast<uint8_t>(len),
             static_cast<uint8_t>(len >> 8),
             static_cast<uint8_t>(len >> 16),
             static_cast<uint8_t>(topic),
             static_cast<uint8_t>(topic >> 8),
             static_cast<uint8_t>(topic >> 16),
             static_cast<uint8_t>(topic >> 24),
             0x00,
             0x00,
             0x00,
             0x00,
             0x00,
             0x00,
             version};
  f.push_back(static_cast<uint8_t>(Crc8(f) ^ header_crc_flip));
  f.insert(f.end(), payload.begin(), payload.end());
  f.push_back(Crc8(f));
  return f;
}

// 一帧的 payload 是 n(2) + DAP 包(n) / The payload of a frame is n(2) + DAP packet(n).
Bytes PacketFrame(const Bytes& packet, uint32_t topic = kDapReq)
{
  Bytes payload = {static_cast<uint8_t>(packet.size()),
                   static_cast<uint8_t>(packet.size() >> 8)};
  payload.insert(payload.end(), packet.begin(), packet.end());
  return PackFrame(topic, payload);
}

Bytes Join(const Bytes& a, const Bytes& b)
{
  Bytes all = a;
  all.insert(all.end(), b.begin(), b.end());
  return all;
}

struct Sink
{
  Requests requests;
  std::vector<bool> control;
  void operator()(const uint8_t* dap, uint16_t len, bool is_control)
  {
    requests.emplace_back(dap, dap + len);
    control.push_back(is_control);
  }
};

// bad 被丢掉，紧跟在后面的好帧照常交出 / bad is dropped, and the good frame right behind
// it still comes out.
bool DroppedAndResyncs(const Bytes& bad)
{
  const Bytes in = Join(bad, kInfoFrame);
  Framer<1024> f;
  Sink sink;
  (void)f.Feed(in.data(), in.size(), sink);
  return sink.requests == Requests{kInfoPacket};
}
}  // namespace

TEST(FrameSplitAcrossFeeds)
{
  Framer<1024> f;
  Sink sink;
  for (const uint8_t b : kInfoFrame)
  {
    (void)f.Feed(&b, 1, sink);
  }
  CHECK(sink.requests == Requests{kInfoPacket});
}

TEST(GarbageBeforeAndBetweenFrames)
{
  Framer<1024> f;
  Sink sink;
  Bytes in = {0x00, 0x5A, 0x13};  // 有一个假前缀 / one false prefix
  in.insert(in.end(), kInfoFrame.begin(), kInfoFrame.end());
  in.push_back(0x77);
  in.insert(in.end(), kInfoFrame.begin(), kInfoFrame.end());
  (void)f.Feed(in.data(), in.size(), sink);
  CHECK(sink.requests == (Requests{kInfoPacket, kInfoPacket}));
}

TEST(BadHeaderCrcIsDropped)
{
  CHECK(DroppedAndResyncs(PackFrame(kDapReq, {0x02, 0x00, 0x00, 0xFF}, 0x01U, 0x01U)));
}

TEST(WrongVersionIsDropped)
{
  CHECK(DroppedAndResyncs(PackFrame(kDapReq, {0x02, 0x00, 0x00, 0xFF}, 0x02U)));
}

TEST(WrongTopicIsDropped)
{
  CHECK(DroppedAndResyncs(PacketFrame({0x00, 0x02}, kDapResp)));
}

TEST(PacketLongerThanItsPayloadIsDropped)
{
  // data_len 4 放不下 n = 3 / A data_len of 4 has no room for n = 3.
  CHECK(DroppedAndResyncs(PackFrame(kDapReq, {0x03, 0x00, 0x00, 0xFF})));
}

TEST(PayloadAboveThePacketLimitIsDropped)
{
  // 包长 2：data_len 最大 2 + 2 / Packet size 2: data_len is at most 2 + 2.
  Framer<2> f;
  Sink sink;
  const Bytes above = PackFrame(kDapReq, {0x02, 0x00, 0x00, 0xFF, 0x00});
  (void)f.Feed(above.data(), above.size(), sink);
  CHECK(sink.requests.empty());
  (void)f.Feed(kInfoFrame.data(), kInfoFrame.size(), sink);
  CHECK(sink.requests == Requests{kInfoPacket});
}

TEST(BadTrailingCrcResyncsInsideTheFrame)
{
  // 一个头合法、尾 CRC 错的帧，42 字节载荷里是两个完整的请求帧：重新同步后缓冲里留下的
  // 字节含两帧，两个请求都要找出来 / A frame with a valid header and a wrong trailing
  // CRC whose 42-byte payload is two complete request frames: after the resync the
  // buffered bytes hold both, and both requests must be found.
  Bytes in = PackFrame(kDapReq, Join(kInfoFrame, kInfoFrame));
  in.back() ^= 0xFFU;
  Framer<1024> f;
  Sink sink;
  (void)f.Feed(in.data(), in.size(), sink);
  CHECK(sink.requests == (Requests{kInfoPacket, kInfoPacket}));
}

TEST(ResponseFrameLayout)
{
  uint8_t out[Framer<1024>::kFrameCapacity];
  const uint8_t dap[] = {0x00, 0x02, 0x00, 0x04};
  const size_t n = Framer<1024>::PackResponse(dap, 4, 0x0000123456789ABCULL, out);
  // 两个 CRC 按第 1 节的参数算出 / Both CRCs worked out with the parameters of section 1.
  const Bytes expect = {0x5A, 0x06, 0x00, 0x00, 0x88, 0xA6, 0xE1, 0x89,
                        0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, 0x01, 0x5C,
                        0x04, 0x00, 0x00, 0x02, 0x00, 0x04, 0xF2};
  CHECK(Bytes(out, out + n) == expect);
}

TEST(ResponseOfAnyLengthIsPackedTheDocumentedWay)
{
  // 没有响应内容时 n 为 0；n 和 data_len 超过一个字节时按小端 / Without content n is 0; n
  // and data_len are little-endian beyond one byte.
  uint8_t out[Framer<1024>::kFrameCapacity];
  for (const uint16_t len : {0U, 1U, 258U, 1024U})
  {
    const Bytes dap(len, 0xA5);
    const size_t n = Framer<1024>::PackResponse(dap.data(), len, 0U, out);
    CHECK(Bytes(out, out + n) == PacketFrame(dap, kDapResp));
  }
}

TEST(FrameOfWholeUsbPacketsGetsOnePadByte)
{
  // 帧长为 64 的整数倍时后面补一个 0x00：USB 全速 bulk 包刚好填满时，主机侧的 USB 转
  // 串口读要等下一个短包才交出数据 / A frame whose length is a multiple of 64 is
  // followed by one 0x00: when it fills USB full-speed bulk packets exactly, the host's
  // USB-serial read waits for the next short packet before handing the data over.
  uint8_t out[Framer<1024>::kTxCapacity];
  const uint8_t dap[1024] = {};
  for (uint16_t n = 0; n <= 1024; ++n)
  {
    const size_t frame = Framer<1024>::PackResponse(dap, n, 0U, out);
    out[frame] = 0xEEU;
    const size_t sent = Framer<1024>::AppendPad(out, frame);
    if (frame % 64U == 0U)
    {
      CHECK_EQ(sent, frame + 1U);
      CHECK_EQ(out[frame], 0x00U);
    }
    else
    {
      CHECK_EQ(sent, frame);
      CHECK_EQ(out[frame], 0xEEU);
    }
  }
}

TEST(PaddedStreamYieldsTheSameRequests)
{
  // 接收端丢掉 0x5A 之前的字节：补过字节的流与不补时交出同样的请求。DAP 包 45 字节的帧长
  // 64，301 字节的帧长 320 / The receiver drops bytes before 0x5A: a padded stream hands
  // out the same requests as an unpadded one. A 45-byte packet makes a 64-byte frame, a
  // 301-byte packet a 320-byte frame.
  Bytes stream;
  Requests expect;
  for (const uint16_t n : {45U, 2U, 301U, 45U})
  {
    expect.emplace_back(n, static_cast<uint8_t>(n));
    Bytes f = PacketFrame(expect.back());
    const size_t len = f.size();
    f.resize(Framer<1024>::kTxCapacity);
    f.resize(Framer<1024>::AppendPad(f.data(), len));
    stream.insert(stream.end(), f.begin(), f.end());
  }
  CHECK_EQ(stream.size(), 64U + 1U + 21U + 320U + 1U + 64U + 1U);
  Framer<1024> fr;
  Sink sink;
  CHECK_EQ(fr.Feed(stream.data(), stream.size(), sink), 4U);
  CHECK(sink.requests == expect);
}

TEST(ControlRequestsAreMarkedAndKeepOrder)
{
  // ctrl_req 帧交给回调时 control 为真,与 dap_req 帧按顺序混在一起也不串 / A ctrl_req
  // frame reaches the callback with control set, and mixed with dap_req frames the order
  // and the flags hold.
  Framer<1024> f;
  Sink sink;
  const Bytes in =
      Join(Join(PacketFrame(kInfoPacket), PacketFrame({0x01, 0x02, 0x03}, kCtrlReq)),
           PacketFrame({0x00, 0xFE}));
  CHECK_EQ(f.Feed(in.data(), in.size(), sink), 3U);
  CHECK(sink.requests == (Requests{kInfoPacket, {0x01, 0x02, 0x03}, {0x00, 0xFE}}));
  CHECK(sink.control == (std::vector<bool>{false, true, false}));
}

TEST(ControlResponseUsesItsTopic)
{
  uint8_t out[Framer<1024>::kFrameCapacity];
  const Bytes body = {0x01, 0x00};
  const size_t n = Framer<1024>::PackResponse(body.data(), 2, 0, out, true);
  CHECK(Bytes(out, out + n) == PacketFrame(body, kCtrlResp));
}
