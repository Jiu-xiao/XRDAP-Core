// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/sampler.hpp"

#include <cstring>

#include "timebase.hpp"

namespace XrDap::Vendor
{
uint64_t SamplerNowUs()
{
  return static_cast<uint64_t>(LibXR::Timebase::GetMicroseconds());
}

namespace
{
void MarkFailed(uint8_t* marks, uint16_t i)
{
  marks[i >> 3] |= static_cast<uint8_t>(1U << (i & 7U));
}
}  // namespace

uint8_t SamplerCore::Configure(const SubItemList& items, uint32_t period_us,
                               bool stop_on_full, uint32_t session)
{
  MemAp::Access access(mem_);
  mem_.ResetCache();
  count_ = 0U;
  items.ForEach([&](const SubItem& sub) { items_[count_++] = sub; });
  period_us_ = period_us;
  stop_on_full_ = stop_on_full;
  session_ = session;

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
    const auto* state = mem_.EnsureAp(ap);
    dst.valid = state != nullptr;
    dst.base_csw = dst.valid ? state->base_csw : 0U;
    ok = ok && dst.valid;
  }
  return ok ? kStatusOk : kStatusNoTarget;
}

void SamplerCore::StopTimer(TickTimer& timer)
{
  if (timer_on_)
  {
    timer.Stop();
    timer_on_ = false;
  }
}

void SamplerCore::StartTimer(TickTimer& timer)
{
  timer.Start(period_us_);
  timer_on_ = true;
}

const SamplerCore::ApCsw* SamplerCore::FindCsw(uint8_t ap) const
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

// 每帧都跑：入口对齐到 16 字节，免得别处代码长度一变，它在取指要等待的 Flash 上（例如
// STM32F1 的 64 位预取行）落到行的后半、每帧慢几微秒。-Os 下 GCC 不管 -falign-functions。
// Runs every frame: the entry is aligned to 16 bytes so that a change in code size
// elsewhere cannot move it to the second half of a fetch line on flash with wait states
// (the 64-bit prefetch lines of the STM32F1) and cost every frame a few microseconds.
// GCC ignores -falign-functions under -Os.
[[gnu::aligned(16)]] void SamplerCore::SampleTick(const TickAccount::Step& step)
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
  Put48(frame + 4, SamplerNowUs());
  std::memset(frame + 10, 0, bitmap + values_size);
  uint8_t* marks = frame + 10;
  uint8_t* values = marks + bitmap;
  // Preserve the failed frame. Recover before a later frame, at most once per second
  // while the target is unavailable; repeated reads can have peripheral side effects.
  const uint64_t now = SamplerNowUs();
  if (resync_pending_ && now >= resync_after_us_)
  {
    resync_after_us_ = now + 1000000U;
    if (mem_.Resync())
    {
      resync_pending_ = false;
      for (uint8_t i = 0; i < csw_count_; ++i)
      {
        const auto* state = mem_.EnsureAp(csw_[i].ap);
        csw_[i].valid = state != nullptr;
        csw_[i].base_csw = csw_[i].valid ? state->base_csw : 0U;
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
}

}  // namespace XrDap::Vendor
