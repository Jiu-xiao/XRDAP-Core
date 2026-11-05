// SPDX-License-Identifier: Apache-2.0
#pragma once

// 两块配对探针之间经无线的 DAP 会话。主机探针（接在电脑的 USB 上）把电脑的 DAP 命令经
// 无线转发出去，再把应答交回 USB；远端探针（接目标）执行它们。包格式见 packet.hpp，会话
// 规则见 session.hpp，设置和控制口操作见 settings.hpp。
//
// 角色：AUTO 设置（默认）下，配对的探针没有自己的调试会话时当远端。USB 上的 DAP_Connect
// 开始一个会话：探针用新会话号 PING 对方，对方应答时会话走无线，直到下一个 DAP_Connect 或
// DAP_Disconnect；否则探针驱动自己的目标。会话不会自己在无线和有线之间切换，对方掉线时
// 命令很快失败，不会落到另一个目标上。LOCAL、HOST、REMOTE 设置固定角色。未配对的探针完全
// 不用无线：无论角色设置，它都是有线探针，也不执行无线收到的命令。
//
// 会话走无线时，主机在 1 s 没有命令后发心跳 PING，DAP_Disconnect 或电脑 5 s 没有命令时
// 结束会话（会话号为 0 的 PING）；远端收到这个 PING、新会话号或 3 s 没有包时结束会话，
// 并停止会话开始的采样。
//
// 配对：RequestPairing()（例如按住按键 3 s）让探针配对至多 10 s，见 identity.hpp。配对
// 结束后设置重新生效，配对期间 USB 命令在本地执行。
//
// 信道（channel.hpp）：配对和建立会话都在控制信道 F 上。主机在 F 上找到对方后选一个数据
// 信道（锁定的、或扫描里最好的；第一次建立时先扫描），在 F 上发 CHANNEL，收到
// CHANNEL_ACK 后双方一起换过去，再 PING 一次确认。只有主机扫描；远端在数据信道上 2.5 s
// 没听到主机，或会话结束时，自己回到 F。会话中重发率过高或接收机“聋了”时，主机拉黑这个
// 信道，换到下一个最好的点；不成就回 F 重新建立。
//
// 上下文：所有成员只在执行上下文（单循环探针的主循环）里调用。Service() 每轮调用一次；
// 远端的应答在 Service() 里逐包发出，不阻塞主循环；主机转发一条命令时阻塞到应答到达或
// 超时。
//
// A DAP session over the radio between two paired probes. The host probe (on the PC's
// USB) forwards the PC's DAP commands over the radio and returns the answers to USB; the
// remote probe (wired to the target) executes them. Packets: packet.hpp; session rules:
// session.hpp; settings and control port operations: settings.hpp.
//
// Roles. With the AUTO setting (the default) a paired probe serves the radio as a remote
// while it has no debug session of its own. DAP_Connect from USB starts a session: the
// probe pings its peer with a new session id, and when the peer answers the session runs
// over the radio until the next DAP_Connect or DAP_Disconnect; otherwise the probe drives
// its own target. A session does not switch between radio and wire on its own, so a peer
// that drops out makes the commands fail quickly instead of reaching another target. The
// LOCAL, HOST and REMOTE settings fix the role. An unpaired probe does not use the radio
// at all: it is a wired probe whatever its role setting, and never executes commands from
// the radio.
//
// While a session runs over the radio the host sends a heartbeat PING after 1 s without
// a command, and ends the session (PING with id 0) on DAP_Disconnect or after 5 s
// without a command from the PC; the remote ends it on that PING, on a new id, or after
// 3 s without a packet, and then stops the sampling the session started.
//
// Pairing: RequestPairing() (e.g. a key held for 3 s) puts the probe into pairing for up
// to 10 s, see identity.hpp. The settings apply again afterwards; USB commands run
// locally meanwhile.
//
// Channels (channel.hpp): pairing and opening a session happen on the control channel
// F. Once the host has reached its peer on F it picks a data channel (the locked one, or
// the best of a scan; the first session scans first), sends CHANNEL on F, and on
// CHANNEL_ACK both move there and confirm with a PING. Only the host scans; the remote
// returns to F on its own after 2.5 s on a data channel without hearing the host, and
// when a session ends. When a session sees too many resends or a deaf receiver, the host
// bans the channel and moves to the next best point, or opens the session on F again
// when that fails.
//
// Context: every member is called in the execution context only (the main loop of a
// single-loop probe). Service() is called once per round; the remote's answer goes out
// packet by packet from Service() without blocking the main loop; the host blocks while
// it forwards a command, until the answer arrives or the wait runs out.

#include <cstdint>
#include <cstring>

#include "timebase.hpp"
#include "xrdap/control/control.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/radio/forwarding.hpp"
#include "xrdap/radio/identity.hpp"
#include "xrdap/radio/packet.hpp"
#include "xrdap/radio/session.hpp"
#include "xrdap/radio/settings.hpp"
#include "xrdap/session/transport.hpp"

namespace XrDap::Radio
{

/// @brief 无线 DAP 会话 / DAP sessions over the radio.
/// @tparam RadioT 收发器驱动，接口同 xrobot-org/SX1281：Config（modulation、
///         frequency_hz、sync_word、crc_seed）、Start、MaxPayload、PrepareSend、Send、
///         Sending、Poll、Receive、Tune、RssiInstant 和计数 / Transceiver driver with
///         the interface of xrobot-org/SX1281: Config (modulation, frequency_hz,
///         sync_word, crc_seed), Start, MaxPayload, PrepareSend, Send, Sending, Poll,
///         Receive, Tune, RssiInstant and the counters.
/// @tparam ExecT 命令执行器（Executor<DapT>）/ Command executor (Executor<DapT>).
template <typename RadioT, typename ExecT>
class RadioLink
{
 public:
  /// @param radio 收发器 / The transceiver.
  /// @param exec 执行器：远端经它执行命令，主机经它的转发钩子拿到 USB 命令 / The
  ///        executor: the remote executes commands through it, the host gets USB
  ///        commands through its forwarder hook.
  /// @param store 设置存储 / The settings store.
  /// @param uid 本机芯片 UID，kUidSize 字节 / This chip's UID, kUidSize bytes.
  /// @param base 射频的板级配置（功率、晶振微调等）；调制、频点、同步字和 CRC 初值由本类
  ///        填 / Board configuration of the radio (power, crystal trim, ...); this class
  ///        fills in the modulation, frequency, sync word and CRC seed.
  /// @note 初始化时调用；参数的生命周期需长于本对象 / Call at initialisation; the
  ///       arguments must outlive this object.
  RadioLink(RadioT& radio, ExecT& exec, Store& store, const uint8_t* uid,
            const typename RadioT::Config& base)
      : radio_(radio),
        exec_(exec),
        store_(store),
        uid_(uid),
        base_(base),
        radio_config_(base)
  {
  }

  RadioLink(const RadioLink&) = delete;
  RadioLink& operator=(const RadioLink&) = delete;

