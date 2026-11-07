// SPDX-License-Identifier: Apache-2.0
#pragma once

// 两块配对探针之间经无线的 DAP 会话。主机探针（接在电脑的 USB 上）把电脑的 DAP 命令经
// 无线转发出去，再把应答交回
// USB；远端探针（接目标）执行它们。这个类决定探针现在是什么角色，
// 并处理控制口操作；具体的事在 host_link.hpp、channel_picker.hpp、remote_link.hpp、
// pairing.hpp 和 radio_port.hpp。包格式见 packet.hpp，会话规则见
// session.hpp，设置和控制口 操作见 settings.hpp。
//
// 角色：AUTO 设置（默认）下，配对的探针没有自己的调试会话时当远端。USB 上的 DAP_Connect
// 开始一个会话：探针用新会话号 PING 对方，对方应答时会话走无线，直到下一个 DAP_Connect 或
// DAP_Disconnect；否则探针驱动自己的目标。会话不会自己在无线和有线之间切换，对方掉线时
// 命令很快失败，不会落到另一个目标上。LOCAL、HOST、REMOTE 设置固定角色。未配对的探针完全
// 不用无线：无论角色设置，它都是有线探针，也不执行无线收到的命令。
//
// 配对：RequestPairing()（例如按住按键 3 s）让探针配对至多 10 s，见 identity.hpp。配对
// 结束后设置重新生效，配对期间 USB 命令在本地执行。
//
// 上下文：所有成员只在执行上下文（单循环探针的主循环）里调用。Service() 每轮调用一次。
//
// A DAP session over the radio between two paired probes. The host probe (on the PC's
// USB) forwards the PC's DAP commands over the radio and returns the answers to USB; the
// remote probe (wired to the target) executes them. This class decides which role the
// probe has now and handles the control port operations; the work itself is in
// host_link.hpp, channel_picker.hpp, remote_link.hpp, pairing.hpp and radio_port.hpp.
// Packets: packet.hpp; session rules: session.hpp; settings and control port operations:
// settings.hpp.
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
// Pairing: RequestPairing() (e.g. a key held for 3 s) puts the probe into pairing for up
// to 10 s, see identity.hpp. The settings apply again afterwards; USB commands run
// locally meanwhile.
//
// Context: every member is called in the execution context only (the main loop of a
// single-loop probe). Service() is called once per round.

#include <cstdint>
#include <cstring>

#include "xrdap/control/control.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/radio/forwarding.hpp"
#include "xrdap/radio/host_link.hpp"
#include "xrdap/radio/identity.hpp"
#include "xrdap/radio/pairing.hpp"
#include "xrdap/radio/radio_port.hpp"
#include "xrdap/radio/remote_link.hpp"
#include "xrdap/radio/settings.hpp"
#include "xrdap/session/transport.hpp"

namespace XrDap::Radio
{

/// @brief 无线 DAP 会话 / DAP sessions over the radio.
/// @tparam RadioT 收发器驱动（transceiver.hpp）/ Transceiver driver (transceiver.hpp).
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
  /// @note 初始化时调用；参数的生命周期需长于本对象 / Call at initialisation; the
  ///       arguments must outlive this object.
  RadioLink(RadioT& radio, ExecT& exec, Store& store, const uint8_t* uid)
      : exec_(exec),
        store_(store),
        uid_(uid),
        port_(radio),
        host_(port_, settings_),
        remote_(port_, exec, &RadioLink::AnswerDoneHook, this),
        pairing_(port_, uid)
  {
  }

  RadioLink(const RadioLink&) = delete;
  RadioLink& operator=(const RadioLink&) = delete;

  /// @brief 装上转发钩子并按设置启动 / Install the forwarder hook and start from the
  ///        settings.
  /// @param safe_mode 安全模式：不管设置是什么都不启动无线；读写设置仍交给 store，它可以
  ///        拒绝（控制口应答状态 2）/ Safe mode: the radio stays off whatever the
  ///        settings say; reading and writing them is still up to the store, which may
  ///        refuse (status 2 on the control port).
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
    if (pair_requested_ && !remote_.Answering())
    {
      pair_requested_ = false;
      StartPairing();
    }
    if (port_.Pairing())
    {
      if (pairing_.Serve())
      {
        FinishPairing();
      }
      return true;
    }
    if (!port_.Running())
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
        host_.Serve(now);
        break;
      case Role::REMOTE:
        remote_.Serve();
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
    if (port_.Pairing() && op != Op::kSettings && op != Op::kStats)
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
    if (!port_.Running())
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
  bool RemoteActive() const { return port_.Running() && remote_.Active(); }

  /// 配对状态 / Pairing state.
  PairState Pairing() const { return pair_state_; }

 private:
  // 有线会话这么久没有命令就结束，探针重新服务无线（工具退出时没发 DAP_Disconnect）。
  // A wired session without a command for this long ends, and the probe serves the
  // radio again (a tool that quit without DAP_Disconnect).
  static constexpr uint32_t kLocalSessionIdleUs = 5'000'000;

  static bool IsOwnOp(uint8_t op)
  {
    return op == Op::kRole || op == Op::kStats || op == Op::kSettings ||
           op == Op::kSetRole || op == Op::kSetRate || op == Op::kUnpair ||
           op == Op::kChannel || op == Op::kPair || op == Op::kForward;
  }

