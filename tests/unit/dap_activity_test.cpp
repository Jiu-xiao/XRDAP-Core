// SPDX-License-Identifier: Apache-2.0
// DAP 活动。判据：CMSIS-DAP 规范 DAP_HostStatus（类型 0 已连接、1 运行中，其他类型不是
// 这两个灯）、用户的灯语规则（DAP_Disconnect 或 5 s 没有命令清除上位机状态）/ DAP
// activity. Oracles: CMSIS-DAP DAP_HostStatus (type 0 connected, 1 running, other types
// are neither light) and the user's light rule (DAP_Disconnect or 5 s without commands
// clears the host state).
#include "xrdap/activity/dap_activity.hpp"

#include "check.hpp"

using XrDap::DapActivity;

namespace
{
const uint8_t kConnectedOn[] = {0x01, 0x00, 0x01};
const uint8_t kRunningOn[] = {0x01, 0x01, 0x01};
const uint8_t kOtherTypeOn[] = {0x01, 0x02, 0x01};
const uint8_t kDisconnect[] = {0x03};
}  // namespace

TEST(HostStatusSetsConnectedAndRunning)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 100, true);
  a.Observe(kRunningOn, 3, 100, true);
  const auto s = a.Read(100);
  CHECK(s.connected);
  CHECK(s.running);
  CHECK(s.wired);
}

TEST(UnknownHostStatusTypeIsIgnored)
{
  DapActivity a;
  a.Observe(kOtherTypeOn, 3, 100, true);
  const auto s = a.Read(100);
  CHECK(!s.connected);
  CHECK(!s.running);
}

TEST(DisconnectClears)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 100, true);
  a.Observe(kDisconnect, 1, 200, true);
  CHECK(!a.Read(200).connected);
}

TEST(QuietHostClearsAfterFiveSeconds)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 1000, false);
  CHECK(a.Read(1000 + DapActivity::kHostQuietMs).connected);
  CHECK(!a.Read(1001 + DapActivity::kHostQuietMs).connected);
  CHECK(!a.Read(1000).wired);
}

TEST(CommandNewerThanReadTimeIsNotQuiet)
{
  // 读的人先取了时间，随后另一个线程记了一条命令：不能算成很久没有命令 / The reader
  // took its time first, then another thread recorded a command: that must not count as
  // a long silence.
  DapActivity a;
  a.Observe(kConnectedOn, 3, 5000, true);
  const auto s = a.Read(4999);
  CHECK(s.connected);
  CHECK(s.wired);
}