  /// @brief 装上转发钩子并按设置启动 / Install the forwarder hook and start from the
  ///        settings.
  /// @param safe_mode 安全模式：设置照常读写，但不启动无线 / Safe mode: the settings are
  ///        read and written as usual, but the radio stays off.
  /// @note 初始化时、命令开始执行之前调用一次 / Call once at initialisation, before
  ///       commands run.
  void Begin(const Settings& settings, bool safe_mode)
  {
    settings_ = settings;
    safe_mode_ = safe_mode;
    exec_.SetForwarder(&RadioLink::ForwardHook, this);
    Apply();
  }

  /// @brief 主循环每轮调用：配对、远端应答与执行、主机心跳 / Call every round of the main
  ///        loop: pairing, the remote's answers and commands, the host's heartbeat.
  /// @return 无线在用、主循环不应等待时为 true / true while the radio is in use and the
  ///         main loop should not wait.
  bool Service()
  {
    if (pair_requested_ && !answering_)
    {
      pair_requested_ = false;
      StartPairing();
    }
    if (pairing_)
    {
      ServePairing();
      return true;
    }
    if (!running_)
    {
      return false;
    }
    const uint32_t now = NowUs();
    if (setting_ == RoleSetting::AUTO && session_local_ &&
        now - last_usb_us_ >= kLocalSessionIdleUs)
    {
      session_local_ = false;
    }
    switch (CurrentRole())
    {
      case Role::HOST:
        ServeHost(now);
        break;
      case Role::REMOTE:
        ServeRemote();
        break;
      case Role::LOCAL:
      default:
        break;
    }
    return true;
  }

  /// @brief 下一轮 Service() 开始配对 / Start pairing in the next Service().
  void RequestPairing() { pair_requested_ = true; }

  /// @brief 处理本类的控制口操作（settings.hpp 的 Op）/ Handle this class's control port
  ///        operations (Op in settings.hpp).
  /// @return 是本类的操作、应答已填好时为 true；别的 op 返回 false，由平台处理 / true
  ///         when the op is one of this class's and the answer is filled in; false for
  ///         other ops, which the platform handles.
  bool HandleControl(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                     uint16_t& out)
  {
    if (req == nullptr || len < 1U || !IsOwnOp(req[0]))
    {
      return false;
    }
    const uint8_t op = req[0];
    if (cap < sizeof(StatsAnswer))
    {
      out = Control::StatusOnly(op, Control::kStatusBadRequest, resp, cap);
      return true;
    }
    // 配对占用着射频。 / Pairing holds the radio.
    if (pairing_ && op != Op::kSettings && op != Op::kStats)
    {
      out = Reply(resp, op, Status::kBusy, cap);
      return true;
    }
    out = HandleOp(op, req, len, resp, cap);
    return true;
  }

  /// 当前角色 / Current role.
  Role CurrentRole() const
  {
    if (!running_)
    {
      return Role::LOCAL;
    }
    switch (setting_)
    {
      case RoleSetting::LOCAL:
        return Role::LOCAL;
      case RoleSetting::HOST:
        return Role::HOST;
      case RoleSetting::REMOTE:
        return Role::REMOTE;
      case RoleSetting::AUTO:
      default:
        // 主机会话空闲结束（H3）后探针重新服务无线；工具接着发的命令仍然转发，不会落到
        // 本地目标上。 / After the host session ended on idle (H3) the probe serves the
        // radio again; commands a tool sends later are still forwarded and do not reach
        // the local target.
        return session_wireless_ && host_.Active()    ? Role::HOST
               : session_wireless_ || !session_local_ ? Role::REMOTE
                                                      : Role::LOCAL;
    }
  }

  /// 远端会话打开（对方经无线在用本探针）/ A remote session is open (the peer drives this
  /// probe over the radio).
  bool RemoteActive() const { return running_ && lease_.Active(); }

  /// 配对状态 / Pairing state.
  PairState Pairing() const { return pair_state_; }

 private:
  // 主机等待：第一次等命令的执行加第一个应答包，之后每次加倍，至多 kMaxWaitUs，直到总共
  // 过了 kBudgetUs 加执行时间。每到一个应答包就再等 kFragmentWaitUs，长应答不会被截断。
  // 远端还在执行时重发的命令无害：远端从缓存应答。
  // Host waits: the first wait covers the command's execution plus the first answer
  // packet; every further wait doubles, up to kMaxWaitUs, until kBudgetUs (plus the
  // execution) has passed. Each answer packet that arrives extends the wait by
  // kFragmentWaitUs, so a long answer is not cut off. A command sent again while the
  // remote still executes it is harmless: the remote answers it from its cache.
  static constexpr uint32_t kFirstWaitUs = 3'000;
  static constexpr uint32_t kFragmentWaitUs = 3'000;
  static constexpr uint32_t kMaxWaitUs = 200'000;
  static constexpr uint32_t kBudgetUs = 1'000'000;
  // 收不到包的接收机：主机的 SX1281 可能停止接收而发射照常：它停在接收、一个 IRQ 都没有，
  // 远端却听到每次重发并应答；重新进入接收没用，复位有用。信道被堵时表现相同。发了
  // kDeafSends 次、kDeafUs 内一个包（好坏都算）都没收到时，转发在数据信道上换信道，
  // 在 F 或锁定的信道上复位射频，一次之后继续。
  // A deaf receiver: the host's SX1281 can stop receiving while its transmit still works:
  // it stays in receive with no IRQ at all, while the remote hears every resend and
  // answers it; entering receive again does not help, a reset does. A jammed channel
  // looks the same. After kDeafSends sends and kDeafUs without any packet, good or bad,
  // the forward changes channel when on a data channel, and resets the radio on F or on a
  // locked channel, once, and goes on.
  static constexpr uint32_t kDeafSends = 3;
  static constexpr uint32_t kDeafUs = 60'000;
  // PING 每 kPingWaitUs 加一段每次不同的偏移重发一次，直到 kPingBudgetUs；心跳和结束会话
  // 只发一次。偏移让同时找对方的几对不会一直撞在一起。
  // A PING is sent again every kPingWaitUs plus an offset that changes every time, until
  // kPingBudgetUs; the heartbeat and the session end send it once. The offset keeps pairs
  // looking for their peers at the same time from colliding again and again.
  static constexpr uint32_t kPingWaitUs = 3'000;
  static constexpr uint32_t kPingOffsetUs = 1'024;
  static constexpr uint32_t kPingBudgetUs = 30'000;
  static constexpr uint32_t kPingOnceUs = 0;
  // 在 F 上找对方：DAP_Connect 和回退重建给这么久。F 忙碌 40% 时，3 ms 一次的 PING 在
  // 这段时间里全部失败的可能可以忽略。
  // Reaching the peer on F: DAP_Connect and a fallback get this long. With F busy 40% of
  // the time, a PING every 3 ms practically never fails for this long.
  static constexpr uint32_t kRendezvousUs = 300'000;
  // 换信道：CHANNEL 至多重发这么久；每 kSwitchProbe 次没有 ACK，就到目标信道上 PING
  // 一次（对方可能已经换过去，只是 ACK 丢了）。
  // Changing channel: CHANNEL is sent again for up to this long; after every
  // kSwitchProbe sends without an ACK the host pings on the target channel once (the peer
  // may have moved and only the ACK was lost).
  static constexpr uint32_t kSwitchUs = 100'000;
  static constexpr uint32_t kSwitchProbe = 4;
  // 远端在数据信道上这么久没听到主机就回 F：长于心跳（1 s）加一次扫描，短于租期（3 s），
  // 所以回 F 不结束会话。
  // The remote returns to F after this long on a data channel without hearing the host:
  // longer than the heartbeat (1 s) plus a scan, shorter than the lease (3 s), so that
  // returning to F does not end the session.
  static constexpr uint32_t kChannelSilenceUs = 2'500'000;
  // 扫描：每 2 个索引（1 MHz）测一个点，每点先等 kSettleUs 再读 kDwellUs；扫 kScanPasses
  // 遍，每个点留最差的一遍，共约 0.6 s。首次扫描在 DAP_Connect 里，要短于工具的 USB 超时
  // （OpenOCD 1 s）。
  // Scan: one point every 2 indices (1 MHz), each read for kDwellUs after kSettleUs;
  // kScanPasses passes, keeping the worst pass of each point, about 0.6 s in all. The
  // first scan runs inside DAP_Connect and must stay below the tools' USB timeout
  // (OpenOCD: 1 s).
  static constexpr uint32_t kSettleUs = 300;
  static constexpr uint32_t kDwellUs = 1'500;
  static constexpr uint8_t kScanStep = 2;
  static constexpr uint8_t kScanPasses = 4;
  // 有线会话这么久没有命令就结束，探针重新服务无线（工具退出时没发 DAP_Disconnect）。
  // A wired session without a command for this long ends, and the probe serves the
  // radio again (a tool that quit without DAP_Disconnect).
  static constexpr uint32_t kLocalSessionIdleUs = 5'000'000;
  // 这么久没听到对方，下一条命令先 PING。 / After this long without hearing the peer,
  // the next command pings first.
  static constexpr uint32_t kQuietUs = 1'000'000;
  // 配对：按 kHelloEveryUs 加由 UID 得出的抖动发 HELLO，至多 kPairUs；完成后再应答
  // kLingerUs。
  // Pairing: HELLO every kHelloEveryUs plus a jitter from the UID, for up to kPairUs;
  // once done, answer for another kLingerUs.
  static constexpr uint32_t kPairUs = 10'000'000;
  static constexpr uint32_t kLingerUs = 1'000'000;
  static constexpr uint32_t kHelloEveryUs = 20'000;
  static constexpr uint32_t kHelloJitterUs = 8'000;

