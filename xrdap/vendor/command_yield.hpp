// SPDX-License-Identifier: Apache-2.0
#pragma once

// 采样给命令让路（docs/protocol.md 3.5，用户决定 B9：探针任何时候都要回应命令，跳拍
// 照常计数）。执行器从一条命令开始排队等执行轮次起，到它执行完为止，把它算作“在等的
// 命令”。有命令在等时，采样最多再读一帧（命令开始等时正在读的那一帧也算），之后到期的
// 节拍都放弃（计入跳拍），直到没有命令在等；最后一条命令做完时唤醒采样，从下一个节拍起
// 照常采。所以一条命令从进入执行器到做完，最多被采样占去一帧，与周期和任务怎么分布在
// 核上都无关。命令和采样没有重叠时什么都不变：主机定时的 SUB_READ 不会因此多出跳拍。
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
// periodic SUB_READ adds no skips.

#include <atomic>
#include <cstdint>

#include "xrdap/vendor/tick_account.hpp"

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
    if (framing_.load(std::memory_order_seq_cst) != 0U)
    {
      used_.store(1U, std::memory_order_seq_cst);
    }
  }

  /// @brief 一条命令执行完 / A command has run.
  /// @return 采样正在等命令、这是最后一条时 true，调用方唤醒采样 / True when sampling
  ///         waits for commands and this was the last one; the caller wakes sampling.
  /// @note 命令的任务里，不能在中断里 / The command's task, not from an interrupt.
  bool End()
  {
    if (waiting_.fetch_sub(1U, std::memory_order_seq_cst) != 1U)
    {
      return false;
    }
    // 最后一条做完：这一轮让过的那一帧作废，下一条命令开始等时有自己的一帧。和采样同时
    // 发生时最多多读一帧，仍在“一帧”之内 / The last one is done: the frame used up in
    // this round is cleared, so the next command to start waiting has its own frame.
    // Racing with sampling costs at most one more frame, still within the one frame.
    used_.store(0U, std::memory_order_seq_cst);
    return parked_.load(std::memory_order_seq_cst) != 0U;
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
