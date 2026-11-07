// SPDX-License-Identifier: Apache-2.0
// 执行上下文的等待、轮次和线程。判据：用户的要求（2026-10-22）：没有传输在等输入时占着
// 执行上下文，它等命令时别的链路照样得到回应；docs/protocol.md 3.5：一轮最多采一帧，命令
// 和采样轮流；executor.hpp：DAP 类只在一个线程里访问，邮箱里的链路轮流。
//
// The execution context's waiting, rounds and thread. Oracle: the user's requirement
// (2026-10-22): no transport holds the execution context while waiting for input, and
// other links are still answered while it waits for a command; docs/protocol.md 3.5: a
// round samples at most one frame, and commands and sampling take turns; executor.hpp:
// the DAP class is accessed by one thread only, and the links in the mailbox take turns.
#include <atomic>
#include <cstdint>
#include <thread>

#include "fake_dap.hpp"

namespace
{
using XrDap::Transport;
using namespace XrDapTest;

const uint8_t kInfo = XrDap::Byte(XrDap::DapCmd::INFO);

// 执行线程已经决定等下去，再也不会自己去看有没有事 / The execution thread has committed
// to waiting and will not look for work by itself any more.
bool WaitUntilParked(Probe& p, uint32_t waits_before)
{
  return WaitFor([&]() { return p.dap.waits.load() != waits_before; });
}

// 等节拍 0 这一帧做完。之前来的节拍会被开始时的清零吃掉，做帧时来的节拍不补，都不算数 /
// Waits until the frame of tick 0 is done. A tick before that is lost to the clearing at
// the start, and one during the frame is not made up.
void WaitForTickZero(Probe& p) { CHECK(WaitUntilParked(p, 0U)); }
}  // namespace

TEST(PostWakesAWaitingExecutionContext)
{
  Probe p;
  ExecThread exec(p);
  CHECK(WaitUntilParked(p, 0U));
  std::atomic<bool> answered{false};
  uint8_t cmd = 0U;
  std::thread link(
      [&]()
      {
        cmd = Send(p, Transport::TCP, kInfo).cmd;
        answered.store(true);
      });
  CHECK(WaitFor([&]() { return answered.load(); }));
  p.dap.WakeStep(false);  // 没被叫醒时放走链路 / Lets the link go when no wake-up came.
  link.join();
  CHECK_EQ(cmd, kInfo);
}

TEST(TickWakesAWaitingExecutionContext)
{
  Probe p;
  ConnectAndStartSampling(p);
  ExecThread exec(p);
  WaitForTickZero(p);
  uint32_t waits = 0U;
  for (uint32_t tick = 1U; tick <= 3U; ++tick)
  {
    CHECK(WaitUntilParked(p, waits));
    waits = p.dap.waits.load();
    p.vendor.OnTickIsr();
    CHECK(WaitFor([&]() { return p.swd.memory_reads.load() == tick + 1U; }));
  }
}

// 节拍可能正好赶在执行上下文要等之前到：它要么被叫醒，要么自己发现。丢了的话它再也不醒，
// 因为唤醒计数已经用掉 / A tick may arrive just as the execution context is about to
// wait: either it is woken or it notices the tick itself. If that were lost it would
// never wake, as the wake count is used up.
TEST(TickJustBeforeTheWaitIsNotLost)
{
  Probe p;
  ConnectAndStartSampling(p);
  ExecThread exec(p);
  std::atomic<bool> done{false};
  std::thread ticks(
      [&]()
      {
        while (!done.load())
        {
          p.vendor.OnTickIsr();
          std::this_thread::yield();
        }
      });
  CHECK(WaitFor([&]() { return p.swd.memory_reads.load() >= 3000U; }));
  done.store(true);
  ticks.join();
}

// 命令不会把采样挤掉，采样也不会把命令挤掉：一轮一条命令加一帧，没有跳拍 / Commands do
// not crowd sampling out and sampling does not crowd commands out: one command and one
// frame per round, with no skipped ticks.
TEST(OneRoundRunsOneCommandAndOneFrame)
{
  Probe p;
  NullTimer timer;
  ConnectAndStartSampling(p);
  p.exec.Run(timer, 0U);
  CHECK_EQ(p.swd.memory_reads.load(), 1U);
  for (uint32_t i = 1U; i <= 5U; ++i)
  {
    p.dap.Feed(kInfo);
    p.vendor.OnTickIsr();
    p.exec.Run(timer, 0U);
    CHECK_EQ(p.dap.usb_done.load(), i);
    CHECK_EQ(p.swd.memory_reads.load(), i + 1U);
  }
}