  static uint32_t NowUs()
  {
    return static_cast<uint32_t>(LibXR::Timebase::GetMicroseconds());
  }

  static bool IsOwnOp(uint8_t op)
  {
    return op == Op::kRole || op == Op::kStats || op == Op::kSettings ||
           op == Op::kSetRole || op == Op::kSetModulation || op == Op::kUnpair ||
           op == Op::kChannel || op == Op::kPair || op == Op::kForward;
  }

  uint8_t Fragment() const { return static_cast<uint8_t>(radio_.MaxPayload() - kHeader); }

  uint32_t PingWaitUs() const
  {
    return kPingWaitUs + ((NowUs() >> 3) ^ radio_config_.crc_seed) % kPingOffsetUs;
  }

  // ---- 射频 / Radio ----

  void StartRadio(Modulation modulation, const Identity& identity)
  {
    FinishAnswer();
    radio_config_ = base_;
    radio_config_.modulation = modulation == Modulation::GFSK ? RadioT::Modulation::GFSK
                                                              : RadioT::Modulation::FLRC;
    radio_config_.frequency_hz = identity.frequency_hz;
    radio_config_.crc_seed = identity.crc_seed;
    std::memcpy(radio_config_.sync_word,
                modulation == Modulation::GFSK ? identity.sync_word : kFlrcSyncWord,
                sizeof(radio_config_.sync_word));
    // 启动失败时，射频驱动在静默超时后自己复位重试。 / A failed start recovers in the
    // driver, which resets the radio after its silence timeout.
    (void)radio_.Start(radio_config_);
    channel_ = kControlChannel;
  }

  // 复位射频，留在当前信道。 / Reset the radio and stay on the current channel.
  void RestartRadio()
  {
    radio_config_.frequency_hz = ChannelHz(channel_);
    (void)radio_.Start(radio_config_);
  }

  void TuneTo(uint8_t channel)
  {
    if (channel == channel_)
    {
      return;
    }
    if (!running_ && !pairing_)
    {
      channel_ = channel;  // 射频没在用，下次启动在 F / radio off; it starts on F
      return;
    }
    FinishSending();
    (void)radio_.Tune(ChannelHz(channel));
    channel_ = channel;
  }

  // 应用设置（或 Op::kRole 临时指定的角色）：未配对、LOCAL 或安全模式时不用无线（R4：
  // 远端会话结束）。
  // Apply the settings (or the role Op::kRole set for the time being): no radio when
  // unpaired, LOCAL or in safe mode (R4: a remote session ends).
  void Apply()
  {
    ResetSessions();
    setting_ = settings_.role;
    if (role_override_pending_)
    {
      role_override_pending_ = false;
      setting_ = override_role_ == Role::HOST     ? RoleSetting::HOST
                 : override_role_ == Role::REMOTE ? RoleSetting::REMOTE
                                                  : RoleSetting::LOCAL;
    }
    if (settings_.paired != 0U && setting_ != RoleSetting::LOCAL && !safe_mode_)
    {
      StartRadio(settings_.modulation, PairIdentity(settings_.link_id));
      running_ = true;
    }
    else
    {
      FinishAnswer();
      running_ = false;
    }
    EndRemoteSession();
  }

  // 发出一包：上一包还在发射时等它发完，新包紧接着发，中间不回接收。
  // Send one packet: wait while the previous one is on air, then send right after it
  // without receiving in between.
  void Transmit(const uint8_t* packet, uint8_t size)
  {
    while (radio_.Send(packet, size) == LibXR::ErrorCode::BUSY)
    {
    }
  }

  // 等最后一包发完，转回接收。 / Wait for the last packet and return to receive.
  void FinishSending()
  {
    while (radio_.Sending())
    {
      radio_.Poll();
    }
  }

  void SendMessage(uint8_t kind, uint8_t sequence, const uint8_t* data, uint16_t size)
  {
    const uint8_t fragment = Fragment();
    const uint8_t count = FragmentCount(size, fragment);
    for (uint8_t i = 0; i < count; ++i)
    {
      Transmit(tx_, BuildFragment(kind, sequence, data, size, fragment, i, tx_));
    }
    FinishSending();
  }

