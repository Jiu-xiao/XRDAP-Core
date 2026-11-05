// SPDX-License-Identifier: Apache-2.0
// 厂商命令 0x86 的编解码。判据：docs/protocol.md 3.7（请求和应答布局、状态、限制）、3.8
// 的 0x86 字节示例、功能位表 / Codec of vendor command 0x86. Oracle: docs/protocol.md
// 3.7 (request and answer layout, statuses, limits), the 0x86 byte examples of 3.8 and
// the feature bit table.
#include "xrdap/vendor/wifi_command.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"

using namespace XrDap::Vendor;
namespace W = XrDap::Vendor::WifiCommand;

namespace
{
using Bytes = std::vector<uint8_t>;

Bytes SaveRequest(const char* ssid, const char* password)
{
  Bytes r = {W::kCmd, 0x01, static_cast<uint8_t>(std::strlen(ssid))};
  r.insert(r.end(), ssid, ssid + std::strlen(ssid));
  r.push_back(static_cast<uint8_t>(std::strlen(password)));
  r.insert(r.end(), password, password + std::strlen(password));
  return r;
}

W::Parse Parse(const Bytes& wire, W::Request& out)
{
  // 去掉 0x86 字节，即控制口上的请求 / Without the 0x86 byte, as on the control port.
  return W::ParseRequest(wire.data() + 1, static_cast<uint16_t>(wire.size() - 1U), out);
}

bool Length(const Bytes& wire, uint16_t& len)
{
  return W::RequestLength(wire.data(), static_cast<uint16_t>(wire.size()), len);
}
}  // namespace

TEST(WifiFeatureBitIsBit4)
{
  // 文档 3.4：功能位 bit4 为 Wi-Fi 配置；bit5 控制口、bit6 紧凑帧不与它重叠 / Doc 3.4:
  // feature bit 4 is Wi-Fi configuration; bit 5 (control port) and bit 6 (compact frames)
  // do not overlap.
  CHECK_EQ(kFeatureWifi, 0x10U);
  CHECK_EQ(kFeatureWifi & (kFeatureControl | kFeatureCompactFrames | kFeatures), 0U);
  CHECK_EQ(W::kCmd, 0x86U);
}

TEST(WifiRequestLengthMatchesDocument)
{
  uint16_t len = 0;
  // 查询 2、清除 3、保存 3 + ssid_len + 1 + pw_len / query 2, clear 3, save
  // 3 + ssid_len + 1 + pw_len.
  CHECK(Length({0x86, 0x00}, len));
  CHECK_EQ(len, 2U);
  CHECK(Length({0x86, 0x02, 0xA5}, len));
  CHECK_EQ(len, 3U);
  Bytes save = SaveRequest("lab", "12345678");
  CHECK(Length(save, len));
  CHECK_EQ(len, 3U + 3U + 1U + 8U);
  // 后面还有别的命令时只取这一条 / Only this command is taken when others follow.
  save.push_back(0x00);
  save.push_back(0xFF);
  CHECK(Length(save, len));
  CHECK_EQ(len, 15U);
}

TEST(WifiRequestLengthRejectsIncomplete)
{
  uint16_t len = 0;
  CHECK(!Length({0x86}, len));
  CHECK(!Length({0x86, 0x02}, len));
  CHECK(!Length({0x86, 0x03}, len));
  CHECK(!Length({0x87, 0x00}, len));
  CHECK(!Length({0x86, 0x01}, len));
  CHECK(!Length({0x86, 0x01, 0x03, 'l', 'a', 'b'}, len));
  Bytes save = SaveRequest("lab", "12345678");
  save.pop_back();
  CHECK(!Length(save, len));
}

