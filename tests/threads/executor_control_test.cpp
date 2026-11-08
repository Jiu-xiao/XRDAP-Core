// SPDX-License-Identifier: Apache-2.0
// 控制口（docs/protocol.md 3.7.1）：请求和 DAP 命令一样在执行上下文里一次一条，所以处理
// 函数不需要锁；没有这个操作时回 0xF2。USB 上是两次传输：SEND 收进请求，RECV 取应答，没好
// 时取到 0 字节，应答取走前新的 SEND 被拒绝。
//
// The control port (docs/protocol.md 3.7.1): like a DAP command, a request runs in the
// execution context one at a time, so a handler needs no lock; an operation the probe
// does not have answers 0xF2. Over USB it takes two transfers: SEND takes the request,
// RECV takes the answer and gets 0 bytes until it is ready, and a new SEND is refused
// until the answer is taken.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "fake_dap.hpp"
#include "xrdap/control/usb_control.hpp"

namespace
{
using LibXR::ErrorCode;
using XrDap::Transport;
using namespace XrDapTest;

constexpr uint8_t kStatusOk = 0x00U;
constexpr uint8_t kStatusUnsupported = 0xF2U;
constexpr uint8_t kUsbSend = 0x01U;
constexpr uint8_t kUsbRecv = 0x02U;
constexpr uint8_t kRequest[3] = {0x21U, 0xAAU, 0xBBU};

// 回 op、成功和请求的长度 / Answers the op, success and the request's length.
uint16_t AnswerLength(void*, const uint8_t* req, uint16_t len, uint8_t* resp, uint16_t)
{
  resp[0] = req[0];
  resp[1] = kStatusOk;
  resp[2] = static_cast<uint8_t>(len);
  return 3U;
}

// 取走的应答，拷了出来；没有应答时内容全 0 / The answer taken, copied out; all zeros when
// there is none.
struct UsbAnswer
{
  size_t size = 0U;
  uint8_t bytes[96] = {};
};

struct UsbRig
{
  Probe p;
  XrDap::Control::UsbControl<FakeDap> control{p.dap, p.exec};
  NullTimer timer;

  ErrorCode Send(uint8_t request = kUsbSend)
  {
    return p.dap.port_->OnVendorOut(false, request, {kRequest, sizeof(kRequest)});
  }
  UsbAnswer Recv(uint8_t request = kUsbRecv)
  {
    const LibXR::ConstRawData data = p.dap.port_->OnVendorIn(false, request);
    UsbAnswer answer;
    answer.size = data.size_;
    if (data.size_ != 0U)
    {
      std::memcpy(answer.bytes, data.addr_, data.size_);
    }
    return answer;
  }
  // SEND 的唤醒由 Step 吸收，下一轮才轮到控制请求 / Step absorbs SEND's wake-up; the
  // control request comes on the next round.
  void Handle()
  {
    p.exec.Run(timer, 0U);
    p.exec.Run(timer, 0U);
  }
};
}  // namespace

TEST(ControlWithoutAHandlerIsUnsupported)
{
  Probe p;
  const uint8_t req[3] = {0x42U, 0x01U, 0x02U};
  uint8_t resp[16] = {};
  uint16_t out = 0U;
  p.exec.ControlHere(req, sizeof(req), resp, sizeof(resp), out);
  CHECK_EQ(out, 2U);
  CHECK_EQ(resp[0], 0x42U);
  CHECK_EQ(resp[1], kStatusUnsupported);
}

// 有控制口的探针在 INFO 里报功能位 5（docs/protocol.md 3.4）/ A probe with a control port
// reports feature bit 5 in INFO (docs/protocol.md 3.4).
TEST(AControlHandlerSetsFeatureBit5)
{
  Probe p;
  p.exec.SetControl(&AnswerLength, nullptr);
  const uint8_t info[1] = {0x80U};
  uint8_t resp[64] = {};
  uint16_t out = 0U;
  p.exec.ExecuteHere(Transport::TCP, info, sizeof(info), resp, sizeof(resp), out);
  CHECK_EQ(resp[1], 0x00U);
  CHECK((XrDap::Vendor::Get32(resp + 3) & (1U << 5)) != 0U);
}

