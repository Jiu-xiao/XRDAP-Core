# XRDAP-Core

[中文](README.md)

Firmware code shared by the XR-DAP family of CMSIS-DAP probes, built on [libxr](https://github.com/xrobot-org/libxr). It holds the XR vendor extension (multi-address read/write, pointer-chain reads, periodic sampling on the probe), the one execution context (the command executor), a control port apart from DAP, the serial DAP link, the status light rule of the whole family, the probe identity and the family constants. Each probe firmware uses this repository as a git submodule:

| Firmware | Chip | How sampling runs |
|---|---|---|
| [XRDAP-ESP](https://github.com/Jiu-xiao/XRDAP-ESP) | ESP32-S3, ESP32-S2, ESP32-C3, ESP32 | one execution task runs `Run()`, ticks counted by a gptimer |
| [XRDAP-F103C8](https://github.com/Jiu-xiao/XRDAP-F103C8) | STM32F103 (STLINK-V2 board) | the main loop runs `Run()`, no RTOS |
| XRDAP-CH32V307 | CH32V307, CH32V305 | the main loop runs `Run()`, no RTOS, ticks counted by TIM7 |

The host tool is [XRDAP-Host](https://github.com/Jiu-xiao/XRDAP-Host). The protocol between probe and host is in [docs/protocol.en.md](docs/protocol.en.md): section 1 is the serial frame, section 3 the vendor extension (protocol version 2), the one-link-at-a-time rule and the control port, section 4 the probe identity.

## Contents

Headers are included as `xrdap/<module>/<file>.hpp`:

| Module | Contents |
|---|---|
| `session/` | `Executor`: every call into the libxr DAP instance goes through it, its `Run()` loop does USB commands, requests posted by other links and sampling one after the other, without locks; `Transport` is only a label of where a command came from |
| `vendor/` | `DapVendor` (commands 0x80 to 0x85, plus one extra command a platform attaches, such as XRDAP-ESP's 0x86 Wi-Fi configuration), `MemAp` (MEM-AP access along pointer chains over SWD), `Sampler`, `TickAccount`, `FrameRing` (sampling on the probe), request checks and reply space |
| `serial/` | `Framer` (serial frames: the header and CRCs are libxr Topic's packing format, only the payload and the streaming resync live here), `SerialLink` (the serial worker thread) |
| `activity/` | `DapActivity`: DAP activity and the host state reported by DAP_HostStatus |
| `control/` | the control port: settings requests (Wi-Fi, radio) separate from DAP; a request is `op + payload`, an answer `op + status + payload`, and `Executor::Control` hands it to the execution context one at a time |
| `light/` | `StatusLight`: computes the colour and level of the light at each moment from the link, the settings and DAP activity |
| `probe/` | `SetProbeInfo`, `FormatSerial` (DAP_Info strings and the serial rule), `family.hpp` (VID:PID and each board's bcdDevice) |
| `protocol/` | `dap_cmd.hpp`: aliases for libxr's CMSIS-DAP command ids and status values, defined nowhere else |

This repository touches no peripherals. Time and semaphores come from libxr's `Timebase` and `Semaphore`; what libxr has no common abstraction for is supplied by the platform:

| Platform supplies | Notes |
|---|---|
| `xrdap_port.hpp` | the sample ring size `Port::kRingBytes` |
| tick timer | implements `Vendor::TickTimer`; its interrupt calls `DapVendor::OnTickIsr()`, which only counts ticks and wakes the execution context when one is due; it reads no time base |
| threads | the execution context (the main loop of a single-loop probe; on ESP a task pinned to the core that drives SWD) and the link tasks for serial and TCP |
| session ends | USB reset count, TCP disconnect, radio session id and ping |
| lights | driver, pins and the query whether USB is configured |

## Integration

A libxr with `WakeStep` (candidate branch `release/0.6-candidate`) is required: `Executor` hands the vendor command length to libxr through its `SetCommandHook(hook, ctx, length)`, which lets vendor commands sit inside `DAP_ExecuteCommands`, `Run` waits in its `WaitForWork` and `WakeStep` for a USB command, a post from another link and the tick interrupt in one place, and its `SetSessionResetGate` lets a USB attach or detach reset SWD only when USB owns the probe or nobody does. CI builds and tests against the libxr commit pinned in `.github/workflows/ci.yml` (`LIBXR_COMMIT`, no older than the version above); CI tests on a PC only and does not cross-compile the chip firmwares, which their own repositories do.

A firmware project includes `xrdap_core.cmake`, adds the sources to its own target and makes the libxr headers and the platform's `xrdap_port.hpp` visible to them:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrdap-core/xrdap_core.cmake)
target_sources(app.elf PRIVATE ${XRDAP_CORE_SOURCES})
target_include_directories(app.elf PRIVATE ${XRDAP_CORE_INCLUDE_DIRS} port)
```

An ESP-IDF project passes `XRDAP_CORE_SOURCES` and `XRDAP_CORE_INCLUDE_DIRS` to `idf_component_register`.

`xrdap_port.hpp` gives only the size of the sample ring (a power of two, reported by INFO):

```cpp
// xrdap_port.hpp
#pragma once
#include <cstdint>

namespace XrDap::Port
{
inline constexpr uint32_t kRingBytes = 4096;
}  // namespace XrDap::Port
```

## Usage

### Execution context

The probe has one execution context: every call into the libxr DAP instance (USB's `Step`, `ExecuteExternal` for commands posted by other links) and sampling are done one after the other in the loop `Executor::Run()`, so SWD, the DAP class and the vendor extension are accessed by one thread only, without locks. That the probe is used over one link at a time is up to the user, so Core does not arbitrate: there is no session owner, no queue and no busy reply. `DAP_Connect` notes the link it came from; `DAP_Disconnect`, that link being lost, or (for a connection on USB) a bus reset makes the connection and sampling lapse, and the extension commands need a new `DAP_Connect`.

One round:

1. with a USB command, `Step(0)` runs one; otherwise a request another link posted to the mailbox runs;
2. when a sampling tick is due, sample **one** frame, then look for commands again;
3. when nothing was done, wait in `WaitForWork`: a USB command, a post from another link and the tick interrupt (`WakeStep`) all wake it.

So a command loses at most one frame to sampling, ticks that cannot be sampled are counted as skipped, and the probe always answers commands:

```cpp
#include "xrdap/session/executor.hpp"

static XrDap::DapVendor vendor(swd);
static XrDap::DapActivity activity;
static XrDap::Executor<Dap> exec(dap, vendor, activity);  // installs the command hook and the tick wake

vendor.SetUsbResetSource(&BusResetCount, &usb);  // a changed reset count ends a connection on USB
```

`BusResetCount` stands for the platform's function that reads the USB bus reset count. The tasks of the other links (serial, TCP, radio) only post a request and wait for the reply, and each link tells the executor when it is lost:

```cpp
// TCP server: each request (in the link's own task, not the execution context)
exec.Execute(XrDap::Transport::TCP, req, len, resp, sizeof(resp), out);
// client disconnected
exec.LinkLost(XrDap::Transport::TCP);

// Radio remote: run the command the peer sent; the link is lost when the session id changes or is 0
exec.Execute(XrDap::Transport::RADIO, req, len, resp, sizeof(resp), out);
exec.LinkLost(XrDap::Transport::RADIO);
```

Each link has one request in flight at a time. The serial link (`SerialLink`) notes the time of the last request itself and calls `LinkLost(SERIAL)` when none came within `kLeaseMs` (5 s). `LinkLost` does nothing when the connection is not on that link.

### Single-loop probe

On a probe without an RTOS the main loop is the execution context. The tick timer interrupt counts ticks (`OnTickIsr()`), and `Run()` samples one frame when a tick is due:

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
  exec.Run(tick, 1);
}
```

`HwTimerStart`, `HwTimerStop` and `HwTimerClear` stand for the platform's own timer code: it counts in 1 µs units and interrupts every `period_us`.

### Probe with an RTOS

The execution context is a task, pinned to the core that drives SWD; the serial and TCP link tasks only call `Execute()`. The tick interrupt wakes it through `WakeStep`, so it does not poll:

```cpp
static LibXR::Thread dap_thread;
dap_thread.Create<XrDap::Executor<Dap>*>(
    &exec,
    [](XrDap::Executor<Dap>* e)
    {
      for (;;)
      {
        e->Run(tick, UINT32_MAX);
      }
    },
    "dap", 4096, LibXR::Thread::Priority::HIGH);
```

`Run(timer, wait_ms)` waits only when it did nothing, for `wait_ms` at most; the interrupt wakes once per tick that is due to be sampled, so a long frame does not pile up wakes. This needs libxr's `WaitForWork` and `WakeStep`.

### Serial link

```cpp
#include "xrdap/serial/serial_link.hpp"

static XrDap::SerialLink<XrDap::Executor<Dap>, 1024> link(exec, uart);
link.Start();  // creates the worker with LibXR::Thread
```

When the thread that runs DAP must be pinned to one core, supply a starter with `SetThreadStarter()` before `Start()`. Without an RTOS no thread is needed: call `link.Feed(data, len)` outside an interrupt; it returns the number of requests it ran.

### Identity and family constants

```cpp
#include "xrdap/probe/family.hpp"
#include "xrdap/probe/probe_identity.hpp"

// uid / uid_bytes: the chip unique id, read by the board support code
static char serial[XrDap::SerialBufferSize(uid_bytes)];
XrDap::FormatSerial(uid, uid_bytes, serial);
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

`tests/` builds the pure logic that depends on neither hardware nor libxr (reply space, request lengths, SUB_START checks, tick accounting, frame ring, item parsing, serial frames and padding, DAP activity, serial numbers) on a PC and runs it with ctest under AddressSanitizer and UBSan; the oracles come from the protocol document, the CMSIS-DAP specification and the user's decisions. Another ctest checks that atomics in `xrdap/` are 32-bit integers only. Given a libxr path, every source and public header is also compiled against libxr's Linux system layer, and three tests that use libxr run: the executor with threads (another link's command answered while USB waits, sampling sharing the execution context with commands, `ExecuteHere`, control requests and `Call` running in the execution context, the two transfers of the USB control port, the connection following its link, a USB reset lapsing only a connection on USB, one command at a time when several links send at once, a USB re-attach leaving another link's SWD alone, serial write results), MEM-AP writes and bus faults, and sampling recovering after bus faults:

```bash
cmake -S tests -B build -DLIBXR_DIR=<libxr>
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests on real probes are separate: the test specification of the whole family, [tests/SPEC.en.md](tests/SPEC.en.md), and the tool that runs it, `xrdap_test` ([usage](tests/README.en.md)), which runs the same cases against any XR-DAP probe over USB, serial, TCP, USB/IP or the wireless pair.

## Code style

C++20 (as libxr), clang-format 21.1.8 with libxr's configuration (`.clang-format`). Public headers carry bilingual Doxygen comments and state the execution context they may be called from. Atomics are 32-bit integers only (`std::atomic<uint32_t>`): the CH32V307 has word-sized atomic instructions only, and the `xrdap_core_word_atomics` test checks this.

## License

Apache-2.0