TEST(WifiParseDocumentExamples)
{
  W::Request r{};
  // 3.8：86 01 03 6C6162 08 3132333435363738 / 3.8: save "lab" with "12345678".
  const Bytes save = {0x86, 0x01, 0x03, 0x6C, 0x61, 0x62, 0x08, 0x31,
                      0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
  CHECK(Parse(save, r) == W::Parse::OK);
  CHECK(r.op == W::Op::SAVE);
  CHECK_EQ(r.ssid_len, 3U);
  CHECK(std::memcmp(r.ssid, "lab", 3) == 0);
  CHECK_EQ(r.password_len, 8U);
  CHECK(std::memcmp(r.password, "12345678", 8) == 0);

  CHECK(Parse({0x86, 0x00}, r) == W::Parse::OK);
  CHECK(r.op == W::Op::QUERY);
  CHECK(Parse({0x86, 0x02, 0xA5}, r) == W::Parse::OK);
  CHECK(r.op == W::Op::CLEAR);
  // 没有确认字节：请求 86 02 被拒 / Without the confirm byte, 86 02 is refused.
  CHECK(Parse({0x86, 0x02}, r) == W::Parse::BAD_ARGUMENT);
  CHECK(Parse({0x86, 0x02, 0xA4}, r) == W::Parse::BAD_ARGUMENT);
  CHECK(Parse({0x86, 0x02, 0xA5, 0x00}, r) == W::Parse::BAD_ARGUMENT);
}

TEST(WifiParseRulesOfSave)
{
  W::Request r{};
  // 开放网络：pw_len 为 0 / Open network: pw_len is 0.
  CHECK(Parse(SaveRequest("lab", ""), r) == W::Parse::OK);
  CHECK_EQ(r.password_len, 0U);
  // 热点名 1 到 32 字节 / SSID 1 to 32 bytes.
  CHECK(Parse(SaveRequest("", "12345678"), r) == W::Parse::BAD_ARGUMENT);
  CHECK(Parse(SaveRequest("a", "12345678"), r) == W::Parse::OK);
  CHECK(Parse(SaveRequest(std::string(32, 'a').c_str(), ""), r) == W::Parse::OK);
  CHECK(Parse(SaveRequest(std::string(33, 'a').c_str(), ""), r) ==
        W::Parse::BAD_ARGUMENT);
  // 密码非空时 8 到 63 字节 / A non-empty password is 8 to 63 bytes.
  CHECK(Parse(SaveRequest("lab", "1234567"), r) == W::Parse::BAD_ARGUMENT);
  CHECK(Parse(SaveRequest("lab", std::string(63, 'p').c_str()), r) == W::Parse::OK);
  CHECK(Parse(SaveRequest("lab", std::string(64, 'p').c_str()), r) ==
        W::Parse::BAD_ARGUMENT);
  // 长度与 op 不符 / The length does not fit the op.
  Bytes extra = SaveRequest("lab", "12345678");
  extra.push_back(0x00);
  CHECK(Parse(extra, r) == W::Parse::BAD_ARGUMENT);
  Bytes cut = SaveRequest("lab", "12345678");
  cut.pop_back();
  CHECK(Parse(cut, r) == W::Parse::BAD_ARGUMENT);
  CHECK(Parse({0x86, 0x01}, r) == W::Parse::BAD_ARGUMENT);
  CHECK(Parse({0x86, 0x01, 0x03, 'l', 'a', 'b'}, r) == W::Parse::BAD_ARGUMENT);
  // 查询的长度必须是 2 字节 / A query must be 2 bytes.
  CHECK(Parse({0x86, 0x00, 0x00}, r) == W::Parse::BAD_ARGUMENT);
  // op 未定义 / Undefined op.
  CHECK(Parse({0x86, 0x03}, r) == W::Parse::UNKNOWN_OP);
  CHECK(W::ParseRequest(nullptr, 0U, r) == W::Parse::BAD_ARGUMENT);
}

TEST(WifiQueryAnswerMatchesDocumentExamples)
{
  uint8_t resp[64];
  // 3.8：已连接 lab，192.168.1.20，-52 dBm，没清除过，上电 / 3.8: connected to lab,
  // 192.168.1.20, -52 dBm, never cleared, power-on.
  W::Query q{};
  q.started = true;
  q.connected = true;
  q.ip[0] = 0xC0;
  q.ip[1] = 0xA8;
  q.ip[2] = 0x01;
  q.ip[3] = 0x14;
  q.rssi = -52;
  q.ssid = "lab";
  q.ssid_len = 3;
  q.last_erase = W::EraseSource::NONE;
  q.reset_reason = W::ResetReason::POWER_ON;
  const Bytes expect = {0x86, 0x00, 0x03, 0xC0, 0xA8, 0x01, 0x14,
                        0xCC, 0x03, 0x6C, 0x61, 0x62, 0x00, 0x01};
  const uint16_t n = W::EncodeQuery(0x86, q, resp, sizeof(resp));
  CHECK_EQ(n, expect.size());
  CHECK(Bytes(resp, resp + n) == expect);

  // 3.8：被 0x86 清除、软件重启之后 / 3.8: after a 0x86 clear and a software restart.
  W::Query cleared{};
  cleared.last_erase = W::EraseSource::VENDOR_COMMAND;
  cleared.reset_reason = W::ResetReason::SOFTWARE;
  const Bytes expect_cleared = {0x86, 0x00, 0x00, 0, 0, 0, 0, 0x00, 0x00, 0x03, 0x02};
  const uint16_t m = W::EncodeQuery(0x86, cleared, resp, sizeof(resp));
  CHECK(Bytes(resp, resp + m) == expect_cleared);
}

TEST(WifiQueryAnswerFlagsAndControlPortFirstByte)
{
  uint8_t resp[64];
  W::Query q{};
  q.storage_repaired = true;
  q.last_erase = W::EraseSource::STORAGE_REPAIR;
  q.reset_reason = W::ResetReason::OTHER;
  // 3.7.1：控制口应答的第 1 字节是 op / 3.7.1: the first byte of a control answer is the
  // op.
  const uint16_t n = W::EncodeQuery(0x00, q, resp, sizeof(resp));
  CHECK(Bytes(resp, resp + n) ==
        (Bytes{0x00, 0x00, 0x04, 0, 0, 0, 0, 0x00, 0x00, 0x04, 0x06}));
}

TEST(WifiQueryAnswerWithoutRoomIsResourceStatus)
{
  uint8_t resp[64];
  W::Query q{};
  q.ssid = "lab";
  q.ssid_len = 3;
  // 完整应答 14 字节；放不下时 86 03，cap 小于 2 时什么都不写 / The full answer is 14
  // bytes; without room it is 86 03, and below 2 nothing is written.
  CHECK_EQ(W::EncodeQuery(0x86, q, resp, 14U), 14U);
  CHECK_EQ(W::EncodeQuery(0x86, q, resp, 13U), 2U);
  CHECK(resp[0] == 0x86 && resp[1] == 0x03);
  CHECK_EQ(W::EncodeQuery(0x86, q, resp, 2U), 2U);
  CHECK_EQ(W::EncodeQuery(0x86, q, resp, 1U), 0U);
}
