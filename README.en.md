# XRDAP-Core

[中文](README.md)

Firmware code shared by the XR-DAP family of CMSIS-DAP probes, built on [libxr](https://github.com/xrobot-org/libxr). It holds the XR vendor extension (multi-address read/write, pointer-chain reads, periodic sampling on the probe), the session owner and command executor that let one transport drive the probe at a time, the serial DAP link, the status light rule of the whole family, the probe identity and the family constants. Each probe firmware uses this repository as a git submodule:

| Firmware | Chip | How sampling runs |
|---|---|---|
| [XRDAP-ESP](https://github.com/Jiu-xiao/XRDAP-ESP) | ESP32-S3, ESP32-C3, ESP32 | sampling task, ticks counted by a gptimer |
| [XRDAP-F103C8](https://github.com/Jiu-xiao/XRDAP-F103C8) | STM32F103 (STLINK-V2 board) | polled from the main loop, no RTOS |
| XRDAP-CH32V307 | CH32V307 | sampling task, ticks counted by TIM7 |

The host tool is [XRDAP-Host](https://github.com/Jiu-xiao/XRDAP-Host). The protocol between probe and host is in [docs/protocol.en.md](docs/protocol.en.md): section 1 is the serial frame, section 3 the vendor extension (protocol version 2) and the session owner, section 4 the probe identity.

## Contents

Headers are included as `xrdap/<module>/<file>.hpp`:

| Module | Contents |
|---|---|
| `session/` | `SessionOwner`: which transport (USB, serial, TCP, radio) owns the probe and when its session ends; `Executor`: every call into the libxr DAP instance goes through it, and a transport that is not the owner gets busy |
| `vendor/` | `DapVendor` (commands 0x80 to 0x85), `MemAp` (MEM-AP access along pointer chains over SWD), `Sampler`, `TickAccount`, `FrameRing` (sampling on the probe), request checks and reply space |
| `serial/` | `Framer` (assembles and packs serial frames), `SerialLink` (the serial worker thread) |
| `activity/` | `DapActivity`: DAP activity and the host state reported by DAP_HostStatus |
| `light/` | `StatusLight`: computes the colour and level of the light at each moment from the link, the settings and DAP activity |
| `probe/` | `SetProbeInfo`, `FormatSerial` (DAP_Info strings and the serial rule), `family.hpp` (VID:PID and each board's bcdDevice), `chip_uid.hpp` (the STM32F1/CH32V3 unique id) |
| `protocol/` | CMSIS-DAP command ids and status values |
| `swd/` | `F1Gpio`: the register-level SWD pin shared by the STM32F1 and the CH32V3 |

This repository touches no peripherals. Time, mutexes and semaphores come from libxr's `Timebase`, `Mutex` and `Semaphore`; what libxr has no common abstraction for is supplied by the platform:

| Platform supplies | Notes |
|---|---|
| `xrdap_port.hpp` | the sample ring size `Port::kRingBytes` and its spinlock `Port::SpinLock` |
| tick timer | implements `Vendor::TickTimer`; its interrupt calls `DapVendor::OnTickIsr()` |
| threads | the sampling task, the USB DAP thread, the serial thread (ESP pins them to the core that drives SWD) |
| session ends | USB reset count, TCP disconnect, radio session id and ping |
| lights | driver, pins and the query whether USB is configured |

## Integration

A firmware project includes `xrdap_core.cmake`, adds the sources to its own target and makes the libxr headers and the platform's `xrdap_port.hpp` visible to them:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrdap-core/xrdap_core.cmake)
target_sources(app.elf PRIVATE ${XRDAP_CORE_SOURCES})
target_include_directories(app.elf PRIVATE ${XRDAP_CORE_INCLUDE_DIRS} port)
```

An ESP-IDF project passes `XRDAP_CORE_SOURCES` and `XRDAP_CORE_INCLUDE_DIRS` to `idf_component_register`.

`xrdap_port.hpp` gives the size of the sample ring (a power of two, reported by INFO) and its lock. Sampling and SUB_READ both hold the lock briefly (at most a 1 KB copy), and it must not block: a blocking mutex would put the highest-priority sampling task to sleep while SUB_READ copies, and ticks would be late at short periods. The dual-core ESP32 parts use a portMUX, a single-core RTOS a critical section, a single-loop probe a no-op:

```cpp
// xrdap_port.hpp (single-core FreeRTOS)
#pragma once
#include <cstdint>

#include "FreeRTOS.h"
#include "task.h"

namespace XrDap::Port
{
inline constexpr uint32_t kRingBytes = 4096;

class SpinLock
{
 public:
  void Lock() { taskENTER_CRITICAL(); }
  void Unlock() { taskEXIT_CRITICAL(); }
};
}  // namespace XrDap::Port
```

## Usage

### Session owner and executor

Every DAP command enters the libxr DAP instance through `Executor`: USB with `StepUsb()`, the other transports with `Execute()`. The executor holds one lock, runs one command at a time and notes which transport each command belongs to. The first transport that runs a command other than `DAP_Info` becomes the session owner; until its session ends the other transports get busy (`command FF` for standard commands, `command 05` for vendor commands), while `DAP_Info` is answered as usual:

```cpp
#include "xrdap/session/executor.hpp"

static XrDap::SessionOwner session;
static XrDap::DapVendor vendor(swd, session);
static XrDap::DapActivity activity;
static XrDap::Executor<Dap> exec(dap, session, vendor, activity);  // installs the hook

session.SetLease(XrDap::Transport::SERIAL, XrDap::kSerialLeaseMs);  // serial: 5 s silence ends it
session.SetUsbResetSource(&BusResetCount, &usb);  // a changed count ends the USB session
```

`BusResetCount` stands for the platform function that returns the USB bus reset count. Each transport ends its session its own way:

```cpp
// TCP server: every request
exec.Execute(XrDap::Transport::TCP, req, len, resp, sizeof(resp), out);
// client disconnected
exec.EndSession(XrDap::Transport::TCP);

// Radio remote: run the commands the peer sends; a ping with an unchanged session id
// refreshes the lease, a changed or zero session id ends the session
exec.Execute(XrDap::Transport::RADIO, req, len, resp, sizeof(resp), out);
exec.TouchSession(XrDap::Transport::RADIO);
exec.EndSession(XrDap::Transport::RADIO);
```

When a session ends, sampling stops and the extension commands need a new `DAP_Connect`.

### Single-loop probe

A probe without an RTOS alternates USB commands and sampling in its main loop. The tick timer only counts ticks in its interrupt, and `Poll()` samples a tick once it sees a new one:

```cpp
#include "xrdap/session/executor.hpp"

class TimTick final : public XrDap::Vendor::TickTimer
{
 public:
  void Start(uint32_t period_us) override { HwTimerStart(period_us); }
  void Stop() override { HwTimerStop(); }
};
static TimTick tick;

extern "C" void TIM2_IRQHandler()
{
  HwTimerClear();
  vendor.OnTickIsr();
}

for (;;)
{
  vendor.Poll(tick);
  (void)exec.StepUsb(vendor.Sampling() ? 0 : 1);  // no waiting in Step while sampling
}
```

`HwTimerStart`, `HwTimerStop` and `HwTimerClear` stand for the platform's own timer code: count at 1 µs and interrupt every `period_us`.

### Probe with an RTOS

The sampling task blocks on a semaphore; the tick interrupt and a stop both wake it, and it should have the highest priority on the core that drives SWD. The USB DAP thread calls `StepUsb()` in a loop:

```cpp
static LibXR::Thread sampler;
sampler.Create<XrDap::DapVendor*>(
    &vendor, [](XrDap::DapVendor* v) { v->RunSamplerTask(tick); }, "dap_sampler", 3072,
    LibXR::Thread::Priority::REALTIME);

static LibXR::Thread usb_dap;
usb_dap.Create<XrDap::Executor<Dap>*>(
    &exec,
    [](XrDap::Executor<Dap>* e)
    {
      for (;;)
      {
        (void)e->StepUsb(1);
      }
    },
    "dap_usb", 4096, LibXR::Thread::Priority::HIGH);
```

`StepUsb(wait_ms)` keeps the executor's lock while it waits for a command, so other transports wait at most `wait_ms` longer.

### Serial link

```cpp
#include "xrdap/serial/serial_link.hpp"

static XrDap::SerialLink<XrDap::Executor<Dap>, 1024> link(exec, uart);
link.Start();  // creates the worker with LibXR::Thread
```

When the thread that runs DAP must be pinned to one core, supply a starter with `SetThreadStarter()` before `Start()`. Without an RTOS no thread is needed: call `link.Feed(data, len)` outside an interrupt; it returns the number of requests it ran.

### Identity and family constants

```cpp
#include "xrdap/probe/chip_uid.hpp"
#include "xrdap/probe/family.hpp"
#include "xrdap/probe/probe_identity.hpp"

static char serial[XrDap::SerialBufferSize(XrDap::kF1ChipUidBytes)];
XrDap::FormatSerial(XrDap::F1ChipUid(), XrDap::kF1ChipUidBytes, serial);
XrDap::SetProbeInfo(dap, {"XR-DAP-F103", "STLINK-V2", serial, "0.6.0"});
// USB device descriptor: XrDap::Family::kUsbVid, kUsbPid, kBcdF103
```

The serial is the prefix `XRobot-XRDAP-` followed by the chip's unique id in upper-case hex, identical to the USB device serial.

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
#include "xrdap/light/status_light.hpp"

XrDap::Light::StatusLight rule(NowMs());
for (;;)
{
  const uint32_t now = NowMs();
  const auto link = usb_configured() ? XrDap::Light::Link::WIRED : XrDap::Light::Link::NONE;
  const XrDap::Light::Frame f =
      rule.Update(link, XrDap::Light::Setting::NONE, activity.Read(now), now);
  const uint32_t level = kPeak * f.level / 1000;  // kPeak: the LED's peak level, 0 to 255
  SetLed(f.color.r * level / 255, f.color.g * level / 255, f.color.b * level / 255);
  Sleep(20);
}
```

## Tests

`tests/` builds the pure logic that depends on neither hardware nor libxr (session owner, busy replies, reply space, SUB_START checks, tick accounting, frame ring, item parsing, serial frames, DAP activity, serial numbers) on a PC and runs it with ctest under AddressSanitizer and UBSan; the oracles come from the protocol document, the CMSIS-DAP specification and the user's decisions. Given a libxr path, every source and public header is also compiled against libxr's Linux system layer:

```bash
cmake -S tests -B build -DLIBXR_DIR=<libxr>
cmake --build build
ctest --test-dir build --output-on-failure
```

## Code style

C++17, clang-format 21.1.8 with libxr's configuration (`.clang-format`). Public headers carry bilingual Doxygen comments and state the execution context they may be called from.

## License

Apache-2.0
