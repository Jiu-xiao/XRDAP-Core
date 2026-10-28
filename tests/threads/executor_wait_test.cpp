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
#include <cstring>
#include <thread>

#include "libxr.hpp"
#include "semaphore.hpp"
#include "xrdap/activity/dap_activity.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/serial/serial_link.hpp"
#include "xrdap/session/executor.hpp"
#include "xrdap/session/transport.hpp"
#include "xrdap/vendor/dap_vendor.hpp"
#include "model_swd.hpp"
#include "xrdap/control/usb_control.hpp"

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
    if (usb_cmds_.load() == 0U)
    {
      return ErrorCode::OK;  // 多出的唤醒，没有命令 / A spare wake-up, no command.
    }
    usb_cmds_.fetch_sub(1U);
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
    usb_cmds_.fetch_add(1U);
    usb_.Post();
  }

  void WakeStep(bool) { usb_.Post(); }
  class VendorPort
  {
   public:
    virtual LibXR::ErrorCode OnVendorOut(bool, uint8_t, LibXR::ConstRawData) = 0;
    virtual LibXR::ConstRawData OnVendorIn(bool, uint8_t) = 0;

   protected:
    ~VendorPort() = default;
  };
  void SetVendorPort(VendorPort* port) { port_ = port; }
  VendorPort* port_ = nullptr;

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
  std::atomic<uint32_t> usb_cmds_{0U};
};

