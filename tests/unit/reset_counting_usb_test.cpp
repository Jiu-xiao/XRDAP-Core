// SPDX-License-Identifier: Apache-2.0
// 数总线复位的 USB 类。判据：libxr 把总线复位以 DeviceEvent::RESET 报给设备上的每个类，
// 别的事件不是复位 / The USB class that counts bus resets. Oracle: libxr reports a bus
// reset to every class on the device as DeviceEvent::RESET, and other events are not
// resets.
#include "xrdap/vendor/reset_counting_usb.hpp"

#include "check.hpp"

namespace
{
using LibXR::USB::DeviceEvent;

// 代替 libxr 的设备类：有带参数的构造函数和虚函数 OnDeviceEvent / Stand-in for a libxr
// device class: a constructor with an argument and the virtual OnDeviceEvent.
class FakeClass
{
 public:
  explicit FakeClass(int tag) : tag(tag) {}
  virtual ~FakeClass() = default;
  virtual void OnDeviceEvent(bool in_isr, DeviceEvent event, uint8_t endpoint = 0)
  {
    ++seen;
    last_isr = in_isr;
    last_event = event;
    last_endpoint = endpoint;
  }
  int tag;
  int seen = 0;
  bool last_isr = false;
  DeviceEvent last_event = DeviceEvent::RESET;
  uint8_t last_endpoint = 0;
};
}  // namespace

TEST(ResetCountingUsbCountsOnlyResets)
{
  XrDap::ResetCountingUsb<FakeClass> usb(7);
  CHECK_EQ(usb.tag, 7);
  CHECK_EQ(usb.BusResets().load(), 0U);
  usb.OnDeviceEvent(true, DeviceEvent::RESET, 0);
  usb.OnDeviceEvent(false, DeviceEvent::ENDPOINT_RESUMED, 2);
  usb.OnDeviceEvent(true, DeviceEvent::RESET, 0);
  CHECK_EQ(usb.BusResets().load(), 2U);
}

TEST(ResetCountingUsbPassesEveryEventOn)
{
  // libxr 经基类指针调用；事件原样交给 Base / libxr calls through the base pointer; the
  // event reaches Base unchanged.
  XrDap::ResetCountingUsb<FakeClass> usb(1);
  FakeClass& as_base = usb;
  as_base.OnDeviceEvent(true, DeviceEvent::ENDPOINT_RESUMED, 3);
  CHECK_EQ(usb.seen, 1);
  CHECK(usb.last_isr);
  CHECK(usb.last_event == DeviceEvent::ENDPOINT_RESUMED);
  CHECK_EQ(usb.last_endpoint, 3U);
  CHECK_EQ(usb.BusResets().load(), 0U);
  as_base.OnDeviceEvent(false, DeviceEvent::RESET);
  CHECK_EQ(usb.seen, 2);
  CHECK(usb.last_event == DeviceEvent::RESET);
  CHECK_EQ(usb.BusResets().load(), 1U);
}
