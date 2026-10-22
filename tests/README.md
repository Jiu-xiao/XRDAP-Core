# XR-DAP 测试工具

[English](README.en.md)

`xrdap_test` 按 [SPEC.md](SPEC.md) 对 XR-DAP 系列的任何一块探针执行测试：同一组用例经 USB、串口、Wi-Fi TCP、USB/IP 或无线板对运行，结果写成 JSON 和 Markdown 表格。工具直接收发 CMSIS-DAP 包（协议见 [docs/protocol.md](../docs/protocol.md)），主机工具类用例调用 OpenOCD、probe-rs、pyOCD。

## 安装

需要 Python 3.11 或更新版本，以及 pyusb、pyserial；Windows 上 pyusb 使用 libusb-package 自带的 libusb：

```bash
python -m pip install pyusb pyserial libusb-package
```

Windows 上探针需要 WinUSB 驱动（XR-DAP 的 MS OS 2.0 描述符会让系统自动安装）；Linux 上需要 udev 规则或用 `sudo` 运行。所有命令都在本目录（`tests/`）下执行。

## 用法

列出用例、目标描述和 USB 上的探针：

```bash
python -m xrdap_test list
```

对一块探针、一条链路、一个目标运行默认用例集（除两项 1 h 用例外的全部）：

```bash
python -m xrdap_test run --link usb:SERIAL --target my-target.toml
```

`--link` 的写法：

| 写法 | 链路 |
|---|---|
| `usb`、`usb:SERIAL` | USB；SERIAL 可以是序列号的末尾几位 |
| `serial:PORT`、`serial:PORT@921600`、`serial:PORT,keep` | 串口帧；默认 4000000 波特；`keep` 不改 DTR/RTS（ESP32-C3 的 USB 口） |
| `tcp:HOST`、`tcp:xrdap-xxxxxx.local:3241` | TCP 帧，默认端口 3241 |
| `usbip:HOST` | USB/IP，默认端口 3240 |

经无线板对测试时，`--link` 指向连在电脑上的那块板，并加 `--via-radio`。

只跑部分用例时用 `--cases`，写用例号或前缀：

```bash
python -m xrdap_test run --link tcp:HOST --target my-target.toml --cases VND-06,VND-08
python -m xrdap_test run --link usb --target my-target.toml --cases STAB-01 --long-s 3600
```

需要第二条链路的用例（MC-02、MC-03）用 `--link2` 给出同一块探针的另一条路径；辅助用例运行到操作者步骤时会提示并等待回车，加 `--no-prompt` 则记为 n/a。

被测镜像的提交号写进结果：

```bash
python -m xrdap_test run --link usb --target my-target.toml --build fw=1a2b3c4,libxr=5d6e7f8,core=9a0b1c2
```

## 结果

每次运行在 `--out`（默认 `xrdap-results/`）下新建一个目录，名字由时间、链路和目标组成，内含 `result.json`、`result.md`、`log.txt` 和主机工具的日志。各字段见 SPEC.md 第 5 节。

手工用例和需要补记的结果用 `record` 写进同一次运行：

```bash
python -m xrdap_test record --run xrdap-results/RUN UI-01 pass --note "紫 / 红 正常"
```

几次运行合成一张表：

```bash
python -m xrdap_test table xrdap-results/RUN-A xrdap-results/RUN-B --output matrix.md
```

## A/B 比较

`ab` 交替烧录两个镜像，每次烧录后用同样的设置运行同一组用例，最后列出每个数值指标的 A、B 平均值和比值。烧录命令由调用者给出：

```bash
python -m xrdap_test ab --link usb --target my-target.toml --cases PERF,VND-06 \
    --a-cmd "./flash.sh old.elf" --b-cmd "./flash.sh new.elf" --ab-rounds 2
```

## 目标描述

目标描述是一个 TOML 文件，写明工具可以改写的 RAM（scratch，运行结束时恢复原内容）、主机工具读出的区域（dump）、可以擦写的 Flash 区（flash_test）和各主机工具的目标名，格式见 [targets/example.toml](targets/example.toml)。目标描述放在使用者自己的目录里，用 `--target 路径` 指定；本仓库只带这个示例。scratch 要选目标程序不用的 RAM；临时改用别的区域时用 `--scratch BASE:SIZE`。

## 串口会话卫生（STAB-08）

STAB-08 检查串口会话会不会让探针重启或丢掉热点参数：串口链路跑 `xrdap share` 和 OpenOCD，同时经另一条链路（TCP 或 USB）读热点参数和探针时钟，所以要同时给出串口和这条链路，以及 `xrdap`、OpenOCD 的位置：

```bash
python -m xrdap_test run --link serial:PORT --link2 tcp:HOST --target my-target.toml --cases STAB-08 --xrdap PATH/xrdap --openocd PATH/openocd
```

探针没有保存热点参数时这一项记为 n/a。

## 覆盖检查

`coverage` 检查 SPEC.md、SPEC.en.md 第 3 节的每个用例都已实现、方式、链路和判据来源与代码一致，判据来源只用 SPEC 1.4 节允许的几种，以及 behavior-suite 的每个分支都对应到用例或注明 N/A：

```bash
python -m xrdap_test coverage
python -m unittest discover -s tool_unit
```

`coverage` 的输出：

```
SPEC.md: 55 cases, 52 behaviour-suite branches mapped
SPEC.en.md: 55 cases, 52 behaviour-suite branches mapped
code: 55 cases ({'auto': 39, 'assisted': 5, 'manual': 11})
coverage OK
```

单元测试用协议文档里的字节示例检查帧、条目编码和 SUB_READ 解码。
