// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/sampler.hpp"

#include <cstring>

#include "timebase.hpp"

namespace XrDap::Vendor
{
namespace
{
uint64_t NowUs() { return static_cast<uint64_t>(LibXR::Timebase::GetMicroseconds()); }

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
  active_.store(true, std::memory_order_release);
  if (task_mode_.load(std::memory_order_relaxed))
  {
    wake_.Post();
  }
  return kStatusOk;
}

void Sampler::Stop()
{
  active_.store(false, std::memory_order_release);
  if (task_mode_.load(std::memory_order_relaxed))
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
  task_mode_.store(true, std::memory_order_relaxed);
  for (;;)
  {
    (void)wake_.Wait(UINT32_MAX);
    Service(timer);
  }
}

// 醒来一次：按需开关定时器，到了新节拍就采一拍。
// One wake: start or stop the timer as needed, and sample a tick when a new one came.
void Sampler::Service(TickTimer& timer)
{
  uint32_t count = ticks_.load(std::memory_order_relaxed);
  MemAp::Lock lock(mem_);
  if (!active_.load(std::memory_order_acquire))
  {
    if (timer_on_)
    {
      timer.Stop();
      timer_on_ = false;
    }
    return;
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
    count = 0U;
    account_.Begin();
    run_gen_ = gen;
    timer.Start(period_us_);
    timer_on_ = true;
  }

  const TickAccount::Step step = account_.OnWake(count);
  if (!step.sample)
  {
    return;
  }
  const uint64_t t0 = NowUs();
  SampleTick(step);
  const auto busy = static_cast<uint32_t>(NowUs() - t0);
  account_.OnDone(ticks_.load(std::memory_order_relaxed), busy, period_us_);

  if (!keep_(keep_ctx_))
  {
    active_.store(false, std::memory_order_release);
    timer.Stop();
    timer_on_ = false;
  }
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
  MemAp::ApState ap{};
  for (uint16_t i = 0; i < due; ++i)
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
    if (mem_.ReadItem(ap, it, values) != kItemOk)
    {
      MarkFailed(marks, i);
    }
    values += it.size;
  }
  ring_.Push(frame, static_cast<uint16_t>(10U + bitmap + values_size), stop_on_full_);
}

}  // namespace XrDap::Vendor
