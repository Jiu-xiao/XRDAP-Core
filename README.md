# XRDAP-Core

[English](README.en.md)

XR-DAP 系列 CMSIS-DAP 探针共用的固件代码，基于 [libxr](https://github.com/xrobot-org/libxr)。它包含 XR 厂商扩展（一次读写多个地址、沿指针链读变量、在探针上按固定周期采样）、全系列统一的状态灯规则和探针身份。各探针固件以 git 子模块引用本仓库：

| 固件 | 芯片 | 采样的运行方式 |
|---|---|---|
| [XRDAP-ESP](https://github.com/Jiu-xiao/XRDAP-ESP) | ESP32-S3、ESP32-C3、ESP32 | 采样任务，gptimer 唤醒 |
| [XRDAP-F103C8](https://github.com/Jiu-xiao/XRDAP-F103C8) | STM32F103（STLINK-V2 板） | 主循环轮询，没有 RTOS |
| XRDAP-CH32V307 | CH32V307 | 采样任务，TIM7 唤醒 |

电脑端工具在 [XRDAP-Host](https://github.com/Jiu-xiao/XRDAP-Host)。探针与电脑之间的协议见 [docs/protocol.md](docs/protocol.md)：第 3 节是厂商扩展，第 4 节是探针身份。

## 内容

| 目录 | 内容 |
|---|---|
| `vendor/` | `DapVendor`（命令 0x80 到 0x85，接在 libxr DAP 类的命令钩子上）、`MemAp`（经 SWD 沿指针链访问 MEM-AP）、`Sampler` 和 `FrameRing`（探针端采样）、`DapActivity`（DAP 活动和 DAP_HostStatus 报告的上位机状态） |
| `light/` | `StatusLight`：由连接、设置和 DAP 活动算出每一刻的灯色和亮度 |
| `probe/` | `SetProbeInfo`、`FormatSerial`：DAP_Info 字符串和序列号规则 |
| `docs/` | 协议文档 |

本仓库不碰硬件。时间、互斥量和信号量用 libxr 的 `Timebase`、`Mutex`、`Semaphore`，三种平台都有；libxr 没有统一抽象的部分由平台提供：采样任务的创建（ESP 要固定到 SWD 所在的核）、节拍定时器（`Vendor::TickTimer`）、灯的驱动和引脚、USB 是否已配置的查询。

## 接入

固件工程 include `xrdap_core.cmake`，把源文件加进自己的目标，并保证这些源文件能找到 libxr 的头文件：

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/third_party/xrdap-core/xrdap_core.cmake)
target_sources(app.elf PRIVATE ${XRDAP_CORE_SOURCES})
target_include_directories(app.elf PRIVATE ${XRDAP_CORE_INCLUDE_DIRS})
```

ESP-IDF 工程把 `XRDAP_CORE_SOURCES`、`XRDAP_CORE_INCLUDE_DIRS` 交给 `idf_component_register`。采样环形缓冲默认 4 KB，RAM 多的平台用编译定义加大，INFO 会报告实际大小：

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE XRDAP_RING_BYTES=16384)
```

## 用法

### 身份

序列号是前缀 `XRobot-XRDAP-` 加芯片唯一标识的大写十六进制，与 USB 设备序列号相同，上位机靠它在 USB、串口、TCP 上认出同一块探针：

```cpp
#include "probe_identity.hpp"

static char serial[XrDap::SerialBufferSize(12)];
XrDap::FormatSerial(chip_uid, 12, serial);  // XRobot-XRDAP-0123456789ABCDEF01234567
XrDap::SetProbeInfo(dap, {"XR-DAP-F103", "STLINK-V2", serial, "0.6.0"});
```

### 单循环探针

没有 RTOS 的探针在主循环里处理 DAP 命令，并在两条命令之间调用 `Poll()`。一拍读完必定回到主循环处理一条命令，采样不会饿死 DAP 命令。USB 掉线时调用 `OnTransportClosed()`，采样停止，扩展命令要求重新 `DAP_Connect`：

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
  (void)dap.Step(vendor.Sampling() ? 0 : 1);  // 采样时不在 Step 里等命令
}
```

### 有 RTOS 的探针

平台实现 `Vendor::TickTimer`：`Start()` 让一个硬件定时器从 `start_us` 起每个周期中断一次，中断里调用 `WakeSamplerFromIsr()`；定时器做不到（例如周期超出计数范围）时返回 `false`，采样任务改为每 1 ms 醒一次。`Start()` 和 `Stop()` 都在采样任务里调用，定时器中断因此分配在采样所在的核上。采样任务应为 SWD 所在核上优先级最高的任务，它在节拍之间阻塞，别的任务在节拍之间运行：

```cpp
#include "dap_vendor.hpp"

class TimerTick final : public XrDap::Vendor::TickTimer
{
 public:
  explicit TimerTick(XrDap::DapVendor& vendor) : vendor_(vendor) {}

  bool Start(uint64_t start_us, uint32_t period_us) override
  {
    // 按 1 MHz 计数、period_us 重装，计数器从 (now - start_us) % period_us 开始
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
vendor.SetUsbResetSource(&BusResetCount, &usb);  // 总线复位后 USB 发起的采样停止
dap.SetCommandHook(&XrDap::DapVendor::Hook, &vendor);
```

`HwTimerStart`、`HwTimerStop`、`BusResetCount` 代表平台自己的定时器和 USB 代码。有多种传输的探针在执行 DAP 命令前调用 `SetTransport()`；串口每收到一个请求调用 `NotifySerialActivity()`，串口发起的采样在 5 s 没有请求后停止；网络客户端连上时调用 `OnNetworkClientConnected()`，断开时调用 `OnTransportClosed()`。不经 `Hook()` 的 DAP 命令（例如转发到无线对端的命令）用 `Observe()` 记进 DAP 活动。

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
#include "status_light.hpp"

XrDap::Light::StatusLight rule(NowMs());
for (;;)
{
  const uint32_t now = NowMs();
  const auto link = usb_configured() ? XrDap::Light::Link::WIRED : XrDap::Light::Link::NONE;
  const XrDap::Light::Frame f =
      rule.Update(link, XrDap::Light::Setting::NONE, vendor.Activity().Read(now), now);
  const uint32_t level = kPeak * f.level / 1000;  // kPeak：灯的峰值亮度，0 到 255
  SetLed(f.color.r * level / 255, f.color.g * level / 255, f.color.b * level / 255);
  Sleep(20);
}
```

## 代码风格

C++17，clang-format 21.1.8，配置与 libxr 相同（`.clang-format`）。公开头文件带中英文 Doxygen 注释。

## 许可证

Apache-2.0
