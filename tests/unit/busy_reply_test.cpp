// SPDX-License-Identifier: Apache-2.0
// “忙”回复。判据：用户决定 B2（“第二条连接回明确的忙，DAP_Info 照常放行”）、CMSIS-DAP
// 规范（DAP_ERROR = 0xFF、DAP_ExecuteCommands/QueueCommands 是命令容器）和
// docs/protocol.md 3.1 / Busy replies. Oracles: user decision B2, the CMSIS-DAP
// specification (DAP_ERROR = 0xFF, DAP_ExecuteCommands/QueueCommands contain commands)
// and docs/protocol.md 3.1.
#include "xrdap/session/busy_reply.hpp"

#include "check.hpp"

using XrDap::RunsWithoutOwner;
using XrDap::WriteBusyReply;

TEST(OnlyInfoAndContainersRunWithoutOwner)
{
  CHECK(RunsWithoutOwner(0x00));   // DAP_Info
  CHECK(RunsWithoutOwner(0x7E));   // DAP_QueueCommands
  CHECK(RunsWithoutOwner(0x7F));   // DAP_ExecuteCommands
  CHECK(!RunsWithoutOwner(0x01));  // DAP_HostStatus
  CHECK(!RunsWithoutOwner(0x02));  // DAP_Connect
  CHECK(!RunsWithoutOwner(0x05));  // DAP_Transfer
  CHECK(!RunsWithoutOwner(0x80));  // INFO 也要主人 / vendor INFO needs the owner too
}

TEST(StandardCommandBusyIsDapError)
{
  uint8_t resp[8] = {};
  uint16_t out = 0;
  WriteBusyReply(0x05, resp, sizeof(resp), out);
  CHECK_EQ(out, 2U);
  CHECK_EQ(resp[0], 0x05U);
  CHECK_EQ(resp[1], 0xFFU);
}

TEST(VendorCommandBusyStatus)
{
  uint8_t resp[8] = {};
  uint16_t out = 0;
  WriteBusyReply(0x81, resp, sizeof(resp), out);
  CHECK_EQ(out, 2U);
  CHECK_EQ(resp[0], 0x81U);
  CHECK_EQ(resp[1], XrDap::kStatusBusy);
  WriteBusyReply(0x9F, resp, sizeof(resp), out);
  CHECK_EQ(resp[1], XrDap::kStatusBusy);
}

TEST(BusyReplyRespectsSpace)
{
  uint8_t resp[3] = {0xAA, 0xAA, 0xAA};
  uint16_t out = 7;
  WriteBusyReply(0x81, resp, 1, out);
  CHECK_EQ(out, 1U);
  CHECK_EQ(resp[0], 0xFFU);
  CHECK_EQ(resp[1], 0xAAU);  // 没写出界 / nothing written past the space
  WriteBusyReply(0x81, resp, 0, out);
  CHECK_EQ(out, 0U);
}
