// SPDX-License-Identifier: Apache-2.0
// Sampler 任务模式（RunTask、OnTickIsr 里的 PostFromCallback、Start/Stop/CommandEnd 里
// 的 Post、让路停车）在 libxr 的 Linux 系统层上跑。sampler_recovery_test.cpp 只走 Poll，
// 这里补任务模式，作为把 task_mode_ 拆成两个策略类型之前的行为基准。
//
// 判据：docs/protocol.md 3.5、xrdap/vendor/sampler.hpp 和 command_yield.hpp 头部的说明。
// 确定性：不读真实时钟来判对错。节拍由测试线程手动调用 OnTickIsr 注入；等待都带超时，
// 并且等的是一个明确的状态（帧到了、定时器 Stop 了、采样停车了）；"不应该发生"的事用一小
// 段静置时间后再查，静置不够只会让坏结果漏过，不会让好结果失败。
//
// Sampler task mode (RunTask, the PostFromCallback in OnTickIsr, the Posts in
// Start/Stop/CommandEnd, parking to yield to commands) on libxr's Linux system layer.
// sampler_recovery_test.cpp only goes through Poll; this fills in task mode as the
// behaviour baseline before task_mode_ is split into two policy types.
//
// Oracle: docs/protocol.md 3.5 and the headers of sampler.hpp and command_yield.hpp.
// Determinism: no verdict depends on the real clock. Ticks are injected by calling
// OnTickIsr from the test thread; every wait has a timeout and waits for a definite state
// (frame arrived, timer stopped, sampler parked); something that must not happen is
// checked after a short settle, which can only let a bad result slip through, never fail
// a good one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "debug/swd.hpp"
#include "libxr_system.hpp"
#include "mutex.hpp"
#include "semaphore.hpp"
#include "timebase.hpp"

// 测试要看采样的内部状态（task_mode_、是否停车、信号量计数）来确定"线程已经到了哪一步"，
// Sampler 没有公开这些，这里只在本文件里放开 private。
// The test needs the sampler's internal state (task_mode_, parked, semaphore count) to
// know which step the thread has reached; Sampler does not expose it, so private is
// opened in this file only.
#define private public
#include "xrdap/vendor/mem_ap.hpp"
#include "xrdap/vendor/sampler.hpp"
#undef private

#ifndef XRDAP_TEST_KNOWN_ISSUES
#define XRDAP_TEST_KNOWN_ISSUES 0
#endif

using namespace XrDap::Vendor;
using LibXR::ErrorCode;
namespace P = LibXR::Debug::SwdProtocol;

