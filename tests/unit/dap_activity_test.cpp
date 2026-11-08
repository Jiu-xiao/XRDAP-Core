// SPDX-License-Identifier: Apache-2.0
// DAP 活动。判据：CMSIS-DAP 规范 DAP_HostStatus（类型 0 已连接、1 运行中，其他类型不是
// 这两个灯；状态 1 亮、0 灭）、灯语规则（tests/SPEC.md UI-02：DAP_Disconnect 或 5 s 没有
// 命令清除上位机状态）和 dap_activity.hpp 里 wired 的说明（5 s 内有经 USB 或串口来的
// 命令）/ DAP activity. Oracles: CMSIS-DAP DAP_HostStatus (type 0 connected, 1 running,
// other types are neither light; status 1 on, 0 off), the light rule (tests/SPEC.md
// UI-02: DAP_Disconnect or 5 s without commands clears the host state) and what
// dap_activity.hpp says of wired (a command over USB or serial within 5 s).
#include "xrdap/activity/dap_activity.hpp"

#include "check.hpp"

using XrDap::DapActivity;

namespace
{
const uint8_t kConnectedOn[] = {0x01, 0x00, 0x01};
const uint8_t kConnectedOff[] = {0x01, 0x00, 0x00};
const uint8_t kRunningOn[] = {0x01, 0x01, 0x01};
const uint8_t kRunningOff[] = {0x01, 0x01, 0x00};
const uint8_t kOtherTypeOn[] = {0x01, 0x02, 0x01};
const uint8_t kShortHostStatus[] = {0x01, 0x00};
const uint8_t kDisconnect[] = {0x03};
const uint8_t kInfo[] = {0x00, 0xFF};
}  // namespace

TEST(HostStatusTypeZeroIsConnected)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 100, true);
  const auto s = a.Read(100);
  CHECK(s.connected);
  CHECK(!s.running);
}

TEST(HostStatusTypeOneIsRunning)
{
  DapActivity a;
  a.Observe(kRunningOn, 3, 100, true);
  const auto s = a.Read(100);
  CHECK(!s.connected);
  CHECK(s.running);
}

TEST(HostStatusZeroTurnsTheLightOff)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 100, true);
  a.Observe(kRunningOn, 3, 100, true);
  a.Observe(kConnectedOff, 3, 100, true);
  CHECK(!a.Read(100).connected);
  CHECK(a.Read(100).running);
  a.Observe(kRunningOff, 3, 100, true);
  CHECK(!a.Read(100).running);
}

TEST(UnknownHostStatusTypeIsIgnored)
{
  DapActivity a;
  a.Observe(kOtherTypeOn, 3, 100, true);
  const auto s = a.Read(100);
  CHECK(!s.connected);
  CHECK(!s.running);
}

TEST(ShortHostStatusIsIgnored)
{
  DapActivity a;
  a.Observe(kShortHostStatus, 2, 100, true);
  CHECK(!a.Read(100).connected);
}

TEST(DisconnectClearsBothLights)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 100, true);
  a.Observe(kRunningOn, 3, 100, true);
  a.Observe(kDisconnect, 1, 200, true);
  const auto s = a.Read(200);
  CHECK(!s.connected);
  CHECK(!s.running);
}

TEST(QuietHostClearsAfterFiveSeconds)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 1000, false);
  a.Observe(kRunningOn, 3, 1000, false);
  CHECK(a.Read(6000).connected);
  CHECK(a.Read(6000).running);
  CHECK(!a.Read(6001).connected);
  CHECK(!a.Read(6001).running);
}

TEST(AnyCommandPutsTheSilenceOff)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 1000, false);
  a.Observe(kInfo, 2, 5000, false);
  CHECK(a.Read(10000).connected);
  CHECK(!a.Read(10001).connected);
}

TEST(QuietTimeCrossesTheCounterWrap)
{
  // 毫秒计数在 2^32 回绕，5000 ms 后的两个读都在回绕之后 / The millisecond count wraps at
  // 2^32; both reads 5000 ms later are past the wrap.
  DapActivity a;
  a.Observe(kConnectedOn, 3, 0xFFFFF000U, false);
  CHECK(a.Read(0xFFFFF000U + 5000U).connected);
  CHECK(!a.Read(0xFFFFF000U + 5001U).connected);
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

TEST(WiredFollowsTheLastWiredCommand)
{
  DapActivity a;
  a.Observe(kInfo, 2, 1000, true);
  CHECK(a.Read(6000).wired);
  CHECK(!a.Read(6001).wired);
}

TEST(WirelessCommandIsNotWired)
{
  DapActivity a;
  a.Observe(kInfo, 2, 1000, false);
  CHECK(!a.Read(1000).wired);
  a.Observe(kInfo, 2, 2000, true);
  a.Observe(kInfo, 2, 8000, false);
  CHECK(!a.Read(8000).wired);
}

TEST(EveryCommandIsCounted)
{
  DapActivity a;
  CHECK_EQ(a.Read(0).commands, 0U);
  a.Observe(kInfo, 2, 0, true);
  a.Observe(kDisconnect, 1, 0, false);
  CHECK_EQ(a.Read(0).commands, 2U);
}

TEST(EmptyRequestChangesNoLight)
{
  DapActivity a;
  a.Observe(kConnectedOn, 3, 100, true);
  a.Observe(nullptr, 0, 100, true);
  CHECK(a.Read(100).connected);
}
