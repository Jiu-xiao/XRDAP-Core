// SPDX-License-Identifier: Apache-2.0
#include "xrdap/vendor/dap_vendor.hpp"

#include <cstring>

#include "timebase.hpp"
#include "xrdap/protocol/dap_cmd.hpp"
#include "xrdap/vendor/reply_space.hpp"
#include "xrdap/vendor/request_length.hpp"
#include "xrdap/vendor/sub_start.hpp"

namespace XrDap
{
using namespace Vendor;

namespace
{
enum class SubCtrlOp : uint8_t
{
  STOP = 0,
  CLEAR = 2,
};

uint32_t NowMs() { return static_cast<uint32_t>(LibXR::Timebase::GetMilliseconds()); }
uint64_t NowUs() { return static_cast<uint64_t>(LibXR::Timebase::GetMicroseconds()); }

uint8_t ToU8(Cmd c) { return static_cast<uint8_t>(c); }
}  // namespace

void DapVendorBase::SetExtraCommand(uint8_t cmd, uint32_t feature, ExtraHandler handler,
                                    void* ctx, ExtraLength length)
{
  extra_length_ = length;
  extra_cmd_ = cmd;
  extra_feature_ = feature;
  extra_handler_ = handler;
  extra_ctx_ = ctx;
}

// 发起采样的会话结束时返回 false，采样随即停止。采样任务或主循环调用。
// Returns false once the session that started sampling has ended, which stops sampling.
// Called by the sampling task or the main loop.
template <class Policy>
bool DapVendorT<Policy>::KeepSampling(void* ctx, uint32_t session)
{
  return static_cast<DapVendorT*>(ctx)->session_.Alive(session, NowMs());
}

// 扩展命令要求本会话里已经 DAP_Connect。 / Extension commands need a DAP_Connect in this
// session.
bool DapVendorBase::Connected()
{
  return connected_session_ != 0U && session_.Alive(connected_session_, NowMs());
}

template <class Policy>
DapVendorBase::Result DapVendorT<Policy>::OnCommand(const uint8_t* req, uint16_t len,
                                                    uint8_t* resp, uint16_t cap,
                                                    uint16_t& out)
{
  if (req == nullptr || resp == nullptr || len < 1U || cap < 1U)
  {
    return Result::PASS;
  }
  const uint8_t cmd = req[0];

  // SUB_READ, both INFO commands and HOST_STATUS only consume metadata/ring data. They
  // should not repeatedly park the SWD sampler while a host drains a high-rate stream.
  if (cmd == ToU8(Cmd::SUB_READ) || cmd == ToU8(Cmd::INFO) || cmd == Byte(DapCmd::INFO) ||
      cmd == Byte(DapCmd::HOST_STATUS))
  {
    sampler_.CommandReadOnly();
  }

  if (sampler_.Active())
  {
    // 采样期间探针被订阅独占：只放行 DAP_Info、DAP_HostStatus 和厂商命令。
    // While sampling the probe belongs to the subscription: only DAP_Info, DAP_HostStatus
    // and vendor commands pass.
    if (cmd == Byte(DapCmd::INFO) || cmd == Byte(DapCmd::HOST_STATUS))
    {
      return Result::PASS;
    }
    if (!IsVendor(cmd))
    {
      resp[0] = Byte(DapCmd::INVALID);
      out = 1U;
      return Result::REFUSED;
    }
    return HandleVendor(cmd, req, len, resp, cap, out) ? Result::HANDLED : Result::PASS;
  }

  if (IsVendor(cmd))
  {
    return HandleVendor(cmd, req, len, resp, cap, out) ? Result::HANDLED : Result::PASS;
  }

  // 跟踪 DAP_Connect / DAP_Disconnect：扩展命令要求先连接到 SWD 目标。
  // Track DAP_Connect / DAP_Disconnect: extension commands need an SWD target connected.
  if (cmd == Byte(DapCmd::CONNECT))
  {
    const uint8_t port = (len >= 2U) ? req[1] : 0U;
    connected_session_ = (port == 0U || port == 1U) ? session_.Session() : 0U;
  }
  else if (cmd == Byte(DapCmd::DISCONNECT))
  {
    connected_session_ = 0U;
  }
  return Result::PASS;
}

bool DapVendorBase::RequestLength(const uint8_t* req, uint16_t avail, uint16_t& len) const
{
  len = 0U;
  if (req == nullptr || avail < 1U)
  {
    return false;
  }
  const uint8_t cmd = req[0];
  if (extra_handler_ != nullptr && cmd == extra_cmd_)
  {
    return extra_length_ != nullptr && extra_length_(req, avail, len);
  }
  return cmd >= kFirstCmd && cmd <= kLastCmd && Vendor::RequestLength(req, avail, len);
}

bool DapVendorBase::IsVendor(uint8_t cmd) const
{
  return (cmd >= kFirstCmd && cmd <= kLastCmd) ||
         (extra_handler_ != nullptr && cmd == extra_cmd_);
}

template <class Policy>
bool DapVendorT<Policy>::HandleVendor(uint8_t cmd, const uint8_t* req, uint16_t len,
                                      uint8_t* resp, uint16_t cap, uint16_t& out)
{
  // 回复空间一处检查：附加命令只保证至少 2 字节，自己的回复长度由它自己检查。
  // One reply space check: the extra command is only guaranteed 2 bytes and checks the
  // length of its own reply.
  if (extra_handler_ != nullptr && cmd == extra_cmd_)
  {
    if (!ReserveReply(cmd, 2U, resp, cap, out))
    {
      return true;
    }
    return extra_handler_(extra_ctx_, req, len, resp, cap, out);
  }
  if (!ReserveReply(cmd, RequiredReply(req, len), resp, cap, out))
  {
    return true;
  }
  switch (static_cast<Cmd>(cmd))
  {
    case Cmd::INFO:
      return CmdInfo(len, resp, out);
    case Cmd::READ:
      return CmdRead(req, len, resp, out, sampler_.Active());
    case Cmd::WRITE:
      return CmdWrite(req, len, resp, out);
    case Cmd::SUB_START:
      return CmdSubStart(req, len, resp, out);
    case Cmd::SUB_CTRL:
      return CmdSubCtrl(req, len, resp, out);
    case Cmd::SUB_READ:
      return CmdSubRead(req, len, resp, cap, out);
  }
  return false;
}

bool DapVendorBase::CmdInfo(uint16_t len, uint8_t* resp, uint16_t& out)
{
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
  out = kInfoReplyLen;
  return true;
}

bool DapVendorBase::CmdRead(const uint8_t* req, uint16_t len, uint8_t* resp,
                            uint16_t& out, bool sampling)
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
  for (uint8_t i = 0; i < count; ++i)
  {
    Item it;
    if (!ParseItem(p, end, it, nullptr) || !SizeValid(it.size))
    {
      return Reply(resp, kStatusArg, out);
    }
  }
  if (p != end)
  {
    return Reply(resp, kStatusArg, out);
  }
  if (!Connected())
  {
    return Reply(resp, kStatusNoTarget, out);
  }
  MemAp::Lock lock(mem_);
  // Sampling owns the link: only our sampler and vendor commands can change CSW.
  // Reuse its stable base bits, but always reprogram size/increment before reading.
  mem_.ResetCache(sampling);
  resp[1] = kStatusOk;
  resp[2] = count;
  uint8_t* q = resp + 3;
  // 第二遍解析：第一遍已校验过同一段字节，这里一定成功 / Second pass: the first pass
  // validated the same bytes, so this always succeeds.
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

bool DapVendorBase::CmdWrite(const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t& out)
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
  if (!Connected())
  {
    return Reply(resp, kStatusNoTarget, out);
  }
  MemAp::Lock lock(mem_);
  mem_.ResetCache();
  resp[1] = kStatusOk;
  resp[2] = count;
  uint8_t* q = resp + 3;
  // 第二遍解析：第一遍已校验过同一段字节，这里一定成功 / Second pass: the first pass
  // validated the same bytes, so this always succeeds.
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

template <class Policy>
bool DapVendorT<Policy>::CmdSubStart(const uint8_t* req, uint16_t len, uint8_t* resp,
                                     uint16_t& out)
{
  resp[0] = ToU8(Cmd::SUB_START);
  if (sampler_.Active())
  {
    return Reply(resp, kStatusState, out);
  }
  SubStartRequest start;
  const uint8_t st = ParseSubStart(req, len, start);
  if (st != kStatusOk)
  {
    return Reply(resp, st, out);
  }
  if (!Connected())
  {
    return Reply(resp, kStatusNoTarget, out);
  }
  // 采样归发起它的会话 / Sampling belongs to the session that starts it.
  return Reply(resp,
               sampler_.Start(start.items, start.period_us, start.stop_on_full,
                              session_.Session()),
               out);
}

template <class Policy>
bool DapVendorT<Policy>::CmdSubCtrl(const uint8_t* req, uint16_t len, uint8_t* resp,
                                    uint16_t& out)
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
    case SubCtrlOp::CLEAR:
      sampler_.ClearFrames();
      return Reply(resp, kStatusOk, out);
  }
  return Reply(resp, kStatusArg, out);
}

