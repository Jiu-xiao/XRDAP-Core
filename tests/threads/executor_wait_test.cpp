// SPDX-License-Identifier: Apache-2.0
// 执行器在多线程下的公平性，按 libxr 的 Linux 系统层运行。判据：用户的要求（2026-10-22）
// ——没有传输在等输入时占着执行器；USB 线程阻塞等命令时，TCP 的 DAP_Info 和“忙”回复照样
// 马上得到回应；一条传输连续不断地来命令时，另一条最多等它一条命令。
//
// Executor fairness with threads, run on libxr's Linux system layer. Oracle: the user's
// requirement (2026-10-22): no transport holds the executor while waiting for input;
// while the USB thread blocks waiting for a command, TCP's DAP_Info and busy replies are
// answered at once; while one transport sends commands back to back, another waits for
// at most one of them.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "libxr.hpp"
#include "semaphore.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/serial/serial_link.hpp"
#include "xrdap/session/executor.hpp"
#include "xrdap/session/session_owner.hpp"
#include "xrdap/vendor/dap_vendor.hpp"

namespace
{
using Clock = std::chrono::steady_clock;
using LibXR::ErrorCode;
using XrDap::Byte;
using XrDap::DapCmd;

int g_failures = 0;
#define EXPECT(cond)                                                \
  do                                                                \
  {                                                                 \
    if (!(cond))                                                    \
    {                                                               \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                 \
    }                                                               \
  } while (0)

// 不接目标的 SWD：这些测试只发 DAP_Info 和 DAP_Connect / SWD with no target: these tests
// send only DAP_Info and DAP_Connect.
class NoSwd : public LibXR::Debug::Swd
{
 public:
  ErrorCode SetClockHz(uint32_t) override { return ErrorCode::OK; }
  void Close() override {}
  ErrorCode LineReset() override { return ErrorCode::OK; }
  ErrorCode EnterSwd() override { return ErrorCode::OK; }
  ErrorCode Transfer(const LibXR::Debug::SwdProtocol::Request&,
                     LibXR::Debug::SwdProtocol::Response&) override
  {
    return ErrorCode::FAILED;
  }
  void IdleClocks(uint32_t) override {}
  ErrorCode SeqWriteBits(uint32_t, const uint8_t*) override { return ErrorCode::OK; }
  ErrorCode SeqReadBits(uint32_t, uint8_t*) override { return ErrorCode::OK; }
};

// libxr DAP 类的替身：USB 命令经 Feed 到达；WaitForWork 和 Step 像 libxr 一样在没有命令时
// 阻塞到超时。Step 执行一条命令要 usb_cost_ms / Stand-in for the libxr DAP class: USB
// commands arrive through Feed; WaitForWork and Step block up to their timeout when no
// command is there, as libxr does. Step takes usb_cost_ms to run a command.
class FakeDap
{
 public:
  using Hook = bool (*)(void*, const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t&);
  using LengthHook = bool (*)(void*, const uint8_t*, uint16_t, uint16_t&);

  void SetCommandHook(Hook hook, void* ctx, LengthHook)
  {
    hook_ = hook;
    ctx_ = ctx;
  }

  using SessionResetGate = bool (*)(void*);
  void SetSessionResetGate(SessionResetGate gate, void* ctx)
  {
    gate_ = gate;
    gate_ctx_ = ctx;
  }

  // 像 libxr 一样：USB 绑定或解绑后，Step 线程按门的回答复位 SWD 的会话状态 / As libxr
  // does: after a USB bind or unbind the Step thread resets the SWD session state as the
  // gate allows.
  void Rebind()
  {
    if (gate_ == nullptr || gate_(gate_ctx_))
    {
      swd_open_ = false;
    }
  }
  bool swd_open_ = true;

  ErrorCode WaitForWork(uint32_t timeout_ms)
  {
    if (usb_.Wait(timeout_ms) != ErrorCode::OK)
    {
      return ErrorCode::TIMEOUT;
    }
    usb_.Post();
    return ErrorCode::OK;
  }

