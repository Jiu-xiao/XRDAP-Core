// SPDX-License-Identifier: Apache-2.0
#pragma once

// 探针端采样的零件：时间常数、节拍账目、让路规则、帧缓冲和两种运行策略（TaskSampling、
// LoopSampling）。采样器本体在 sampler.hpp。
//
// The parts of probe-side sampling: time constants, tick accounting, the yielding rules, the
// frame ring and the two running policies (TaskSampling, LoopSampling). The sampler itself
// is in sampler.hpp.

#include <cstdint>

// ===== Time constants 时间常数 =====

namespace XrDap::Vendor
{
/// @brief 一拍采样的占用时间（µs，32 位回绕差）/ Busy time of one sampling tick (µs,
///        32-bit wrapping difference).
/// @param end 这一拍做完的时刻 / Time the tick finished.
/// @param begin 开始读帧的时刻 / Time the frame read began.
/// @param woke_this_tick 这一拍是由节拍中断唤醒的 / The tick was woken by its interrupt.
/// @param isr 节拍中断唤醒的时刻 / Time of the tick interrupt's wake.
/// @param period 采样周期 / Sampling period.
/// @return end 减去起点，起点是 isr 或 begin / end minus the start, which is isr or begin.
/// @note 占用从节拍中断唤醒的那一刻算起，包括唤醒、切换任务和取链路锁的时间：只量读帧
///       的时间会少算，周期接近一帧时留给命令的间隙就只是纸面上的。没有这一拍的中断时刻
///       时（开始时的节拍 0、不等唤醒接着做的一拍）从读帧开始算。单循环里中断到这里之间
///       是主循环在做完手上的命令，不是采样的开销，所以只有任务策略（TaskSampling）用本函数，
///       单循环策略（LoopSampling）直接从读帧开始算 / Busy
///       time counts from the moment the tick interrupt woke us, including the wake, the
///       task switch and taking the link lock: timing only the frame read undercounts, and
///       with a period close to one frame the gap left to commands exists only on paper.
///       Without this tick's interrupt time (tick 0 at the start, a tick done without
///       waiting for a wake) it counts from the start of the frame read. In a single loop
///       the time from the interrupt to here is the main loop finishing its current
///       command, not a sampling cost, so only the task policy (TaskSampling) calls this
///       function; the single-loop policy (LoopSampling) counts from the start of the frame
///       read.
inline uint32_t FrameBusyUs(uint32_t end, uint32_t begin, bool woke_this_tick,
                            uint32_t isr, uint32_t period)
{
  const uint32_t start = woke_this_tick && begin - isr <= period ? isr : begin;
  return end - start;
}
}  // namespace XrDap::Vendor

// ===== Tick accounting 节拍账目 =====

// 采样节拍的账目（docs/protocol.md 3.5）。节拍定时器是唯一的时钟：它的中断每个周期把
// 节拍计数加上实际跨过的周期数。采样醒来时读计数 c，没读过的最新节拍就是
// c；从上一次采到的节拍到 c 之间 没采的节拍都算跳过。来不及时不补做。
// 每帧之后定下一个要采的节拍：它晚于采完时的计数，并且从这一帧的节拍算起至少过了
// “这一帧的占用 + 间隙”，间隙为 max(kMinGapUs, 周期/8, 占用/4)。所以两帧之间同一核上
// 的别的任务至少有这段间隙（减去唤醒的延迟），约占这个核 1/5 以上的时间，与周期无关。
// 中断只在计数到达下一个要采的节拍时唤醒采样（TickWakes），采样任务因此只为要采的节拍
// 醒来，不会为跳过的节拍一拍拍空转。
//
// Sampling tick accounting (docs/protocol.md 3.5). The tick timer is the only clock: its
// interrupt adds the number of elapsed periods to the tick count. When sampling wakes it
// reads the count c, and the newest tick not yet read is c; every tick between the last
// one sampled and c counts as skipped. Late ticks are not made up. After each frame the
// next tick to sample is chosen: it is after the count when the frame ended, and at least
// "this frame's busy time + gap" after this frame's tick, with gap = max(kMinGapUs,
// period/8, busy/4). Other tasks on the same core therefore get at least that gap between
// two frames (less the wake latency), about 1/5 of the core or more, whatever the period.
// The interrupt wakes sampling only when the count reaches the next tick to sample
// (TickWakes), so the sampling task wakes only for ticks it samples and does not spin
// through skipped ones.

#include <cstdint>

