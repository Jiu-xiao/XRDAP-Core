// SPDX-License-Identifier: Apache-2.0
// TCP 帧。判据：docs/protocol.md 第 2 节（"DAP\0" + 长度(2) + 类型(1) + 保留(1)，类型 1
// 请求、2 响应，保留字节填 0，帧头无效的条件）和 3.7.1（类型 3 控制请求、4 控制应答）/
// TCP frames. Oracle: docs/protocol.md section 2 ("DAP\0" + length(2) + type(1) +
// reserved(1), type 1 request, 2 response, the reserved byte zero, the conditions for an
// invalid header) and 3.7.1 (type 3 control request, 4 control answer).
#include "xrdap/net/tcp_frame.hpp"

#include <cstring>

#include "check.hpp"

using namespace XrDap::Net;

namespace
{
constexpr uint16_t kMax = 1024U;
}  // namespace

TEST(TcpFrameEncodeMatchesDocument)
{
  // 先填满别的值，保留字节也要由编码写成 0 / Filled with something else first: the
  // encoder has to write the reserved zero too.
  uint8_t out[8];
  std::memset(out, 0xEE, sizeof(out));
  EncodeHeader(out, FrameType::RESPONSE, 0x0102U);
  const uint8_t expect[] = {'D', 'A', 'P', 0x00, 0x02, 0x01, 0x02, 0x00};
  CHECK(std::memcmp(out, expect, sizeof(expect)) == 0);

  std::memset(out, 0xEE, sizeof(out));
  EncodeHeader(out, FrameType::CONTROL_RESPONSE, 11U);
  const uint8_t expect_ctrl[] = {'D', 'A', 'P', 0x00, 0x0B, 0x00, 0x04, 0x00};
  CHECK(std::memcmp(out, expect_ctrl, sizeof(expect_ctrl)) == 0);
}

TEST(TcpFrameDecodesRequests)
{
  Header h{};
  const uint8_t req[] = {'D', 'A', 'P', 0x00, 0x02, 0x00, 0x01, 0x00};
  CHECK(DecodeRequestHeader(req, kMax, h));
  CHECK(h.type == FrameType::REQUEST);
  CHECK_EQ(h.length, 2U);

  const uint8_t ctrl[] = {'D', 'A', 'P', 0x00, 0x00, 0x04, 0x03, 0x00};
  CHECK(DecodeRequestHeader(ctrl, kMax, h));
  CHECK(h.type == FrameType::CONTROL_REQUEST);
  CHECK_EQ(h.length, 1024U);
}

TEST(TcpFrameBadMagicIsInvalid)
{
  for (size_t i = 0; i < 4U; ++i)
  {
    uint8_t hdr[] = {'D', 'A', 'P', 0x00, 0x02, 0x00, 0x01, 0x00};
    hdr[i] ^= 0x01U;
    Header h{};
    CHECK(!DecodeRequestHeader(hdr, kMax, h));
  }
}

TEST(TcpFrameTypeOtherThanOneOrThreeIsInvalid)
{
  for (uint32_t type = 0; type < 256U; ++type)
  {
    const uint8_t hdr[] = {'D', 'A', 'P', 0x00, 0x02, 0x00, static_cast<uint8_t>(type),
                           0x00};
    Header h{};
    CHECK_EQ(DecodeRequestHeader(hdr, kMax, h), type == 1U || type == 3U);
  }
}

TEST(TcpFrameBadLengthIsInvalid)
{
  Header h{};
  const uint8_t zero_len[] = {'D', 'A', 'P', 0x00, 0x00, 0x00, 0x01, 0x00};
  const uint8_t too_long[] = {'D', 'A', 'P', 0x00, 0x01, 0x04, 0x01, 0x00};
  CHECK(!DecodeRequestHeader(zero_len, kMax, h));
  CHECK(!DecodeRequestHeader(too_long, kMax, h));
  // 上限由调用者给：512 字节的探针拒绝 513 / The limit is the caller's: a 512-byte probe
  // refuses 513.
  const uint8_t len513[] = {'D', 'A', 'P', 0x00, 0x01, 0x02, 0x01, 0x00};
  CHECK(!DecodeRequestHeader(len513, 512U, h));
  CHECK(DecodeRequestHeader(len513, kMax, h));
}

TEST(TcpFrameResponseTypeAnswersRequest)
{
  CHECK(ResponseTypeOf(FrameType::REQUEST) == FrameType::RESPONSE);
  CHECK(ResponseTypeOf(FrameType::CONTROL_REQUEST) == FrameType::CONTROL_RESPONSE);
}
