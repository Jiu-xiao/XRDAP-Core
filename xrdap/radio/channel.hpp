// SPDX-License-Identifier: Apache-2.0
#pragma once

// 无线信道：频点索引、噪声测量、扫描结果和选点。纯逻辑。
//
// 频点用 0.5 MHz 为单位的索引表示：hz = 2402 MHz + index × 0.5 MHz，index 0..159，即
// 2402.0–2481.5 MHz。这一段在免执照的 2400–2483.5 MHz 内，两档速率的带宽都放得下；
// 射频只经 ChannelHz() 设频率，越界的索引落到 F。
//
// F（kControlChannel，2448.5 MHz，Wi-Fi 6 与 11 信道之间）是所有探针共用的控制信道：
// 配对、建立会话和协商数据信道都在 F 上，不同的对靠 CRC 初值和链路地址分开。
// 数据信道由主机扫描后选出：每个点测一段时间，记下 RSSI 超过 -85 dBm 的时间占比（忙碌）
// 和平均 RSSI；Wi-Fi 是突发的，扫几遍、每个点留最差的一遍。选忙碌最低的点，相同时选平均
// RSSI 低的。会话中变差的点拉黑 10 分钟。
//
// Radio channels: frequency indices, noise measurement, scan results and the choice of a
// channel. Pure logic.
//
// A frequency is an index in steps of 0.5 MHz: hz = 2402 MHz + index × 0.5 MHz, index
// 0..159, i.e. 2402.0–2481.5 MHz. This lies within the licence-free 2400–2483.5 MHz and
// leaves room for the bandwidth of both rates; the radio is tuned only through
// ChannelHz(), which maps an index out of range to F.
//
// F (kControlChannel, 2448.5 MHz, between Wi-Fi channels 6 and 11) is the control channel
// all probes share: pairing, opening a session and agreeing on the data channel happen
// on F, and the CRC seed and link address keep pairs apart. The host picks
// the data channel from a scan: each point is measured for a while, recording the share
// of time the RSSI is above -85 dBm (busy) and the mean RSSI; Wi-Fi comes in bursts, so
// the scan makes several passes and keeps the worst pass of each point. The point with
// the lowest busy share wins, the lower mean breaks a tie. A point that turns bad during
// a session is banned for 10 minutes.

#include <cstdint>

namespace XrDap::Radio
{

/// 信道数 / Number of channels.
inline constexpr uint8_t kChannels = 160;
/// 控制信道 F（2448.5 MHz）/ The control channel F (2448.5 MHz).
inline constexpr uint8_t kControlChannel = 93;
/// 不是信道（控制口里表示“自动”或“没有”）/ Not a channel ("auto" or "none" on the
/// control port).
inline constexpr uint8_t kNoChannel = 0xFF;
/// 忙碌阈值 -85 dBm，单位 0.5 dBm / Busy threshold -85 dBm, in units of 0.5 dBm.
inline constexpr int16_t kBusyRssi = -170;
/// 拉黑多久 / How long a ban lasts.
inline constexpr uint32_t kBanUs = 600'000'000;
/// 拉黑一个点时，索引相差不到这么多的点也不用：2 MHz，约为高速率的带宽，离得更近的点还会
/// 受同样的干扰 / Banning a point also rules out points less than this many indices away:
/// 2 MHz, about the bandwidth of the high rate; closer points would meet the same
/// interference.
inline constexpr uint8_t kBanSpan = 4;

/// 是不是信道索引 / Whether `index` is a channel.
inline constexpr bool IsChannel(uint8_t index) { return index < kChannels; }

/// @brief 信道的频率；越界的索引按 F 算 / The frequency of a channel; an index out of
///        range counts as F.
inline constexpr uint32_t ChannelHz(uint8_t index)
{
  return 2'402'000'000U + (IsChannel(index) ? index : kControlChannel) * 500'000U;
}

/// 一个点的测量结果 / Measurement of one point.
struct ChannelSample
{
  uint8_t busy = 0;     ///< 忙碌占比，0..200 为 0..100 % / busy share, 0..200 = 0..100 %
  int8_t mean_dbm = 0;  ///< 平均 RSSI，dBm / mean RSSI, dBm
};

/// 累积一个点上的 RSSI 读数 / Accumulates the RSSI readings of one point.
class RssiMeter
{
 public:
  /// @param half_dbm 一个读数，单位 0.5 dBm / One reading, in units of 0.5 dBm.
  void Add(int16_t half_dbm);
  /// 结果；没有读数时为全忙 / The result; all busy without readings.
  ChannelSample Result() const;

 private:
  uint32_t count_ = 0;
  uint32_t busy_ = 0;
  int32_t sum_ = 0;
};

/// 扫描结果和黑名单 / Scan results and bans.
class ChannelTable
{
 public:
  /// 记下一个点的测量 / Record the measurement of a point.
  void Record(uint8_t index, ChannelSample sample);
  /// @brief 再测一遍：留较差的（忙碌和平均 RSSI 各取大）；没测过时同 Record / Add
  ///        another pass: keep the worse (the larger busy share and mean RSSI each); like
  ///        Record for a point not measured yet.
  void Merge(uint8_t index, ChannelSample sample);
  /// 忘掉测量，黑名单不变 / Forget the measurements; the bans stay.
  void ClearSamples();
  /// 这个点测过 / Whether the point was measured.
  bool Measured(uint8_t index) const;
  /// 这个点的测量 / The measurement of a point.
  ChannelSample Sample(uint8_t index) const { return samples_[index]; }
  /// 有测量结果 / Whether any point was measured.
  bool Scanned() const;
  /// 拉黑一个点 kBanUs / Ban a point for kBanUs.
  void Ban(uint8_t index, uint32_t now_us);
  /// 这个点在黑名单里；到期的条目在这里清掉 / Whether the point is banned; an entry that
  /// ran out is cleared here.
  bool Banned(uint8_t index, uint32_t now_us);
  /// @brief 测过、没拉黑的最好的点；没有时为 kNoChannel / The best measured point that is
  ///        not banned; kNoChannel when there is none.
  uint8_t Best(uint32_t now_us);

 private:
  struct BanEntry
  {
    uint8_t index = kNoChannel;
    uint32_t since_us = 0;
  };

  ChannelSample samples_[kChannels] = {};
  uint8_t measured_[kChannels / 8] = {};
  BanEntry bans_[8] = {};
  uint8_t next_ban_ = 0;
};

/// @brief 重发率窗口：每 kWindow 次往返看一次，重发多于 kLimit 次为差 / Retry window:
///        every kWindow exchanges, more than kLimit retried ones means bad.
class RetryWindow
{
 public:
  static constexpr uint16_t kWindow = 200;
  static constexpr uint16_t kLimit = 10;  ///< 5 %

  /// @brief 记一次往返 / Note one exchange.
  /// @return 窗口刚满且重发过多时为 true / true when the window just filled with too many
  ///         retries.
  bool Add(bool retried);
  /// 从头数 / Start counting again.
  void Reset()
  {
    count_ = 0;
    retried_ = 0;
  }

 private:
  uint16_t count_ = 0;
  uint16_t retried_ = 0;
};

}  // namespace XrDap::Radio