namespace XrDap::Vendor
{

/// @brief 计数 a 是否已到达或越过 b，计数回绕也成立 / Whether count a has reached or
///        passed b, also across wraparound.
inline bool TickDue(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }

/// @brief 中断把计数加到 count 后是否唤醒采样：只在到达下一个要采的节拍时 / Whether the
///        interrupt wakes sampling after raising the count to `count`: when it
///        reaches or passes the next tick to sample.
/// @note 中断上下文 / Interrupt context.
inline bool TickWakes(uint32_t count, uint32_t wake_at)
{
  return TickDue(count, wake_at);
}

/// 节拍账目 / Tick accounting.
/// @note 只在采样的执行上下文里用（采样任务或单循环的主循环）/ Used only in the
///       sampling context (the sampling task or the single main loop).
class TickAccount
{
 public:
  /// 两帧之间至少留给别的任务的时间 / Least time between two frames left to other
  /// tasks.
  static constexpr uint32_t kMinGapUs = 15U;

  /// 醒来时的决定 / Decision on a wake.
  struct Step
  {
    bool sample;       ///< 采这一拍 / Sample this tick.
    uint32_t tick;     ///< 节拍号 / Tick number.
    uint32_t skipped;  ///< 这次新跳过的节拍数 / Ticks newly skipped.
  };

  /// 一次采样开始：节拍 0 马上采 / A sampling run starts: tick 0 is sampled at once.
  void Begin();

  /// @brief 醒来，计数为 count / Woken with the tick count at `count`.
  Step OnWake(uint32_t count);

  /// @brief 刚采完 OnWake 给的节拍，定下一个要采的节拍 / The tick from OnWake has just
  ///        been sampled; choose the next tick to sample.
  /// @param count_after 采完时的计数 / Tick count when done.
  /// @param busy_us 这一拍占用的时间 / Time the tick took.
  /// @param period_us 周期 / Period.
  void OnDone(uint32_t count_after, uint32_t busy_us, uint32_t period_us);

  /// @brief 计数为 count 时是否有节拍要采（OnWake 会采）/ Whether a tick is due at
  ///        count (OnWake would sample it).
  bool Due(uint32_t count) const;

  /// @brief 放弃到 count 为止到期的节拍，下一个要采的是 count + 1；放弃的在下次采时计入
  ///        跳拍 / Give up the ticks due up to count; the next to sample is count + 1,
  ///        and the ones given up count as skipped at the next sample.
  void Defer(uint32_t count);

  /// @brief 下一个要采的节拍 / Next tick to sample.
  uint32_t Next() const { return next_; }

 private:
  bool has_last_ = false;
  uint32_t last_ = 0U;
  uint32_t next_ = 0U;
};

}  // namespace XrDap::Vendor

// ===== Yielding to commands 给命令让路 =====

// 采样给命令让路（docs/protocol.md 3.5，用户决定 B9：探针任何时候都要回应命令，跳拍
// 照常计数）。执行器从一条命令开始排队等执行轮次起，到它执行完为止，把它算作“在等的
// 命令”。有命令在等时，采样最多再读一帧（命令开始等时正在读的那一帧也算），之后到期的
// 节拍都放弃（计入跳拍），直到没有命令在等；最后一条命令做完时唤醒采样，从下一个节拍起
// 照常采。所以一条命令从进入执行器到做完，最多被采样占去一帧，与周期和任务怎么分布在
// 核上都无关。命令和采样没有重叠时什么都不变：主机定时的 SUB_READ 不会因此多出跳拍。
// 只有任务策略（TaskSampling）用它：命令和采样在不同的任务里才会抢链路。单循环策略
// （LoopSampling）里命令和采样串行，Poll 只在两条命令之间被调用，没有命令在等，所以不
// 实例化它。
//
// Sampling yields to commands (docs/protocol.md 3.5, user decision B9: the probe always
// answers commands and skips are counted as usual). The executor counts a command as
// waiting from the moment it queues for an execution turn until it has run. While a
// command waits, sampling reads at most one more frame (the one being read when the
// command started waiting counts); every tick due after that is given up (counted as
// skipped) until no command waits, and the last command to finish wakes sampling, which
// carries on from the next tick. A command therefore loses at most one frame to sampling
// between entering the executor and finishing, whatever the period and however tasks sit
// on the cores. Nothing changes when commands and sampling do not overlap: the host's
// periodic SUB_READ adds no skips. Only the task policy (TaskSampling) uses it: commands
// and sampling compete for the link only when they run in different tasks. Under the
// single-loop policy (LoopSampling) they run one after another and Poll is called only
// between two commands, when none waits, so it is not instantiated.

