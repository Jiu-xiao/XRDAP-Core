// SPDX-License-Identifier: Apache-2.0
#include "dap_vendor.hpp"

#include <cstring>

#include "timebase.hpp"

namespace XrDap
{
using namespace Vendor;

namespace
{
// 标准 DAP 命令里需要跟踪的几个 / Standard DAP commands this class tracks.
constexpr uint8_t kDapInfo = 0x00U;
constexpr uint8_t kDapHostStatus = 0x01U;
constexpr uint8_t kDapConnect = 0x02U;
constexpr uint8_t kDapDisconnect = 0x03U;
// 命令不支持时的回复 / Reply to an unsupported command.
constexpr uint8_t kDapInvalid = 0xFFU;

// 回复的最小长度：INFO 26 字节，SUB_SET 8 字节，SUB_READ 头 24 字节 / Minimum reply
// sizes: INFO 26 bytes, SUB_SET 8 bytes, SUB_READ header 24 bytes.
constexpr uint16_t kInfoReplyLen = 26U;
constexpr uint16_t kSubSetReplyLen = 8U;
constexpr uint16_t kSubReadHeaderLen = 24U;

enum class SubCtrlOp : uint8_t
{
  STOP = 0,
  START = 1,
  CLEAR = 2,
};

uint32_t NowMs() { return static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds()); }
uint64_t NowUs() { return static_cast<uint64_t>(LibXR::Timebase::GetMicroseconds()); }

uint8_t ToU8(Cmd c) { return static_cast<uint8_t>(c); }
}  // namespace

void DapVendor::SetExtraCommand(uint8_t cmd, uint32_t feature, ExtraHandler handler,
                                void* ctx)
{
  extra_cmd_ = cmd;
  extra_feature_ = feature;
  extra_handler_ = handler;
  extra_ctx_ = ctx;
}

void DapVendor::NotifySerialActivity()
{
  last_serial_ms_.store(NowMs(), std::memory_order_release);
}

void DapVendor::OnTransportClosed()
{
  sampler_.Stop();
  DropConnection();
}

// 连接状态回到初始：扩展命令要求重新 DAP_Connect，SWD 的 SELECT 缓存作废。
void DapVendor::DropConnection()
{
  connected_ = false;
  owner_ = Transport::USB;
  transport_ = Transport::USB;
  mem_.ForgetSelect();
}

void DapVendor::OnNetworkClientConnected()
{
  OnTransportClosed();
  transport_ = Transport::TCP;
}

void DapVendor::SetUsbResetSource(ResetCountFn fn, void* ctx)
{
  usb_reset_fn_ = fn;
  usb_reset_ctx_ = ctx;
}

void DapVendor::Observe(const uint8_t* req, uint16_t len)
{
  if (req != nullptr && len >= 1U)
  {
    activity_.Observe(req, len, NowMs(), transport_.load() != Transport::TCP);
  }
}

bool DapVendor::Hook(void* ctx, const uint8_t* req, uint16_t len, uint8_t* resp,
                     uint16_t cap, uint16_t& out)
{
  auto* self = static_cast<DapVendor*>(ctx);
  self->Observe(req, len);
  return self->OnCommand(req, len, resp, cap, out);
}

// 每拍末尾调用：发起采样的传输已经离开时返回 false，采样随即停止。
bool DapVendor::KeepSampling(void* ctx)
{
  auto* self = static_cast<DapVendor*>(ctx);
  const Transport owner = self->owner_;
  const bool usb_gone =
      owner == Transport::USB && self->usb_reset_fn_ != nullptr &&
      self->usb_reset_fn_(self->usb_reset_ctx_) != self->usb_reset_base_;
  const bool serial_gone =
      owner == Transport::SERIAL &&
      NowMs() - self->last_serial_ms_.load(std::memory_order_acquire) > kSerialLeaseMs;
  if (usb_gone || serial_gone)
  {
    self->DropConnection();
    return false;
  }
  return true;
}

bool DapVendor::OnCommand(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                          uint16_t& out)
{
  if (req == nullptr || resp == nullptr || len < 1U || cap < 1U)
  {
    return false;
  }
  const uint8_t cmd = req[0];

  if (sampler_.Active())
  {
    // 采样期间探针被订阅独占：只放行 DAP_Info、DAP_HostStatus 和厂商命令。
    if (cmd == kDapInfo || cmd == kDapHostStatus)
    {
      return false;
    }
    if (!IsVendor(cmd))
    {
      resp[0] = kDapInvalid;
      out = 1U;
      return true;
    }
    return HandleVendor(cmd, req, len, resp, cap, out);
  }

  if (IsVendor(cmd))
  {
    return HandleVendor(cmd, req, len, resp, cap, out);
  }

  // 跟踪 DAP_Connect / DAP_Disconnect，扩展命令要求先连接到 SWD 目标。
  if (cmd == kDapConnect)
  {
    const uint8_t port = (len >= 2U) ? req[1] : 0U;
    connected_ = (port == 0U || port == 1U);
  }
  else if (cmd == kDapDisconnect)
  {
    connected_ = false;
    mem_.ForgetSelect();
  }
  return false;
}

