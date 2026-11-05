// SPDX-License-Identifier: Apache-2.0
#pragma once

// 数 USB 总线复位的 libxr USB 设备类。libxr 把总线复位（DeviceEvent::RESET）报给设备上的
// 每个类，所以任何一个类都能数；DapVendor::SetUsbResetSource 读这个计数，计数变了而连接
// 在 USB 上时，连接和采样作废。用法：把端口原来用的 USB 类包一层，
//   using UsbDap = XrDap::ResetCountingUsb<LibXR::USB::DapLinkV2Class<Swd, ...>>;
//   vendor.SetUsbResetSource(&dap.BusResets());
//
// A libxr USB device class that counts USB bus resets. libxr reports a bus reset
// (DeviceEvent::RESET) to every class on the device, so any one of them can count;
// DapVendor::SetUsbResetSource reads the count, and when it changes while the connection
// is on USB, the connection and sampling lapse. Use: wrap the USB class the port already
// uses,
//   using UsbDap = XrDap::ResetCountingUsb<LibXR::USB::DapLinkV2Class<Swd, ...>>;
//   vendor.SetUsbResetSource(&dap.BusResets());

#include <atomic>
#include <cstdint>

#include "device_events.hpp"

namespace XrDap
{

/// @brief 数总线复位的 USB 设备类 / A USB device class that counts bus resets.
/// @tparam Base libxr 的 USB 设备类（有虚函数 OnDeviceEvent），构造函数原样继承 / A libxr
///         USB device class (with the virtual OnDeviceEvent); its constructors are
///         inherited as they are.
template <typename Base>
class ResetCountingUsb : public Base
{
 public:
  using Base::Base;

  /// @brief 总线复位的次数 / Number of bus resets.
  /// @note 任何任务和中断可读 / Readable from any task or interrupt.
  const std::atomic<uint32_t>& BusResets() const { return resets_; }

  /// @brief 数一次复位，再交给 Base / Count a reset, then pass the event to Base.
  /// @note libxr 在 USB 的 owner 中调用（中断或应用线程）/ libxr calls it in the USB
  ///       owner (an interrupt or an application thread).
  void OnDeviceEvent(bool in_isr, LibXR::USB::DeviceEvent event,
                     uint8_t endpoint = 0) override
  {
    if (event == LibXR::USB::DeviceEvent::RESET)
    {
      resets_.fetch_add(1U, std::memory_order_relaxed);
    }
    Base::OnDeviceEvent(in_isr, event, endpoint);
  }

 private:
  std::atomic<uint32_t> resets_{0U};
};

}  // namespace XrDap