  void SendPing(uint8_t kind, uint8_t sequence, uint16_t session)
  {
    Transmit(tx_, BuildPing(kind, sequence, session, tx_));
    FinishSending();
  }

  // ---- 主机 / Host ----

  // 主机：发 PING(session)，timeout_us 内对方回 PONG 时为 true；每 kPingWaitUs 重发。
  // Host: send PING(session); true when the peer answers with a PONG within timeout_us;
  // repeats every kPingWaitUs.
  bool Ping(uint16_t session, uint32_t timeout_us)
  {
    if (!running_)
    {
      return false;
    }
    const uint8_t sequence = ++host_sequence_;
    const uint32_t begin = NowUs();
    do
    {
      SendPing(kPing, sequence, session);
      const uint32_t deadline = NowUs() + PingWaitUs();
      while (static_cast<int32_t>(deadline - NowUs()) > 0)
      {
        radio_.Poll();
        uint16_t echoed = 0;
        const int n = radio_.Receive(rx_);
        if (n > 0 && rx_[0] == kPong && rx_[1] == sequence && ParsePing(rx_, n, echoed) &&
            echoed == session)
        {
          return true;
        }
      }
    } while (NowUs() - begin < timeout_us);
    return false;
  }

  // 主机：wait_us 内等一个 kind 类型、序号为 sequence 的包，收到时它在 rx_ 里。
  // Host: wait up to wait_us for a packet of `kind` with `sequence`; it is in rx_ then.
  int AwaitReply(uint8_t kind, uint8_t sequence, uint32_t wait_us)
  {
    const uint32_t deadline = NowUs() + wait_us;
    while (static_cast<int32_t>(deadline - NowUs()) > 0)
    {
      radio_.Poll();
      const int n = radio_.Receive(rx_);
      if (n >= kHeader && rx_[0] == kind && rx_[1] == sequence)
      {
        return n;
      }
    }
    return 0;
  }

  // 测一个点；调用方之后回到原信道。 / Measure one point; the caller returns to its
  // channel afterwards.
  ChannelSample Measure(uint8_t channel)
  {
    TuneTo(channel);
    LibXR::Timebase::DelayMicroseconds(kSettleUs);
    RssiMeter meter;
    const uint32_t begin = NowUs();
    do
    {
      meter.Add(radio_.RssiInstant());
    } while (NowUs() - begin < kDwellUs);
    return meter.Result();
  }

  // 主机：选信道用的整段扫描。 / Host: the full scan the choice of channel uses.
  void ScanAll()
  {
    const uint8_t back = channel_;
    channels_.ClearSamples();
    for (uint8_t pass = 0; pass < kScanPasses; ++pass)
    {
      for (uint8_t channel = 0; channel < kChannels; channel += kScanStep)
      {
        channels_.Merge(channel, Measure(channel));
      }
    }
    TuneTo(back);
  }

  uint8_t LockedChannel() const
  {
    return settings_.channel_lock != 0U && settings_.channel_lock <= kChannels
               ? static_cast<uint8_t>(settings_.channel_lock - 1U)
               : kNoChannel;
  }

  // 主机：该用的数据信道：锁定的，或扫描里最好的；都没有时为 F。
  // Host: the data channel to use: the locked one or the best of the scan; F when there
  // is none.
  uint8_t ChooseChannel()
  {
    const uint8_t locked = LockedChannel();
    if (locked != kNoChannel)
    {
      return locked;
    }
    const uint8_t best = channels_.Best(NowUs());
    return best != kNoChannel ? best : kControlChannel;
  }

  // 主机：在当前信道上让对方换到 target，双方都换过去并 PING 通时为 true。失败时主机留在
  // 当前信道或 F 上，对方可能已在 target 上，它沉默超时后会回 F。
  // Host: make the peer move to `target` from the current channel; true once both are
  // there and a PING went through. On failure the host stays on the current channel or
  // F; the peer may be on `target` already and returns to F after its silence timeout.
  bool SwitchTo(uint8_t target)
  {
    if (target == channel_)
    {
      return true;
    }
    const uint16_t session = host_.Session();
    const uint8_t from = channel_;
    const uint32_t begin = NowUs();
    for (uint32_t sends = 1; NowUs() - begin < kSwitchUs; ++sends)
    {
      const uint8_t sequence = ++host_sequence_;
      Transmit(tx_, BuildChannel(kChannel, sequence, session, target, tx_));
      FinishSending();
      uint16_t echoed = 0;
      uint8_t channel = kNoChannel;
      const int n = AwaitReply(kChannelAck, sequence, PingWaitUs());
      const bool acked = n > 0 && ParseChannel(rx_, n, echoed, channel) &&
                         echoed == session && channel == target;
      if (!acked && sends % kSwitchProbe != 0U)
      {
        continue;
      }
      TuneTo(target);
      if (Ping(session, acked ? kPingBudgetUs : kPingOnceUs))
      {
        return true;
      }
      TuneTo(from);
      if (acked)
      {
        // 对方换过去了却不通。 / The peer moved but does not answer.
        NoteStray(target, session);
        return false;
      }
    }
    return false;
  }

  // 主机：记下对方可能还留着的数据信道。对方要到沉默超时才回 F，这段时间里 F 上找不到它。
  // Host: note a data channel the peer may still be on. It returns to F only after its
  // silence timeout, and until then F does not find it.
  void NoteStray(uint8_t channel, uint16_t session)
  {
    stray_ = channel;
    stray_session_ = session;
  }

  // 主机：找到对方。先试它可能留着的数据信道；同一个会话时它留在那里，别的会话号让它结束
  // 旧会话回 F。再试 F。
  // Host: find the peer. First the data channel it may have stayed on: with the same
  // session it stays there, and another id makes it end the old session and return to
  // F. Then F.
  bool FindPeer()
  {
    const uint16_t session = host_.Session();
    const uint8_t left = channel_;
    if (stray_ != kNoChannel)
    {
      const uint8_t stray = stray_;
      const uint16_t stray_session = stray_session_;
      stray_ = kNoChannel;
      TuneTo(stray);
      if (Ping(session, kPingBudgetUs) && stray_session == session)
      {
        return true;
      }
    }
    TuneTo(kControlChannel);
    if (Ping(session, kRendezvousUs))
    {
      return true;
    }
    if (left != kControlChannel)
    {
      NoteStray(left, session);
    }
    return false;
  }

  // 主机：找到对方（第一次先扫描），需要时换到数据信道。对方在时为 true；换信道失败时留在
  // 对方所在的信道上。
  // Host: find the peer (scanning first the first time) and move to the data channel if
  // needed. True when the peer is there; a failed change of channel stays where the
  // peer is.
  bool Rendezvous()
  {
    if (!FindPeer())
    {
      return false;
    }
    if (channel_ != kControlChannel)
    {
      return true;  // 对方还在数据信道上，接着用 / the peer is still on a data channel
    }
    if (LockedChannel() == kNoChannel && !channels_.Scanned())
    {
      ScanAll();
    }
    const uint8_t target = ChooseChannel();
    if (!SwitchTo(target))
    {
      channels_.Ban(target, NowUs());
      return FindPeer();
    }
    return true;
  }