bool DapVendor::IsVendor(uint8_t cmd) const
{
  return (cmd >= kFirstCmd && cmd <= kLastCmd) ||
         (extra_handler_ != nullptr && cmd == extra_cmd_);
}

bool DapVendor::HandleVendor(uint8_t cmd, const uint8_t* req, uint16_t len, uint8_t* resp,
                             uint16_t cap, uint16_t& out)
{
  if (extra_handler_ != nullptr && cmd == extra_cmd_)
  {
    return extra_handler_(extra_ctx_, req, len, resp, cap, out);
  }
  switch (static_cast<Cmd>(cmd))
  {
    case Cmd::INFO:
      return CmdInfo(len, resp, cap, out);
    case Cmd::READ:
      return CmdRead(req, len, resp, cap, out);
    case Cmd::WRITE:
      return CmdWrite(req, len, resp, cap, out);
    case Cmd::SUB_SET:
      return CmdSubSet(req, len, resp, cap, out);
    case Cmd::SUB_CTRL:
      return CmdSubCtrl(req, len, resp, out);
    case Cmd::SUB_READ:
      return CmdSubRead(req, len, resp, cap, out);
  }
  return false;
}

bool DapVendor::CmdInfo(uint16_t len, uint8_t* resp, uint16_t cap, uint16_t& out)
{
  if (cap < kInfoReplyLen)
  {
    out = 0U;
    return true;
  }
  resp[0] = ToU8(Cmd::INFO);
  if (len != 1U)
  {
    return Reply(resp, kStatusArg, out);
  }
  resp[1] = kStatusOk;
  resp[2] = kVersion;
  Put32(&resp[3], kFeatures | extra_feature_);
  Put16(&resp[7], kMaxItems);
  resp[9] = kMaxDepth;
  Put32(&resp[10], kRingBytes);
  Put64(&resp[14], NowUs());
  Put32(&resp[22], sampler_.AccessUs());
  out = kInfoReplyLen;
  return true;
}

bool DapVendor::CmdRead(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                        uint16_t& out)
{
  resp[0] = ToU8(Cmd::READ);
  if (len < 2U || req[1] == 0U)
  {
    return Reply(resp, kStatusArg, out);
  }
  const uint8_t count = req[1];
  if (count > kMaxItems)
  {
    return Reply(resp, kStatusResource, out);
  }
  const uint8_t* const first = req + 2;
  const uint8_t* const end = req + len;
  const uint8_t* p = first;
  uint32_t resp_size = 3U;
  for (uint8_t i = 0; i < count; ++i)
  {
    Item it;
    if (!ParseItem(p, end, it, nullptr) || !SizeValid(it.size))
    {
      return Reply(resp, kStatusArg, out);
    }
    resp_size += 3U + it.size;
  }
  if (p != end)
  {
    return Reply(resp, kStatusArg, out);
  }
  if (resp_size > cap || resp_size > max_packet_)
  {
    return Reply(resp, kStatusResource, out);
  }
  if (!connected_)
  {
    return Reply(resp, kStatusNoTarget, out);
  }
  MemAp::Lock lock(mem_);
  mem_.ResetCache();
  resp[1] = kStatusOk;
  resp[2] = count;
  uint8_t* q = resp + 3;
  p = first;
  for (uint8_t i = 0; i < count; ++i)
  {
    Item it;
    (void)ParseItem(p, end, it, nullptr);
    uint8_t value[8] = {};
    const uint8_t st = mem_.Read(it, value);
    Put16(q, it.id);
    q[2] = st;
    std::memcpy(q + 3, value, it.size);
    q += 3 + it.size;
  }
  out = static_cast<uint16_t>(q - resp);
  return true;
}

bool DapVendor::CmdWrite(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                         uint16_t& out)
{
  resp[0] = ToU8(Cmd::WRITE);
  if (len < 2U || req[1] == 0U)
  {
    return Reply(resp, kStatusArg, out);
  }
  const uint8_t count = req[1];
  if (count > kMaxItems)
  {
    return Reply(resp, kStatusResource, out);
  }
  // 每个条目后面紧跟 size 字节的值 / Each item is followed by its size-byte value.
  const uint8_t* const first = req + 2;
  const uint8_t* const end = req + len;
  const uint8_t* p = first;
  for (uint8_t i = 0; i < count; ++i)
  {
    Item it;
    if (!ParseItem(p, end, it, nullptr) || !SizeValid(it.size) ||
        static_cast<size_t>(end - p) < it.size)
    {
      return Reply(resp, kStatusArg, out);
    }
    p += it.size;
  }
  if (p != end)
  {
    return Reply(resp, kStatusArg, out);
  }
  if (3U + 3U * count > cap || 3U + 3U * count > max_packet_)
  {
    return Reply(resp, kStatusResource, out);
  }
  if (!connected_)
  {
    return Reply(resp, kStatusNoTarget, out);
  }
  MemAp::Lock lock(mem_);
  mem_.ResetCache();
  resp[1] = kStatusOk;
  resp[2] = count;
  uint8_t* q = resp + 3;
  p = first;
  for (uint8_t i = 0; i < count; ++i)
  {
    Item it;
    (void)ParseItem(p, end, it, nullptr);
    const uint8_t st = mem_.Write(it, p);
    p += it.size;
    Put16(q, it.id);
    q[2] = st;
    q += 3;
  }
  out = static_cast<uint16_t>(q - resp);
  return true;
}