struct Probe
{
  ModelSwd swd;
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
  if (t == XrDap::Transport::USB)
  {
    // USB 的命令经 libxr 的 DAP 类到达 / USB commands arrive through the libxr DAP class.
    const uint32_t before = p.dap.usb_done_.load();
    p.dap.Feed(cmd);
    while (p.dap.usb_done_.load() == before)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {0.0, cmd, 0x00};
  }
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

struct NullTimer : XrDap::Vendor::TickTimer
{
  void Start(uint32_t) override {}
  void Stop() override {}
};

// 执行上下文的线程照 ESP 的 DAP 线程那样循环 Run(timer, wait_ms) / The execution context's
// thread loops Run(timer, wait_ms) like ESP's DAP thread.
class UsbThread
{
 public:
  UsbThread(Probe& p, uint32_t wait_ms)
      : thread_(
            [this, &p, wait_ms]()
            {
              NullTimer timer;
              while (!stop_.load())
              {
                p.exec.Run(timer, wait_ms);
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
  // 执行线程每次最多等 500 ms 而一直没有命令：TCP 的 DAP_Info 仍在几毫秒内得到回应 / The
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

void SamplingSharesTheContextWithCommands()
{
  // 采样（节拍中断每 2 ms 一次，由 OnTickIsr 叫醒）和 TCP、USB 的命令在同一个执行上下文里
  // 一件一件做：命令一直得到回应，采样一直有帧产出，同一时刻只有一条命令在 DAP 类里 / Sampling
  // (the tick interrupt every 2 ms, waking through OnTickIsr) and the commands of TCP and USB run
  // one after the other in the same execution context: commands keep being answered, sampling
  // keeps producing frames, and only one command is in the DAP class at a time.
  Probe p;
  p.dap.usb_cost_ms_ = 1U;
  UsbThread usb(p, 50U);
  (void)CommandOn(p, XrDap::Transport::TCP, Byte(DapCmd::CONNECT));
  // SUB_START：周期 2000 us，一个条目（AP 0，u32，0x08000000）/ SUB_START: period 2000 us,
  // one item (AP 0, u32 at 0x08000000).
  const uint8_t start[] = {0x83U, 0xD0U, 0x07U, 0U, 0U, 0U, 1U, 0U, 0, 1, 0, 4, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0};
  uint8_t resp[128] = {};
  uint16_t out = 0U;
  p.exec.Execute(XrDap::Transport::TCP, start, sizeof(start), resp, sizeof(resp), out);
  EXPECT(resp[1] == 0x00U);
  EXPECT(p.vendor.Sampling());

  std::atomic<bool> stop{false};
  std::thread ticks(
      [&]()
      {
        while (!stop.load())
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          p.vendor.OnTickIsr();
        }
      });
  std::thread feeder(
      [&]()
      {
        while (!stop.load())
        {
          p.dap.Feed(Byte(DapCmd::INFO));
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      });
  double worst = 0.0;
  for (int i = 0; i < 100; ++i)
  {
    const Answer a = TcpCommand(p, Byte(DapCmd::INFO));
    EXPECT(a.cmd == Byte(DapCmd::INFO));
    worst = a.ms > worst ? a.ms : worst;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  stop.store(true);
  ticks.join();
  feeder.join();
  EXPECT(worst < 50.0);
  EXPECT(p.dap.max_in_flight_.load() == 1);
  // SUB_READ：采样在跑，帧已经产出 / SUB_READ: sampling runs and frames were produced.
  const uint8_t read[] = {0x85U, 0xFFU, 0x00U};
  p.exec.Execute(XrDap::Transport::TCP, read, sizeof(read), resp, sizeof(resp), out);
  EXPECT(resp[1] == 0x00U);
  const uint16_t frames = static_cast<uint16_t>(resp[22] | (resp[23] << 8));
  std::printf("  SamplingSharesTheContextWithCommands: worst %.2f ms, %u frames\n", worst,
              frames);
  EXPECT(frames >= 5U);  // 回复只放得下 6 帧 / the reply holds 6 frames at most
}

void ExecuteHereRunsInTheCallersContext()
{
  // 单循环探针：链路在主循环里被轮询，命令在调用者的上下文里直接执行，没有执行线程也不等 /
  // Single-loop probe: the link is polled in the main loop and the command runs directly in the
  // caller's context, without an execution thread and without waiting.
  Probe p;
  const uint8_t connect[2] = {Byte(DapCmd::CONNECT), 0x01};
  uint8_t resp[64] = {};
  uint16_t out = 0U;
  p.exec.ExecuteHere(XrDap::Transport::RADIO, connect, sizeof(connect), resp, sizeof(resp),
                     out);
  EXPECT(out == 2U && resp[0] == Byte(DapCmd::CONNECT));
  EXPECT(p.vendor.ConnectedLink() == XrDap::Transport::RADIO);
  std::printf("  ExecuteHereRunsInTheCallersContext: done\n");
}

void ControlRequestsRunInTheExecutionContext()
{
  // 控制请求和 DAP 命令一样在执行上下文里一次一条：处理函数里不会有第二个调用者；没有处理函数时
  // 回“不支持” / Control requests run in the execution context one at a time like DAP commands: the
  // handler never has a second caller; without a handler the answer is "unsupported".
  {
    Probe none;
    UsbThread usb(none, 5U);
    uint8_t resp[16] = {};
    uint16_t out = 0U;
    const uint8_t req[3] = {0x42U, 0x01U, 0x02U};
    none.exec.Control(XrDap::Transport::CONSOLE, req, sizeof(req), resp, sizeof(resp), out);
    EXPECT(out == 2U && resp[0] == 0x42U && resp[1] == XrDap::Control::kStatusUnsupported);
  }
  Probe p;

  static std::atomic<int> in_handler{0};
  static std::atomic<int> max_in_handler{0};
  static std::atomic<int> calls{0};
  p.exec.SetControl(
      +[](void*, const uint8_t* r, uint16_t, uint8_t* o, uint16_t) -> uint16_t
      {
        const int now = in_handler.fetch_add(1) + 1;
        int seen = max_in_handler.load();
        while (now > seen && !max_in_handler.compare_exchange_weak(seen, now))
        {
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        calls.fetch_add(1);
        in_handler.fetch_sub(1);
        o[0] = r[0];
        o[1] = XrDap::Control::kStatusOk;
        o[2] = r[1];
        return 3U;
      },
      nullptr);
  {
    // 有控制口的探针在 INFO 里报功能位 5 / A probe with a control port reports feature bit 5
    // in INFO.
    const uint8_t info[1] = {0x80U};
    uint8_t r[64] = {};
    uint16_t o = 0U;
    p.exec.ExecuteHere(XrDap::Transport::TCP, info, sizeof(info), r, sizeof(r), o);
    EXPECT(o >= 7U && r[1] == 0x00U);
    const uint32_t features = r[3] | (r[4] << 8) | (r[5] << 16) | (static_cast<uint32_t>(r[6]) << 24);
    EXPECT((features & (1U << 5)) != 0U);
  }
  UsbThread usb(p, 5U);
  std::thread links[4];
  const XrDap::Transport kinds[4] = {XrDap::Transport::TCP, XrDap::Transport::SERIAL,
                                     XrDap::Transport::RADIO, XrDap::Transport::CONSOLE};
  for (int i = 0; i < 4; ++i)
  {
    links[i] = std::thread(
        [&p, &kinds, i]()
        {
          for (int n = 0; n < 100; ++n)
          {
            uint8_t r[16] = {};
            uint16_t o = 0U;
            const uint8_t q[2] = {0x10U, static_cast<uint8_t>(i)};
            p.exec.Control(kinds[i], q, sizeof(q), r, sizeof(r), o);
            EXPECT(o == 3U && r[0] == 0x10U && r[1] == 0U && r[2] == i);
          }
        });
  }
  for (auto& t : links)
  {
    t.join();
  }
  EXPECT(calls.load() == 400);
  EXPECT(max_in_handler.load() == 1);
  std::printf("  ControlRequestsRunInTheExecutionContext: done\n");
}

// 一次 USB 控制传输：SEND 一个请求，再 RECV 到应答 / One USB control exchange: SEND a request,
// then RECV the answer.
size_t UsbExchange(FakeDap& dap, const uint8_t* q, size_t n, uint8_t* a)
{
  EXPECT(dap.port_->OnVendorOut(false, 1U, {q, n}) == ErrorCode::OK);
  // 新 SEND 在应答取走前被拒绝 / A new SEND is refused before the answer is taken.
  EXPECT(dap.port_->OnVendorOut(false, 1U, {q, n}) != ErrorCode::OK);
  for (int i = 0; i < 2000; ++i)
  {
    const LibXR::ConstRawData d = dap.port_->OnVendorIn(false, 2U);
    if (d.size_ != 0U)
    {
      std::memcpy(a, d.addr_, d.size_);
      return d.size_;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 0U;
}

void UsbControlTakesTwoTransfers()
{
  // USB 控制口：SEND 收进请求并叫醒执行上下文，处理后 RECV 取应答；没好时 RECV 取到 0 字节；
  // 应答取走前新的 SEND 被拒绝 / USB control port: SEND copies the request and wakes the execution
  // context, RECV takes the answer after it is handled; not ready, RECV gets 0 bytes; a new SEND is
  // refused until the answer is taken.
  const uint8_t req[3] = {0x21U, 0xAAU, 0xBBU};
  uint8_t ans[96] = {};
  {
    // 没有处理函数：不支持 / No handler: unsupported.
    Probe p;
    XrDap::Control::UsbControl<FakeDap> port(p.dap, p.exec);
    UsbThread usb(p, 50U);
    const size_t an = UsbExchange(p.dap, req, sizeof(req), ans);
    EXPECT(an == 2U && ans[0] == 0x21U && ans[1] == XrDap::Control::kStatusUnsupported);
    EXPECT(p.dap.port_->OnVendorIn(false, 2U).size_ == 0U);  // 没有应答 / no answer
    EXPECT(p.dap.port_->OnVendorOut(false, 9U, {req, sizeof(req)}) != ErrorCode::OK);
  }
  {
    Probe p;
    p.exec.SetControl(
        +[](void*, const uint8_t* r, uint16_t len, uint8_t* o, uint16_t) -> uint16_t
        {
          o[0] = r[0];
          o[1] = XrDap::Control::kStatusOk;
          o[2] = static_cast<uint8_t>(len);
          return 3U;
        },
        nullptr);
    XrDap::Control::UsbControl<FakeDap> port(p.dap, p.exec);
    UsbThread usb(p, 50U);
    for (int i = 0; i < 20; ++i)
    {
      const size_t an = UsbExchange(p.dap, req, sizeof(req), ans);
      EXPECT(an == 3U && ans[0] == 0x21U && ans[1] == 0U && ans[2] == 3U);
    }
  }
  std::printf("  UsbControlTakesTwoTransfers: done\n");
}

void CallRunsInTheExecutionContext()
{
  // Call 让执行上下文运行一个函数:函数在执行线程里跑,与 DAP 命令不同时进行 / Call has the
  // execution context run a function: it runs in the execution thread, never together with a DAP
  // command.
  Probe p;
  p.dap.usb_cost_ms_ = 1U;
  UsbThread usb(p, 5U);
  std::atomic<bool> stop{false};
  std::thread feeder(
      [&]()
      {
        while (!stop.load())
        {
          p.dap.Feed(Byte(DapCmd::INFO));
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      });
  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<int> runs{0};
  for (int i = 0; i < 100; ++i)
  {
    struct Ctx
    {
      Probe* p;
      std::thread::id caller;
      std::atomic<int>* runs;
      bool other_thread = false;
      bool dap_idle = false;
    } ctx{&p, caller, &runs};
    p.exec.Call(
        XrDap::Transport::CONSOLE,
        +[](void* c)
        {
          auto* x = static_cast<Ctx*>(c);
          x->other_thread = std::this_thread::get_id() != x->caller;
          x->dap_idle = x->p->dap.in_flight_.load() == 0;
          x->runs->fetch_add(1);
        },
        &ctx);
    EXPECT(ctx.other_thread && ctx.dap_idle);
  }
  stop.store(true);
  feeder.join();
  EXPECT(runs.load() == 100);
  std::printf("  CallRunsInTheExecutionContext: done\n");
}

void ConnectionFollowsTheLink()
{
  // DAP_Connect 记下它来自哪条链路；别的链路丢了不影响，这条丢了或 DAP_Disconnect 才作废 /
  // DAP_Connect notes the link it came from; another link being lost changes nothing, this
  // one being lost or DAP_Disconnect makes it lapse.
  Probe p;
  UsbThread usb(p, 5U);
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
  UsbThread usb(p, 5U);
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
        NullTimer timer;
        while (!stop.load())
        {
          p.exec.Run(timer, 5U);
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
  UsbThread usb(p, 5U);
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
  SamplingSharesTheContextWithCommands();
  ExecuteHereRunsInTheCallersContext();
  ControlRequestsRunInTheExecutionContext();
  UsbControlTakesTwoTransfers();
  CallRunsInTheExecutionContext();
  UsbRebindLeavesAnotherLinksSwdAlone();
  ConnectionFollowsTheLink();
  UsbResetEndsAUsbConnectionOnly();
  LinksNeverRunTwoCommandsAtOnce();
  SerialWriteResultsAreUnambiguous();
  std::printf("%d failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
