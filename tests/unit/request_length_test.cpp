// SPDX-License-Identifier: Apache-2.0
// 厂商命令的请求长度。判据：CMSIS-DAP 规范的 DAP_ExecuteCommands（[0x7F, 命令数,
// 命令...]，命令首尾相接、没有长度字段；参考实现 DAP_ProcessCommand 对厂商命令同样返回
// 请求长度）和 docs/protocol.md 3.4、3.7 的请求格式。
//
// Request length of vendor commands. Oracles: DAP_ExecuteCommands of the CMSIS-DAP
// specification ([0x7F, count, commands...], commands back to back with no length
// field; the reference DAP_ProcessCommand returns the request length for vendor commands
// too) and the request formats of docs/protocol.md 3.4 and 3.7.
#include "xrdap/vendor/request_length.hpp"

#include <vector>

#include "check.hpp"

using XrDap::Vendor::RequestLength;

namespace
{
// READ 的一个条目：ap、id(2)、size、base(4)、n=0、final_off(2) 共 11 字节 / One READ
// item: ap, id(2), size, base(4), n=0, final_off(2), 11 bytes in all.
void PutItem(std::vector<uint8_t>& v, uint8_t size)
{
  const uint8_t item[] = {0x00, 0x01, 0x00, size, 0x00, 0x00,
                          0x00, 0x20, 0x00, 0x00, 0x00};
  v.insert(v.end(), item, item + sizeof(item));
}

uint16_t Len(const std::vector<uint8_t>& v)
{
  uint16_t len = 0xFFFFU;
  return RequestLength(v.data(), static_cast<uint16_t>(v.size()), len) ? len : 0U;
}
}  // namespace

TEST(FixedLengths)
{
  CHECK_EQ(Len({0x80}), 1U);
  CHECK_EQ(Len({0x84, 0x00}), 2U);
  CHECK_EQ(Len({0x85, 0x00, 0x04}), 3U);
}

TEST(ReadAndWriteItems)
{
  std::vector<uint8_t> read = {0x81, 0x02};
  PutItem(read, 4);
  PutItem(read, 2);
  CHECK_EQ(Len(read), 24U);

  std::vector<uint8_t> write = {0x82, 0x01};
  PutItem(write, 4);
  write.insert(write.end(), {0x11, 0x22, 0x33, 0x44});
  CHECK_EQ(Len(write), 17U);
}

TEST(SubStartItems)
{
  std::vector<uint8_t> v = {0x83, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00};
  PutItem(v, 4);
  v.insert(v.end(), {0x01, 0x00, 0x00, 0x00});  // div 1, phase 0
  CHECK_EQ(Len(v), 23U);
}

TEST(IncompleteOrUnknownIsRejected)
{
  CHECK_EQ(Len({0x84}), 0U);
  CHECK_EQ(Len({0x85, 0x00}), 0U);
  CHECK_EQ(Len({0x86, 0x00}), 0U);  // 附加命令由平台算 / extra commands are the platform's
  CHECK_EQ(Len({0x87}), 0U);
  CHECK_EQ(Len({0x00, 0x00}), 0U);  // 标准命令归 libxr / standard commands are libxr's
  std::vector<uint8_t> read = {0x81, 0x01};
  PutItem(read, 4);
  read.pop_back();
  CHECK_EQ(Len(read), 0U);
}

TEST(ExecuteCommandsWithVendorCommandsIsWalkedExactly)
{
  // [7F, 3, DAP_Info(00 FF), READ 一个条目, DAP_Info(00 FF)]：DAP_Info 的 2 字节由 libxr
  // 定，厂商命令的长度由这里定，加起来正好占满整个包 / DAP_Info's 2 bytes come from
  // libxr, the vendor command's length from here, and together they fill the packet.
  std::vector<uint8_t> pkt = {0x7F, 0x03, 0x00, 0xFF, 0x81, 0x01};
  PutItem(pkt, 4);
  pkt.insert(pkt.end(), {0x00, 0xFF});
  uint16_t off = 2U;
  for (uint8_t i = 0; i < pkt[1]; ++i)
  {
    uint16_t len = 0U;
    if (pkt[off] == 0x00U)
    {
      len = 2U;
    }
    else
    {
      CHECK(RequestLength(&pkt[off], static_cast<uint16_t>(pkt.size() - off), len));
      CHECK_EQ(len, 13U);
    }
    CHECK(len > 0U);
    off = static_cast<uint16_t>(off + len);
  }
  CHECK_EQ(off, pkt.size());
}
