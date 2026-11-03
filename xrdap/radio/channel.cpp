// SPDX-License-Identifier: Apache-2.0
#include "xrdap/radio/channel.hpp"

namespace XrDap::Radio
{

void RssiMeter::Add(int16_t half_dbm)
{
  ++count_;
  sum_ += half_dbm;
  busy_ += half_dbm > kBusyRssi ? 1U : 0U;
}

ChannelSample RssiMeter::Result() const
{
  if (count_ == 0U)
  {
    return {200, 0};
  }
  ChannelSample sample;
  sample.busy = static_cast<uint8_t>(busy_ * 200U / count_);
  sample.mean_dbm = static_cast<int8_t>(sum_ / static_cast<int32_t>(count_) / 2);
  return sample;
}

void ChannelTable::Record(uint8_t index, ChannelSample sample)
{
  if (!IsChannel(index))
  {
    return;
  }
  samples_[index] = sample;
  measured_[index / 8] |= static_cast<uint8_t>(1U << (index % 8));
}

bool ChannelTable::Measured(uint8_t index) const
{
  return IsChannel(index) && (measured_[index / 8] & (1U << (index % 8))) != 0U;
}

bool ChannelTable::Scanned() const
{
  for (uint8_t byte : measured_)
  {
    if (byte != 0U)
    {
      return true;
    }
  }
  return false;
}

void ChannelTable::Ban(uint8_t index, uint32_t now_us)
{
  if (!IsChannel(index))
  {
    return;
  }
  for (BanEntry& entry : bans_)
  {
    if (entry.index == index)
    {
      entry.until_us = now_us + kBanUs;
      return;
    }
  }
  bans_[next_ban_] = {index, now_us + kBanUs};
  next_ban_ = static_cast<uint8_t>((next_ban_ + 1U) % (sizeof(bans_) / sizeof(bans_[0])));
}

bool ChannelTable::Banned(uint8_t index, uint32_t now_us) const
{
  for (const BanEntry& entry : bans_)
  {
    if (entry.index == index && static_cast<int32_t>(entry.until_us - now_us) > 0)
    {
      return true;
    }
  }
  return false;
}

uint8_t ChannelTable::Best(uint32_t now_us) const
{
  uint8_t best = kNoChannel;
  for (uint8_t i = 0; i < kChannels; ++i)
  {
    if (!Measured(i) || Banned(i, now_us))
    {
      continue;
    }
    if (best == kNoChannel || samples_[i].busy < samples_[best].busy ||
        (samples_[i].busy == samples_[best].busy &&
         samples_[i].mean_dbm < samples_[best].mean_dbm))
    {
      best = i;
    }
  }
  return best;
}

bool RetryWindow::Add(bool retried)
{
  ++count_;
  retried_ = static_cast<uint16_t>(retried_ + (retried ? 1U : 0U));
  if (count_ < kWindow)
  {
    return false;
  }
  const bool bad = retried_ > kLimit;
  Reset();
  return bad;
}

}  // namespace XrDap::Radio