#include <atomic>
#include <cstdint>

#include "xrdap/vendor/sampling.hpp"

namespace XrDap::Vendor
{

/// 采样给命令让路的状态 / State of sampling yielding to commands.
class CommandYield
{
 public:
  /// @brief 一条命令开始等执行（排队前）。正在读的一帧算作它要等的那一帧 / A command
  ///        starts waiting to run (before it queues). A frame being read counts as the
  ///        one frame it waits for.
  /// @note 命令的任务里，不能在中断里 / The command's task, not from an interrupt.
  void Begin()
  {
    waiting_.fetch_add(1U, std::memory_order_seq_cst);
    read_only_turn_ = false;
    if (framing_.load(std::memory_order_seq_cst) != 0U)
    {
      used_.store(1U, std::memory_order_seq_cst);
    }
  }

  /// @brief 一条命令执行完 / A command has run.
  /// @return 采样正在等命令、这是最后一条时 true，调用方唤醒采样 / True when sampling
  ///         waits for commands and this was the last one; the caller wakes sampling.
  /// @note 命令的任务里，不能在中断里 / The command's task, not from an interrupt.
  /// @note 最后一条做完：这一轮让过的那一帧作废，下一条命令开始等时有自己的一帧。和采样
  ///       同时发生时最多多读一帧，仍在“一帧”之内 / When the last one is done, the frame
  ///       used up in this round is cleared, so the next command to start waiting has its
  ///       own frame. Racing with sampling costs at most one more frame, still within the
  ///       one frame.
  bool End()
  {
    if (read_only_turn_)
    {
      read_only_turn_ = false;
      if (parked_.load(std::memory_order_seq_cst) != 0U &&
          waiting_.load(std::memory_order_seq_cst) == 0U)
      {
        used_.store(0U, std::memory_order_seq_cst);
        return true;
      }
      return false;
    }
    if (waiting_.fetch_sub(1U, std::memory_order_seq_cst) != 1U)
    {
      return false;
    }
    // 清掉这一轮让过的那一帧 / Clear the frame used up in this round.
    used_.store(0U, std::memory_order_seq_cst);
    return parked_.load(std::memory_order_seq_cst) != 0U;
  }

  /// Mark the current command as a read-only frame fetch or status query. It may
  /// run concurrently with the producer, so it is removed from the blocking count
  /// after Begin() and End() will not decrement it a second time.
  void ReadOnly()
  {
    if (read_only_turn_)
    {
      return;
    }
    read_only_turn_ = true;
    (void)waiting_.fetch_sub(1U, std::memory_order_seq_cst);
  }

  /// @brief 停下后醒来：命令都做完了就结束停下 / A wake while parked: ends the park
  ///        once every command has run.
  /// @return 刚结束停下时 true，从下一个节拍起再采 / True when the park just ended;
  ///         sampling resumes from the next tick.
  /// @note 只在采样的执行上下文里 / Sampling context only.
  bool Resume()
  {
    if (parked_.load(std::memory_order_seq_cst) == 0U ||
        waiting_.load(std::memory_order_seq_cst) != 0U)
    {
      return false;
    }
    parked_.store(0U, std::memory_order_seq_cst);
    used_.store(0U, std::memory_order_seq_cst);
    return true;
  }

  /// 是否停下等命令 / Whether sampling is parked waiting for commands.
  bool Parked() const { return parked_.load(std::memory_order_seq_cst) != 0U; }

  /// @brief 要开始读一帧：先标记，再用 MaySample 问能不能读 / About to read a frame:
  ///        mark it first, then ask MaySample whether it may be read.
  /// @note 只在采样的执行上下文里 / Sampling context only.
  void FrameStart() { framing_.store(1U, std::memory_order_seq_cst); }

  /// @brief 一帧读完或不读了 / A frame was read or will not be read.
  /// @note 只在采样的执行上下文里 / Sampling context only.
  void FrameEnd() { framing_.store(0U, std::memory_order_seq_cst); }

