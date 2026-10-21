// SPDX-License-Identifier: Apache-2.0
#include "sampler.hpp"

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

uint8_t Sampler::Configure(const SubItemList& items, uint32_t period_us,
                           bool stop_on_full, Estimate& est)
{
  uint32_t max_frame = 10U + (items.count + 7U) / 8U;
  uint8_t first_ap = 0U;
  bool first = true;
  items.ForEach(
      [&](const SubItem& sub)
      {
        max_frame += sub.item.size;
        if (first)
        {
          first_ap = sub.item.ap;
          first = false;
        }
      });
  est.max_frame = static_cast<uint16_t>(max_frame);

  // 按当前 SWD 时钟实测：先标定单次访问耗时（INFO 会报告），再把全部条目试读一遍，得到
  // 所有条目都到期时一帧的耗时。
  uint32_t frame_us = 0U;
  {
    MemAp::Lock lock(mem_);
    uint32_t access_us = 0U;
    if (!mem_.Calibrate(first_ap, access_us))
    {
      return kStatusNoTarget;
    }
    access_us_ = access_us;
  }
  if (!MeasureFrame(items, frame_us))
  {
    return kStatusNoTarget;
  }

  // 乘 1.6：采样时 DAP 命令（SUB_READ）会抢占采样，每帧实际耗时约为试读的 1.5 倍，周期
  // 再短就开始跳拍。
  est.frame_us = 10U + (frame_us * 8U) / 5U;
  if (max_frame + 2U > kRingBytes || est.frame_us > period_us)
  {
    return kStatusResource;
  }

  // 测量通过才换掉配置：被拒的 SUB_SET 保留之前的配置和缓冲（协议 3.4 节）。
  count_ = 0U;
  items.ForEach([&](const SubItem& sub) { items_[count_++] = sub; });
  period_us_ = period_us;
  stop_on_full_ = stop_on_full;
  ring_.Reset();
  configured_ = true;
  return kStatusOk;
}

// 按采样时的访问序列试读两遍，返回第二遍的耗时：第一遍在预热缓存，偶尔慢很多，会让估计
// 偏大而误拒订阅。各 AP 的 CSW 先读好，不计入耗时。试读和采样访问同一批地址，副作用相同。
bool Sampler::MeasureFrame(const SubItemList& items, uint32_t& us)
{
  MemAp::Lock lock(mem_);
  mem_.ResetCache();
  bool ok = true;
  items.ForEach([&](const SubItem& sub)
                { ok = ok && mem_.EnsureAp(sub.item.ap) != nullptr; });
  for (int pass = 0; ok && pass < 2; ++pass)
  {
    MemAp::ApState ap{};
    const uint64_t t0 = NowUs();
    items.ForEach(
        [&](const SubItem& sub)
        {
          const Item& it = sub.item;
          if (ap.ap != it.ap)
          {
            ap = *mem_.EnsureAp(it.ap);
            ap.written = false;
          }
          uint8_t value[8];
          (void)mem_.ReadItem(ap, it, value);
        });
    us = static_cast<uint32_t>(NowUs() - t0);
  }
  return ok;
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

bool Sampler::Start()
{
  // 开始时读一次各 AP 的 CSW，采样期间不再重读。
  {
    MemAp::Lock lock(mem_);
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
      return false;
    }
  }

  ring_.Reset();
  start_us_ = NowUs();
  poll_tick_ = 0U;
  gen_.fetch_add(1, std::memory_order_acq_rel);
  active_.store(true, std::memory_order_release);
  wake_.Post();
  return true;
}

void Sampler::Stop()
{
  active_.store(false, std::memory_order_release);
  // 正在读的一拍持有链路；取一次锁就等到它读完，之后的拍在锁内看到已停止而不读。
  MemAp::Lock lock(mem_);
}

// 读一拍里到期的条目填进 frame（失败的在状态位图里置位）并放进缓冲。持锁后发现已停止时
// 什么都不做：停止后的 SUB_SET 可能已经换了配置、清了缓冲，这一帧不能再进去。
void Sampler::SampleTick(const uint16_t* order, uint16_t due, uint8_t* frame,
                         uint16_t len)
{
  MemAp::Lock lock(mem_);
  if (!Active())
  {
    return;
  }
  uint8_t* marks = frame + 10;
  uint8_t* values = marks + (due + 7U) / 8U;
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
  ring_.Push(frame, len, stop_on_full_);
}

