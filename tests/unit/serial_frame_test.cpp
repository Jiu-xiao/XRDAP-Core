// SPDX-License-Identifier: Apache-2.0
// 串口帧。判据：docs/protocol.md 第 1 节（帧布局、CRC 参数、"123456789" 的 CRC32、topic
// 的 CRC32、完整的 DAP_Info 请求帧示例和丢帧规则）/ Serial frames. Oracle:
// docs/protocol.md section 1 (layout, CRC parameters, the CRC32 of "123456789", the topic
// CRC32s, the complete DAP_Info request example and the drop rules).
#include "xrdap/serial/serial_frame.hpp"

#include <vector>

#include "check.hpp"

using namespace XrDap::Serial;

namespace
{
uint8_t Crc8(const uint8_t* data, size_t len) { return LibXR::CRC8::Calculate(data, len); }
}  // namespace

namespace
{
// 文档第 1 节的例子：DAP_Info(0xFF) 请求，DAP 包 00 FF / The example of section 1:
// the DAP_Info(0xFF) request, DAP packet 00 FF.
const uint8_t kInfoFrame[] = {0x5A, 0x04, 0x00, 0x00, 0xC5, 0x09, 0x83,
                              0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x01, 0xB1, 0x02, 0x00, 0x00, 0xFF, 0x32};

struct Sink
{
  std::vector<std::vector<uint8_t>> requests;
  void operator()(const uint8_t* dap, uint16_t len)
  {
    requests.emplace_back(dap, dap + len);
  }
};

const uint8_t* Str(const char* s) { return reinterpret_cast<const uint8_t*>(s); }
}  // namespace

TEST(CrcCheckValues)
{
  // libxr 的 CRC 与文档第 1 节的参数一致 / libxr's CRCs match the parameters of section 1.
  CHECK_EQ(LibXR::CRC32::Calculate(Str("123456789"), 9), 0x340BC6D9U);
  CHECK_EQ(TopicKey(kRequestTopicName), 0xDC8309C5U);
  CHECK_EQ(TopicKey(kResponseTopicName), 0x89E1A688U);
  CHECK_EQ(Crc8(kInfoFrame, 15), 0xB1U);
  CHECK_EQ(Crc8(kInfoFrame, sizeof(kInfoFrame) - 1), 0x32U);
}

TEST(DocumentExampleIsOneRequest)
{
  Framer<1024> f;
  Sink sink;
  CHECK_EQ(f.Feed(kInfoFrame, sizeof(kInfoFrame), sink), 1U);
  CHECK_EQ(sink.requests.size(), 1U);
  CHECK(!sink.requests.empty() && sink.requests[0] == (std::vector<uint8_t>{0x00, 0xFF}));
}

TEST(FrameSplitAcrossFeeds)
{
  Framer<1024> f;
  Sink sink;
  for (uint8_t b : kInfoFrame)
  {
    (void)f.Feed(&b, 1, sink);
  }
  CHECK_EQ(sink.requests.size(), 1U);
}

TEST(GarbageBeforeAndBetweenFrames)
{
  Framer<1024> f;
  Sink sink;
  std::vector<uint8_t> in = {0x00, 0x5A, 0x13};  // 有一个假前缀 / one false prefix
  in.insert(in.end(), kInfoFrame, kInfoFrame + sizeof(kInfoFrame));
  in.push_back(0x77);
  in.insert(in.end(), kInfoFrame, kInfoFrame + sizeof(kInfoFrame));
  (void)f.Feed(in.data(), in.size(), sink);
  CHECK_EQ(sink.requests.size(), 2U);
}

TEST(BadTrailingCrcIsDropped)
{
  Framer<1024> f;
  Sink sink;
  uint8_t bad[sizeof(kInfoFrame)];
  std::memcpy(bad, kInfoFrame, sizeof(bad));
  bad[sizeof(bad) - 1] ^= 0x01U;
  (void)f.Feed(bad, sizeof(bad), sink);
  CHECK_EQ(sink.requests.size(), 0U);
  (void)f.Feed(kInfoFrame, sizeof(kInfoFrame), sink);  // 之后照常 / works afterwards
  CHECK_EQ(sink.requests.size(), 1U);
}

TEST(WrongTopicIsHeaderError)
{
  Framer<1024> f;
  Sink sink;
  uint8_t resp[Framer<1024>::kFrameCapacity];
  const uint8_t dap[] = {0x00, 0x02};
  const size_t n = Framer<1024>::PackResponse(dap, 2, 0, resp);  // topic dap_resp
  (void)f.Feed(resp, n, sink);
  CHECK_EQ(sink.requests.size(), 0U);
}