  /// @brief 到期的节拍能不能采：没有命令在等时能；有命令在等时只能再采一帧 / Whether a
  ///        due tick may be sampled: yes with no command waiting; with commands waiting
  ///        only one more frame.
  /// @return false 时采样停下等 End 唤醒 / On false, sampling parks until End wakes it.
  /// @note 只在采样的执行上下文里，FrameStart 之后 / Sampling context only, after
  ///       FrameStart.
  bool MaySample()
  {
    if (waiting_.load(std::memory_order_seq_cst) == 0U)
    {
      used_.store(0U, std::memory_order_seq_cst);
      return true;
    }
    if (used_.exchange(1U, std::memory_order_seq_cst) == 0U)
    {
      return true;
    }
    parked_.store(1U, std::memory_order_seq_cst);
    // 先标记停下再看一次：最后一条命令若在这之前做完，它没看到标记，不会唤醒采样。
    // Mark parked, then look again: a last command that finished before that did not
    // see the mark and will not wake sampling.
    if (waiting_.load(std::memory_order_seq_cst) == 0U)
    {
      parked_.store(0U, std::memory_order_seq_cst);
      used_.store(0U, std::memory_order_seq_cst);
      return true;
    }
    return false;
  }

  /// @brief 一次新的采样开始：不再停下 / A new sampling run starts: no longer parked.
  /// @note 只在采样的执行上下文里 / Sampling context only.
  void Restart()
  {
    parked_.store(0U, std::memory_order_seq_cst);
    used_.store(0U, std::memory_order_seq_cst);
    framing_.store(0U, std::memory_order_seq_cst);
  }

 private:
  // 标志用 32 位：QingKe V4（CH32V307）只有字宽的原子指令，单字节的 exchange 会变成
  // 链接不到的 libatomic 调用 / Flags are 32-bit: QingKe V4 (CH32V307) has word-sized
  // atomic instructions only, and a one-byte exchange becomes a libatomic call that does
  // not link.
  std::atomic<uint32_t> waiting_{0U};
  std::atomic<uint32_t> parked_{0U};
  /// 有命令在等以来读过或正在读一帧 / A frame was read or is being read since a command
  /// started waiting.
  std::atomic<uint32_t> used_{0U};
  /// 正在读一帧 / A frame is being read.
  std::atomic<uint32_t> framing_{0U};
  static inline thread_local bool read_only_turn_ = false;
};

/// 一次醒来的决定 / Decision on one wake.
struct WakeDecision
{
  /// step.sample 时采这一拍，读完调 FrameEnd / Sample this tick when step.sample, then
  /// call FrameEnd.
  TickAccount::Step step;
  /// 停下等命令，不按计数唤醒 / Parked for commands, no wake on the count.
  bool parked;
};

/// @brief 醒来时按节拍账目和命令让路决定做什么；Sampler 和测试共用 / Decide what to do
///        on a wake from the tick account and the yield to commands; shared by Sampler
///        and the tests.
/// @note 只在采样的执行上下文里；step.sample 时读完这一帧要调 yield.FrameEnd() /
///       Sampling context only; when step.sample, call yield.FrameEnd() after reading.
inline WakeDecision DecideWake(TickAccount& account, CommandYield& yield, uint32_t count)
{
  if (yield.Resume())
  {
    account.Defer(count);
    return {{false, count, 0U}, false};
  }
  if (yield.Parked())
  {
    return {{false, count, 0U}, true};
  }
  if (!account.Due(count))
  {
    return {account.OnWake(count), false};
  }
  yield.FrameStart();
  if (!yield.MaySample())
  {
    yield.FrameEnd();
    account.Defer(count);
    return {{false, count, 0U}, true};
  }
  return {account.OnWake(count), false};
}

}  // namespace XrDap::Vendor

// ===== Frame ring 帧缓冲 =====

// 采样帧的环形缓冲。每帧前面有 2 字节小端长度，只按整帧取出。采样写、SUB_READ 读，两边
// 可以在不同的任务、不同的核上，经平台的自旋锁（xrdap_port.hpp 的 Port::SpinLock）互斥。
// 不用会阻塞的互斥量：SUB_READ 拷贝时采样任务来写，会让优先级最高的采样任务睡下、经优先级
// 继承来回切换，短周期下节拍因此迟到。
//
// Ring buffer of sample frames. Each frame is preceded by a 2-byte little-endian length
// and is only taken out whole. Sampling writes and SUB_READ reads, possibly in different
// tasks or on different cores, under the platform's spinlock (Port::SpinLock from
// xrdap_port.hpp). A blocking mutex is not used: a tick that arrives while SUB_READ
// copies would put the highest-priority sampling task to sleep and switch back and forth
// through priority inheritance, making ticks late at short periods.

#include <cstdint>

#include "xrdap/vendor/protocol.hpp"
#include "xrdap_port.hpp"

