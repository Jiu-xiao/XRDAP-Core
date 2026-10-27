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
#include "xrdap/session/transport.hpp"
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
    const uint8_t req[2] = {usb_cmd_.load(), 0x01};
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
  /// 同时在 Run 里的调用数最大值：执行器的锁应保证它是 1 / Largest number of calls in Run
  /// at once: the executor's lock must keep it at 1.
  std::atomic<int> max_in_flight_{0};
  std::atomic<int> in_flight_{0};

 private:
  // 钩子没处理的标准命令由“libxr”回复成功 / Standard commands the hook leaves are
  // answered OK by the "libxr".
  ErrorCode Run(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                uint16_t& out)
  {
    out = 0U;
    const int now_in = in_flight_.fetch_add(1) + 1;
    int seen = max_in_flight_.load();
    while (now_in > seen && !max_in_flight_.compare_exchange_weak(seen, now_in))
    {
    }
    if (!hook_(ctx_, req, len, resp, cap, out))
    {
      resp[0] = req[0];
      resp[1] = 0x00U;
      out = 2U;
    }
    in_flight_.fetch_sub(1);
    return ErrorCode::OK;
  }

  Hook hook_ = nullptr;
  void* ctx_ = nullptr;
  SessionResetGate gate_ = nullptr;
  void* gate_ctx_ = nullptr;
  LibXR::Semaphore usb_{0};
  std::atomic<uint8_t> usb_cmd_{Byte(DapCmd::INFO)};
};

struct Probe
{
  NoSwd swd;
  XrDap::DapVendor vendor{swd};
  XrDap::DapActivity activity;
  FakeDap dap;
  XrDap::Executor<FakeDap> exec{dap, vendor, activity};
};

// TCP 发一条命令，返回用时（毫秒）和回复的前两个字节 / TCP sends one command; returns the
// time it took (ms) and the first two reply bytes.
struct Answer
{
  double ms;
  uint8_t cmd;
  uint8_t status;
};

Answer CommandOn(Probe& p, XrDap::Transport t, uint8_t cmd)
{
  const uint8_t req[2] = {cmd, 0x01};
  uint8_t resp[64] = {};
  uint16_t out = 0U;
  const auto t0 = Clock::now();
  p.exec.Execute(t, req, sizeof(req), resp, sizeof(resp), out);
  const std::chrono::duration<double, std::milli> dt = Clock::now() - t0;
  return {dt.count(), resp[0], resp[1]};
}

Answer TcpCommand(Probe& p, uint8_t cmd)
{
  return CommandOn(p, XrDap::Transport::TCP, cmd);
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

void ConnectionFollowsTheLink()
{
  // DAP_Connect 记下它来自哪条链路；别的链路丢了不影响，这条丢了或 DAP_Disconnect 才作废 /
  // DAP_Connect notes the link it came from; another link being lost changes nothing, this
  // one being lost or DAP_Disconnect makes it lapse.
  Probe p;
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::NONE);
  (void)CommandOn(p, XrDap::Transport::TCP, Byte(DapCmd::INFO));
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::NONE);  // 只查信息不算连接
  (void)CommandOn(p, XrDap::Transport::TCP, Byte(DapCmd::CONNECT));
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::TCP);
  p.exec.LinkLost(XrDap::Transport::SERIAL);
  p.exec.LinkLost(XrDap::Transport::USB);
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::TCP);
  p.exec.LinkLost(XrDap::Transport::TCP);
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::NONE);
  (void)CommandOn(p, XrDap::Transport::SERIAL, Byte(DapCmd::CONNECT));
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::SERIAL);
  (void)CommandOn(p, XrDap::Transport::SERIAL, Byte(DapCmd::DISCONNECT));
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::NONE);
  std::printf("  ConnectionFollowsTheLink: done\n");
}

void UsbResetEndsAUsbConnectionOnly()
{
  // 连接在 USB 上时总线复位使它作废；连接在别的链路上时 USB 复位不影响 / A bus reset makes
  // a connection on USB lapse; with the connection on another link it changes nothing.
  Probe p;
  static std::atomic<uint32_t> resets{0U};
  p.vendor.SetUsbResetSource(+[](void*) -> uint32_t { return resets.load(); }, nullptr);
  (void)CommandOn(p, XrDap::Transport::TCP, Byte(DapCmd::CONNECT));
  resets.fetch_add(1U);
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::TCP);
  p.exec.LinkLost(XrDap::Transport::TCP);
  (void)CommandOn(p, XrDap::Transport::USB, Byte(DapCmd::CONNECT));
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::USB);
  resets.fetch_add(1U);
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::NONE);
  std::printf("  UsbResetEndsAUsbConnectionOnly: done\n");
}

void LinksNeverRunTwoCommandsAtOnce()
{
  // USB 线程和三条别的链路的线程不停地发命令：libxr 的 DAP 类里同一时刻只有一条命令在
  // 执行 / The USB thread and three threads of other links send commands without pause:
  // only one command runs in the libxr DAP class at any moment.
  Probe p;
  p.dap.usb_cost_ms_ = 1U;
  std::atomic<bool> stop{false};
  std::thread feeder(
      [&]()
      {
        while (!stop.load())
        {
          p.dap.Feed(Byte(DapCmd::INFO));
          std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
      });
  std::thread usb(
      [&]()
      {
        while (!stop.load())
        {
          (void)p.exec.StepUsb(5U);
        }
      });
  std::thread links[3];
  const XrDap::Transport kinds[3] = {XrDap::Transport::TCP, XrDap::Transport::SERIAL,
                                     XrDap::Transport::RADIO};
  for (int i = 0; i < 3; ++i)
  {
    links[i] = std::thread(
        [&p, &kinds, i]()
        {
          for (int n = 0; n < 300; ++n)
          {
            (void)CommandOn(p, kinds[i], Byte(DapCmd::INFO));
          }
        });
  }
  for (auto& t : links)
  {
    t.join();
  }
  stop.store(true);
  feeder.join();
  usb.join();
  EXPECT(p.dap.max_in_flight_.load() == 1);
  std::printf("  LinksNeverRunTwoCommandsAtOnce: max in flight %d\n",
              p.dap.max_in_flight_.load());
}

void UsbRebindLeavesAnotherLinksSwdAlone()
{
  // USB 绑定或解绑时：连接在别的链路上不复位 SWD，连接在 USB 上或没有连接时照常复位（CH32
  // 上开关一次 USB 曾让无线连接的 DP 读一直回 ACK 7）/ On a USB bind or unbind: SWD is not
  // reset while the connection is on another link, and is as usual when it is on USB or
  // there is none (opening and closing USB once on the CH32 used to make every DP read of
  // the radio connection answer ACK 7).
  Probe p;
  (void)CommandOn(p, XrDap::Transport::RADIO, Byte(DapCmd::CONNECT));
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::RADIO);
  p.dap.Rebind();
  EXPECT(p.dap.swd_open_);
  p.exec.LinkLost(XrDap::Transport::RADIO);
  p.dap.Rebind();
  EXPECT(!p.dap.swd_open_);
  std::printf("  UsbRebindLeavesAnotherLinksSwdAlone: done\n");
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
  UsbRebindLeavesAnotherLinksSwdAlone();
  ConnectionFollowsTheLink();
  UsbResetEndsAUsbConnectionOnly();
  LinksNeverRunTwoCommandsAtOnce();
  SerialWriteResultsAreUnambiguous();
  std::printf("%d failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
