// SPDX-License-Identifier: Apache-2.0
#pragma once

// 无线探针的设置和控制口操作（docs/protocol.md 第 8.3 节）。设置由平台保存（Store），
// RadioLink 只读写这份结构。
//
// Settings and control port operations of a radio probe (docs/protocol.md section 8.3).
// The platform keeps the settings (Store); RadioLink only reads and writes this
// structure.

#include <cstdint>

#include "xrdap/radio/channel.hpp"
#include "xrdap/radio/identity.hpp"

namespace XrDap::Radio
{

/// 保存的角色设置 / Stored role setting.
enum class RoleSetting : uint8_t
{
  AUTO = 0,    ///< 没有自己的会话时当远端 / remote while it has no session of its own
  LOCAL = 1,   ///< 不用无线 / no radio
  HOST = 2,    ///< 总是经无线转发 / always forward over the radio
  REMOTE = 3,  ///< 总是执行无线收到的命令 / always execute commands from the radio
};

/// 当前角色 / Current role.
enum class Role : uint8_t
{
  LOCAL = 0,   ///< 驱动本地目标 / the probe drives its own target
  HOST = 1,    ///< 经无线转发 DAP 命令 / forward DAP commands over the radio
  REMOTE = 2,  ///< 执行无线收到的 DAP 命令 / execute DAP commands from the radio
};

/// 调制方式 / Modulation.
enum class Modulation : uint8_t
{
  GFSK = 0,  ///< 2 Mb/s，255 字节包 / 255-byte packets
  FLRC = 1,  ///< 1.3 Mb/s，127 字节包 / 127-byte packets
};

/// 配对状态 / Pairing state.
enum class PairState : uint8_t
{
  IDLE = 0,
  RUNNING = 1,
  DONE = 2,
  FAILED = 3,
};

/// 保存的设置 / Stored settings.
struct Settings
{
  RoleSetting role = RoleSetting::AUTO;
  Modulation modulation = Modulation::GFSK;
  uint8_t paired = 0;  ///< 配对后为 1；0 用默认身份 / 1 once paired; 0 uses the default
                       ///< identity
  uint8_t reserved = 0;
  uint32_t link_id = 0;             ///< 链路号，两块探针相同 / link id, the same on both
  uint8_t peer_uid[kUidSize] = {};  ///< 对方芯片 UID / the peer's chip UID
  /// 锁定的数据信道：0 自动，否则为信道索引 + 1（channel.hpp）/ Locked data channel: 0
  /// for automatic, otherwise the channel index + 1 (channel.hpp)
  uint16_t channel_lock = 0;
  uint8_t spare[10] = {};
};
static_assert(sizeof(Settings) == 32, "the stored record keeps its size");

/// @brief 平台的设置存储 / The platform's settings store.
class Store
{
 public:
  /// @brief 保存设置 / Save the settings.
  /// @return 写入成功时为 true / true when they were written.
  virtual bool Save(const Settings& settings) = 0;