bool DapVendor::CmdSubSet(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                          uint16_t& out)
{
  if (cap < kSubSetReplyLen)
  {
    out = 0U;
    return true;
  }
  resp[0] = ToU8(Cmd::SUB_SET);
  if (sampler_.Active())
  {
    return Reply(resp, kStatusState, out);
  }
  if (len < 8U)
  {
    return Reply(resp, kStatusArg, out);
  }
  const uint32_t period = Get32(&req[1]);
  const uint8_t flags = req[5];
  const uint16_t count = Get16(&req[6]);
  if (period == 0U || (flags & 0xFEU) != 0U || count == 0U)
  {
    return Reply(resp, kStatusArg, out);
  }
  if (count > kMaxItems)
  {
    return Reply(resp, kStatusResource, out);
  }
  const uint8_t* p = req + 8;
  const uint8_t* end = req + len;
  uint8_t aps_seen[kMaxAps];
  uint8_t ap_count = 0U;
  for (uint16_t i = 0; i < count; ++i)
  {
    Item it;
    SubItem sub;
    if (!ParseItem(p, end, it, &sub) || !SizeValid(it.size) || sub.div == 0U ||
        sub.phase >= sub.div)
    {
      return Reply(resp, kStatusArg, out);
    }
    bool found = false;
    for (uint8_t j = 0; j < ap_count; ++j)
    {
      found = found || (aps_seen[j] == it.ap);
    }
    if (!found)
    {
      if (ap_count >= kMaxAps)
      {
        return Reply(resp, kStatusResource, out);
      }
      aps_seen[ap_count++] = it.ap;
    }
  }
  if (p != end)
  {
    return Reply(resp, kStatusArg, out);
  }
  if (!connected_)
  {
    return Reply(resp, kStatusNoTarget, out);
  }

  Sampler::Estimate est;
  const uint8_t st = sampler_.Configure(SubItemList{req + 8, end, count}, period,
                                        (flags & 1U) != 0U, est);
  if (st == kStatusNoTarget)
  {
    return Reply(resp, kStatusNoTarget, out);
  }
  resp[1] = st;
  Put16(&resp[2], est.max_frame);
  Put32(&resp[4], est.frame_us);
  out = kSubSetReplyLen;
  return true;
}

bool DapVendor::CmdSubCtrl(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out)
{
  resp[0] = ToU8(Cmd::SUB_CTRL);
  if (len != 2U)
  {
    return Reply(resp, kStatusArg, out);
  }
  switch (static_cast<SubCtrlOp>(req[1]))
  {
    case SubCtrlOp::STOP:
      sampler_.Stop();
      return Reply(resp, kStatusOk, out);
    case SubCtrlOp::START:
      if (sampler_.Active() || !sampler_.Configured())
      {
        return Reply(resp, kStatusState, out);
      }
      if (!connected_ || !StartSampling())
      {
        return Reply(resp, kStatusNoTarget, out);
      }
      return Reply(resp, kStatusOk, out);
    case SubCtrlOp::CLEAR:
      sampler_.ClearFrames();
      return Reply(resp, kStatusOk, out);
  }
  return Reply(resp, kStatusArg, out);
}

// 记下发起采样的传输和它的存活依据，再开始采样。
bool DapVendor::StartSampling()
{
  owner_ = transport_.load();
  last_serial_ms_.store(NowMs(), std::memory_order_release);
  usb_reset_base_ = (usb_reset_fn_ != nullptr) ? usb_reset_fn_(usb_reset_ctx_) : 0U;
  return sampler_.Start();
}

bool DapVendor::CmdSubRead(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t cap,
                           uint16_t& out)
{
  if (cap < kSubReadHeaderLen)
  {
    out = 0U;
    return true;
  }
  resp[0] = ToU8(Cmd::SUB_READ);
  if (len != 3U)
  {
    return Reply(resp, kStatusArg, out);
  }
  if (!sampler_.Configured())
  {
    return Reply(resp, kStatusState, out);
  }
  const uint32_t want = Get16(req + 1);
  const uint32_t room = cap - kSubReadHeaderLen;
  const FrameRing::Taken t =
      sampler_.TakeFrames(resp + kSubReadHeaderLen, (want < room) ? want : room);

  resp[1] = kStatusOk;
  Put64(&resp[2], NowUs());
  Put32(&resp[10], t.remaining);
  Put32(&resp[14], t.dropped);
  Put32(&resp[18], t.skipped);
  Put16(&resp[22], t.frames);
  out = static_cast<uint16_t>(kSubReadHeaderLen + t.bytes);
  return true;
}

}  // namespace XrDap
