# XRDAP-Core

[English](README.en.md)

XR-DAP 系列 CMSIS-DAP 探针共用的固件代码，基于 [libxr](https://github.com/xrobot-org/libxr)。它包含 XR 厂商扩展（一次读写多个地址、沿指针链读变量、在探针上按固定周期采样）、同一时间只让一条传输驱动探针的会话主人和命令执行器、串口 DAP 链路、全系列统一的状态灯规则、探针身份和家族常量。各探针固件以 git 子模块引用本仓库：

| 固件 | 芯片 | 采样的运行方式 |
|---|---|---|
| [XRDAP-ESP](https://github.com/Jiu-xiao/XRDAP-ESP) | ESP32-S3、ESP32-S2、ESP32-C3、ESP32 | 采样任务，gptimer 计节拍 |
| [XRDAP-F103C8](https://github.com/Jiu-xiao/XRDAP-F103C8) | STM32F103（STLINK-V2 板） | 主循环轮询，没有 RTOS |
| XRDAP-CH32V307 | CH32V307、CH32V305 | 采样任务，TIM7 计节拍 |

电脑端工具在 [XRDAP-Host](https://github.com/Jiu-xiao/XRDAP-Host)。探针与电脑之间的协议见 [docs/protocol.md](docs/protocol.md)：第 1 节是串口帧，第 3 节是厂商扩展（协议版本 2）和会话主人，第 4 节是探针身份。

## 内容

头文件按 `xrdap/<模块>/<文件>.hpp` 引用：

| 模块 | 内容 |
|---|---|
| `session/` | `SessionOwner`：哪条传输（USB、串口、TCP、无线）拥有探针，会话什么时候结束；`Executor`：所有进入 libxr DAP 实例的调用都经过它，不是主人的传输回“忙” |
| `vendor/` | `DapVendor`（命令 0x80 到 0x85，以及平台挂上的一条附加命令，例如 XRDAP-ESP 的 0x86 Wi-Fi 配置）、`MemAp`（经 SWD 沿指针链访问 MEM-AP）、`Sampler`、`TickAccount`、`FrameRing`（探针端采样）、请求检查和回复空间 |
| `serial/` | `Framer`（串口帧的拼接和打包）、`SerialLink`（串口收发线程） |
| `activity/` | `DapActivity`：DAP 活动和 DAP_HostStatus 报告的上位机状态 |
| `light/` | `StatusLight`：由连接、设置和 DAP 活动算出每一刻的灯色和亮度 |
| `probe/` | `SetProbeInfo`、`FormatSerial`（DAP_Info 字符串和序列号规则）、`family.hpp`（VID:PID 和各板的 bcdDevice）、`chip_uid.hpp`（STM32F1/CH32V3 的芯片唯一标识） |
| `protocol/` | CMSIS-DAP 命令号和状态值 |
| `swd/` | `F1Gpio`：STM32F1 和 CH32V3 共用的寄存器级 SWD 引脚 |

除 `swd/F1Gpio` 直接读写 STM32F1/CH32V3 的 GPIO 寄存器外，本仓库不碰外设。时间、互斥量和信号量用 libxr 的 `Timebase`、`Mutex`、`Semaphore`；libxr 没有统一抽象的部分由平台提供：

| 平台提供 | 说明 |
|---|---|
| `xrdap_port.hpp` | 采样帧缓冲大小 `Port::kRingBytes` 和它的自旋锁 `Port::SpinLock` |
| 节拍定时器 | 实现 `Vendor::TickTimer`，中断里调用 `DapVendor::OnTickIsr()`；它会读 libxr 的 `Timebase`，时基须能在中断里读 |
| 线程 | 采样任务、USB 的 DAP 线程、串口线程（ESP 要固定到 SWD 所在的核） |
| 会话的结束 | USB 复位计数、TCP 断开、无线会话号和 ping |
| 灯 | 驱动、引脚和 USB 是否已配置的查询 |

## 接入

需要 libxr 1c06b874（`feat/xrdap-r2-swd-loop`）或之后：`Executor` 用它的 `SetCommandHook(hook, ctx, length)` 把厂商命令的长度交给 libxr，`DAP_ExecuteCommands` 里才能放厂商命令；`StepUsb` 用它的 `WaitForWork` 在不占执行轮次时等 USB 命令；它的 `SetSessionResetGate` 让 USB 的连接和断开只在 USB 拥有探针或没有主人时复位 SWD。CI 用 `.github/workflows/ci.yml` 里固定的 libxr 提交（`LIBXR_COMMIT`，不早于上面的版本）编译和测试；CI 只在电脑上测试，不交叉编译各芯片的固件，那由各固件仓库负责。

固件工程 include `xrdap_core.cmake`，把源文件加进自己的目标，并保证这些源文件能找到 libxr 的头文件和平台的 `xrdap_port.hpp`：

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrdap-core/xrdap_core.cmake)
target_sources(app.elf PRIVATE ${XRDAP_CORE_SOURCES})
target_include_directories(app.elf PRIVATE ${XRDAP_CORE_INCLUDE_DIRS} port)
```

ESP-IDF 工程把 `XRDAP_CORE_SOURCES`、`XRDAP_CORE_INCLUDE_DIRS` 交给 `idf_component_register`。

`xrdap_port.hpp` 给出采样帧缓冲的大小（2 的幂，INFO 会报告）和它的锁。采样和 SUB_READ 都会短暂持有这把锁（最多拷贝 1 KB），它不能阻塞：会阻塞的互斥量让优先级最高的采样任务在 SUB_READ 拷贝时睡下，短周期下节拍会迟到。双核的 ESP32 用 portMUX，单核 RTOS 用临界区，单循环的探针用空操作：

```cpp
// xrdap_port.hpp（单核 FreeRTOS）
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

## 用法

### 会话主人和执行器

所有 DAP 命令都经 `Executor` 进入 libxr 的 DAP 实例：USB 用 `StepUsb()`，别的传输用 `Execute()`。执行器每次只执行一条命令，并记下命令属于哪条传输。命令按 `session/command_class.hpp` 的一张表分两类：只读的状态和设置查询、状态灯（`DAP_Info`、`DAP_HostStatus`、厂商 INFO、0x86 查询、0x9F 0x13）不需要拥有探针，任何传输都照常执行；其余命令要拥有探针。第一条执行要拥有探针的命令的传输成为会话主人；会话结束前别的传输的这类命令回“忙”（标准命令回 `命令号 FF`，厂商命令回 `命令号 05`）：

```cpp
#include "xrdap/session/executor.hpp"

static XrDap::SessionOwner session;
static XrDap::DapVendor vendor(swd, session);
static XrDap::DapActivity activity;
static XrDap::Executor<Dap> exec(dap, session, vendor, activity);  // 装好命令钩子

session.SetLease(XrDap::Transport::SERIAL, XrDap::kSerialLeaseMs);  // 串口 5 s 无请求即结束
session.SetUsbResetSource(&BusResetCount, &usb);  // 复位计数变了，USB 的会话结束
```

`BusResetCount` 代表平台读取 USB 总线复位次数的函数。各传输按自己的方式结束会话：

```cpp
// TCP 服务：每个请求
exec.Execute(XrDap::Transport::TCP, req, len, resp, sizeof(resp), out);
// 客户端断开
exec.EndSession(XrDap::Transport::TCP);

// 无线远端：执行对端发来的命令；收到会话号不变的 ping 时刷新租期，
// 会话号变了或为 0 时结束会话
exec.Execute(XrDap::Transport::RADIO, req, len, resp, sizeof(resp), out);
exec.TouchSession(XrDap::Transport::RADIO);
exec.EndSession(XrDap::Transport::RADIO);
```

会话结束后采样停止，扩展命令要求重新 `DAP_Connect`。

### 单循环探针

没有 RTOS 的探针在主循环里交替执行 USB 命令和采样。节拍定时器只在中断里数节拍，`Poll()` 看到新节拍就采一拍：

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
  (void)exec.StepUsb(vendor.Sampling() ? 0 : 1);  // 采样时不在 Step 里等命令
}
```

`HwTimerStart`、`HwTimerStop`、`HwTimerClear` 代表平台自己的定时器代码：按 1 µs 计数，每 `period_us` 中断一次。

### 有 RTOS 的探针

采样任务在信号量上阻塞，节拍中断和停止都会唤醒它；它应为 SWD 所在核上优先级最高的任务。USB 的 DAP 线程循环调用 `StepUsb()`：

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

`StepUsb(wait_ms)` 先在 libxr 的 `WaitForWork` 里等 USB 命令，等待时不占执行轮次；有了命令才排队执行。执行轮次只在执行一条命令时持有，按到达先后交接（`TurnQueue`），持有者不能抢在已经排队的传输前面，所以任何传输的 `DAP_Info` 和“忙”回复都不取决于别的传输什么时候空闲。需要 libxr 提供 `WaitForWork`。

### 串口链路

```cpp
#include "xrdap/serial/serial_link.hpp"

static XrDap::SerialLink<XrDap::Executor<Dap>, 1024> link(exec, uart);
link.Start();  // 用 LibXR::Thread 建收发线程
```

要把执行 DAP 的线程固定到某个核时，在 `Start()` 之前用 `SetThreadStarter()` 提供启动器。没有 RTOS 时不建线程，在非中断上下文里直接调用 `link.Feed(data, len)`，返回执行的请求数。

### 身份和家族常量

```cpp
#include "xrdap/probe/chip_uid.hpp"
#include "xrdap/probe/family.hpp"
#include "xrdap/probe/probe_identity.hpp"

static char serial[XrDap::SerialBufferSize(XrDap::kF1ChipUidBytes)];
XrDap::FormatSerial(XrDap::F1ChipUid(), XrDap::kF1ChipUidBytes, serial);
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

`tests/` 在电脑上编译不依赖硬件和 libxr 的纯逻辑（会话主人、命令归类、执行轮次、忙回复、回复空间、请求长度、SUB_START 检查、节拍账目、采样给命令让路、按 CPU 模拟的采样与命令、帧缓冲、条目解析、串口帧和补位、DAP 活动、序列号），用 ctest 运行，带 AddressSanitizer 和 UBSan；判据来自协议文档、CMSIS-DAP 规范和用户的决定。另一个 ctest 检查 `xrdap/` 里的原子变量只用 32 位整数。给出 libxr 的路径时，另外把全部源文件和公开头文件按 libxr 的 Linux 系统层编译一遍，并运行两个用 libxr 的测试：多线程下的执行器（等输入时不占轮次、忙回复、转发的断开、USB 重新连接不动别人的 SWD、串口写的结果归类）和 MEM-AP 的写入与总线错误：

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