  // 主机：会话中当前数据信道变差：拉黑它，换到下一个最好的点；不成就回 F，下一条命令在
  // F 上重新建立。
  // Host: the data channel turned bad during a session: ban it and move to the next best
  // point; when that fails, return to F, and the next command opens the session there
  // again.
  void Reselect()
  {
    channels_.Ban(channel_, NowUs());
    retries_.Reset();
    if (LockedChannel() != kNoChannel)
    {
      return;  // 锁定时不换 / a locked channel stays
    }
    const uint8_t from = channel_;
    const uint8_t target = ChooseChannel();
    if (!SwitchTo(target))
    {
      channels_.Ban(target, NowUs());
      if (stray_ == kNoChannel && from != kControlChannel)
      {
        NoteStray(from, host_.Session());
      }
      TuneTo(kControlChannel);
      peer_lost_ = true;
    }
  }

  // 主机：转发一条命令并等应答；1 s 加 execution_us 内收到完整应答时为 true。more 表示
  // 之后还有命令，应答到达后射频提前准备发射。
  // Host: forward one command and wait for its answer; true with a complete answer
  // within 1 s plus execution_us. `more` means more commands follow: the radio prepares
  // to transmit once the answer is in.
  bool Exchange(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                uint16_t& out, uint32_t execution_us, bool more, bool control)
  {
    if (!running_ || len == 0U || len > kMaxMessage)
    {
      return false;
    }
    const uint8_t sequence = ++host_sequence_;
    ++stats_.commands;
    const uint32_t budget = kBudgetUs + execution_us;
    Reassembler answers(resp, cap);
    uint32_t wait = kFirstWaitUs + execution_us;
    const uint32_t begin = NowUs();
    uint32_t sends = 0;
    uint32_t heard = begin;  // 上一个包，好坏都算 / last packet, good or bad
    bool reset = false;
    while (NowUs() - begin < budget)
    {
      SendMessage(control ? kControl : kCommand, sequence, req, len);
      ++sends;
      uint32_t deadline = NowUs() + wait;
      while (static_cast<int32_t>(deadline - NowUs()) > 0)
      {
        radio_.Poll();
        const int n = radio_.Receive(rx_);
        if (n != 0)
        {
          heard = NowUs();
        }
        if (n < kHeader || rx_[0] != kAnswer || rx_[1] != sequence)
        {
          continue;
        }
        if (answers.Add(rx_, n, Fragment()) == Reassembler::Result::COMPLETE)
        {
          // 会话的下一条命令不必再从接收切出来。 / The next command of the session
          // goes out without the switch out of receive.
          if (more)
          {
            radio_.PrepareSend();
          }
          out = answers.Size();
          stats_.retries += sends - 1U;
          stats_.last_rtt_us = NowUs() - begin;
          if (stats_.last_rtt_us > stats_.max_rtt_us)
          {
            stats_.max_rtt_us = stats_.last_rtt_us;
          }
          reselect_ = retries_.Add(sends > 1U) || reselect_;
          return true;
        }
        deadline = NowUs() + kFragmentWaitUs;
      }
      if (!reset && sends >= kDeafSends && NowUs() - heard >= kDeafUs)
      {
        reset = true;
        if (channel_ != kControlChannel && LockedChannel() == kNoChannel)
        {
          Reselect();
          if (peer_lost_ && Rendezvous())
          {
            peer_lost_ = false;
          }
        }
        else
        {
          RestartRadio();
        }
        heard = NowUs();
      }
      wait = wait * 2U < kMaxWaitUs ? wait * 2U : kMaxWaitUs;
    }
    stats_.retries += sends - 1U;
    ++stats_.failures;
    reselect_ = retries_.Add(true) || reselect_;
    return false;
  }

  // 主机：结束会话（PING 0，H3）并回 F。在数据信道上重发到对方应答，免得它留在那里
  // 等沉默超时；F 上发一次。
  // Host: end the session (PING 0, H3) and return to F. On a data channel it is sent
  // until the peer answers, so that the peer does not stay there until its silence
  // timeout; on F it is sent once.
  void EndHostSession()
  {
    const uint8_t from = channel_;
    if (!Ping(0, from != kControlChannel ? kPingBudgetUs : kPingOnceUs) &&
        from != kControlChannel)
    {
      NoteStray(from, 0);
    }
    TuneTo(kControlChannel);
  }

  void ResetSessions()
  {
    session_wireless_ = false;
    session_local_ = false;
    peer_lost_ = false;
    if (host_.OnDisconnect())
    {
      EndHostSession();
    }
    else if (running_)
    {
      TuneTo(kControlChannel);
    }
  }

  // 需要时打开主机会话（H1），并确认对方在。 / Open the host session if needed (H1)
  // and make sure the peer is there.
  bool ReachPeer()
  {
    const uint32_t now = NowUs();
    const bool fresh = host_.OnCommand(now);
    if (fresh || peer_lost_ || now - heard_peer_us_ >= kQuietUs)
    {
      // 新会话在 F 上建立；会话中先在当前信道上 PING（可能只丢了一个包），不通再回 F。
      // A new session opens on F; within a session ping on the current channel first (a
      // single packet may have been lost), and return to F only when that fails.
      if (!(!fresh && Ping(host_.Session(), kPingBudgetUs)) && !Rendezvous())
      {
        return false;
      }
      peer_lost_ = false;
      heard_peer_us_ = NowUs();
      host_.OnSent(heard_peer_us_);
    }
    return true;
  }

  static uint16_t Fail(const uint8_t* req, uint8_t* resp)
  {
    resp[0] = req[0];
    resp[1] = Byte(DapStatus::ERROR);
    return 2U;
  }

  bool Forward(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
               uint16_t& out, bool control)
  {
    // 控制请求不是 DAP 命令：没有 SWJ 时钟要记，没有 DAP_Disconnect 结束会话，远端执行
    // 它不花 SWD 时间。
    // A control request is not a DAP command: no SWJ clock to note, no DAP_Disconnect to
    // end the session, and the remote executes it without SWD time.
    const uint32_t hz = control ? 0U : SwjClockHz(req, len);
    if (hz != 0U)
    {
      swd_hz_ = hz;
    }
    if (!ReachPeer())
    {
      peer_lost_ = true;
      out = Fail(req, resp);
      return false;
    }
    const bool disconnect = !control && IsCommand(req, len, DapCmd::DISCONNECT);
    if (!Exchange(req, len, resp, cap, out, control ? 0U : ExecutionUs(req, len, swd_hz_),
                  !disconnect, control))
    {
      peer_lost_ = true;
      out = Fail(req, resp);
      return false;
    }
    heard_peer_us_ = NowUs();
    host_.OnSent(heard_peer_us_);
    if (disconnect && host_.OnDisconnect())
    {
      EndHostSession();  // H3
    }
    else if (reselect_)
    {
      reselect_ = false;
      Reselect();
    }
    return true;
  }