TEST(PacketLongerThanMaximumIsDropped)
{
  // MaxPacket 1：DAP 包 2 字节的请求丢掉，不回应 / MaxPacket 1: a 2-byte DAP packet is
  // dropped without a response.
  Framer<1> f;
  Sink sink;
  (void)f.Feed(kInfoFrame, sizeof(kInfoFrame), sink);
  CHECK_EQ(sink.requests.size(), 0U);
}

TEST(ResponseFrameLayout)
{
  uint8_t out[Framer<1024>::kFrameCapacity];
  const uint8_t dap[] = {0x00, 0x02, 0x00, 0x04};
  const size_t n = Framer<1024>::PackResponse(dap, 4, 0x0000123456789ABCULL, out);
  CHECK_EQ(n, kHeaderSize + 2U + 4U + 1U);
  CHECK_EQ(out[0], 0x5AU);
  CHECK_EQ(out[1], 6U);     // data_len = n(2) + 4
  CHECK_EQ(out[4], 0x88U);  // dap_resp 0x89E1A688, little-endian
  CHECK_EQ(out[7], 0x89U);
  CHECK_EQ(out[8], 0xBCU);  // timestamp_us, little-endian
  CHECK_EQ(out[13], 0x12U);
  CHECK_EQ(out[14], 0x01U);
  CHECK_EQ(out[15], Crc8(out, 15));
  CHECK_EQ(out[16], 4U);
  CHECK_EQ(out[n - 1], Crc8(out, n - 1));
}

TEST(FramesInsideABadFrameAreAllFound)
{
  // 一个头合法、尾 CRC 错的帧，42 字节载荷里是两个完整的请求帧：重新同步后缓冲里留下的
  // 字节含两帧，两个请求都要找出来 / A frame with a valid header and a wrong trailing
  // CRC whose 42-byte payload is two complete request frames: after the resync the
  // buffered bytes hold both, and both requests must be found.
  std::vector<uint8_t> in = {0x5A, 42,   0x00, 0x00, 0xC5, 0x09, 0x83, 0xDC,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  in.push_back(Crc8(in.data(), in.size()));
  in.insert(in.end(), kInfoFrame, kInfoFrame + sizeof(kInfoFrame));
  in.insert(in.end(), kInfoFrame, kInfoFrame + sizeof(kInfoFrame));
  in.push_back(static_cast<uint8_t>(Crc8(in.data(), in.size()) ^ 0xFFU));
  Framer<1024> f;
  Sink sink;
  (void)f.Feed(in.data(), in.size(), sink);
  CHECK_EQ(sink.requests.size(), 2U);
}

namespace
{
// 一条 DAP 包为 n 字节的请求帧，帧长 19 + n / A request frame carrying an n-byte DAP
// packet; the frame is 19 + n bytes long.
std::vector<uint8_t> RequestFrame(uint16_t n, uint8_t fill)
{
  const uint32_t payload = 2U + n;
  std::vector<uint8_t> f = {0x5A,
                            static_cast<uint8_t>(payload),
                            static_cast<uint8_t>(payload >> 8),
                            0x00,
                            0xC5,
                            0x09,
                            0x83,
                            0xDC,
                            0x00,
                            0x00,
                            0x00,
                            0x00,
                            0x00,
                            0x00,
                            0x01};
  f.push_back(Crc8(f.data(), f.size()));
  f.push_back(static_cast<uint8_t>(n));
  f.push_back(static_cast<uint8_t>(n >> 8));
  f.insert(f.end(), n, fill);
  f.push_back(Crc8(f.data(), f.size()));
  return f;
}
}  // namespace

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
  // 接收端丢掉 0x5A 之前的字节：补过字节的流与不补时交出同样的请求 / The receiver drops
  // bytes before 0x5A: a padded stream hands out the same requests as an unpadded one.
  std::vector<uint8_t> stream;
  for (uint16_t n : {45U, 2U, 109U, 45U})
  {
    std::vector<uint8_t> f = RequestFrame(n, static_cast<uint8_t>(n));
    const size_t len = f.size();
    f.resize(Framer<1024>::kTxCapacity);
    f.resize(Framer<1024>::AppendPad(f.data(), len));
    stream.insert(stream.end(), f.begin(), f.end());
  }
  CHECK_EQ(stream.size(), 64U + 1U + 21U + 128U + 1U + 64U + 1U);
  Framer<1024> fr;
  Sink sink;
  CHECK_EQ(fr.Feed(stream.data(), stream.size(), sink), 4U);
  CHECK_EQ(sink.requests.size(), 4U);
  CHECK(sink.requests.size() == 4U && sink.requests[0].size() == 45U &&
        sink.requests[2].size() == 109U && sink.requests[3][0] == 45U);
}