namespace XrDap::Vendor
{

/// 采样帧环形缓冲 / Ring buffer of sample frames.
/// @note Push、AddSkipped 在采样的执行上下文里调用，Take、Reset 在 DAP 执行上下文里调用，
///       两者可以在不同的核上；都不能在中断里调用 / Push and AddSkipped are called in the
///       sampling context, Take and Reset in the DAP execution context, possibly on
///       different cores; none of them from an interrupt.
class FrameRing
{
 public:
  /// 一次取帧的结果 / Result of one take.
  struct Taken
  {
    /// 拷出的字节数 / Bytes copied out.
    uint32_t bytes = 0;
    /// 拷出的帧数 / Frames copied out.
    uint16_t frames = 0;
    /// 缓冲里剩下的帧数据字节数（不含长度头）/ Frame bytes left (length headers
    /// excluded).
    uint32_t remaining = 0;
    /// 累计丢弃的帧数 / Frames dropped so far.
    uint32_t dropped = 0;
    /// 累计跳过的节拍数 / Ticks skipped so far.
    uint32_t skipped = 0;
  };

  /// 清空缓冲和计数 / Clear the buffer and the counters.
  void Reset();
  /// @brief 放入一帧 / Put one frame in.
  /// @param stop_on_full 缓冲满时丢新帧；否则丢最旧的帧 / When full, drop the new frame;
  ///        otherwise drop the oldest frames.
  void Push(const uint8_t* frame, uint16_t len, bool stop_on_full);
  /// 取出不超过 limit 字节的整帧，帧之间不留长度头 / Take whole frames, at most limit
  /// bytes, without the length headers.
  Taken Take(uint8_t* dst, uint32_t limit);
  /// 记下跳过的节拍 / Count skipped ticks.
  void AddSkipped(uint32_t n);

 private:
  uint16_t FrameLen(uint32_t pos) const;
  void CopyIn(uint32_t at, const uint8_t* src, uint32_t len);
  void CopyOut(uint32_t at, uint8_t* dst, uint32_t len) const;

  Port::SpinLock lock_;
  uint8_t buf_[kRingBytes] = {};
  uint32_t head_ = 0U;
  uint32_t tail_ = 0U;
  uint32_t used_ = 0U;
  uint32_t payload_ = 0U;
  uint32_t dropped_ = 0U;
  uint32_t skipped_ = 0U;
};

}  // namespace XrDap::Vendor

// ===== Running policies 运行策略 =====

// 采样的两个运行策略，平台在 xrdap_port.hpp 里二选一（Port::SamplerPolicy），编译期决定，
// 运行时没有分支。
//   TaskSampling：有 RTOS 的探针。采样任务运行 RunSamplerTask()，在信号量上无超时地阻塞；
//     节拍中断、Start、Stop 和最后一条命令做完都经这个信号量唤醒它。信号量在策略构造时就
//     存在，所以这些唤醒不依赖采样任务是否已经进入 RunSamplerTask：采样任务晚启动时，
//     先到的唤醒留在信号量里，任务一进来就收到。采样给命令让路（CommandYield）只在这个
//     策略里存在：命令和采样在不同的任务里，才会抢链路。
//   LoopSampling：没有 RTOS 的探针。主循环在两条 DAP 命令之间调用 Poll()，命令和采样在
//     同一个线程里串行，没有信号量，也没有让路。除节拍计数（中断加、主循环读）外都是普通
//     成员，节拍中断里不读时基。
// 两个策略有同一组成员函数，由 SamplerT 调用（接口见 sampler.hpp）；它们是互相独立的
// 类型，没有公共基类，用错入口（单循环调 RunSamplerTask、任务调 Poll）是编译错误。
//
// The two ways to run sampling, chosen by the platform in xrdap_port.hpp
// (Port::SamplerPolicy) at compile time, with no run-time branch.
//   TaskSampling: a probe with an RTOS. The sampling task runs RunSamplerTask() and blocks on
//     a semaphore with no timeout; the tick interrupt, Start, Stop and the last command to
//     finish all wake it through that semaphore. The semaphore exists from the policy's
//     construction, so these wakes do not depend on the sampling task having entered
//     RunSamplerTask: when the task starts late, a wake that came first stays in the
//     semaphore and the task receives it on entry. Sampling yielding to commands
//     (CommandYield) exists in this policy only: commands and sampling run in different
//     tasks, so they can compete for the link.
//   LoopSampling: a probe without an RTOS. The main loop calls Poll() between two DAP
//     commands; commands and sampling run one after another in the same thread, so there
//     is no semaphore and no yielding. Everything but the tick count (the interrupt adds,
//     the main loop reads) is a plain member, and the tick interrupt does not read the time
//     base.
// Both policies have the same member functions, called by SamplerT (the interface is in
// sampler.hpp). They are independent types with no common base, and calling the wrong
// entry (RunSamplerTask in a single loop, Poll in a task) is a compile error.

