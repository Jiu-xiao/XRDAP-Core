// SPDX-License-Identifier: Apache-2.0
// 厂商命令的请求长度。判据：CMSIS-DAP 规范的 DAP_ExecuteCommands（[0x7F, 命令数,
// 命令...]，命令首尾相接、没有长度字段；参考实现 DAP_ProcessCommand 对厂商命令同样返回
// 请求长度）和 docs/protocol.md 3.4、3.7 的请求格式。
//
// Request length of vendor commands. Oracles: DAP_ExecuteCommands of the CMSIS-DAP
// specification ([0x7F, count, commands...], commands back to back with no length
// field; the reference DAP_ProcessCommand returns the request length for vendor commands
// too) and the request formats of docs/protocol.md 3.4 and 3.7.
#include <vector>

#include "check.hpp"
#include "xrdap/vendor/protocol.hpp"

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

// 一条 READ、WRITE、SUB_START 请求，各带一个条目 / One READ, WRITE and SUB_START request
// with one item each.
std::vector<uint8_t> OneItemRead()
{
  std::vector<uint8_t> v = {0x81, 0x01};
  PutItem(v, 4);
  return v;
}

std::vector<uint8_t> OneItemWrite()
{
  std::vector<uint8_t> v = {0x82, 0x01};
  PutItem(v, 4);
  v.insert(v.end(), {0x11, 0x22, 0x33, 0x44});
  return v;
}

std::vector<uint8_t> OneItemSubStart()
{
  std::vector<uint8_t> v = {0x83, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00};
  PutItem(v, 4);
  v.insert(v.end(), {0x01, 0x00, 0x00, 0x00});  // div 1, phase 0
  return v;
}

std::vector<uint8_t> FollowedByInfo(std::vector<uint8_t> v)
{
  v.insert(v.end(), {0x00, 0xFF});
  return v;
}

std::vector<uint8_t> WithoutLastByte(std::vector<uint8_t> v)
{
  v.pop_back();
  v.shrink_to_fit();
  return v;
}
}  // namespace

TEST(LengthOfEveryCommand)
{
  CHECK_EQ(Len({0x80}), 1U);
  CHECK_EQ(Len({0x84, 0x00}), 2U);
  CHECK_EQ(Len({0x85, 0x00, 0x04}), 3U);
  CHECK_EQ(Len(OneItemRead()), 13U);
  CHECK_EQ(Len(OneItemWrite()), 17U);
  CHECK_EQ(Len(OneItemSubStart()), 23U);
}

TEST(EveryItemIsCounted)
{
  std::vector<uint8_t> read = {0x81, 0x02};
  PutItem(read, 4);
  PutItem(read, 2);
  CHECK_EQ(Len(read), 24U);

  std::vector<uint8_t> write = {0x82, 0x02};
  PutItem(write, 4);
  write.insert(write.end(), {0x11, 0x22, 0x33, 0x44});
  PutItem(write, 2);
  write.insert(write.end(), {0x55, 0x66});
  CHECK_EQ(Len(write), 30U);

  std::vector<uint8_t> sub = {0x83, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00};
  for (int i = 0; i < 2; ++i)
  {
    PutItem(sub, 4);
    sub.insert(sub.end(), {0x01, 0x00, 0x00, 0x00});  // div 1, phase 0
  }
  CHECK_EQ(Len(sub), 38U);
}

// 容器里的命令首尾相接，后面的字节不算这一条的 / Commands in a container sit back to
// back, and the bytes behind a command are not part of it.
TEST(LengthStopsAtTheEndOfTheCommand)
{
  CHECK_EQ(Len(FollowedByInfo({0x80})), 1U);
  CHECK_EQ(Len(FollowedByInfo({0x84, 0x00})), 2U);
  CHECK_EQ(Len(FollowedByInfo({0x85, 0x00, 0x04})), 3U);
  CHECK_EQ(Len(FollowedByInfo(OneItemRead())), 13U);
  CHECK_EQ(Len(FollowedByInfo(OneItemWrite())), 17U);
  CHECK_EQ(Len(FollowedByInfo(OneItemSubStart())), 23U);
}

TEST(IncompleteCommandIsRejected)
{
  CHECK_EQ(Len({0x81}), 0U);
  CHECK_EQ(Len({0x84}), 0U);
  CHECK_EQ(Len({0x85, 0x00}), 0U);
  CHECK_EQ(Len({0x83, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x01}),
           0U);  // 头差 1 字节 / head short
  CHECK_EQ(Len(WithoutLastByte(OneItemRead())), 0U);
  CHECK_EQ(Len(WithoutLastByte(OneItemWrite())), 0U);  // 值差 1 字节 / value short
  CHECK_EQ(Len(WithoutLastByte(OneItemSubStart())), 0U);
}

TEST(OtherCommandsAreNotOurs)
{
  // 附加命令由平台算 / extra commands are the platform's
  CHECK_EQ(Len({0x86, 0x00}), 0U);
  CHECK_EQ(Len({0x87}), 0U);
  // 标准命令归 libxr / standard commands are libxr's
  CHECK_EQ(Len({0x00, 0x00}), 0U);
}