// SERIAL 的第二条在 TCP 执行之前就到了，也要等 TCP 先执行：连续发命令的链路占不住邮箱 /
// SERIAL's second request is there before TCP's runs, and still waits for it: a link
// sending back to back cannot hold the mailbox.
TEST(MailboxServesLinksInTurn)
{
  Probe p;
  NullTimer timer;
  const auto post = [&p](Transport t, uint8_t marker)
  {
    const uint8_t req[2] = {kInfo, marker};
    uint8_t resp[8] = {};
    uint16_t out = 0U;
    p.exec.Execute(t, req, sizeof(req), resp, sizeof(resp), out);
  };
  // 一直跑到又执行了一条请求，叫醒要先被 Step 吸收 / Runs until one more request has run;
  // Step absorbs the wake-ups first.
  const auto next_marker = [&]()
  {
    const size_t before = p.dap.markers.size();
    for (int round = 0; round < 8 && p.dap.markers.size() == before; ++round)
    {
      p.exec.Run(timer, 0U);
    }
    return p.dap.markers.size() == before ? uint8_t{0U} : p.dap.markers.back();
  };
  const auto posted = [&p](uint32_t count)
  { return WaitFor([&]() { return p.dap.wakes.load() == count; }); };

  std::thread serial(
      [&]()
      {
        post(Transport::SERIAL, 1U);
        post(Transport::SERIAL, 2U);
      });
  CHECK(posted(1U));
  std::thread tcp([&]() { post(Transport::TCP, 3U); });
  CHECK(posted(2U));
  CHECK_EQ(next_marker(), 1U);
  CHECK(posted(3U));
  CHECK_EQ(next_marker(), 3U);
  CHECK_EQ(next_marker(), 2U);
  serial.join();
  tcp.join();
}

// 一拍之后的命令执行完，这一拍一定采过了，每一拍正好一帧 / Once the commands after a tick
// have run, that tick has been sampled, so each tick gives exactly one frame.
TEST(SamplingAndCommandsShareTheExecutionContext)
{
  Probe p;
  ConnectAndStartSampling(p);
  ExecThread exec(p);
  WaitForTickZero(p);
  uint32_t frames = 0U;
  const uint32_t ticks = 50U;
  for (uint32_t i = 0U; i < ticks; ++i)
  {
    p.vendor.OnTickIsr();
    CHECK_EQ(Send(p, Transport::TCP, kInfo).cmd, kInfo);
    SendOverUsb(p, kInfo);
    const uint8_t read[3] = {0x85U, 0xFFU, 0x00U};  // SUB_READ
    uint8_t resp[128] = {};
    uint16_t out = 0U;
    p.exec.Execute(Transport::TCP, read, sizeof(read), resp, sizeof(resp), out);
    const SubReadHead head = ParseSubReadHead(resp);
    CHECK_EQ(head.status, 0x00U);
    CHECK_EQ(head.dropped, 0U);
    CHECK_EQ(head.skipped, 0U);
    frames += head.frames;
  }
  CHECK_EQ(frames, ticks + 1U);
}

// USB 和三条别的链路不停地发命令，节拍也不停地来：DAP 类只在执行线程上被调用，每条命令都
// 得到回应 / USB and three other links send commands without pause while ticks keep
// coming: the DAP class is called on the execution thread only, and every command is
// answered.
TEST(LinksAndTicksStayOnTheExecutionThread)
{
  Probe p;
  ConnectAndStartSampling(p);
  ExecThread exec(p);
  std::atomic<bool> done{false};
  std::thread ticks(
      [&]()
      {
        while (!done.load())
        {
          p.vendor.OnTickIsr();
          std::this_thread::yield();
        }
      });
  const uint32_t commands = 300U;
  std::thread links[3];
  const Transport kinds[3] = {Transport::TCP, Transport::SERIAL, Transport::RADIO};
  for (int i = 0; i < 3; ++i)
  {
    links[i] = std::thread(
        [&, i]()
        {
          for (uint32_t n = 0U; n < commands; ++n)
          {
            CHECK_EQ(Send(p, kinds[i], kInfo).cmd, kInfo);
          }
        });
  }
  for (uint32_t n = 0U; n < commands; ++n)
  {
    SendOverUsb(p, kInfo);
  }
  for (auto& link : links)
  {
    link.join();
  }
  done.store(true);
  ticks.join();
  CHECK_EQ(p.dap.usb_done.load(), commands);
  CHECK_EQ(p.dap.off_thread_calls.load(), 0U);
}

// Call 让执行上下文运行一个函数：函数在执行线程上跑，所以不会和 DAP 命令同时进行 / Call
// has the execution context run a function: it runs on the execution thread, so never
// together with a DAP command.
TEST(CallRunsOnTheExecutionThread)
{
  Probe p;
  ExecThread exec(p);
  std::thread::id ran_on;
  p.exec.Call(
      Transport::CONSOLE,
      +[](void* ctx)
      { *static_cast<std::thread::id*>(ctx) = std::this_thread::get_id(); },
      &ran_on);
  CHECK(ran_on == exec.Id());
}