#include <atomic>
#include <cstdint>

#include "semaphore.hpp"
#include "timebase.hpp"
#include "xrdap/vendor/sampling.hpp"
#include "xrdap/vendor/sampling.hpp"
#include "xrdap/vendor/sampling.hpp"

namespace XrDap::Vendor
{

/// 任务策略：采样在自己的任务里，信号量唤醒 / Task policy: sampling runs in its own task,
/// woken through a semaphore.
class TaskSampling
{
 public:
  /// 有 RunTask 入口、有让路 / Has the RunTask entry and the yield to commands.
  static constexpr bool kRunsInTask = true;

  /// @brief 是否正在采样 / Whether sampling is running.
  /// @note 任何上下文 / Any context.
  bool Active() const { return active_.load(std::memory_order_acquire) != 0U; }

  /// @brief 开始一次新的运行，并唤醒采样任务 / Begin a new run and wake the sampling task.
  /// @note DAP 执行上下文；采样任务是否已经运行都可以 / DAP execution context; the sampling
  ///       task may or may not be running yet.
  void RequestStart()
  {
    gen_.fetch_add(1U, std::memory_order_acq_rel);
    active_.store(1U, std::memory_order_release);
    wake_.Post();
  }

  /// @brief 请求停止，并唤醒采样任务去停定时器 / Request a stop and wake the sampling task
  ///        to stop the timer.
  /// @note DAP 执行上下文 / DAP execution context.
  void RequestStop()
  {
    active_.store(0U, std::memory_order_release);
    wake_.Post();
  }

  /// @brief 会话结束时采样自己停下（不再唤醒）/ Sampling stops itself when the session
  ///        ends (no wake needed).
  /// @note 只在采样任务里 / Sampling task only.
  void Deactivate() { active_.store(0U, std::memory_order_release); }

  /// @brief 当前这次运行的代号，Start 一次加一 / Generation of the current run, one more
  ///        per Start.
  /// @note 只在采样任务里 / Sampling task only.
  uint32_t Generation() const { return gen_.load(std::memory_order_acquire); }

  /// 节拍计数 / The tick count.
  /// @note 只在采样任务里 / Sampling task only.
  uint32_t Ticks() const { return ticks_.load(std::memory_order_relaxed); }

  /// @brief 节拍中断：计数加 elapsed_ticks，到达唤醒计数时记下这一刻并唤醒采样任务 /
  ///        Tick interrupt: add elapsed_ticks to the count; when it reaches the wake count,
  ///        note the time and wake the sampling task.
  /// @note 中断上下文，elapsed_ticks 不为 0；时基须能在中断里读 / Interrupt context,
  ///       elapsed_ticks is not 0; the time base must be readable from an interrupt.
  /// @note 期限被消耗一次后，采样任务重新装填前不会再唤醒：一帧很长时，反复的中断不能攒下
  ///       信号量，饿死命令处理 / Once the deadline is consumed there is no further wake
  ///       until the sampling task rearms it: while a long frame runs, repeated interrupts
  ///       must not accumulate semaphore posts and starve command handling.
  /// @note 加计数和读唤醒计数用 seq_cst，与 Arm 里装填唤醒计数和读计数配对：两边各写一个量
  ///       再读对方的，seq_cst 的全序保证至少一边看得到对方，不会既没唤醒、又没发现计数已
  ///       到（relaxed 序不保证；这一处原来是 relaxed）/ Raising the count and reading the
  ///       wake count are seq_cst, paired with rearming the wake count and reading the count
  ///       in Arm: each side writes one variable and then reads the other's, and the total
  ///       order of seq_cst guarantees at least one side sees the other, so a due tick can no
  ///       longer be both unwoken and unnoticed (relaxed order does not guarantee this, and
  ///       these four accesses were relaxed).
  void OnTickIsr(uint32_t elapsed_ticks)
  {
    const uint32_t count =
        ticks_.fetch_add(elapsed_ticks, std::memory_order_seq_cst) + elapsed_ticks;
    uint32_t wake_at = wake_at_.load(std::memory_order_seq_cst);
    if (TickWakes(count, wake_at) &&
        wake_at_.compare_exchange_strong(wake_at, count + kParkedTicks,
                                         std::memory_order_relaxed))
    {
      woke_us_.store(static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds()),
                     std::memory_order_relaxed);
      woke_tick_.store(count, std::memory_order_release);
      wake_.PostFromCallback(true);
    }
  }