// 读第 k 拍，返回下一个要读的节拍号：来不及时跳到下一个还没过去的节拍；leave_gap 时
// 离下一拍太近也再跳一拍，给别的任务留出时间。
uint64_t Sampler::Tick(uint64_t k, uint64_t start, uint32_t period, bool leave_gap)
{
  const uint64_t tick_us = NowUs();
  uint8_t frame[kMaxFrameBytes];
  uint16_t order[kMaxItems];
  uint16_t due = 0U;
  uint16_t values_size = 0U;
  for (uint16_t i = 0; i < count_; ++i)
  {
    if ((k + items_[i].phase) % items_[i].div == 0U)
    {
      order[due++] = i;
      values_size += items_[i].item.size;
    }
  }

  if (due != 0U)
  {
    // 帧：节拍序号 u32 + 时间戳 u48 + 状态位图 + 各到期条目的值。
    const uint16_t bitmap = static_cast<uint16_t>((due + 7U) / 8U);
    Put32(frame, static_cast<uint32_t>(k));
    Put48(frame + 4, tick_us);
    std::memset(frame + 10, 0, bitmap + values_size);
    SampleTick(order, due, frame, static_cast<uint16_t>(10U + bitmap + values_size));
  }

  const uint64_t done = NowUs();
  ++k;
  uint64_t next_k = k;
  if (done > start + k * period)
  {
    next_k = (done - start) / period + 1U;
  }
  const uint64_t min_gap = (period / 8U > kMinGapUs) ? period / 8U : kMinGapUs;
  if (leave_gap && start + next_k * period - done < min_gap)
  {
    ++next_k;
  }
  if (next_k != k)
  {
    ring_.AddSkipped(static_cast<uint32_t>(next_k - k));
  }

  if (!keep_(keep_ctx_))
  {
    Stop();
  }
  return next_k;
}

void Sampler::Poll()
{
  if (Active() && NowUs() >= start_us_ + poll_tick_ * period_us_)
  {
    poll_tick_ = Tick(poll_tick_, start_us_, period_us_, false);
  }
}

void Sampler::RunTask(TickTimer* timer)
{
  for (;;)
  {
    if (!Active())
    {
      (void)wake_.Wait(100U);
      continue;
    }
    Run(gen_.load(std::memory_order_acquire), timer);
  }
}

// 阻塞等节拍中断。被中断唤醒后离 deadline 还差不到 kSpinUs（定时器与时基之间的偏差）时
// 忙等补上。没被唤醒过之前一律阻塞、不忙等：采样任务优先级最高，一拍做完离下一拍很近时
// 忙等会占满这个核，别的任务连停止命令都收不到。
void Sampler::SleepUntil(uint64_t deadline_us, bool timed)
{
  // 上一拍做得慢时，中途经过的节拍边界留下了唤醒；先清掉，只认这次阻塞之后的唤醒。
  while (wake_.Wait(0U) == LibXR::ErrorCode::OK)
  {
  }
  bool woke = false;
  for (;;)
  {
    if (!Active())
    {
      return;
    }
    const uint64_t now = NowUs();
    if (now >= deadline_us)
    {
      return;
    }
    if (woke && deadline_us - now <= kSpinUs)
    {
      continue;
    }
    // 超时只是为了定期检查是否已停止；没有定时器时就是每 1 ms 醒一次
    woke = wake_.Wait(timed ? 20U : 1U) == LibXR::ErrorCode::OK;
  }
}

void Sampler::Run(uint32_t gen, TickTimer* timer)
{
  const uint64_t start = start_us_;
  const uint32_t period = period_us_;
  const bool timed = (timer != nullptr) && timer->Start(start, period);

  auto running = [&] { return Active() && gen_.load(std::memory_order_acquire) == gen; };
  uint64_t k = 0U;
  while (running())
  {
    SleepUntil(start + k * period, timed);
    if (!running())
    {
      break;
    }
    k = Tick(k, start, period, true);
  }
  if (timed)
  {
    timer->Stop();
  }
}

}  // namespace XrDap::Vendor
