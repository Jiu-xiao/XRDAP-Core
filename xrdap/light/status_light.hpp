// SPDX-License-Identifier: Apache-2.0
#pragma once

// XR-DAP 全系列共用的状态灯规则：由连接、设置和 DAP 活动算出每一刻的颜色和亮度。不碰
// 硬件，驱动和引脚留在各平台：平台按自己的刷新节奏调用 Update()，再把 Frame 画到灯上。
//   上电          彩虹扫过一次（0.8 秒）
//   主色          红：有线（USB、串口）；蓝：无线（Wi-Fi、射频）；紫：都没连上
//   正在连接      紫色呼吸
//   改设置        按住按键等正在改时紫色快闪；改好、即将重启时紫色常亮
//   调试器已连接  主色与紫色来回，2 秒一次（DAP_HostStatus 的“已连接”）
//   目标运行中    红蓝来回，1 秒一次（DAP_HostStatus 的“运行中”）
//   每条 DAP 命令  一次活动脉冲，命令不断时约 15 次每秒
// 上位机状态由 DAP_Disconnect 或 5 秒没有命令清除（见 DapActivity）。
//
// Status light rule shared by the whole XR-DAP family: computes the colour and level at
// each moment from the link, the settings and DAP activity. It does not touch hardware;
// drivers and pins stay in each platform, which calls Update() at its own refresh rate
// and draws the Frame on its LEDs.
//   power-up             one rainbow sweep (0.8 s)
//   main colour          red: wired (USB, serial); blue: wireless (Wi-Fi, radio);
//                        purple: neither
//   joining              purple breathing
//   changing a setting   fast purple blink while it is being changed (e.g. a button
//                        held); solid purple once applied, before a restart
//   debugger connected   main colour and purple back and forth every 2 s
//                        (DAP_HostStatus "connected")
//   target running       red and blue back and forth every second (DAP_HostStatus
//                        "running")
//   every DAP command    one activity pulse, about 15 per second under steady traffic
// The host state is cleared by DAP_Disconnect or 5 s without commands (see DapActivity).

#include <cstdint>

#include "xrdap/activity/dap_activity.hpp"

namespace XrDap::Light
{

/// 8 位 RGB / 8-bit RGB.
struct Rgb
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

/// 有线 / Wired.
inline constexpr Rgb kRed{255, 0, 0};
/// 无线 / Wireless.
inline constexpr Rgb kBlue{0, 0, 255};
/// 没有连接或改设置 / No link, or a setting being changed.
inline constexpr Rgb kPurple{160, 0, 255};

/// 主连接，由平台判断 / Main link, decided by the platform.
enum class Link : uint8_t
{
  NONE,      ///< 都没连上 / Nothing connected.
  JOINING,   ///< 正在连接（例如 Wi-Fi 正在入网）/ Joining (e.g. Wi-Fi associating).
  WIRED,     ///< 有线：USB 已配置或最近有串口命令 / Wired: USB configured or recent
             ///< serial commands.
  WIRELESS,  ///< 无线：网络客户端、Wi-Fi 或射频会话 / Wireless: a network client, Wi-Fi
             ///< or a radio session.
};

/// 设置状态 / Setting state.
enum class Setting : uint8_t
{
  NONE,      ///< 没在改 / Not changing.
  CHANGING,  ///< 正在改（按住按键、正在配对）/ Being changed (button held, pairing).
  APPLIED,   ///< 已改好，即将重启 / Applied, restart pending.
};

/// 某一刻要显示的内容 / What to show at one moment.
struct Frame
{
  /// 颜色 / Colour.
  Rgb color;
  /// @brief 亮度，千分比，1000 为该灯的峰值亮度，0 为熄灭；已含活动脉冲 / Level in
  ///        permille of the LED's peak, 0 is off; includes the activity pulse.
  uint16_t level;
  /// @brief 活动脉冲，千分比，命令到达时为 1000，随后线性降到 0；只能亮灭的灯在脉冲大于
  ///        500 时熄灭 / Activity pulse in permille: 1000 when a command arrives, then
  ///        falling linearly to 0. An on/off-only LED goes dark while it is above 500.
  uint16_t pulse;
};

/// 状态灯规则 / Status light rule.
class StatusLight
{
 public:
  /// @param now_ms 上电彩虹从这一刻开始 / The power-up rainbow starts at this moment.
  explicit StatusLight(uint32_t now_ms);

  /// @brief 算出此刻的颜色和亮度 / Compute the colour and level for now.
  /// @param link 主连接 / Main link.
  /// @param setting 设置状态 / Setting state.
  /// @param dap DapActivity::Read(now_ms) 的结果 / Result of DapActivity::Read(now_ms).
  Frame Update(Link link, Setting setting, const DapActivity::State& dap,
               uint32_t now_ms);

 private:
  /// 上电彩虹的时长 / Duration of the power-up rainbow.
  static constexpr uint32_t kBootMs = 800U;
  /// 平时的亮度，千分比；其余留给活动脉冲 / Resting level in permille; the rest is left
  /// to the activity pulse.
  static constexpr uint32_t kRestLevel = 200U;
  /// 一次活动脉冲的时长，也是两次脉冲的最短间隔 / Length of one activity pulse, and the
  /// shortest interval between two pulses.
  static constexpr uint32_t kPulseMs = 66U;

  uint32_t start_ms_;
  uint32_t pulse_ms_;
  uint32_t seen_commands_ = 0U;
  bool pulse_pending_ = false;
};

}  // namespace XrDap::Light
