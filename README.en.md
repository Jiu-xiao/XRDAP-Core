# XRDAP-Core

[中文](README.md)

Firmware code shared by the XR-DAP family of CMSIS-DAP probes, built on [libxr](https://github.com/xrobot-org/libxr). It holds the XR vendor extension (multi-address read/write, pointer-chain reads, periodic sampling on the probe), the status light rule of the whole family and the probe identity. Each probe firmware uses this repository as a git submodule:

| Firmware | Chip | How sampling runs |
|---|---|---|
| [XRDAP-ESP](https://github.com/Jiu-xiao/XRDAP-ESP) | ESP32-S3, ESP32-C3, ESP32 | sampling task, woken by a gptimer |
| [XRDAP-F103C8](https://github.com/Jiu-xiao/XRDAP-F103C8) | STM32F103 (STLINK-V2 board) | polled from the main loop, no RTOS |
| XRDAP-CH32V307 | CH32V307 | sampling task, woken by TIM7 |

The host tool is [XRDAP-Host](https://github.com/Jiu-xiao/XRDAP-Host). The protocol between probe and host is in [docs/protocol.en.md](docs/protocol.en.md): section 3 is the vendor extension, section 4 the probe identity.

## Contents

| Directory | Contents |
|---|---|
| `vendor/` | `DapVendor` (commands 0x80 to 0x85, attached to the command hook of the libxr DAP class), `MemAp` (MEM-AP access along pointer chains over SWD), `Sampler` and `FrameRing` (sampling on the probe), `DapActivity` (DAP activity and the host state reported by DAP_HostStatus) |
| `light/` | `StatusLight`: computes the colour and level of the light at each moment from the link, the settings and DAP activity |
| `probe/` | `SetProbeInfo`, `FormatSerial`: DAP_Info strings and the serial rule |
| `docs/` | protocol document |
| `tests/` | test specification of the whole family ([SPEC.en.md](tests/SPEC.en.md)) and the test tool `xrdap_test` ([usage](tests/README.en.md)) |

This repository does not touch hardware. Time, mutexes and semaphores come from libxr's `Timebase`, `Mutex` and `Semaphore`, which all three platforms have; what libxr has no common abstraction for is supplied by the platform: creating the sampling task (ESP pins it to the core that drives SWD), the tick timer (`Vendor::TickTimer`), the LED driver and pins, and the query whether USB is configured.

## Integration

A firmware project includes `xrdap_core.cmake`, adds the sources to its own target and makes the libxr headers visible to them:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrdap-core/xrdap_core.cmake)
target_sources(app.elf PRIVATE ${XRDAP_CORE_SOURCES})
target_include_directories(app.elf PRIVATE ${XRDAP_CORE_INCLUDE_DIRS})
```

An ESP-IDF project passes `XRDAP_CORE_SOURCES` and `XRDAP_CORE_INCLUDE_DIRS` to `idf_component_register`. The sampling ring buffer is 4 KB by default; platforms with more RAM raise it with a compile definition, and INFO reports the actual size:

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE XRDAP_RING_BYTES=16384)
```

## Usage

### Identity

The serial is the prefix `XRobot-XRDAP-` followed by the chip's unique id in upper-case hex, identical to the USB device serial, so host tools recognise one probe across USB, serial and TCP:

```cpp
#include "probe_identity.hpp"

static char serial[XrDap::SerialBufferSize(12)];
XrDap::FormatSerial(chip_uid, 12, serial);  // XRobot-XRDAP-0123456789ABCDEF01234567
XrDap::SetProbeInfo(dap, {"XR-DAP-F103", "STLINK-V2", serial, "0.6.0"});
```

### Single-loop probe

A probe without an RTOS handles DAP commands in its main loop and calls `Poll()` between two commands. After every tick control returns to the main loop, which handles one command, so sampling does not starve DAP commands. When USB goes away it calls `OnTransportClosed()`: sampling stops and the extension commands need a new `DAP_Connect`:

```cpp
#include "dap_vendor.hpp"

static XrDap::DapVendor vendor(swd, kDapPacketSize);
dap.SetCommandHook(&XrDap::DapVendor::Hook, &vendor);

bool usb_up = false;
for (;;)
{
  const bool up = dap.IsInited();
  if (usb_up && !up)
  {
    vendor.OnTransportClosed();
  }
  usb_up = up;
  vendor.Poll();
  (void)dap.Step(vendor.Sampling() ? 0 : 1);  // no waiting in Step while sampling
}
```

### Probe with an RTOS

The platform implements `Vendor::TickTimer`: `Start()` makes a hardware timer interrupt once per period from `start_us` on and call `WakeSamplerFromIsr()` in the interrupt; when the timer cannot do it (for example the period exceeds its counter) it returns `false` and the sampling task wakes every 1 ms instead. `Start()` and `Stop()` are both called from the sampling task, so the timer interrupt is allocated on the sampling core. The sampling task should have the highest priority on the core that drives SWD; it blocks between ticks and other tasks run in the gaps:

```cpp
#include "dap_vendor.hpp"

class TimerTick final : public XrDap::Vendor::TickTimer
{
 public:
  explicit TimerTick(XrDap::DapVendor& vendor) : vendor_(vendor) {}

  bool Start(uint64_t start_us, uint32_t period_us) override
  {
    // count at 1 MHz, reload at period_us, counter starts at (now - start_us) % period_us
    return HwTimerStart(start_us, period_us, &TimerTick::OnIrq, this);
  }
  void Stop() override { HwTimerStop(); }

 private:
  static void OnIrq(void* ctx) { static_cast<TimerTick*>(ctx)->vendor_.WakeSamplerFromIsr(); }
  XrDap::DapVendor& vendor_;
};

static XrDap::DapVendor vendor(swd, kDapPacketSize);
static TimerTick tick(vendor);
static LibXR::Thread sampler;
sampler.Create<XrDap::DapVendor*>(
    &vendor, [](XrDap::DapVendor* v) { v->RunSamplerTask(&tick); }, "dap_sampler", 3072,
    LibXR::Thread::Priority::REALTIME);
vendor.SetUsbResetSource(&BusResetCount, &usb);  // USB-started sampling stops on a bus reset
dap.SetCommandHook(&XrDap::DapVendor::Hook, &vendor);
```

`HwTimerStart`, `HwTimerStop` and `BusResetCount` stand for the platform's own timer and USB code. A probe with several transports calls `SetTransport()` before executing DAP commands; the serial link calls `NotifySerialActivity()` on every request, and serial-started sampling stops after 5 s without one; a network server calls `OnNetworkClientConnected()` when a client connects and `OnTransportClosed()` when it leaves. DAP commands that do not go through `Hook()` (for example those forwarded to a radio peer) are counted as DAP activity with `Observe()`.

### Status light

`StatusLight` does not touch hardware: the platform decides the main link and the setting state at its own refresh rate, calls `Update()` and draws the resulting `Frame` on its LEDs:

| State | Light |
|---|---|
| Power-up | one rainbow sweep |
| Wired (USB, serial) | red |
| Wireless (Wi-Fi, radio) | blue |
| Nothing connected | purple; breathing while joining |
| Setting being changed / applied, restart pending | fast purple blink / solid purple |
| Debugger connected (DAP_HostStatus) | main colour and purple back and forth every 2 s |
| Target running (DAP_HostStatus) | red and blue back and forth every second |
| Every DAP command | one activity pulse, about 15 per second under steady traffic |

The host state is cleared by `DAP_Disconnect` or 5 s without commands. `Frame::level` is in permille of the LED's peak and includes the activity pulse; an on/off-only LED goes dark while `Frame::pulse` is above 500:

```cpp
#include "status_light.hpp"

XrDap::Light::StatusLight rule(NowMs());
for (;;)
{
  const uint32_t now = NowMs();
  const auto link = usb_configured() ? XrDap::Light::Link::WIRED : XrDap::Light::Link::NONE;
  const XrDap::Light::Frame f =
      rule.Update(link, XrDap::Light::Setting::NONE, vendor.Activity().Read(now), now);
  const uint32_t level = kPeak * f.level / 1000;  // kPeak: the LED's peak level, 0 to 255
  SetLed(f.color.r * level / 255, f.color.g * level / 255, f.color.b * level / 255);
  Sleep(20);
}
```

## Code style

C++17, clang-format 21.1.8 with libxr's configuration (`.clang-format`). Public headers carry bilingual Doxygen comments.

## License

Apache-2.0