  /// @brief 新的一次运行开始：计数清零，不记中断唤醒，不再停下 / A new run starts: clear
  ///        the count, forget any interrupt wake, no longer parked.
  /// @note 只在采样任务里，节拍定时器已停 / Sampling task only, tick timer stopped.
  void BeginRun()
  {
    ticks_.store(0U, std::memory_order_relaxed);
    woke_tick_.store(UINT32_MAX, std::memory_order_relaxed);
    yield_.Restart();
  }

  /// @brief 定下中断下一次唤醒的计数，并告诉调用方要采的节拍是否已经到了 / Set the count
  ///        of the interrupt's next wake, and tell the caller whether the tick to sample
  ///        has already come.
  /// @return true 时要采的节拍已到，中断不会再唤醒，调用方接着做 / True when it has come:
  ///         the interrupt will not wake again, so the caller carries on.
  /// @note 只在采样任务里。先装填、再读计数，都是 seq_cst，与 OnTickIsr 配对 / Sampling
  ///       task only. Rearm, then read the count, both seq_cst, paired with OnTickIsr.
  bool Arm(uint32_t next)
  {
    wake_at_.store(next, std::memory_order_seq_cst);
    return TickDue(ticks_.load(std::memory_order_seq_cst), next);
  }

  /// @brief 停下等命令：中断不再按计数唤醒，由 CommandEnd 唤醒 / Parked for commands: the
  ///        interrupt no longer wakes on the count; CommandEnd wakes the task.
  /// @note 只在采样任务里 / Sampling task only.
  void Park(uint32_t count) { wake_at_.store(count + kParkedTicks, std::memory_order_relaxed); }

  /// @brief 醒来时的决定：节拍账目加命令让路 / Decision on a wake: tick accounting plus the
  ///        yield to commands.
  /// @note 只在采样任务里 / Sampling task only.
  WakeDecision Decide(TickAccount& account, uint32_t count)
  {
    return DecideWake(account, yield_, count);
  }

  /// @brief 一拍的占用时间：有这一拍的中断唤醒时刻时从它算起 / Busy time of one tick:
  ///        counted from the interrupt's wake when this tick has one.
  /// @note 只在采样任务里；规则见 FrameBusyUs / Sampling task only; see FrameBusyUs.
  uint32_t BusyUs(uint32_t end, uint32_t begin, uint32_t tick, uint32_t period) const
  {
    return FrameBusyUs(end, begin, woke_tick_.load(std::memory_order_acquire) == tick,
                       woke_us_.load(std::memory_order_relaxed), period);
  }

  /// 一帧读完或不读了 / A frame was read or will not be read.
  void FrameEnd() { yield_.FrameEnd(); }

  /// 一条命令开始等执行 / A command starts waiting to run.
  void CommandBegin() { yield_.Begin(); }

  /// 这条命令只读，不占链路 / This command is read-only and leaves the link alone.
  void CommandReadOnly() { yield_.ReadOnly(); }

  /// 一条命令执行完；最后一条做完且采样在停车时唤醒采样 / A command has run; the last
  /// one wakes sampling if it is parked.
  void CommandEnd()
  {
    if (yield_.End())
    {
      wake_.Post();
    }
  }

  /// @brief 阻塞到有唤醒 / Block until there is a wake.
  /// @note 只在采样任务里 / Sampling task only.
  void WaitForWork() { (void)wake_.Wait(UINT32_MAX); }

 private:
  /// 停下时中断唤醒的计数放到很远处，实际上不会到 / While parked or consumed, the wake
  /// count is put far away, where it is never reached in practice.
  static constexpr uint32_t kParkedTicks = 0x40000000U;