  ErrorCode Step(uint32_t timeout_ms)
  {
    if (usb_.Wait(timeout_ms) != ErrorCode::OK)
    {
      return ErrorCode::TIMEOUT;
    }
    const uint8_t req[2] = {usb_cmd_, 0x01};
    uint8_t resp[64];
    uint16_t out = 0U;
    (void)Run(req, sizeof(req), resp, sizeof(resp), out);
    std::this_thread::sleep_for(std::chrono::milliseconds(usb_cost_ms_));
    ++usb_done_;
    return ErrorCode::OK;
  }

  ErrorCode ExecuteExternal(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                            uint16_t& out)
  {
    return Run(req, len, resp, cap, out);
  }

  void Feed(uint8_t cmd)
  {
    usb_cmd_ = cmd;
    usb_.Post();
  }

  std::atomic<uint32_t> usb_done_{0U};
  uint32_t usb_cost_ms_ = 0U;

 private:
  // 钩子没处理的标准命令由“libxr”回复成功 / Standard commands the hook leaves are
  // answered OK by the "libxr".
  ErrorCode Run(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                uint16_t& out)
  {
    out = 0U;
    if (!hook_(ctx_, req, len, resp, cap, out))
    {
      resp[0] = req[0];
      resp[1] = 0x00U;
      out = 2U;
    }
    return ErrorCode::OK;
  }

