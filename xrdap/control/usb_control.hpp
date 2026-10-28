// SPDX-License-Identifier: Apache-2.0
#pragma once

// USB 上的控制口：EP0 的厂商控制传输，发给 DAP 接口。不增加接口，不占端点。
//   SEND  bmRequestType 0x41（OUT，厂商，接口）bRequest 0x01
//         数据 = 控制请求（op + 载荷）
//   RECV  bmRequestType 0xC1（IN，厂商，接口）bRequest 0x02
//         数据 = 控制应答（op + 状态 + 载荷）
// 请求和应答是两次传输：SEND 在 USB 上下文里把请求收进缓冲，叫醒执行上下文；执行上下文
// 在 Executor::Run 里处理（和 DAP 命令一样一次一条）；主机再发 RECV 取应答。应答还没好时
// RECV 返回 0 字节，主机稍后重试；一次只有一个请求在途，应答被取走前新的 SEND 会被拒绝
// （STALL）以免丢掉应答，所以主机每次 SEND 之后要 RECV。
//
// The probe's control port over USB: vendor control transfers on EP0, addressed to the
// DAP interface. No extra interface and no endpoint.
//   SEND  bmRequestType 0x41 (OUT, vendor, interface), bRequest 0x01, data = the control
//         request (op + payload)
//   RECV  bmRequestType 0xC1 (IN, vendor, interface), bRequest 0x02, data = the control
//         answer (op + status + payload)
// Request and answer are two transfers: SEND copies the request into a buffer in the USB
// context and wakes the execution context, which handles it in Executor::Run (one at a
// time like a DAP command); the host then sends RECV for the answer. While the answer is
// not ready RECV returns 0 bytes and the host retries later. One request is in flight; a
// new SEND is refused (STALL) until the answer has been taken, so an answer is not lost:
// the host sends a RECV after every SEND.

#include <atomic>
#include <cstdint>
#include <cstring>

#include "libxr_def.hpp"
#include "xrdap/session/executor.hpp"

namespace XrDap::Control
{

inline constexpr uint8_t kUsbSend = 0x01U;
inline constexpr uint8_t kUsbRecv = 0x02U;

/// @tparam DapT libxr DAP 类，提供 VendorPort、SetVendorPort 和 WakeStep / libxr DAP
///         class providing VendorPort, SetVendorPort and WakeStep.
/// @tparam Cap 请求和应答缓冲各自的字节数 / Bytes of each of the request and answer
///         buffers.
template <typename DapT, uint16_t Cap = 96>
class UsbControl final : public DapT::VendorPort
{
 public:
  /// @note 初始化时构造，生命周期需长于 dap 和 exec / Construct at initialisation; must
  ///       outlive dap and exec.
  UsbControl(DapT& dap, Executor<DapT>& exec) : dap_(dap), exec_(exec)
  {
    dap_.SetVendorPort(this);
    exec_.SetExtraWork(&UsbControl::Work, this);
  }

  LibXR::ErrorCode OnVendorOut(bool in_isr, uint8_t request,
                               LibXR::ConstRawData data) override
  {
    if (request != kUsbSend || data.size_ > Cap || data.size_ == 0U ||
        state_.load(std::memory_order_acquire) != kIdle)
    {
      return LibXR::ErrorCode::FAILED;
    }
    std::memcpy(req_, data.addr_, data.size_);
    req_len_ = static_cast<uint16_t>(data.size_);
    state_.store(kPending, std::memory_order_release);
    dap_.WakeStep(in_isr);
    return LibXR::ErrorCode::OK;
  }

  LibXR::ConstRawData OnVendorIn(bool, uint8_t request) override
  {
    if (request != kUsbRecv || state_.load(std::memory_order_acquire) != kReady)
    {
      return {nullptr, 0U};
    }
    // 应答缓冲在被取走之前不会被改：状态到 kReady 之后只有这里会把它改回 kIdle，而下一个
    // SEND 要等 kIdle。IN 数据阶段发完之前缓冲不动，所以这里直接把状态放回 kIdle 之前先把
    // 数据拷到发送缓冲 / The answer buffer does not change until it has been taken: once
    // in kReady only this function sets kIdle, and the next SEND waits for kIdle. The
    // data is copied to the send buffer before the state goes back to kIdle.
    std::memcpy(send_, resp_, resp_len_);
    const uint16_t len = resp_len_;
    state_.store(kIdle, std::memory_order_release);
    return {send_, len};
  }

 private:
  enum : uint32_t
  {
    kIdle = 0,
    kPending = 1,
    kReady = 2,
  };

  /// 执行上下文里调用：处理一个待处理的请求 / Called in the execution context: handle a
  /// pending request.
  static bool Work(void* ctx)
  {
    auto* self = static_cast<UsbControl*>(ctx);
    if (self->state_.load(std::memory_order_acquire) != kPending)
    {
      return false;
    }
    uint16_t out = 0U;
    self->exec_.ControlHere(self->req_, self->req_len_, self->resp_, Cap, out);
    self->resp_len_ = out;
    self->state_.store(kReady, std::memory_order_release);
    return true;
  }

  DapT& dap_;
  Executor<DapT>& exec_;
  std::atomic<uint32_t> state_{kIdle};
  uint8_t req_[Cap] = {};
  uint8_t resp_[Cap] = {};
  uint8_t send_[Cap] = {};
  uint16_t req_len_ = 0U;
  uint16_t resp_len_ = 0U;
};

}  // namespace XrDap::Control