  static bool ForwardHook(void* ctx, Transport t, const uint8_t* req, uint16_t len,
                          uint8_t* resp, uint16_t cap, uint16_t& out)
  {
    return static_cast<RadioLink*>(ctx)->OnUsbCommand(t, req, len, resp, cap, out);
  }

  // USB 来的 DAP 命令：经无线转发并填好应答时为 true；在本地执行时为 false。远端执行的
  // 无线命令也经过这里（Transport::RADIO），一律本地执行。
  // A DAP command from USB: true when it was forwarded over the radio and the answer is
  // filled in; false when it runs locally. Commands the remote executes for the radio
  // pass here too (Transport::RADIO) and always run locally.
  bool OnUsbCommand(Transport t, const uint8_t* req, uint16_t len, uint8_t* resp,
                    uint16_t cap, uint16_t& out)
  {
    if (t != Transport::USB || !running_ || pairing_ || len == 0U)
    {
      return false;
    }
    last_usb_us_ = NowUs();
    switch (setting_)
    {
      case RoleSetting::HOST:
        (void)Forward(req, len, resp, cap, out, false);
        return true;
      case RoleSetting::LOCAL:
      case RoleSetting::REMOTE:
        return false;
      case RoleSetting::AUTO:
      default:
        break;
    }
    // DAP_Info 在两块探针上相同；工具连接前就会问。 / DAP_Info is the same on both
    // probes; tools ask it before they connect.
    if (IsCommand(req, len, DapCmd::INFO))
    {
      return false;
    }
    if (IsCommand(req, len, DapCmd::CONNECT))
    {
      // 新会话：上一个会话（如果有）对远端也结束了。 / A new session: the previous one
      // (if any) is over for the remote too.
      ResetSessions();
      session_wireless_ = ReachPeer();
      session_local_ = !session_wireless_;
      if (session_wireless_)
      {
        for (uint8_t i = 0; i < SettingsReplay::kSettings; ++i)
        {
          if (replay_.Size(i) == 0U)
          {
            continue;
          }
          uint8_t ignored[16];
          uint16_t ignored_len = 0;
          (void)Forward(replay_.Data(i), replay_.Size(i), ignored, sizeof(ignored),
                        ignored_len, false);
        }
      }
      else
      {
        (void)host_.OnDisconnect();  // 没人应答：不走无线 / nobody answered
      }
    }
    replay_.Note(req, len);
    if (!session_wireless_)
    {
      if (IsCommand(req, len, DapCmd::DISCONNECT))
      {
        session_local_ = false;
      }
      return false;
    }
    (void)Forward(req, len, resp, cap, out, false);
    if (IsCommand(req, len, DapCmd::DISCONNECT))
    {
      session_wireless_ = false;
    }
    return true;
  }

  void ServeHost(uint32_t now)
  {
    switch (host_.Tick(now))
    {
      case HostSession::Action::HEARTBEAT:  // H2
        if (Ping(host_.Session(), kPingOnceUs))
        {
          heard_peer_us_ = NowUs();
        }
        else
        {
          peer_lost_ = true;
        }
        break;
      case HostSession::Action::END:  // H3
        EndHostSession();
        break;
      case HostSession::Action::NONE:
      default:
        break;
    }
  }

  // ---- 远端 / Remote ----

  // R4：清掉结束的会话留下的东西，连接在无线上时作废。 / R4: drop what the ended
  // session left behind; a connection on the radio lapses.
  void EndSession()
  {
    commands_.Reset();
    cache_.Clear();
    exec_.LinkLost(Transport::RADIO);
    TuneTo(kControlChannel);
  }

  void EndRemoteSession()
  {
    if (lease_.OnPing(0, NowUs()) != RemoteLease::Event::NONE)
    {
      EndSession();
    }
    commands_.Reset();
    cache_.Clear();
  }

  void StartAnswer(uint8_t sequence)
  {
    answer_sequence_ = sequence;
    answer_index_ = 0;
    answer_packet_ = 0;
    answer_count_ = FragmentCount(answer_size_, Fragment());
    answering_ = true;
  }

  // 推进应答：上一包发完就发下一包，中间不回接收；全部发出后执行等着的设置变更。返回
  // true 表示还没发完。
  // Advance the answer: send the next packet once the previous one is out, without
  // receiving in between; when all are out, carry out a pending settings change.
  // Returns true while the answer is not finished.
  bool PumpAnswer()
  {
    if (!answering_)
    {
      return false;
    }
    if (answer_index_ < answer_count_)
    {
      if (answer_packet_ == 0U)
      {
        answer_packet_ = BuildFragment(kAnswer, answer_sequence_, answer_, answer_size_,
                                       Fragment(), answer_index_, tx_);
      }
      if (radio_.Send(tx_, answer_packet_) != LibXR::ErrorCode::BUSY)
      {
        answer_packet_ = 0;
        ++answer_index_;
      }
      return true;
    }
    if (radio_.Sending())
    {
      radio_.Poll();
      return true;
    }
    answering_ = false;
    if (apply_pending_)
    {
      apply_pending_ = false;
      Apply();
    }
    return false;
  }

  // 重启或停用射频前，先发完还在路上的应答。 / Before the radio restarts or stops, an
  // answer still on its way is finished.
  void FinishAnswer()
  {
    while (PumpAnswer())
    {
    }
  }

  // 改设置的请求要先应答：经无线来的等应答发出后再生效，USB 来的立即生效。
  // A request that changes the settings answers first: one from the radio takes effect
  // once its answer is out, one from USB at once.
  void ApplyAfterAnswer()
  {
    if (executing_)
    {
      apply_pending_ = true;
    }
    else
    {
      Apply();
    }
  }

  // 远端：收下命令的一个分片；其余分片紧跟着到，在这里接着收，不等主循环下一圈。命令
  // 齐了时为 true。
  // Remote: take one fragment of a command; the others follow right behind it and are
  // received here instead of on the next turns of the main loop. True once the command
  // is complete.
  bool ReceiveCommand(int n)
  {
    const uint8_t kind = rx_[0];
    auto added = commands_.Add(rx_, n, Fragment());
    uint32_t deadline = NowUs() + kFragmentWaitUs;
    while (added == Reassembler::Result::INCOMPLETE &&
           static_cast<int32_t>(deadline - NowUs()) > 0)
    {
      radio_.Poll();
      const int m = radio_.Receive(rx_);
      if (m < kHeader || rx_[0] != kind || !lease_.OnCommand(NowUs()))
      {
        continue;
      }
      added = commands_.Add(rx_, m, Fragment());
      deadline = NowUs() + kFragmentWaitUs;
    }
    return added == Reassembler::Result::COMPLETE;
  }