template <class Policy>
bool DapVendorT<Policy>::CmdSubRead(const uint8_t* req, uint16_t len, uint8_t* resp,
                                    uint16_t cap, uint16_t& out)
{
  resp[0] = ToU8(Cmd::SUB_READ);
  if (len != 3U)
  {
    return Reply(resp, kStatusArg, out);
  }
  const uint32_t want = Get16(req + 1);
  const uint32_t room = cap - kSubReadHeaderLen;
  const FrameRing::Taken t =
      sampler_.TakeFrames(resp + kSubReadHeaderLen, (want < room) ? want : room);

  // 采样已停时状态为 0x02，帧照常回 / Status is 0x02 once sampling stopped; frames
  // still go out.
  resp[1] = sampler_.Active() ? kStatusOk : kStatusState;
  Put64(&resp[2], NowUs());
  Put32(&resp[10], t.remaining);
  Put32(&resp[14], t.dropped);
  Put32(&resp[18], t.skipped);
  Put16(&resp[22], t.frames);
  out = static_cast<uint16_t>(kSubReadHeaderLen + t.bytes);
  return true;
}

// 只为平台选定的策略生成代码 / Code is generated only for the policy the platform chose.
template class DapVendorT<Port::SamplerPolicy>;

}  // namespace XrDap
