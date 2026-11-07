// SPDX-License-Identifier: Apache-2.0
// 连接跟着链路走（docs/protocol.md 3.1）：DAP_Connect 记下它来自哪条链路，这条链路丢了
// 才作废；USB 的绑定和复位只影响 USB 上的连接。这些用例只有一个线程，它就是执行上下文。
//
// The connection follows the link (docs/protocol.md 3.1): DAP_Connect notes the link it
// came from, and only losing that link makes it lapse; a USB bind or reset only affects a
// connection on USB. These cases have one thread, which is the execution context.
#include <atomic>
#include <cstdint>

#include "fake_dap.hpp"

namespace
{
using XrDap::Transport;
using namespace XrDapTest;

const uint8_t kInfo = XrDap::Byte(XrDap::DapCmd::INFO);
const uint8_t kConnect = XrDap::Byte(XrDap::DapCmd::CONNECT);
const uint8_t kDisconnect = XrDap::Byte(XrDap::DapCmd::DISCONNECT);
}  // namespace

// 单循环探针：命令在调用者的上下文里直接执行，没有执行线程也不等 / Single-loop probe: the
// command runs directly in the caller's context, without an execution thread and without
// waiting.
TEST(ExecuteHereRunsInTheCallersContext)
{
  Probe p;
  CHECK_EQ(SendHere(p, Transport::RADIO, kConnect), kConnect);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::RADIO);
}

TEST(UsbCommandsCountAsTheUsbLink)
{
  Probe p;
  NullTimer timer;
  p.dap.Feed(kConnect);
  p.exec.Run(timer, 0U);
  CHECK_EQ(p.dap.usb_done.load(), 1U);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::USB);
}

TEST(InfoIsNotAConnection)
{
  Probe p;
  (void)SendHere(p, Transport::TCP, kInfo);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::NONE);
}

TEST(LosingAnotherLinkKeepsTheConnection)
{
  Probe p;
  (void)SendHere(p, Transport::TCP, kConnect);
  p.exec.LinkLost(Transport::SERIAL);
  p.exec.LinkLost(Transport::USB);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::TCP);
}

TEST(LosingTheConnectedLinkEndsTheConnection)
{
  Probe p;
  (void)SendHere(p, Transport::TCP, kConnect);
  p.exec.LinkLost(Transport::TCP);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::NONE);
}

TEST(DisconnectEndsTheConnection)
{
  Probe p;
  (void)SendHere(p, Transport::SERIAL, kConnect);
  (void)SendHere(p, Transport::SERIAL, kDisconnect);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::NONE);
}

TEST(UsbResetLeavesAnotherLinksConnectionAlone)
{
  std::atomic<uint32_t> resets{0U};
  Probe p;
  p.vendor.SetUsbResetSource(&resets);
  (void)SendHere(p, Transport::TCP, kConnect);
  resets.fetch_add(1U);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::TCP);
}

TEST(UsbResetEndsAUsbConnection)
{
  std::atomic<uint32_t> resets{0U};
  Probe p;
  p.vendor.SetUsbResetSource(&resets);
  (void)SendHere(p, Transport::USB, kConnect);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::USB);
  resets.fetch_add(1U);
  CHECK_EQ(p.vendor.ConnectedLink(), Transport::NONE);
}

// USB 绑定或解绑时：连接在别的链路上不复位 SWD，连接在 USB 上或没有连接时照常复位。复位
// 是 libxr 做的，这里只看 Core 怎么回答 / On a USB bind or unbind: SWD is not reset while
// the connection is on another link, and is as usual when it is on USB or there is none.
// libxr does the reset; this only looks at what Core answers.
TEST(SessionResetIsRefusedWhileAnotherLinkIsConnected)
{
  Probe p;
  (void)SendHere(p, Transport::RADIO, kConnect);
  CHECK(!p.dap.SessionResetAllowed());
}

TEST(SessionResetIsAllowedWithoutAConnection)
{
  Probe p;
  (void)SendHere(p, Transport::RADIO, kConnect);
  p.exec.LinkLost(Transport::RADIO);
  CHECK(p.dap.SessionResetAllowed());
}

TEST(SessionResetIsAllowedWhileConnectedOverUsb)
{
  Probe p;
  (void)SendHere(p, Transport::USB, kConnect);
  CHECK(p.dap.SessionResetAllowed());
}
