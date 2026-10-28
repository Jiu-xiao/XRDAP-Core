# XRDAP-Core

[English](README.en.md)

XR-DAP 系列 CMSIS-DAP 探针共用的固件代码，基于 [libxr](https://github.com/xrobot-org/libxr)。它包含 XR 厂商扩展（一次读写多个地址、沿指针链读变量、在探针上按固定周期采样）、唯一的执行上下文（命令执行器）、与 DAP 分开的控制口、串口 DAP 链路、全系列统一的状态灯规则、探针身份和家族常量。各探针固件以 git 子模块引用本仓库：

| 固件 | 芯片 | 采样的运行方式 |
|---|---|---|
| [XRDAP-ESP](https://github.com/Jiu-xiao/XRDAP-ESP) | ESP32-S3、ESP32-S2、ESP32-C3、ESP32 | 一个执行任务跑 `Run()`，gptimer 计节拍 |
| [XRDAP-F103C8](https://github.com/Jiu-xiao/XRDAP-F103C8) | STM32F103（STLINK-V2 板） | 主循环跑 `Run()`，没有 RTOS |
| XRDAP-CH32V307 | CH32V307、CH32V305 | 主循环跑 `Run()`，没有 RTOS，TIM7 计节拍 |

电脑端工具在 [XRDAP-Host](https://github.com/Jiu-xiao/XRDAP-Host)。探针与电脑之间的协议见 [docs/protocol.md](docs/protocol.md)：第 1 节是串口帧，第 3 节是厂商扩展（协议版本 2）、一次一条链路的规则和控制口，第 4 节是探针身份。

## 内容

头文件按 `xrdap/<模块>/<文件>.hpp` 引用：

| 模块 | 内容 |
|---|---|
| `session/` | `Executor`：所有进入 libxr DAP 实例的调用都经过它，`Run()` 循环把 USB 命令、别的链路投来的请求和采样一件一件做，没有锁；`Transport` 只是命令来源的标签 |
| `vendor/` | `DapVendor`（命令 0x80 到 0x85，以及平台挂上的一条附加命令，例如 XRDAP-ESP 的 0x86 Wi-Fi 配置）、`MemAp`（经 SWD 沿指针链访问 MEM-AP）、`Sampler`、`TickAccount`、`FrameRing`（探针端采样）、请求检查和回复空间 |
| `serial/` | `Framer`（串口帧：帧头和 CRC 用 libxr Topic 的打包格式，这里只做 payload 和流式重同步）、`SerialLink`（串口收发线程） |
| `activity/` | `DapActivity`：DAP 活动和 DAP_HostStatus 报告的上位机状态 |
| `control/` | 控制口：设置类请求（Wi-Fi、射频）与 DAP 无关；`op + 载荷` 的请求、`op + 状态 + 载荷` 的应答，由 `Executor::Control` 交给执行上下文一次处理一条 |
| `light/` | `StatusLight`：由连接、设置和 DAP 活动算出每一刻的灯色和亮度 |
| `probe/` | `SetProbeInfo`、`FormatSerial`（DAP_Info 字符串和序列号规则）、`family.hpp`（VID:PID 和各板的 bcdDevice） |
| `protocol/` | `dap_cmd.hpp`：取 libxr 的 CMSIS-DAP 命令号和状态值的别名，不再重复定义 |

本仓库不碰外设。时间和信号量用 libxr 的 `Timebase`、`Semaphore`；libxr 没有统一抽象的部分由平台提供：

| 平台提供 | 说明 |
|---|---|
| `xrdap_port.hpp` | 采样帧缓冲大小 `Port::kRingBytes` |
| 节拍定时器 | 实现 `Vendor::TickTimer`，中断里调用 `DapVendor::OnTickIsr()`；中断只数节拍并在该采时叫醒执行上下文，不读时基 |
| 线程 | 执行上下文（单循环的探针就是主循环；ESP 是钉在 SWD 所在核上的一个任务）、串口和 TCP 的链路任务 |
| 会话的结束 | USB 复位计数、TCP 断开、无线会话号和 ping |
| 灯 | 驱动、引脚和 USB 是否已配置的查询 |

## 接入

需要带 `WakeStep` 的 libxr（候选分支 `release/0.6-candidate`）：`Executor` 用它的 `SetCommandHook(hook, ctx, length)` 把厂商命令的长度交给 libxr，`DAP_ExecuteCommands` 里才能放厂商命令；`Run` 用它的 `WaitForWork` 和 `WakeStep` 在同一个地方等 USB 命令、别的链路的投递和节拍中断；它的 `SetSessionResetGate` 让 USB 的连接和断开只在 USB 拥有探针或没有主人时复位 SWD。CI 用 `.github/workflows/ci.yml` 里固定的 libxr 提交（`LIBXR_COMMIT`，不早于上面的版本）编译和测试；CI 只在电脑上测试，不交叉编译各芯片的固件，那由各固件仓库负责。

固件工程 include `xrdap_core.cmake`，把源文件加进自己的目标，并保证这些源文件能找到 libxr 的头文件和平台的 `xrdap_port.hpp`：

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrdap-core/xrdap_core.cmake)
target_sources(app.elf PRIVATE ${XRDAP_CORE_SOURCES})
target_include_directories(app.elf PRIVATE ${XRDAP_CORE_INCLUDE_DIRS} port)
```

ESP-IDF 工程把 `XRDAP_CORE_SOURCES`、`XRDAP_CORE_INCLUDE_DIRS` 交给 `idf_component_register`。

`xrdap_port.hpp` 只给出采样帧缓冲的大小（2 的幂，INFO 会报告）：

```cpp
// xrdap_port.hpp
#pragma once
#include <cstdint>

namespace XrDap::Port
{
inline constexpr uint32_t kRingBytes = 4096;
}  // namespace XrDap::Port
```

## 用法

### 执行上下文

探针上只有一个执行上下文：所有进入 libxr DAP 实例的调用（USB 的 `Step`、别的链路投来的 `ExecuteExternal`）和采样都在 `Executor::Run()` 这个循环里一件一件做，SWD、DAP 类和厂商扩展因此只在一个线程里访问，没有锁。探针一次只使用一条链路由使用者保证，Core 不仲裁：没有会话主人、没有排队、不回“忙”。`DAP_Connect` 记下它来自哪条链路，`DAP_Disconnect`、这条链路丢了、或连接在 USB 上时总线复位，连接和采样就作废，扩展命令要求重新 `DAP_Connect`。

一轮的顺序：

1. 有 USB 命令就 `Step(0)` 执行一条，否则邮箱里有别的链路投来的请求就执行一条；
2. 采样的节拍到了就采**一帧**，采完回头看有没有命令；
3. 什么都没做就在 `WaitForWork` 里等：USB 命令、别的链路的投递、节拍中断（`WakeStep`）都会叫醒。

命令因此最多被采样占一帧，采不过来的节拍计为跳拍，探针始终回应命令：

```cpp
#include "xrdap/session/executor.hpp"

static XrDap::DapVendor vendor(swd);
static XrDap::DapActivity activity;
static XrDap::Executor<Dap> exec(dap, vendor, activity);  // 装好命令钩子和节拍唤醒

vendor.SetUsbResetSource(&BusResetCount, &usb);  // 复位计数变了，连接在 USB 上时连接作废
```

`BusResetCount` 代表平台读取 USB 总线复位次数的函数。别的链路（串口、TCP、无线）的任务只投递请求并等应答，各链路在自己丢失时告诉执行器：

```cpp
// TCP 服务：每个请求（在链路自己的任务里，不是执行上下文）
exec.Execute(XrDap::Transport::TCP, req, len, resp, sizeof(resp), out);
// 客户端断开
exec.LinkLost(XrDap::Transport::TCP);

// 无线远端：执行对端发来的命令；会话号变了或为 0 时链路丢了
exec.Execute(XrDap::Transport::RADIO, req, len, resp, sizeof(resp), out);
exec.LinkLost(XrDap::Transport::RADIO);
```

每条链路同时只有一条请求在途。串口链路（`SerialLink`）自己记最后一个请求的时间，`kLeaseMs`（5 s）内没有请求就调 `LinkLost(SERIAL)`。连接不在这条链路上时，`LinkLost` 什么也不做。

### 单循环探针

没有 RTOS 的探针，主循环就是执行上下文。节拍定时器的中断数节拍（`OnTickIsr()`），`Run()` 看到该采的节拍就采一帧：

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

`HwTimerStart`、`HwTimerStop`、`HwTimerClear` 代表平台自己的定时器代码：按 1 µs 计数，每 `period_us` 中断一次。

### 有 RTOS 的探针

执行上下文是一个任务，应钉在 SWD 所在的核上；串口、TCP 的链路任务只调 `Execute()`。节拍中断通过 `WakeStep` 叫醒它，所以它不必轮询：

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

`Run(timer, wait_ms)` 在什么都没做时才等，最长 `wait_ms`；中断每次只在节拍到了要采的那一拍时叫醒一次，一帧很长时不会攒下唤醒。需要 libxr 提供 `WaitForWork` 和 `WakeStep`。

### 串口链路

```cpp
#include "xrdap/serial/serial_link.hpp"

static XrDap::SerialLink<XrDap::Executor<Dap>, 1024> link(exec, uart);
link.Start();  // 用 LibXR::Thread 建收发线程
```

要把执行 DAP 的线程固定到某个核时，在 `Start()` 之前用 `SetThreadStarter()` 提供启动器。没有 RTOS 时不建线程，在非中断上下文里直接调用 `link.Feed(data, len)`，返回执行的请求数。

### 身份和家族常量

```cpp
#include "xrdap/probe/family.hpp"
#include "xrdap/probe/probe_identity.hpp"

// uid、uid_bytes：芯片唯一标识，由板级代码读取
static char serial[XrDap::SerialBufferSize(uid_bytes)];
XrDap::FormatSerial(uid, uid_bytes, serial);
XrDap::SetProbeInfo(dap, {"XR-DAP-F103", "STLINK-V2", serial, "0.6.0"});
// USB 设备描述符：XrDap::Family::kUsbVid、kUsbPid、kBcdF103
```

序列号是前缀 `XRobot-XRDAP-` 加芯片唯一标识的大写十六进制，与 USB 设备序列号相同。

### 状态灯

`StatusLight` 不碰硬件：平台按自己的刷新节奏判断主连接和设置状态，调用 `Update()`，再把得到的 `Frame` 画到灯上：

| 状态 | 灯 |
|---|---|
| 上电 | 彩虹扫过一次 |
| 有线（USB、串口） | 红 |
| 无线（Wi-Fi、射频） | 蓝 |
| 都没连上 | 紫；正在连接时紫色呼吸 |
| 正在改设置 / 已改好、即将重启 | 紫色快闪 / 紫色常亮 |
| 调试器已连接（DAP_HostStatus） | 主色与紫色来回，2 s 一次 |
| 目标运行中（DAP_HostStatus） | 红蓝来回，1 s 一次 |
| 每条 DAP 命令 | 一次活动脉冲，命令不断时约 15 次每秒 |

上位机状态由 `DAP_Disconnect` 或 5 s 没有命令清除。`Frame::level` 是相对于灯峰值亮度的千分比，已含活动脉冲；只能亮灭的灯在 `Frame::pulse` 大于 500 时熄灭：

```cpp
#include "xrdap/light/status_light.hpp"

XrDap::Light::StatusLight rule(NowMs());
for (;;)
{
  const uint32_t now = NowMs();
  const auto link = usb_configured() ? XrDap::Light::Link::WIRED : XrDap::Light::Link::NONE;
  const XrDap::Light::Frame f =
      rule.Update(link, XrDap::Light::Setting::NONE, activity.Read(now), now);
  const uint32_t level = kPeak * f.level / 1000;  // kPeak：灯的峰值亮度，0 到 255
  SetLed(f.color.r * level / 255, f.color.g * level / 255, f.color.b * level / 255);
  Sleep(20);
}
```

## 测试

`tests/` 在电脑上编译不依赖硬件和 libxr 的纯逻辑（回复空间、请求长度、SUB_START 检查、节拍账目、帧缓冲、条目解析、串口帧和补位、DAP 活动、序列号），用 ctest 运行，带 AddressSanitizer 和 UBSan；判据来自协议文档、CMSIS-DAP 规范和用户的决定。另一个 ctest 检查 `xrdap/` 里的原子变量只用 32 位整数。给出 libxr 的路径时，另外把全部源文件和公开头文件按 libxr 的 Linux 系统层编译一遍，并运行三个用 libxr 的测试：多线程下的执行器（USB 等命令时别的链路的命令照常执行、采样和命令共用执行上下文、`ExecuteHere`、控制请求和 `Call` 在执行上下文里运行、USB 控制口的两次传输、连接跟着链路、USB 复位只让 USB 上的连接作废、几条链路同时发命令时一次只执行一条、USB 重新连接不动别的链路的 SWD、串口写的结果归类），MEM-AP 的写入与总线错误，以及采样在总线错误后的恢复：

```bash
cmake -S tests -B build -DLIBXR_DIR=<libxr>
cmake --build build
ctest --test-dir build --output-on-failure
```

在真实探针上的测试另有一套：全系列共用的测试规范 [tests/SPEC.md](tests/SPEC.md) 和执行它的工具 `xrdap_test`（[用法](tests/README.md)），经 USB、串口、TCP、USB/IP 或无线板对对任何一块 XR-DAP 探针运行同一组用例。

## 代码风格

C++17，clang-format 21.1.8，配置与 libxr 相同（`.clang-format`）。公开头文件带中英文 Doxygen 注释，并写明可以在哪个执行上下文里调用。原子变量只用 32 位整数（`std::atomic<uint32_t>`）：CH32V307 只有字宽的原子指令，测试 `xrdap_core_word_atomics` 会检查。

## 许可证

Apache-2.0