  void ServeRemote()
  {
    if (PumpAnswer())
    {
      return;
    }
    radio_.Poll();
    const int n = radio_.Receive(rx_);
    const uint32_t now = NowUs();
    if (n < kHeader)
    {
      if (lease_.Tick(now) == RemoteLease::Event::ENDED)
      {
        EndSession();
      }
      if (channel_ != kControlChannel && now - heard_host_us_ >= kChannelSilenceUs)
      {
        TuneTo(kControlChannel);  // 主机不在这个信道上了 / the host left this channel
      }
      return;
    }
    heard_host_us_ = now;
    const uint8_t sequence = rx_[1];
    if (rx_[0] == kChannel)
    {
      uint16_t session = 0;
      uint8_t channel = kNoChannel;
      if (!ParseChannel(rx_, n, session, channel) || !IsChannel(channel) ||
          session != lease_.Session() || !lease_.OnCommand(now))
      {
        return;
      }
      Transmit(tx_, BuildChannel(kChannelAck, sequence, session, channel, tx_));
      FinishSending();
      TuneTo(channel);
      return;
    }
    if (rx_[0] == kPing)
    {
      uint16_t session = 0;
      if (!ParsePing(rx_, n, session))
      {
        return;
      }
      const bool changed = lease_.OnPing(session, now) != RemoteLease::Event::NONE;
      // 先在 PING 来的信道上应答，再结束会话（结束时回 F）。 / Answer on the channel
      // the PING came on first, then end the session (which returns to F).
      SendPing(kPong, sequence, lease_.Session());
      if (changed)
      {
        EndSession();
      }
      return;
    }
    const bool control = rx_[0] == kControl;
    if ((rx_[0] != kCommand && !control) || !lease_.OnCommand(now))  // R5
    {
      return;
    }
    if (!ReceiveCommand(n))
    {
      return;
    }
    if (cache_.Has(commands_.Sequence()))
    {
      // 主机没收到应答：重发缓存，不再执行一次。 / The host did not get the answer:
      // send it again instead of executing twice.
      ++stats_.duplicates;
      StartAnswer(commands_.Sequence());
      return;
    }
    radio_.PrepareSend();  // 切到 FS 与执行重叠 / the switch to FS overlaps the execution
    executing_ = true;
    uint16_t size = 0;
    if (control)
    {
      exec_.ControlHere(command_, commands_.Size(), answer_, sizeof(answer_), size);
    }
    else
    {
      exec_.ExecuteHere(Transport::RADIO, command_, commands_.Size(), answer_,
                        sizeof(answer_), size);
    }
    executing_ = false;
    answer_size_ = size;
    cache_.Store(commands_.Sequence());
    StartAnswer(commands_.Sequence());
  }

  // ---- 配对 / Pairing ----

  void StartPairing()
  {
    FinishAnswer();
    ResetSessions();
    EndRemoteSession();
    pair_state_ = PairState::RUNNING;
    pairing_ = true;
    peer_known_ = false;
    pair_done_ = false;
    StartRadio(Modulation::GFSK, DefaultIdentity());
    pair_begin_us_ = NowUs();
    next_hello_us_ = pair_begin_us_;
  }

  void ServePairing()
  {
    const uint32_t now = NowUs();
    if ((pair_done_ && now - done_at_us_ >= kLingerUs) ||
        (!pair_done_ && now - pair_begin_us_ >= kPairUs))
    {
      FinishPairing();
      return;
    }
    radio_.Poll();
    if (radio_.Sending())
    {
      return;
    }
    if (static_cast<int32_t>(now - next_hello_us_) >= 0)
    {
      (void)radio_.Send(tx_, BuildHello(uid_, peer_known_ ? peer_ : nullptr, tx_));
      // 由 UID 得出的抖动让两块探针不同步发送。 / A jitter from the UID keeps two
      // probes from sending in lockstep.
      next_hello_us_ = now + kHelloEveryUs + Fnv1a(uid_, kUidSize) % kHelloJitterUs;
      return;
    }
    const int n = radio_.Receive(rx_);
    if (n != kHelloSize || rx_[0] != kHello)
    {
      return;
    }
    const uint8_t* from = rx_ + 1;
    const uint8_t* names = rx_ + 1 + kUidSize;
    if (std::memcmp(from, uid_, kUidSize) == 0)
    {
      return;
    }
    if (!peer_known_)
    {
      std::memcpy(peer_, from, kUidSize);
      peer_known_ = true;
      next_hello_us_ = now;  // 立即告诉对方 / tell the peer about it at once
    }
    if (std::memcmp(from, peer_, kUidSize) != 0)
    {
      return;  // 第三块探针 / a third probe
    }
    if (!pair_done_ && std::memcmp(names, uid_, kUidSize) == 0)
    {
      pair_done_ = true;
      done_at_us_ = now;
    }
  }

  void FinishPairing()
  {
    pairing_ = false;
    pair_state_ = PairState::FAILED;
    if (pair_done_)
    {
      Settings settings = settings_;
      settings.paired = 1;
      settings.link_id = LinkId(uid_, peer_);
      settings.modulation = Modulation::GFSK;
      std::memcpy(settings.peer_uid, peer_, kUidSize);
      if (store_.Save(settings))
      {
        settings_ = settings;
        pair_state_ = PairState::DONE;
      }
    }
    Apply();
  }

  // ---- 控制口 / Control port ----

  uint16_t Reply(uint8_t* resp, uint8_t op, uint8_t status, uint16_t cap)
  {
    return Control::StatusOnly(op, status, resp, cap);
  }

  uint16_t Save(uint8_t* resp, uint8_t op, const Settings& settings, uint16_t cap)
  {
    if (!store_.Save(settings))
    {
      return Reply(resp, op, Status::kError, cap);
    }
    settings_ = settings;
    ApplyAfterAnswer();
    return Reply(resp, op, Status::kOk, cap);
  }