namespace
{
int g_soft_failures = 0;

// 致命：直接结束进程（采样线程永远不返回，没有别的收尾办法）/ Fatal: end the process
// (the sampling thread never returns, so there is no other way to wind down).
#define CHECK(x)                                                      \
  do                                                                  \
  {                                                                   \
    if (!(x))                                                         \
    {                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      std::fflush(stderr);                                            \
      std::_Exit(1);                                                  \
    }                                                                 \
  } while (0)

// 非致命：用在已知问题用例里，每个变体各报各的 / Non-fatal: used by the known-issue case so
// every variant reports on its own.
#define SOFT_CHECK(x, what)                                                        \
  do                                                                               \
  {                                                                                \
    if (!(x))                                                                      \
    {                                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #x, what); \
      ++g_soft_failures;                                                           \
    }                                                                              \
  } while (0)

constexpr unsigned kWaitMs = 5000U;   // 等一个明确状态的上限 / Upper bound for a definite state
constexpr unsigned kSettleMs = 30U;   // "不应发生"的静置时间 / Settle time for "must not happen"
// 周期取很大：定时器是假的，周期只进账目；大周期让"占用+间隙"在任何调度抖动下都不超过
// 一个周期，下一个要采的节拍恒为上一拍加一。
// The period is large: the timer is fake and the period only enters the accounting; with
// a big one, "busy + gap" stays below one period under any scheduling jitter, so the next
// tick to sample is always the previous one plus one.
constexpr uint32_t kPeriodUs = 1000000U;

template <typename Pred>
bool WaitUntil(Pred pred, unsigned timeout_ms = kWaitMs)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;)
  {
    if (pred())
    {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

void Settle() { std::this_thread::sleep_for(std::chrono::milliseconds(kSettleMs)); }

// ADIv5 posted-read 目标模型：地址 a 读出 (a ^ (0x12340000 + ap)) + bias / ADIv5
// posted-read target model: address a reads (a ^ (0x12340000 + ap)) + bias.
class ModelSwd final : public LibXR::Debug::Swd
{
 public:
  std::atomic<uint32_t> bias{0U};
  std::atomic<unsigned> memory_reads{0U};
  std::atomic<unsigned> csw_reads{0U};

  ErrorCode SetClockHz(uint32_t) override { return ErrorCode::OK; }
  void Close() override {}
  ErrorCode EnterSwd() override { return LineReset(); }
  ErrorCode LineReset() override
  {
    select_ = 0;
    InvalidateSelectCache();
    return ErrorCode::OK;
  }
  void IdleClocks(uint32_t) override {}
  ErrorCode SeqWriteBits(uint32_t, const uint8_t*) override { return ErrorCode::OK; }
  ErrorCode SeqReadBits(uint32_t, uint8_t*) override { return ErrorCode::OK; }
  ErrorCode Transfer(const P::Request& req, P::Response& resp) override
  {
    resp = {};
    resp.ack = P::Ack::OK;
    if (req.port == P::Port::DP)
    {
      if (req.rnw)
      {
        resp.rdata = req.addr2b == 0 ? 0x2BA01477U : posted_;
      }
      else if (req.addr2b == 2)
      {
        select_ = req.wdata;
      }
      return ErrorCode::OK;
    }
    const unsigned ap = select_ >> 24;
    CHECK(ap < 2);
    CHECK((select_ & 0xF0U) == 0U);
    if (req.rnw)
    {
      resp.rdata = posted_;
      if (req.addr2b == 0)
      {
        csw_reads.fetch_add(1U);
        posted_ = csw_[ap];
      }
      else if (req.addr2b == 3)
      {
        memory_reads.fetch_add(1U);
        posted_ = (tar_[ap] ^ (0x12340000U + ap)) + bias.load();
        if ((csw_[ap] & 0x30U) == 0x10U)
        {
          tar_[ap] = (tar_[ap] & ~1023U) | ((tar_[ap] + 4U) & 1023U);
        }
      }
    }
    else if (req.addr2b == 0)
    {
      csw_[ap] = req.wdata;
    }
    else if (req.addr2b == 1)
    {
      tar_[ap] = req.wdata;
    }
    else
    {
      CHECK(false);  // These tests never write target memory.
    }
    return ErrorCode::OK;
  }

 private:
  // 只在 MemAp::Lock 里访问 / Accessed under MemAp::Lock only.
  uint32_t select_ = 0U;
  uint32_t posted_ = 0U;
  uint32_t csw_[2] = {0x23000002U, 0x23000002U};
  uint32_t tar_[2] = {};
};

struct FakeTimer final : TickTimer
{
  std::atomic<unsigned> starts{0U};
  std::atomic<unsigned> stops{0U};
  std::atomic<uint32_t> period_us{0U};
  void Start(uint32_t period) override
  {
    period_us.store(period);
    starts.fetch_add(1U);
  }
  void Stop() override { stops.fetch_add(1U); }
};

// 两个条目：A 每拍采，B 隔一拍采（偶数拍），两者地址相邻，偶数拍合成一次连读。
// 帧 = 节拍号 4 + 时间戳 6 + 位图 1 + 值（偶数拍 8 字节，奇数拍 4 字节）。
// Two items: A every tick, B every other tick (even ticks), at adjacent addresses, so
// an even tick is one burst read. Frame = tick 4 + timestamp 6 + bitmap 1 + values (8
// bytes on even ticks, 4 on odd ticks).
constexpr uint32_t kAddrA = 0x08000000U;
constexpr uint32_t kAddrB = 0x08000004U;

void AppendItem(std::vector<uint8_t>& out, uint8_t id, uint32_t base, uint16_t div)
{
  out.push_back(0U);  // ap
  out.push_back(id);
  out.push_back(0U);
  out.push_back(4U);  // size
  const auto at = out.size();
  out.resize(at + 4U);
  Put32(out.data() + at, base);
  out.push_back(0U);  // n: no dereference
  const auto tail = out.size();
  out.resize(tail + 6U);
  Put16(out.data() + tail, 0U);        // final_off
  Put16(out.data() + tail + 2U, div);  // div
  Put16(out.data() + tail + 4U, 0U);   // phase
}

std::vector<uint8_t> Entries()
{
  std::vector<uint8_t> bytes;
  AppendItem(bytes, 1U, kAddrA, 1U);
  AppendItem(bytes, 2U, kAddrB, 2U);
  return bytes;
}

struct Frame
{
  uint32_t tick = 0U;
  std::vector<uint8_t> body;  // bitmap + values; the timestamp is left out
};

struct Rig
{
  ModelSwd swd;
  MemAp mem;
  FakeTimer timer;
  Sampler sampler;
  std::vector<uint8_t> entries = Entries();
  std::vector<Frame> frames;  // taken from the ring so far
  uint32_t skipped = 0U;      // cumulative, as of the last Take
  uint32_t dropped = 0U;

  Rig() : mem(swd), sampler(mem, +[](void*) { return true; }, nullptr) {}

  uint8_t Start()
  {
    return sampler.Start({entries.data(), entries.data() + entries.size(), 2}, kPeriodUs,
                         false);
  }

  void Drain()
  {
    uint8_t buf[1024];
    for (;;)
    {
      const auto got = sampler.TakeFrames(buf, sizeof(buf));
      skipped = got.skipped;
      dropped = got.dropped;
      if (got.frames == 0U)
      {
        return;
      }
      uint32_t off = 0U;
      for (uint16_t i = 0; i < got.frames; ++i)
      {
        Frame f;
        f.tick = Get32(buf + off);
        const uint32_t len = 10U + 1U + ((f.tick % 2U == 0U) ? 8U : 4U);
        f.body.assign(buf + off + 10U, buf + off + len);
        frames.push_back(f);
        off += len;
      }
      CHECK(off == got.bytes);
    }
  }

  /// 等到至少 n 帧；超时返回 false / Wait for at least n frames; false on timeout.
  bool WaitFrames(size_t n, unsigned timeout_ms = kWaitMs)
  {
    return WaitUntil(
        [&]
        {
          Drain();
          return frames.size() >= n;
        },
        timeout_ms);
  }

  /// 采样线程（若在跑）已把手上的一拍做完：取一次链路锁，Service 整个在锁里 / The
  /// sampling thread (if running) has finished the tick in hand: Service runs entirely
  /// under the link lock, so taking it once waits for that.
  /// @note 只在刚看到期望的帧之后用，那时不会再有未处理的唤醒 / Use only right after the
  ///       expected frame was seen, when no wake is left unhandled.
  void Quiesce() { MemAp::Lock lock(mem); }

  void CheckFrame(size_t index, uint32_t tick, uint32_t bias) const
  {
    CHECK(index < frames.size());
    const Frame& f = frames[index];
    CHECK(f.tick == tick);
    CHECK(f.body.size() == ((tick % 2U == 0U) ? 9U : 5U));
    CHECK(f.body[0] == 0U);  // no failed item
    CHECK(Get32(f.body.data() + 1) == ((kAddrA ^ 0x12340000U) + bias));
    if (tick % 2U == 0U)
    {
      CHECK(Get32(f.body.data() + 5) == ((kAddrB ^ 0x12340000U) + bias));
    }
  }

  /// 开始后所有帧读完时 memory_reads 的值：偶数拍 2 次，奇数拍 1 次 / memory_reads
  /// once the frames of ticks 0..n-1 are read: 2 on an even tick, 1 on an odd one.
  static unsigned ReadsAfter(uint32_t n)
  {
    unsigned reads = 0U;
    for (uint32_t t = 0U; t < n; ++t)
    {
      reads += (t % 2U == 0U) ? 2U : 1U;
    }
    return reads;
  }
};

// 采样线程里 RunTask 不返回，Rig 和线程一起泄漏到进程结束 / RunTask never returns in
// the sampling thread, so the Rig and the thread leak until the process ends.
Rig& NewRig() { return *new Rig; }

void SpawnTask(Rig& r)
{
  std::thread([&r] { r.sampler.RunTask(r.timer); }).detach();
}

void StartTaskAndWaitReady(Rig& r)
{
  SpawnTask(r);
  // RunTask 第一行置 task_mode_；之后 Start/Stop/ISR 才会 Post / RunTask sets task_mode_
  // on its first line; only after that do Start, Stop and the ISR Post.
  CHECK(WaitUntil([&] { return r.sampler.task_mode_.load() == 1U; }));
}

// ---------------------------------------------------------------------------------
// 1. 任务模式基本流程 / Basic task-mode flow
// ---------------------------------------------------------------------------------
void TaskBasicFlow()
{
  Rig& r = NewRig();
  StartTaskAndWaitReady(r);
  r.swd.bias = 1000U;
  CHECK(r.Start() == kStatusOk);
  CHECK(r.sampler.Active());
  CHECK(r.WaitFrames(1));  // tick 0 at once, woken by Start's Post
  CHECK(r.timer.starts == 1U && r.timer.period_us == kPeriodUs);
  CHECK(r.timer.stops == 0U);
  r.CheckFrame(0, 0U, 1000U);

  for (uint32_t i = 1U; i <= 8U; ++i)
  {
    r.swd.bias = 1000U + 7U * i;
    r.sampler.OnTickIsr();  // woken by PostFromCallback
    CHECK(r.WaitFrames(i + 1U));
    CHECK(r.frames.size() == i + 1U);  // one frame per tick
    r.CheckFrame(i, i, 1000U + 7U * i);
  }
  CHECK(r.skipped == 0U && r.dropped == 0U);
  CHECK(r.timer.starts == 1U && r.timer.stops == 0U);

  r.sampler.Stop();
  CHECK(!r.sampler.Active());
  CHECK(WaitUntil([&] { return r.timer.stops == 1U; }));  // Stop's Post stops the timer
  const unsigned reads = r.swd.memory_reads;
  for (int i = 0; i < 3; ++i)
  {
    r.sampler.OnTickIsr();
  }
  Settle();
  r.Drain();
  CHECK(r.frames.size() == 9U);  // no frame after the stop
  CHECK(r.swd.memory_reads == reads);
  CHECK(r.timer.starts == 1U && r.timer.stops == 1U);
}

// ---------------------------------------------------------------------------------
// 2. 启动顺序：Start/OnTickIsr 早于 RunTask / Start order: Start and OnTickIsr before
//    RunTask
// ---------------------------------------------------------------------------------
// 当前代码在这个顺序下会卡住：task_mode_ 在 RunTask 第一行（sampler.cpp:95）才置 1，
// 之前 Start（sampler.cpp:61）和 OnTickIsr（sampler.hpp:137）都因 task_mode_==0 不 Post，
// RunTask 随后在 wake_.Wait(UINT32_MAX)（sampler.cpp:98）永远等下去。所以默认不编译；
// cmake -DXRDAP_TEST_KNOWN_ISSUES=ON 打开。
// The current code hangs in this order: task_mode_ is set only on RunTask's first line
// (sampler.cpp:95); before that Start (sampler.cpp:61) and OnTickIsr (sampler.hpp:137)
// skip their Post because task_mode_ is 0, and RunTask then waits forever in
// wake_.Wait(UINT32_MAX) (sampler.cpp:98). So it is compiled out by default; turn it on
// with cmake -DXRDAP_TEST_KNOWN_ISSUES=ON.
#if XRDAP_TEST_KNOWN_ISSUES
void StartBeforeRunTask()
{
  for (int variant = 0; variant < 2; ++variant)
  {
    Rig& r = NewRig();
    CHECK(r.Start() == kStatusOk);
    if (variant == 1)
    {
      r.sampler.OnTickIsr();  // the interrupt also comes before RunTask
    }
    SpawnTask(r);
    const bool sampled = r.WaitFrames(1, 2000U);
    SOFT_CHECK(sampled, variant == 0 ? "Start only, then RunTask: no frame within 2 s"
                                     : "Start + OnTickIsr, then RunTask: no frame in 2 s");
    if (sampled)
    {
      r.CheckFrame(0, 0U, 0U);
    }
  }
}
#endif

// ---------------------------------------------------------------------------------
// 3. 让路：命令在等时停车，最后一条命令结束时唤醒 / Yielding: park while a command
//    waits, wake when the last one ends
// ---------------------------------------------------------------------------------
void YieldParkAndResume()
{
  Rig& r = NewRig();
  StartTaskAndWaitReady(r);
  CHECK(r.Start() == kStatusOk);
  CHECK(r.WaitFrames(1));
  r.swd.bias = 1U;
  r.sampler.OnTickIsr();
  CHECK(r.WaitFrames(2));  // no command waiting: sampled as usual
  r.Quiesce();

  r.sampler.CommandBegin();
  CHECK(!r.sampler.yield_.Parked());
  r.swd.bias = 2U;
  r.sampler.OnTickIsr();  // tick 2: the one frame a waiting command still allows
  CHECK(r.WaitFrames(3));
  r.CheckFrame(2, 2U, 2U);
  r.Quiesce();

  r.sampler.OnTickIsr();  // tick 3: parks
  CHECK(WaitUntil([&] { return r.sampler.yield_.Parked(); }));
  r.Quiesce();
  r.sampler.OnTickIsr();  // ticks 4 and 5 no longer wake the sampler
  r.sampler.OnTickIsr();
  Settle();
  r.Drain();
  CHECK(r.frames.size() == 3U);
  CHECK(r.sampler.wake_.Value() == 0U);  // no semaphore post accumulated
  CHECK(r.sampler.yield_.Parked());
  CHECK(r.skipped == 0U);  // given-up ticks are counted at the next sample

  r.sampler.CommandEnd();  // the last command wakes the parked sampler
  CHECK(WaitUntil([&] { return !r.sampler.yield_.Parked(); }));
  Settle();
  r.Drain();
  CHECK(r.frames.size() == 3U);  // it carries on from the next tick, no catch-up frame

  r.swd.bias = 6U;
  r.sampler.OnTickIsr();  // tick 6
  CHECK(r.WaitFrames(4));
  r.CheckFrame(3, 6U, 6U);
  CHECK(r.skipped == 3U);  // ticks 3, 4, 5 given up
  CHECK(r.dropped == 0U);
}

void YieldReadOnlyCommandDoesNotPark()
{
  Rig& r = NewRig();
  StartTaskAndWaitReady(r);
  CHECK(r.Start() == kStatusOk);
  CHECK(r.WaitFrames(1));
  r.Quiesce();
  r.sampler.CommandBegin();
  r.sampler.CommandReadOnly();  // SUB_READ / status query: sampling goes on
  for (uint32_t i = 1U; i <= 4U; ++i)
  {
    r.sampler.OnTickIsr();
    CHECK(r.WaitFrames(i + 1U));
    r.Quiesce();
    CHECK(!r.sampler.yield_.Parked());
  }
  r.sampler.CommandEnd();
  Settle();
  CHECK(r.sampler.wake_.Value() == 0U);  // nothing was parked, so End posts nothing
  r.sampler.OnTickIsr();
  CHECK(r.WaitFrames(6));
  r.CheckFrame(5, 5U, 0U);
  CHECK(r.skipped == 0U && r.dropped == 0U);
}

void YieldTwoCommandsWakeOnLast()
{
  Rig& r = NewRig();
  StartTaskAndWaitReady(r);
  CHECK(r.Start() == kStatusOk);
  CHECK(r.WaitFrames(1));
  r.Quiesce();
  r.sampler.CommandBegin();
  r.sampler.CommandBegin();
  r.sampler.OnTickIsr();  // tick 1: the allowed frame
  CHECK(r.WaitFrames(2));
  r.Quiesce();
  r.sampler.OnTickIsr();  // tick 2: parks
  CHECK(WaitUntil([&] { return r.sampler.yield_.Parked(); }));
  r.Quiesce();

  r.sampler.CommandEnd();  // one command still waits: no wake
  Settle();
  CHECK(r.sampler.yield_.Parked());
  CHECK(r.sampler.wake_.Value() == 0U);
  r.sampler.OnTickIsr();  // tick 3 still does not wake
  Settle();
  r.Drain();
  CHECK(r.frames.size() == 2U);

  r.sampler.CommandEnd();  // the last one wakes
  CHECK(WaitUntil([&] { return !r.sampler.yield_.Parked(); }));
  r.sampler.OnTickIsr();  // tick 4
  CHECK(r.WaitFrames(3));
  r.CheckFrame(2, 4U, 0U);
  CHECK(r.skipped == 2U);  // ticks 2 and 3
}

void YieldStopWhileParked()
{
  Rig& r = NewRig();
  StartTaskAndWaitReady(r);
  CHECK(r.Start() == kStatusOk);
  CHECK(r.WaitFrames(1));
  r.Quiesce();
  r.sampler.CommandBegin();
  r.sampler.OnTickIsr();  // tick 1: the allowed frame
  CHECK(r.WaitFrames(2));
  r.Quiesce();
  r.sampler.OnTickIsr();  // tick 2: parks
  CHECK(WaitUntil([&] { return r.sampler.yield_.Parked(); }));
  r.Quiesce();

  r.sampler.Stop();
  CHECK(WaitUntil([&] { return r.timer.stops == 1U; }));
  r.sampler.CommandEnd();  // wakes a sampler that is already stopped; must do no harm
  Settle();
  CHECK(!r.sampler.Active());

  r.frames.clear();  // Start clears the ring
  CHECK(r.Start() == kStatusOk);
  CHECK(r.WaitFrames(1));  // the new run is not parked and samples tick 0 at once
  r.CheckFrame(0, 0U, 0U);
  CHECK(!r.sampler.yield_.Parked());
  r.Quiesce();
  r.sampler.OnTickIsr();
  CHECK(r.WaitFrames(2));
  r.CheckFrame(1, 1U, 0U);
  CHECK(r.skipped == 0U && r.dropped == 0U);
  CHECK(r.timer.starts == 2U && r.timer.stops == 1U);
}

// ---------------------------------------------------------------------------------
// 4. 跳拍记账：采样线程被推迟，节拍合并 / Skip accounting: the sampling thread is held
//    back and ticks merge
// ---------------------------------------------------------------------------------
void SkippedTicksWhenSamplerIsHeldBack()
{
  Rig& r = NewRig();
  StartTaskAndWaitReady(r);
  CHECK(r.Start() == kStatusOk);
  CHECK(r.WaitFrames(1));
  r.Quiesce();  // from here the thread waits for a post

  r.swd.bias = 50U;
  {
    // 占着链路锁：采样线程被第一次中断唤醒后卡在 Service 的取锁上，节拍继续计数。
    // Holding the link lock: the sampling thread, woken by the first interrupt, blocks
    // on Service's lock while the tick count keeps rising.
    MemAp::Lock lock(r.mem);
    for (int i = 0; i < 5; ++i)
    {
      r.sampler.OnTickIsr();  // ticks 1..5, only the first one posts
    }
    CHECK(r.sampler.wake_.Value() <= 1U);
  }
  CHECK(r.WaitFrames(2));
  r.Quiesce();
  r.CheckFrame(1, 5U, 50U);  // only the newest tick is sampled
  CHECK(r.skipped == 4U);    // ticks 1..4
  CHECK(r.dropped == 0U);

  // 一次中断跨过多个周期（elapsed_ticks > 1）和单拍混合 / One interrupt that crosses
  // several periods (elapsed_ticks > 1), mixed with single ticks.
  r.swd.bias = 90U;
  {
    MemAp::Lock lock(r.mem);
    r.sampler.OnTickIsr(3U);  // count 8
    r.sampler.OnTickIsr();    // count 9
  }
  CHECK(r.WaitFrames(3));
  r.Quiesce();
  r.CheckFrame(2, 9U, 90U);
  CHECK(r.skipped == 7U);  // + ticks 6, 7, 8
  CHECK(r.dropped == 0U);
  CHECK(r.frames.size() == 3U);

  // 每拍不再被推迟时又是一拍一帧 / Without the hold it is one frame per tick again.
  r.sampler.OnTickIsr();
  CHECK(r.WaitFrames(4));
  r.CheckFrame(3, 10U, 90U);
  CHECK(r.skipped == 7U && r.dropped == 0U);
}

// ---------------------------------------------------------------------------------
// 5. Stop 与 Start 重入 / Stop and Start re-entry
// ---------------------------------------------------------------------------------
void StopStartReentry(bool wait_for_stop)
{
  Rig& r = NewRig();
  StartTaskAndWaitReady(r);
  r.swd.bias = 500U;
  CHECK(r.Start() == kStatusOk);
  CHECK(r.WaitFrames(1));
  for (uint32_t i = 1U; i <= 3U; ++i)
  {
    r.Quiesce();
    r.sampler.OnTickIsr();
    CHECK(r.WaitFrames(i + 1U));
  }
  r.CheckFrame(3, 3U, 500U);
  // ticks 4 and 5 stay in the ring, undrained: leftovers of the old run.
  for (uint32_t i = 4U; i <= 5U; ++i)
  {
    r.Quiesce();
    r.sampler.OnTickIsr();
    CHECK(WaitUntil([&] { return r.swd.memory_reads >= Rig::ReadsAfter(i + 1U); }));
  }
  r.Quiesce();  // tick 5 is in the ring now

  r.sampler.Stop();
  if (wait_for_stop)
  {
    CHECK(WaitUntil([&] { return r.timer.stops == 1U; }));
  }
  r.frames.clear();
  r.swd.bias = 900U;
  CHECK(r.Start() == kStatusOk);  // the generation changes; the ring is cleared
  CHECK(r.WaitFrames(1));
  r.CheckFrame(0, 0U, 900U);  // the new run starts at tick 0, no leftover of the old one
  CHECK(r.skipped == 0U && r.dropped == 0U);
  for (uint32_t i = 1U; i <= 3U; ++i)
  {
    r.Quiesce();
    r.swd.bias = 900U + i;
    r.sampler.OnTickIsr();
    CHECK(r.WaitFrames(i + 1U));
    r.CheckFrame(i, i, 900U + i);
  }
  Settle();
  r.Drain();
  CHECK(r.frames.size() == 4U);  // nothing of the old run (ticks 4, 5) slipped in
  CHECK(r.skipped == 0U && r.dropped == 0U);
  // Whichever way the thread saw Stop and Start, the timer was stopped once and started
  // twice in all.
  CHECK(r.timer.starts == 2U && r.timer.stops == 1U);
}

// ---------------------------------------------------------------------------------
// 6. 任务模式和 Poll 给出同样的帧 / Task mode and Poll produce the same frames
// ---------------------------------------------------------------------------------
void TaskAndPollGiveSameFrames()
{
  // 每步：先改目标内存（bias），再来 elapsed 个节拍 / Per step: change the target memory
  // (bias), then elapsed ticks arrive.
  const uint32_t elapsed[] = {1U, 1U, 3U, 1U, 2U, 1U};
  const uint32_t ticks[] = {0U, 1U, 2U, 5U, 6U, 8U, 9U};
  const size_t steps = sizeof(elapsed) / sizeof(elapsed[0]);

  Rig& poll = NewRig();
  poll.swd.bias = 5U;
  CHECK(poll.Start() == kStatusOk);
  poll.sampler.Poll(poll.timer);
  poll.Drain();
  for (size_t s = 0; s < steps; ++s)
  {
    poll.swd.bias = static_cast<uint32_t>(5U + 11U * (s + 1U));
    poll.sampler.OnTickIsr(elapsed[s]);
    poll.sampler.Poll(poll.timer);
    poll.Drain();
  }

  Rig& task = NewRig();
  StartTaskAndWaitReady(task);
  task.swd.bias = 5U;
  CHECK(task.Start() == kStatusOk);
  CHECK(task.WaitFrames(1));
  for (size_t s = 0; s < steps; ++s)
  {
    task.Quiesce();
    task.swd.bias = static_cast<uint32_t>(5U + 11U * (s + 1U));
    task.sampler.OnTickIsr(elapsed[s]);
    CHECK(task.WaitFrames(s + 2U));
  }
  Settle();
  task.Drain();

  CHECK(poll.frames.size() == steps + 1U);
  CHECK(task.frames.size() == poll.frames.size());
  for (size_t i = 0; i < poll.frames.size(); ++i)
  {
    CHECK(poll.frames[i].tick == ticks[i]);
    CHECK(task.frames[i].tick == poll.frames[i].tick);
    CHECK(task.frames[i].body == poll.frames[i].body);
    poll.CheckFrame(i, ticks[i], static_cast<uint32_t>(5U + 11U * i));
  }
  CHECK(poll.skipped == 3U && task.skipped == poll.skipped);
  CHECK(poll.dropped == 0U && task.dropped == poll.dropped);
  CHECK(task.swd.memory_reads == poll.swd.memory_reads);
  CHECK(task.swd.csw_reads == poll.swd.csw_reads);
  CHECK(task.timer.starts == 1U && poll.timer.starts == 1U);

  // 停止后两种模式都停了定时器（任务模式经 Post，Poll 经下一圈）/ After Stop both modes
  // have stopped the timer (task mode through the Post, Poll on the next pass).
  poll.sampler.Stop();
  poll.sampler.Poll(poll.timer);
  CHECK(poll.timer.stops == 1U);
  task.sampler.Stop();
  CHECK(WaitUntil([&] { return task.timer.stops == 1U; }));
}

void Run(const char* name, void (*fn)())
{
  fn();
  std::printf("PASS %s\n", name);
  std::fflush(stdout);
}
}  // namespace

int main()
{
  LibXR::PlatformInit();
  Run("TaskBasicFlow", TaskBasicFlow);
  Run("YieldParkAndResume", YieldParkAndResume);
  Run("YieldReadOnlyCommandDoesNotPark", YieldReadOnlyCommandDoesNotPark);
  Run("YieldTwoCommandsWakeOnLast", YieldTwoCommandsWakeOnLast);
  Run("YieldStopWhileParked", YieldStopWhileParked);
  Run("SkippedTicksWhenSamplerIsHeldBack", SkippedTicksWhenSamplerIsHeldBack);
  Run("StopStartReentryAfterTimerStopped", [] { StopStartReentry(true); });
  Run("StopStartReentryBackToBack", [] { StopStartReentry(false); });
  Run("TaskAndPollGiveSameFrames", TaskAndPollGiveSameFrames);
#if XRDAP_TEST_KNOWN_ISSUES
  StartBeforeRunTask();
  std::printf("%s StartBeforeRunTask (known issue)\n", g_soft_failures ? "FAIL" : "PASS");
  std::fflush(stdout);
#endif
  if (g_soft_failures != 0)
  {
    std::fflush(stderr);
    std::_Exit(1);
  }
  std::puts("sampler task mode tests passed");
  std::fflush(stdout);
  // 采样线程不会返回，不跑静态析构直接结束 / Sampling threads never return; end without
  // running static destructors.
  std::_Exit(0);
}