 protected:
  ~Store() = default;
};

/// 控制口操作 / Control port operations.
namespace Op
{
/// `10 role` 在下次应用设置前改用这个角色（Role）/ use this role until the settings are
/// applied again (Role)
inline constexpr uint8_t kRole = 0x10;
/// `11` 无线统计，应答见 StatsAnswer / radio statistics, answered with StatsAnswer
inline constexpr uint8_t kStats = 0x11;
/// `13` 读设置，应答见 SettingsAnswer / read the settings, answered with SettingsAnswer
inline constexpr uint8_t kSettings = 0x13;
/// `14 setting` 保存角色设置（RoleSetting）/ store the role setting (RoleSetting)
inline constexpr uint8_t kSetRole = 0x14;
/// `15 modulation` 保存调制方式（Modulation）/ store the modulation (Modulation)
inline constexpr uint8_t kSetModulation = 0x15;
/// `16` 忘掉配对 / forget the pairing
inline constexpr uint8_t kUnpair = 0x16;
/// `18 start count dwell_ms` 从信道 start 起扫 count 个（1..20），每个 dwell_ms
/// （1..20）；应答 `18 00 count` 加每个信道的忙碌（0..200）和平均 RSSI（dBm，有符号）/
/// scan `count` channels (1..20) from `start` for `dwell_ms` each (1..20); the answer is
/// `18 00 count` plus the busy share (0..200) and mean RSSI (dBm, signed) of each
inline constexpr uint8_t kScan = 0x18;
/// `19` 读信道，`19 mode channel` 设：mode 0 自动，1 锁定（保存），2 立即换到（不保存）；
/// 应答见 ChannelAnswer / read the channel, or `19 mode channel` to set it: mode 0
/// automatic, 1 locked (stored), 2 change to it now (not stored); answered with
/// ChannelAnswer
inline constexpr uint8_t kChannel = 0x19;
/// `20` 开始配对 / start pairing
inline constexpr uint8_t kPair = 0x20;
/// `30 request` 把一个控制请求（不能是 0x30）经无线发给对方，应答为对方的应答 / send a
/// control request (not another 0x30) to the peer over the radio; the answer is the
/// peer's
inline constexpr uint8_t kForward = 0x30;
}  // namespace Op

/// 操作的状态码（低位；0xF0 以上属于控制口）/ Status codes of the operations (low codes;
/// 0xF0 and up belong to the control port).
namespace Status
{
inline constexpr uint8_t kOk = 0;
inline constexpr uint8_t kTimeout = 1;
inline constexpr uint8_t kError = 2;
inline constexpr uint8_t kBadRequest = 3;
inline constexpr uint8_t kNotPaired = 4;
}  // namespace Status

/// 读设置（0x13）的应答 / Answer of reading the settings (0x13).
struct __attribute__((packed)) SettingsAnswer
{
  uint8_t op;
  uint8_t status;
  uint8_t role_setting;  ///< RoleSetting
  uint8_t modulation;    ///< Modulation
  uint8_t paired;
  uint8_t safe_mode;   ///< 1：安全模式，设置不生效 / 1: safe mode, settings not applied
  uint8_t role;        ///< Role
  uint8_t pair_state;  ///< PairState
  uint8_t link_id[4];  ///< 小端 / little endian
  uint8_t peer_uid[kUidSize];
};
static_assert(sizeof(SettingsAnswer) == 24, "the answer layout is fixed");

/// 统计（0x11）的应答；计数从启动起累计，小端 / Answer of the statistics (0x11);
/// counters since start, little endian.
struct __attribute__((packed)) StatsAnswer
{
  uint8_t op;
  uint8_t status;
  uint8_t count;         ///< 后面的 u32 个数 / number of u32 that follow
  uint32_t radio_tx;     ///< 发出的包 / packets sent
  uint32_t radio_rx;     ///< 收到的好包 / good packets received
  uint32_t crc_errors;   ///< 坏包 / bad packets
  uint32_t tx_timeouts;  ///< 发送失败 / failed transmissions
  uint32_t commands;     ///< 转发的命令和控制请求 / forwarded commands and requests
  uint32_t retries;      ///< 重发 / resends
  uint32_t failures;     ///< 没等到应答的 / forwards without an answer
  uint32_t duplicates;   ///< 远端从缓存应答的 / answers the remote sent from its cache
  uint32_t last_rtt_us;  ///< 上一条的往返时间 / round trip of the last one
  uint32_t max_rtt_us;   ///< 最长往返时间 / longest round trip
  uint32_t channel;      ///< 当前信道索引 / current channel index
  uint32_t scans;        ///< 整段扫描次数 / full scans
};
static_assert(sizeof(StatsAnswer) == 3 + 12 * 4, "the answer layout is fixed");

/// 读信道（0x19）的应答 / Answer of reading the channel (0x19).
struct __attribute__((packed)) ChannelAnswer
{
  uint8_t op;
  uint8_t status;
  uint8_t channel;  ///< 射频现在的信道 / the channel the radio is on
  uint8_t control;  ///< 控制信道 F / the control channel F
  uint8_t lock;     ///< 锁定的信道，自动为 kNoChannel / locked channel, kNoChannel: auto
  uint8_t best;     ///< 扫描里最好的点，没有时为 kNoChannel / best scanned point, or
                    ///< kNoChannel
};
static_assert(sizeof(ChannelAnswer) == 6, "the answer layout is fixed");

}  // namespace XrDap::Radio