  uint16_t HandleOp(uint8_t op, const uint8_t* req, uint16_t len, uint8_t* resp,
                    uint16_t cap)
  {
    switch (op)
    {
      case Op::kRole:
      {
        if (len < 2U || req[1] > static_cast<uint8_t>(Role::REMOTE))
        {
          return Reply(resp, op, Status::kBadRequest, cap);
        }
        if (settings_.paired == 0U && req[1] != static_cast<uint8_t>(Role::LOCAL))
        {
          return Reply(resp, op, Status::kNotPaired, cap);
        }
        // 只改运行中的角色，不保存。 / Changes the running role only; nothing is stored.
        override_role_ = static_cast<Role>(req[1]);
        role_override_pending_ = true;
        ApplyAfterAnswer();
        return Reply(resp, op, Status::kOk, cap);
      }
      case Op::kSettings:
      {
        SettingsAnswer answer{};
        answer.op = op;
        answer.status = Status::kOk;
        answer.role_setting = static_cast<uint8_t>(settings_.role);
        answer.modulation = static_cast<uint8_t>(settings_.modulation);
        answer.paired = settings_.paired;
        answer.safe_mode = safe_mode_ ? 1U : 0U;
        answer.role = static_cast<uint8_t>(CurrentRole());
        answer.pair_state = static_cast<uint8_t>(pair_state_);
        for (int i = 0; i < 4; ++i)
        {
          answer.link_id[i] = static_cast<uint8_t>(settings_.link_id >> (8 * i));
        }
        std::memcpy(answer.peer_uid, settings_.peer_uid, kUidSize);
        std::memcpy(resp, &answer, sizeof(answer));
        return sizeof(answer);
      }
      case Op::kSetRole:
      {
        if (len < 2U || req[1] > static_cast<uint8_t>(RoleSetting::REMOTE))
        {
          return Reply(resp, op, Status::kBadRequest, cap);
        }
        Settings settings = settings_;
        settings.role = static_cast<RoleSetting>(req[1]);
        return Save(resp, op, settings, cap);
      }
      case Op::kSetModulation:
      {
        if (len < 2U || req[1] > static_cast<uint8_t>(Modulation::FLRC))
        {
          return Reply(resp, op, Status::kBadRequest, cap);
        }
        Settings settings = settings_;
        settings.modulation = static_cast<Modulation>(req[1]);
        return Save(resp, op, settings, cap);
      }
      case Op::kUnpair:
      {
        Settings settings = settings_;
        settings.paired = 0;
        settings.link_id = 0;
        std::memset(settings.peer_uid, 0, sizeof(settings.peer_uid));
        return Save(resp, op, settings, cap);
      }
      case Op::kPair:
        RequestPairing();
        return Reply(resp, op, Status::kOk, cap);
      case Op::kStats:
        return StatsReply(resp);
      case Op::kChannel:
        return ChannelReply(req, len, resp, cap);
      case Op::kForward:
      default:
      {
        // 发给对方的请求是其余字节；不转发转发请求。 / The request for the peer is the
        // rest; forwarding a forward is refused.
        if (len < 2U || req[1] == Op::kForward)
        {
          return Reply(resp, op, Status::kBadRequest, cap);
        }
        if (!running_)
        {
          return Reply(resp, op, Status::kNotPaired, cap);
        }
        uint16_t size = 0;
        if (!Forward(req + 1, static_cast<uint16_t>(len - 1U), resp, cap, size, true))
        {
          return Reply(resp, op, Status::kTimeout, cap);
        }
        return size;
      }
    }
  }

  uint16_t StatsReply(uint8_t* resp)
  {
    StatsAnswer answer{};
    answer.op = Op::kStats;
    answer.status = Status::kOk;
    answer.count = 13;
    answer.radio_tx = radio_.TxPackets();
    answer.radio_rx = radio_.RxPackets();
    answer.crc_errors = radio_.RxErrors();
    answer.tx_timeouts = radio_.TxErrors();
    answer.commands = stats_.commands;
    answer.retries = stats_.retries;
    answer.failures = stats_.failures;
    answer.duplicates = stats_.duplicates;
    answer.last_rtt_us = stats_.last_rtt_us;
    answer.max_rtt_us = stats_.max_rtt_us;
    answer.channel = channel_;
    answer.rearms = radio_.Rearms();
    answer.restarts = radio_.Restarts();
    std::memcpy(resp, &answer, sizeof(answer));
    return sizeof(answer);
  }

  uint16_t ChannelReply(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap)
  {
    if (len >= 3U)
    {
      const uint8_t mode = req[1];
      const uint8_t channel = req[2];
      if (mode > 1U || (mode != 0U && !IsChannel(channel)))
      {
        return Reply(resp, Op::kChannel, Status::kBadRequest, cap);
      }
      // 只存锁定，不重新应用设置：下次选信道时生效。 / Store the lock only, without
      // applying the settings again: it takes effect at the next choice of channel.
      Settings settings = settings_;
      settings.channel_lock = mode == 0U ? 0U : static_cast<uint16_t>(channel + 1U);
      if (!store_.Save(settings))
      {
        return Reply(resp, Op::kChannel, Status::kError, cap);
      }
      settings_ = settings;
    }
    else if (len != 1U)
    {
      return Reply(resp, Op::kChannel, Status::kBadRequest, cap);
    }
    ChannelAnswer answer{};
    answer.op = Op::kChannel;
    answer.status = Status::kOk;
    answer.channel = channel_;
    answer.lock = LockedChannel();
    std::memcpy(resp, &answer, sizeof(answer));
    return sizeof(answer);
  }

  RadioT& radio_;
  ExecT& exec_;
  Store& store_;
  const uint8_t* uid_;
  const typename RadioT::Config base_;
  typename RadioT::Config radio_config_;

  Settings settings_{};
  RoleSetting setting_ = RoleSetting::AUTO;
  bool safe_mode_ = false;
  bool running_ = false;
  Role override_role_ = Role::LOCAL;
  bool role_override_pending_ = false;

  // 信道 / Channels.
  uint8_t channel_ = kControlChannel;  // 射频现在所在的信道 / the channel the radio is on
  uint8_t stray_ = kNoChannel;         // 对方可能留着的信道 / channel the peer may be on
  uint16_t stray_session_ = 0;         // 记下时的会话号 / the session noted with it
  bool reselect_ = false;              // 重发率过高，命令之后换信道 / too many resends
  ChannelTable channels_;
  RetryWindow retries_;
  struct
  {
    uint32_t commands = 0;
    uint32_t retries = 0;
    uint32_t failures = 0;
    uint32_t duplicates = 0;
    uint32_t last_rtt_us = 0;
    uint32_t max_rtt_us = 0;
  } stats_;

  uint8_t tx_[256] = {};
  uint8_t rx_[256] = {};

  // 主机 / Host.
  HostSession host_;
  uint8_t host_sequence_ = 0;
  bool session_wireless_ = false;
  bool session_local_ = false;
  bool peer_lost_ = false;
  uint32_t last_usb_us_ = 0;
  uint32_t heard_peer_us_ = 0;
  uint32_t swd_hz_ = kDefaultSwdHz;
  SettingsReplay replay_;

  // 远端 / Remote.
  uint8_t command_[kMaxMessage] = {};
  uint8_t answer_[kMaxMessage] = {};
  uint16_t answer_size_ = 0;
  Reassembler commands_{command_, sizeof(command_)};
  RemoteLease lease_;
  AnswerCache cache_;
  uint32_t heard_host_us_ = 0;  // 上一个主机的包 / last packet from the host
  bool executing_ = false;
  bool apply_pending_ = false;
  bool answering_ = false;
  uint8_t answer_sequence_ = 0;
  uint8_t answer_index_ = 0;
  uint8_t answer_count_ = 0;
  // tx_ 里待发的应答包长，0 为还没生成。 / Size of the answer packet waiting in tx_, 0
  // before it is built.
  uint8_t answer_packet_ = 0;

  // 配对 / Pairing.
  PairState pair_state_ = PairState::IDLE;
  bool pair_requested_ = false;
  bool pairing_ = false;
  bool peer_known_ = false;
  bool pair_done_ = false;
  uint32_t pair_begin_us_ = 0;
  uint32_t done_at_us_ = 0;
  uint32_t next_hello_us_ = 0;
  uint8_t peer_[kUidSize] = {};
};

}  // namespace XrDap::Radio