  Hook hook_ = nullptr;
  void* ctx_ = nullptr;
  SessionResetGate gate_ = nullptr;
  void* gate_ctx_ = nullptr;
  LibXR::Semaphore usb_{0};
  uint8_t usb_cmd_ = Byte(DapCmd::INFO);
};

struct Probe
{
  NoSwd swd;
  XrDap::SessionOwner session;
  XrDap::DapVendor vendor{swd, session};
  XrDap::DapActivity activity;
  FakeDap dap;
  XrDap::Executor<FakeDap> exec{dap, session, vendor, activity};
};

// TCP 发一条命令，返回用时（毫秒）和回复的前两个字节 / TCP sends one command; returns the
// time it took (ms) and the first two reply bytes.
struct Answer
{
  double ms;
  uint8_t cmd;
  uint8_t status;
};

Answer TcpCommand(Probe& p, uint8_t cmd)
{
  const uint8_t req[2] = {cmd, 0x01};
  uint8_t resp[64] = {};
  uint16_t out = 0U;
  const auto t0 = Clock::now();
  p.exec.Execute(XrDap::Transport::TCP, req, sizeof(req), resp, sizeof(resp), out);
  const std::chrono::duration<double, std::milli> dt = Clock::now() - t0;
  return {dt.count(), resp[0], resp[1]};
}

// USB 线程照 ESP 的 DAP 线程那样循环 StepUsb(wait_ms) / The USB thread loops
// StepUsb(wait_ms) like ESP's DAP thread.
class UsbThread
{
 public:
  UsbThread(Probe& p, uint32_t wait_ms)
      : thread_(
            [this, &p, wait_ms]()
            {
              while (!stop_.load())
              {
                (void)p.exec.StepUsb(wait_ms);
              }
            })
  {
  }
  ~UsbThread()
  {
    stop_.store(true);
    thread_.join();
  }

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

void TcpAnsweredWhileUsbWaits()
{
  // USB 线程每次最多等 500 ms 而一直没有命令：TCP 的 DAP_Info 仍在几毫秒内得到回应 / The
  // USB thread waits up to 500 ms each time and no command comes: TCP's DAP_Info is still
  // answered within a few milliseconds.
  Probe p;
  UsbThread usb(p, 500U);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  double worst = 0.0;
  for (int i = 0; i < 10; ++i)
  {
    const Answer a = TcpCommand(p, Byte(DapCmd::INFO));
    EXPECT(a.cmd == Byte(DapCmd::INFO));
    worst = a.ms > worst ? a.ms : worst;
    std::this_thread::sleep_for(std::chrono::milliseconds(7));
  }
  std::printf("  TcpAnsweredWhileUsbWaits: worst %.2f ms\n", worst);
  EXPECT(worst < 50.0);
}

void BusyAnsweredWhileUsbOwnerWaits()
{
  // USB 执行过 DAP_Connect 成为主人，之后阻塞等下一条：TCP 的 DAP_Connect 马上得到
  // “忙”（02 FF），DAP_Info 照常回复 / USB ran DAP_Connect and owns the probe, then
  // blocks waiting for the next command: TCP's DAP_Connect gets busy (02 FF) at once and
  // DAP_Info is answered as usual.
  Probe p;
  UsbThread usb(p, 500U);
  p.dap.Feed(Byte(DapCmd::CONNECT));
  for (int i = 0; i < 100 && p.dap.usb_done_.load() == 0U; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT(p.session.Owner() == XrDap::Transport::USB);
  const Answer busy = TcpCommand(p, Byte(DapCmd::CONNECT));
  EXPECT(busy.cmd == Byte(DapCmd::CONNECT));
  EXPECT(busy.status == 0xFFU);
  EXPECT(busy.ms < 50.0);
  const Answer info = TcpCommand(p, Byte(DapCmd::INFO));
  EXPECT(info.cmd == Byte(DapCmd::INFO));
  EXPECT(info.ms < 50.0);
  // 状态灯命令不需要拥有探针：照常执行，不回忙，USB 仍是主人 / The status light command
  // does not need to own the probe: it runs, is not busy, and USB stays the owner.
  const Answer led = TcpCommand(p, Byte(DapCmd::HOST_STATUS));
  EXPECT(led.cmd == Byte(DapCmd::HOST_STATUS));
  EXPECT(led.status == 0x00U);
  EXPECT(p.session.Owner() == XrDap::Transport::USB);
  std::printf("  BusyAnsweredWhileUsbOwnerWaits: busy %.2f ms, info %.2f ms\n", busy.ms,
              info.ms);
}

void TcpServedBetweenBackToBackUsbCommands()
{
  // USB 不停地来 DAP_Info，每条执行 5 ms：TCP 最多等一条 USB 命令 / USB sends DAP_Info
  // back to back, 5 ms each: TCP waits for at most one USB command.
  Probe p;
  p.dap.usb_cost_ms_ = 5U;
  std::atomic<bool> feeding{true};
  std::thread feeder(
      [&]()
      {
        while (feeding.load())
        {
          p.dap.Feed(Byte(DapCmd::INFO));
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      });
  double worst = 0.0;
  {
    UsbThread usb(p, 10U);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int i = 0; i < 20; ++i)
    {
      const Answer a = TcpCommand(p, Byte(DapCmd::INFO));
      worst = a.ms > worst ? a.ms : worst;
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
  }
  feeding.store(false);
  feeder.join();
  std::printf("  TcpServedBetweenBackToBackUsbCommands: worst %.2f ms\n", worst);
  EXPECT(worst < 25.0);
}
void ForwardedDisconnectEndsTheSession()
{
  // CH32 的无线主机把 USB 的命令全部交给转发器执行，DAP_Disconnect 也是：主人发的
  // DAP_Disconnect 不管由谁执行都结束主人的会话（protocol 3.1），之后无线的命令不回忙。
  // The CH32 radio host has its forwarder execute every USB command, DAP_Disconnect
  // included: the owner's DAP_Disconnect ends its session whoever executes it (protocol
  // 3.1), and radio commands are not busy afterwards.
  Probe p;
  p.exec.SetForwarder(
      +[](void*, XrDap::Transport, const uint8_t* req, uint16_t, uint8_t* resp, uint16_t,
          uint16_t& out)
      {
        resp[0] = req[0];
        resp[1] = 0x00U;
        out = 2U;
        return true;
      },
      nullptr);
  auto run = [&](XrDap::Transport t, uint8_t cmd)
  {
    const uint8_t req[2] = {cmd, 0x01};
    uint8_t resp[8] = {};
    uint16_t out = 0U;
    p.exec.Execute(t, req, sizeof(req), resp, sizeof(resp), out);
    return resp[1];
  };
  EXPECT(run(XrDap::Transport::USB, Byte(DapCmd::CONNECT)) == 0x00U);
  EXPECT(p.session.Owner() == XrDap::Transport::USB);
  EXPECT(run(XrDap::Transport::USB, Byte(DapCmd::DISCONNECT)) == 0x00U);
  EXPECT(p.session.Owner() == XrDap::Transport::NONE);
  EXPECT(run(XrDap::Transport::RADIO, Byte(DapCmd::CONNECT)) == 0x00U);
  EXPECT(p.session.Owner() == XrDap::Transport::RADIO);
  std::printf("  ForwardedDisconnectEndsTheSession: done\n");
}

void UsbRebindLeavesAnotherOwnersSwdAlone()
{
  // USB 绑定或解绑只属于 USB 自己的会话：无线拥有探针时不复位 SWD，USB 拥有或没有主人
  // 时照常复位（CH32 上开关一次 USB 曾让无线会话的 DP 读一直回 ACK 7）/ A USB bind or
  // unbind belongs to USB's own session: with the radio owning the probe SWD is not
  // reset, with USB owning it or nobody it is (opening and closing USB once on the CH32
  // used to make every DP read of the radio session answer ACK 7).
  Probe p;
  const uint8_t connect[2] = {Byte(DapCmd::CONNECT), 0x01};
  uint8_t resp[8] = {};
  uint16_t out = 0U;
  p.exec.Execute(XrDap::Transport::RADIO, connect, sizeof(connect), resp, sizeof(resp),
                 out);
  EXPECT(p.session.Owner() == XrDap::Transport::RADIO);
  p.dap.Rebind();
  EXPECT(p.dap.swd_open_);
  p.exec.EndSession(XrDap::Transport::RADIO);
  p.dap.Rebind();
  EXPECT(!p.dap.swd_open_);
  std::printf("  UsbRebindLeavesAnotherOwnersSwdAlone: done\n");
}
void SerialWriteResultsAreUnambiguous()
{
  // 串口链路不带完成通知地写：OK 表示已进队列、一定发出，不重试；FULL 和 BUSY 表示没进
  // 队列，重试；别的返回值丢帧。阻塞写的 TIMEOUT 含义不定，链路不用它 / The serial link
  // writes without completion notification: OK means queued and it will be sent, no
  // retry; FULL and BUSY mean nothing was queued, retry; any other value drops the frame.
  // A blocking write's TIMEOUT is ambiguous, and the link does not use it.
  using Link = XrDap::SerialLink<XrDap::Executor<FakeDap>, 64U>;
  EXPECT(Link::TxStepFor(ErrorCode::OK) == Link::TxStep::QUEUED);
  EXPECT(Link::TxStepFor(ErrorCode::FULL) == Link::TxStep::RETRY);
  EXPECT(Link::TxStepFor(ErrorCode::BUSY) == Link::TxStep::RETRY);
  EXPECT(Link::TxStepFor(ErrorCode::NOT_SUPPORT) == Link::TxStep::FAILED);
  std::printf("  SerialWriteResultsAreUnambiguous: done\n");
}
}  // namespace

int main()
{
  LibXR::PlatformInit();
  TcpAnsweredWhileUsbWaits();
  BusyAnsweredWhileUsbOwnerWaits();
  TcpServedBetweenBackToBackUsbCommands();
  ForwardedDisconnectEndsTheSession();
  UsbRebindLeavesAnotherOwnersSwdAlone();
  SerialWriteResultsAreUnambiguous();
  std::printf("%d failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