// 四条链路同时发请求：处理函数都在执行线程上跑，每条链路拿到自己的应答 / Four links send
// requests at once: the handler always runs on the execution thread, and each link gets
// its own answer.
TEST(ControlRequestsRunOnTheExecutionThread)
{
  struct Notes
  {
    std::thread::id execution_thread;
    std::atomic<uint32_t> off_thread{0U};
    std::atomic<uint32_t> calls{0U};
  } notes;
  Probe p;
  p.exec.SetControl(
      +[](void* ctx, const uint8_t* r, uint16_t, uint8_t* o, uint16_t) -> uint16_t
      {
        auto* n = static_cast<Notes*>(ctx);
        if (std::this_thread::get_id() != n->execution_thread)
        {
          n->off_thread.fetch_add(1U);
        }
        n->calls.fetch_add(1U);
        o[0] = r[0];
        o[1] = kStatusOk;
        o[2] = r[1];
        return 3U;
      },
      &notes);
  ExecThread exec(p);
  notes.execution_thread = exec.Id();
  std::thread links[4];
  const Transport kinds[4] = {Transport::TCP, Transport::SERIAL, Transport::RADIO,
                              Transport::CONSOLE};
  for (uint8_t i = 0U; i < 4U; ++i)
  {
    links[i] = std::thread(
        [&, i]()
        {
          for (int n = 0; n < 100; ++n)
          {
            uint8_t r[16] = {};
            uint16_t o = 0U;
            const uint8_t q[2] = {0x10U, i};
            p.exec.Control(kinds[i], q, sizeof(q), r, sizeof(r), o);
            CHECK_EQ(o, 3U);
            CHECK_EQ(r[0], 0x10U);
            CHECK_EQ(r[1], kStatusOk);
            CHECK_EQ(r[2], i);
          }
        });
  }
  for (auto& link : links)
  {
    link.join();
  }
  CHECK_EQ(notes.calls.load(), 400U);
  CHECK_EQ(notes.off_thread.load(), 0U);
}

TEST(UsbControlAnswerIsEmptyUntilHandled)
{
  UsbRig rig;
  rig.p.exec.SetControl(&AnswerLength, nullptr);
  CHECK_EQ(rig.Send(), ErrorCode::OK);
  CHECK_EQ(rig.Recv().size, 0U);
  rig.Handle();
  const UsbAnswer answer = rig.Recv();
  CHECK_EQ(answer.size, 3U);
  CHECK_EQ(answer.bytes[0], kRequest[0]);
  CHECK_EQ(answer.bytes[1], kStatusOk);
  CHECK_EQ(answer.bytes[2], sizeof(kRequest));
}

TEST(UsbControlRefusesASecondSendUntilTheAnswerIsTaken)
{
  UsbRig rig;
  CHECK_EQ(rig.Send(), ErrorCode::OK);
  CHECK(rig.Send() != ErrorCode::OK);
  rig.Handle();
  CHECK(rig.Send() != ErrorCode::OK);
  CHECK(rig.Recv().size != 0U);
  CHECK_EQ(rig.Send(), ErrorCode::OK);
}

TEST(UsbControlWithoutAHandlerAnswersUnsupported)
{
  UsbRig rig;
  CHECK_EQ(rig.Send(), ErrorCode::OK);
  rig.Handle();
  const UsbAnswer answer = rig.Recv();
  CHECK_EQ(answer.size, 2U);
  CHECK_EQ(answer.bytes[0], kRequest[0]);
  CHECK_EQ(answer.bytes[1], kStatusUnsupported);
}

TEST(UsbControlAnswerIsTakenOnce)
{
  UsbRig rig;
  CHECK_EQ(rig.Send(), ErrorCode::OK);
  rig.Handle();
  CHECK(rig.Recv().size != 0U);
  CHECK_EQ(rig.Recv().size, 0U);
}

TEST(UsbControlRefusesOtherRequests)
{
  UsbRig rig;
  CHECK(rig.Send(9U) != ErrorCode::OK);
  CHECK_EQ(rig.Send(), ErrorCode::OK);
  rig.Handle();
  CHECK_EQ(rig.Recv(9U).size, 0U);
  CHECK(rig.Recv().size != 0U);
}

TEST(UsbControlSendWakesTheExecutionContext)
{
  UsbRig rig;
  rig.p.exec.SetControl(&AnswerLength, nullptr);
  ExecThread exec(rig.p);
  CHECK(WaitFor([&]() { return rig.p.dap.waits.load() != 0U; }));
  CHECK_EQ(rig.Send(), ErrorCode::OK);
  CHECK(WaitFor([&]() { return rig.Recv().size != 0U; }));
}