  std::atomic<uint32_t> active_{0U};  ///< 在运行吗：DAP 执行与采样任务写，任何上下文读 / Running: written by DAP execution and the sampling task, read anywhere.
  std::atomic<uint32_t> gen_{0U};  ///< 运行代号：DAP 执行写（Start），采样任务读 / Run generation: written by DAP execution (Start), read by the sampling task.
  std::atomic<uint32_t> ticks_{0U};  ///< 节拍计数：中断加，采样任务读和清零 / Tick count: the interrupt adds, the sampling task reads and clears.
  std::atomic<uint32_t> wake_at_{0U};  ///< 中断唤醒的计数：采样任务装填，中断消耗 / Interrupt wake count: armed by the sampling task, consumed by the interrupt.
  std::atomic<uint32_t> woke_tick_{0U};  ///< 中断唤醒时的节拍：中断写，采样任务读 / Tick at the interrupt's wake: written by the interrupt, read by the sampling task.
  std::atomic<uint32_t> woke_us_{0U};  ///< 中断唤醒时的 µs 低 32 位：中断写，采样任务读 / Low 32 bits of µs at the interrupt's wake: written by the interrupt, read by the sampling task.
  LibXR::Semaphore wake_{0};  ///< 采样任务的唤醒：DAP 执行、命令任务、中断投，采样任务等 / Sampling task wake: posted by DAP execution, command tasks and the interrupt, awaited by the sampling task.
  CommandYield yield_;  ///< 命令让路：命令任务与采样任务各写一部分（成员都是原子量）/ Yield to commands: command tasks and the sampling task each write part of it (all atomics).
};

/// 单循环策略：采样在主循环里，没有信号量 / Single-loop policy: sampling runs in the main
/// loop, with no semaphore.
class LoopSampling
{
 public:
  /// 有 Poll 入口、没有让路 / Has the Poll entry and no yield to commands.
  static constexpr bool kRunsInTask = false;

  /// @brief 是否正在采样 / Whether sampling is running.
  /// @note 只在主循环里 / Main loop only.
  bool Active() const { return active_; }

  /// 开始一次新的运行；主循环下一圈的 Poll 发现 / Begin a new run; the next Poll finds it.
  void RequestStart()
  {
    ++gen_;
    active_ = true;
  }

  /// 请求停止；主循环下一圈的 Poll 停定时器 / Request a stop; the next Poll stops the timer.
  void RequestStop() { active_ = false; }

  /// 会话结束时采样自己停下 / Sampling stops itself when the session ends.
  void Deactivate() { active_ = false; }

  /// 当前这次运行的代号 / Generation of the current run.
  uint32_t Generation() const { return gen_; }

  /// 节拍计数 / The tick count.
  uint32_t Ticks() const { return ticks_.load(std::memory_order_relaxed); }

  /// @brief 节拍中断：计数加 elapsed_ticks，不读时基，不唤醒 / Tick interrupt: add
  ///        elapsed_ticks to the count; no time base read, no wake.
  /// @note 中断上下文，elapsed_ticks 不为 0 / Interrupt context, elapsed_ticks is not 0.
  void OnTickIsr(uint32_t elapsed_ticks)
  {
    ticks_.fetch_add(elapsed_ticks, std::memory_order_relaxed);
  }

  /// @brief 新的一次运行开始：计数清零 / A new run starts: clear the count.
  /// @note 节拍定时器已停 / Tick timer stopped.
  void BeginRun() { ticks_.store(0U, std::memory_order_relaxed); }

  /// @brief 要采的节拍是否已经到了（Poll 下一圈本来就会再看，这里让 Service 的返回值一致）
  ///        / Whether the tick to sample has already come (the next Poll looks again anyway;
  ///        this keeps Service's return value the same in both policies).
  bool Arm(uint32_t next) { return TickDue(Ticks(), next); }

  /// 单循环不停车 / A single loop never parks.
  void Park(uint32_t) {}

  /// @brief 醒来时的决定：只有节拍账目，没有让路 / Decision on a wake: tick accounting
  ///        only, no yielding.
  WakeDecision Decide(TickAccount& account, uint32_t count)
  {
    return {account.OnWake(count), false};
  }

  /// @brief 一拍的占用时间，从读帧开始算：中断到这里是主循环在做命令，不是采样的开销 /
  ///        Busy time of one tick, counted from the start of the frame read: the time from
  ///        the interrupt to here is the main loop running a command, not a sampling cost.
  uint32_t BusyUs(uint32_t end, uint32_t begin, uint32_t, uint32_t) const
  {
    return end - begin;
  }

  /// 没有让路，什么都不做 / No yielding, nothing to do.
  void FrameEnd() {}
  void CommandBegin() {}
  void CommandReadOnly() {}
  void CommandEnd() {}

 private:
  bool active_ = false;  ///< 在运行吗：主循环读写 / Running: read and written by the main loop.
  uint32_t gen_ = 0U;  ///< 运行代号：主循环读写 / Run generation: read and written by the main loop.
  std::atomic<uint32_t> ticks_{0U};  ///< 节拍计数：中断加，主循环读和清零（定时器已停）/ Tick count: the interrupt adds, the main loop reads and clears it (timer stopped).
};

}  // namespace XrDap::Vendor
