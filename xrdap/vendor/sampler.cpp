// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/sampler.hpp"

#include <cstring>

#include "timebase.hpp"

namespace XrDap::Vendor
{
namespace
{
uint64_t NowUs() { return static_cast<uint64_t>(LibXR::Timebase::GetMicroseconds()); }

// 停下时中断唤醒的计数放到很远处，实际上不会到 / While parked, the interrupt's wake count
// is put far away, where it is never reached in practice.
constexpr uint32_t kParkedTicks = 0x40000000U;

void MarkFailed(uint8_t* marks, uint16_t i)
{
  marks[i >> 3] |= static_cast<uint8_t>(1U << (i & 7U));
}
}  // namespace

uint8_t Sampler::Start(const SubItemList& items, uint32_t period_us, bool stop_on_full)
{
  {
    MemAp::Lock lock(mem_);
    count_ = 0U;
    items.ForEach([&](const SubItem& sub) { items_[count_++] = sub; });
    period_us_ = period_us;
    stop_on_full_ = stop_on_full;

    // 开始时读一次各 AP 的 CSW，采样期间不再重读 / Every AP's CSW is read once at the
    // start and not again while sampling.
    bool ok = true;
    csw_count_ = 0U;
    for (uint16_t i = 0; i < count_; ++i)
    {
      const uint8_t ap = items_[i].item.ap;
      if (FindCsw(ap) != nullptr)
      {
        continue;
      }
      ApCsw& dst = csw_[csw_count_++];
      dst.ap = ap;
      dst.valid = mem_.ReadCsw(ap, dst.base_csw);
      ok = ok && dst.valid;
    }
    if (!ok)
    {
      return kStatusNoTarget;
    }
  }

  ring_.Reset();
  gen_.fetch_add(1U, std::memory_order_acq_rel);
  active_.store(1U, std::memory_order_release);
  if (task_mode_.load(std::memory_order_relaxed) != 0U)
  {
    wake_.Post();
  }
  return kStatusOk;
}

void Sampler::Stop()
{
  active_.store(0U, std::memory_order_release);
  if (task_mode_.load(std::memory_order_relaxed) != 0U)
  {
    wake_.Post();
  }
  // 正在读的一拍持有链路；取一次锁就等到它读完，之后的拍在锁内看到已停止而不读。
  // A tick in progress holds the link; taking the lock once waits for it, and later
  // ticks see the stop under the lock and do not read.
  MemAp::Lock lock(mem_);
}

const Sampler::ApCsw* Sampler::FindCsw(uint8_t ap) const
{
  for (uint8_t i = 0; i < csw_count_; ++i)
  {
    if (csw_[i].ap == ap)
    {
      return &csw_[i];
    }
  }
  return nullptr;
}

void Sampler::RunTask(TickTimer& timer)
{
  task_mode_.store(1U, std::memory_order_relaxed);
  for (;;)
  {
    (void)wake_.Wait(UINT32_MAX);
    while (Service(timer))
    {
    }
  }
}

// 醒来一次：按需开关定时器，到了要采的节拍就采一拍，再定下一次唤醒的计数。
// One wake: start or stop the timer as needed, sample a tick when the one to sample came,
// then set the count of the next wake.
bool Sampler::Service(TickTimer& timer)
{
  MemAp::Lock lock(mem_);
  uint32_t count = ticks_.load(std::memory_order_relaxed);
  if (active_.load(std::memory_order_acquire) == 0U)
  {
    if (timer_on_)
    {
      timer.Stop();
      timer_on_ = false;
    }
    return false;
  }
  const uint32_t gen = gen_.load(std::memory_order_acquire);
  if (gen != run_gen_)
  {
    // 新的一次采样：计数清零后再开定时器，节拍 0 马上采 / A new run: the count is
    // cleared before the timer starts, and tick 0 is sampled at once.
    if (timer_on_)
    {
      timer.Stop();
    }
    ticks_.store(0U, std::memory_order_relaxed);
    frames_.store(0U);
    skipped_.store(0U);
    max_frame_us_.store(0U);
    max_wake_us_.store(0U);
    merged_callbacks_.store(0U);
    merged_ticks_.store(0U);
    const auto memory = mem_.Stats();
    swd_failures_start_.store(memory.failed_transactions);
    swd_resyncs_start_.store(memory.resyncs);
    woke_tick_.store(UINT32_MAX, std::memory_order_relaxed);
    count = 0U;
    account_.Begin();
    yield_.Restart();
    resync_pending_ = false;
    resync_after_us_ = 0U;
    run_gen_ = gen;
    timer.Start(period_us_);
    timer_on_ = true;
  }

  const WakeDecision decision = DecideWake(account_, yield_, count);
  if (decision.parked)
  {
    // 给命令让路：中断不再按计数唤醒，最后一条命令做完时 CommandEnd 唤醒 / Yielding to
    // commands: the interrupt no longer wakes on the count; CommandEnd wakes us when the
    // last command has run.
    wake_at_.store(count + kParkedTicks, std::memory_order_relaxed);
    return false;
  }
  const TickAccount::Step& step = decision.step;
  if (step.sample)
  {
    const auto t0 = static_cast<uint32_t>(NowUs());
    const uint32_t woke_tick = woke_tick_.load(std::memory_order_acquire);
    if (woke_tick != UINT32_MAX && TickDue(step.tick, woke_tick))
    {
      const uint32_t latency =
          WakeLatencyUs(t0, woke_us_.load(std::memory_order_relaxed),
                        wake_late_us_.load(std::memory_order_relaxed),
                        wake_lag_ticks_.load(std::memory_order_relaxed), period_us_);
      if (latency > max_wake_us_.load(std::memory_order_relaxed))
      {
        max_wake_us_.store(latency, std::memory_order_relaxed);
      }
    }
    SampleTick(step);
    const uint32_t frame_us = static_cast<uint32_t>(NowUs()) - t0;
    if (frame_us > max_frame_us_.load(std::memory_order_relaxed))
    {
      max_frame_us_.store(frame_us, std::memory_order_relaxed);
    }
    // 占用从节拍中断唤醒的那一刻算起，包括唤醒、切换任务和取链路锁的时间：只量读帧的
    // 时间会少算，周期接近一帧时留给命令的间隙就只是纸面上的。没有这一拍的中断时刻时
    // （开始时的节拍 0、不等唤醒接着做的一拍）从读帧开始算。单循环里中断到这里之间是
    // 主循环在做完手上的命令，不是采样的开销，所以只在任务方式下从中断算起。
    // Busy time counts from the moment the tick interrupt woke us, including the wake,
    // the task switch and taking the link lock: timing only the frame read undercounts,
    // and with a period close to one frame the gap left to commands exists only on
    // paper. Without this tick's interrupt time (tick 0 at the start, a tick done
    // without waiting for a wake) it counts from the start of the frame read. In a single
    // loop the time from the interrupt to here is the main loop finishing its current
    // command, not a sampling cost, so the interrupt time is used in task mode only.
    const uint32_t busy =
        FrameBusyUs(static_cast<uint32_t>(NowUs()), t0,
                    task_mode_.load(std::memory_order_relaxed) != 0U &&
                        woke_tick_.load(std::memory_order_acquire) == step.tick,
                    woke_us_.load(std::memory_order_relaxed), period_us_);
    account_.OnDone(ticks_.load(std::memory_order_relaxed), busy, period_us_);
    yield_.FrameEnd();

    if (!keep_(keep_ctx_))
    {
      active_.store(0U, std::memory_order_release);
      timer.Stop();
      timer_on_ = false;
      return false;
    }
  }
  // 先定下唤醒的计数再看计数：要采的节拍若在这之前已到，中断不会再唤醒，这里接着做。
  // Set the wake count first, then read the count: if the tick to sample came before
  // that, the interrupt will not wake us again, so carry on here.
  const uint32_t next = account_.Next();
  wake_at_.store(next, std::memory_order_relaxed);
  return TickDue(ticks_.load(std::memory_order_relaxed), next);
}

// 读一拍里到期的条目，组成帧放进缓冲：节拍号 u32 + 时间戳 u48 + 状态位图 + 各到期条目
// 的值。持有 MemAp::Lock 时调用。
// Read the items due in one tick and put the frame into the buffer: tick u32 + timestamp
// u48 + status bitmap + the due items' values. Called while holding MemAp::Lock.
void Sampler::SampleTick(const TickAccount::Step& step)
{
  uint8_t frame[kMaxFrameBytes];
  uint16_t order[kMaxItems];
  uint16_t due = 0U;
  uint16_t values_size = 0U;
  for (uint16_t i = 0; i < count_; ++i)
  {
    if ((step.tick + items_[i].phase) % items_[i].div == 0U)
    {
      order[due++] = i;
      values_size += items_[i].item.size;
    }
  }
  if (step.skipped != 0U)
  {
    ring_.AddSkipped(step.skipped);
    skipped_.fetch_add(step.skipped, std::memory_order_relaxed);
  }
  if (due == 0U)
  {
    return;
  }

  const uint16_t bitmap = static_cast<uint16_t>((due + 7U) / 8U);
  Put32(frame, step.tick);
  Put48(frame + 4, NowUs());
  std::memset(frame + 10, 0, bitmap + values_size);
  uint8_t* marks = frame + 10;
  uint8_t* values = marks + bitmap;
  // Preserve the failed frame. Recover before a later frame, at most once per second
  // while the target is unavailable; repeated reads can have peripheral side effects.
  const uint64_t now = NowUs();
  if (resync_pending_ && now >= resync_after_us_)
  {
    resync_after_us_ = now + 1000000U;
    if (mem_.Resync())
    {
      resync_pending_ = false;
      for (uint8_t i = 0; i < csw_count_; ++i)
      {
        csw_[i].valid = mem_.ReadCsw(csw_[i].ap, csw_[i].base_csw);
        resync_pending_ = resync_pending_ || !csw_[i].valid;
      }
    }
  }
  MemAp::ApState ap{};
  for (uint16_t i = 0; i < due;)
  {
    const Item& it = items_[order[i]].item;
    if (ap.ap != it.ap)
    {
      const ApCsw* csw = FindCsw(it.ap);
      ap.ap = it.ap;
      ap.base_csw = (csw != nullptr) ? csw->base_csw : 0U;
      ap.valid = (csw != nullptr) && csw->valid;
      ap.written = false;
    }
    uint16_t run = 1U;
    const uint32_t addr = it.base + static_cast<uint32_t>(it.final_off);
    if (it.n == 0U && it.size == 4U && (addr & 3U) == 0U && !resync_pending_)
    {
      const uint16_t window = static_cast<uint16_t>((1024U - (addr & 1023U)) / 4U);
      while (i + run < due && run < window)
      {
        const Item& next = items_[order[i + run]].item;
        if (next.ap != it.ap || next.n != 0U || next.size != 4U ||
            next.base + static_cast<uint32_t>(next.final_off) != addr + run * 4U)
        {
          break;
        }
        ++run;
      }
    }
    if (run > 1U)
    {
      const uint16_t confirmed = mem_.ReadWords(ap, addr, run, values);
      for (uint16_t j = confirmed; j < run; ++j)
      {
        MarkFailed(marks, static_cast<uint16_t>(i + j));
      }
      if (confirmed != run)
      {
        resync_pending_ = true;
        ap.written = false;
      }
      values += run * 4U;
      i += run;
      continue;
    }
    const uint8_t status = mem_.ReadItem(ap, it, values);
    if (status != kItemOk)
    {
      MarkFailed(marks, i);
      if (status >= kItemSwdChain && status <= kItemSwdLast)
      {
        resync_pending_ = true;
        ap.written = false;
      }
    }
    values += it.size;
    ++i;
  }
  ring_.Push(frame, static_cast<uint16_t>(10U + bitmap + values_size), stop_on_full_);
  frames_.fetch_add(1U, std::memory_order_relaxed);
}

}  // namespace XrDap::Vendor