  void StartRadio(Rate rate, const Identity& identity)
  {
    remote_.FinishAnswer();
    port_.Start(rate, identity);
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
      StartRadio(settings_.rate, PairIdentity(settings_.link_id));
      port_.SetRunning(true);
    }
    else
    {
      remote_.FinishAnswer();
      port_.SetRunning(false);
    }
    remote_.Reset();
  }

  void ResetSessions()
  {
    session_wireless_ = false;
    session_local_ = false;
    host_.Reset();
  }

  static void AnswerDoneHook(void* ctx) { static_cast<RadioLink*>(ctx)->Apply(); }

  // 改设置的请求要先应答：经无线来的等应答发出后再生效，USB 来的立即生效。
  // A request that changes the settings answers first: one from the radio takes effect
  // once its answer is out, one from USB at once.
  void ApplyAfterAnswer()
  {
    if (remote_.Executing())
    {
      remote_.DeferUntilAnswered();
    }
    else
    {
      Apply();
    }
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
    if (t != Transport::USB || !port_.Running() || port_.Pairing() || len == 0U)
    {
      return false;
    }
    last_usb_us_ = NowUs();
    switch (setting_)
    {
      case RoleSetting::HOST:
        (void)host_.Forward(req, len, resp, cap, out, false);
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
      session_wireless_ = host_.ReachPeer();
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
          (void)host_.Forward(replay_.Data(i), replay_.Size(i), ignored, sizeof(ignored),
                              ignored_len, false);
        }
      }
      else
      {
        host_.Abandon();  // 没人应答：不走无线 / nobody answered
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
    (void)host_.Forward(req, len, resp, cap, out, false);
    if (IsCommand(req, len, DapCmd::DISCONNECT))
    {
      session_wireless_ = false;
    }
    return true;
  }

  void StartPairing()
  {
    remote_.FinishAnswer();
    ResetSessions();
    remote_.Reset();
    pair_state_ = PairState::RUNNING;
    port_.SetPairing(true);
    StartRadio(Rate::kHigh, DefaultIdentity());
    pairing_.Begin();
  }

  void FinishPairing()
  {
    port_.SetPairing(false);
    pair_state_ = PairState::FAILED;
    if (pairing_.Done())
    {
      Settings settings = settings_;
      settings.paired = 1;
      settings.link_id = LinkId(uid_, pairing_.Peer());
      settings.rate = Rate::kHigh;
      std::memcpy(settings.peer_uid, pairing_.Peer(), kUidSize);
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
        answer.rate = static_cast<uint8_t>(settings_.rate);
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
      case Op::kSetRate:
      {
        if (len < 2U || req[1] > static_cast<uint8_t>(Rate::kLow))
        {
          return Reply(resp, op, Status::kBadRequest, cap);
        }
        Settings settings = settings_;
        settings.rate = static_cast<Rate>(req[1]);
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
        if (!port_.Running())
        {
          return Reply(resp, op, Status::kNotPaired, cap);
        }
        uint16_t size = 0;
        const bool answered = host_.Forward(req + 1, static_cast<uint16_t>(len - 1U),
                                            resp, cap, size, true);
        // 探针不是主机角色时，主循环不服务这个会话，也没有 DAP_Disconnect 来结束它：就此
        // 结束，两块探针都回到 F。
        // Outside the host role the main loop does not serve this session and no
        // DAP_Disconnect will end it: end it here, so that both probes return to F.
        if (CurrentRole() != Role::HOST)
        {
          host_.Reset();
        }
        if (!answered)
        {
          return Reply(resp, op, Status::kTimeout, cap);
        }
        return size;
      }
    }
  }

  uint16_t StatsReply(uint8_t* resp)
  {
    RadioT& radio = port_.Radio();
    const auto& host = host_.Counters();
    StatsAnswer answer{};
    answer.op = Op::kStats;
    answer.status = Status::kOk;
    answer.count = 13;
    answer.radio_tx = radio.TxPackets();
    answer.radio_rx = radio.RxPackets();
    answer.crc_errors = radio.RxErrors();
    answer.tx_timeouts = radio.TxErrors();
    answer.commands = host.commands;
    answer.retries = host.retries;
    answer.failures = host.failures;
    answer.duplicates = remote_.Duplicates();
    answer.last_rtt_us = host.last_rtt_us;
    answer.max_rtt_us = host.max_rtt_us;
    answer.channel = port_.Channel();
    answer.rearms = radio.Rearms();
    answer.restarts = radio.Restarts();
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
    answer.channel = port_.Channel();
    answer.lock = host_.LockedChannel();
    std::memcpy(resp, &answer, sizeof(answer));
    return sizeof(answer);
  }

  ExecT& exec_;
  Store& store_;
  const uint8_t* uid_;

  Settings settings_{};
  RoleSetting setting_ = RoleSetting::AUTO;
  bool safe_mode_ = false;
  Role override_role_ = Role::LOCAL;
  bool role_override_pending_ = false;

  RadioPort<RadioT> port_;
  HostLink<RadioT> host_;
  RemoteLink<RadioT, ExecT> remote_;
  PairingAgent<RadioT> pairing_;

  bool session_wireless_ = false;
  bool session_local_ = false;
  uint32_t last_usb_us_ = 0;
  SettingsReplay replay_;

  PairState pair_state_ = PairState::IDLE;
  bool pair_requested_ = false;
};

}  // namespace XrDap::Radio
