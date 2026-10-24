// SPDX-License-Identifier: Apache-2.0
// 会话主人。判据：用户决定 B2（第二条连接回明确的忙）、B6（无线会话号与租期）和
// docs/protocol.md 3.1 / Session owner. Oracles: user decisions B2 (the second link gets
// an explicit busy), B6 (radio session id and lease) and docs/protocol.md 3.1.
#include "xrdap/session/session_owner.hpp"

#include "check.hpp"

using XrDap::SessionOwner;
using XrDap::Transport;

TEST(SecondTransportIsBusy)
{
  SessionOwner s;
  CHECK(s.Admit(Transport::USB, 0));
  CHECK(s.Owner() == Transport::USB);
  CHECK(!s.Admit(Transport::TCP, 1));
  CHECK(!s.Admit(Transport::SERIAL, 2));
  CHECK(s.Admit(Transport::USB, 3));
}

TEST(SessionNumberStableWhileOwned)
{
  SessionOwner s;
  CHECK_EQ(s.Session(), 0U);
  CHECK(s.Admit(Transport::TCP, 0));
  const uint32_t id = s.Session();
  CHECK(id != 0U);
  CHECK(s.Admit(Transport::TCP, 10));
  CHECK_EQ(s.Session(), id);
  CHECK(s.Alive(id, 20));
}

TEST(EndByOwnerFreesProbe)
{
  SessionOwner s;
  CHECK(s.Admit(Transport::TCP, 0));
  const uint32_t id = s.Session();
  s.End(Transport::USB);  // 不是主人：没有效果 / not the owner: no effect
  CHECK(s.Owner() == Transport::TCP);
  s.End(Transport::TCP);
  CHECK(s.Owner() == Transport::NONE);
  CHECK_EQ(s.Session(), 0U);
  CHECK(!s.Alive(id, 1));
  CHECK(s.Admit(Transport::USB, 2));
  CHECK(s.Session() != id);
}

TEST(SerialLeaseExpires)
{
  SessionOwner s;
  s.SetLease(Transport::SERIAL, XrDap::kSerialLeaseMs);
  CHECK(s.Admit(Transport::SERIAL, 1000));
  const uint32_t id = s.Session();
  CHECK(!s.Admit(Transport::USB, 1000 + XrDap::kSerialLeaseMs));
  CHECK(s.Alive(id, 1000 + XrDap::kSerialLeaseMs));
  CHECK(!s.Alive(id, 1001 + XrDap::kSerialLeaseMs));
  CHECK(s.Admit(Transport::USB, 1002 + XrDap::kSerialLeaseMs));
}

TEST(CommandsRefreshTheLease)
{
  SessionOwner s;
  s.SetLease(Transport::SERIAL, 100);
  CHECK(s.Admit(Transport::SERIAL, 0));
  CHECK(s.Admit(Transport::SERIAL, 90));
  CHECK(!s.Admit(Transport::USB, 180));
  CHECK(s.Admit(Transport::USB, 191));
}

TEST(TouchRefreshesRadioLease)
{
  SessionOwner s;
  s.SetLease(Transport::RADIO, 2000);
  CHECK(s.Admit(Transport::RADIO, 0));
  const uint32_t id = s.Session();
  s.Touch(Transport::RADIO, 1500);
  s.Touch(Transport::USB, 3000);  // 不是主人：不刷新 / not the owner: no refresh
  CHECK(s.Alive(id, 3500));
  CHECK(!s.Alive(id, 3501));
}

TEST(NoLeaseNeverExpires)
{
  SessionOwner s;
  CHECK(s.Admit(Transport::TCP, 0));
  CHECK(s.Alive(s.Session(), 0xF0000000U));
  CHECK(!s.Admit(Transport::SERIAL, 0xF0000001U));
}

namespace
{
uint32_t resets = 0;
uint32_t ResetCount(void*) { return resets; }
}  // namespace

TEST(UsbBusResetEndsUsbSession)
{
  SessionOwner s;
  resets = 7;
  s.SetUsbResetSource(&ResetCount, nullptr);
  CHECK(s.Admit(Transport::USB, 0));
  const uint32_t id = s.Session();
  CHECK(s.Alive(id, 1));
  resets = 8;
  CHECK(!s.Alive(id, 2));
  CHECK(s.Owner() == Transport::NONE);
  CHECK(s.Admit(Transport::TCP, 3));
}

TEST(UsbResetDoesNotEndOtherSessions)
{
  SessionOwner s;
  resets = 0;
  s.SetUsbResetSource(&ResetCount, nullptr);
  CHECK(s.Admit(Transport::TCP, 0));
  resets = 5;
  CHECK(s.Alive(s.Session(), 1));
}

TEST(LeaseTimeWrapsAround)
{
  SessionOwner s;
  s.SetLease(Transport::SERIAL, 100);
  CHECK(s.Admit(Transport::SERIAL, 0xFFFFFFF0U));
  CHECK(s.Alive(s.Session(), 0x00000010U));  // 32 ms 之后 / 32 ms later
  CHECK(!s.Alive(s.Session(), 0x00000080U));
}
