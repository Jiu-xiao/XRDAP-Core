# XR-DAP 测试规范

[English](SPEC.en.md)

本文规定 XR-DAP 系列探针（XRDAP-ESP、XRDAP-F103C8、XRDAP-CH32V307）共用的测试用例、固定条件和结果记录方式。三种探针、各种链路使用同一组用例和同一套设置，测得的数字因此可以直接比较。用例由本目录的测试工具 `xrdap_test` 执行，用法见 [README.md](README.md)；协议细节见 [docs/protocol.md](../docs/protocol.md)。

目录：1 范围；2 固定条件；3 用例总表；4 用例说明；5 结果记录。

## 1. 范围

### 1.1 探针与链路

| 链路 | 写法 | 适用的探针 |
|---|---|---|
| `usb` | CMSIS-DAP v2 Bulk | 全部（ESP32-S3、F103、CH32V307；ESP32-C3 和传统 ESP32 没有 USB DAP） |
| `serial` | libxr Topic 串口帧（协议第 1 节） | XRDAP-ESP |
| `tcp` | OpenOCD CMSIS-DAP TCP 帧（协议第 2 节） | XRDAP-ESP；`xrdap share openocd` 共享出来的任何探针 |
| `usbip` | USB/IP，TCP 3240（协议第 6 节） | XRDAP-ESP；`xrdap share usbip` 共享出来的任何探针 |
| `radio` | 经无线对端到目标，上位机侧是 USB | XRDAP-CH32V307 无线板对 |

每个用例在第 3 节列出适用的链路。探针不具备某条链路时，该格记为 n/a。

### 1.2 执行方式

| 方式 | 含义 |
|---|---|
| `auto` | 工具独立完成并判定 |
| `assisted` | 工具判定，中间需要操作者动手（拔插、断电、关热点），工具提示后等待回车；给出 `--replug-cmd`、`--restart-cmd` 时由命令代替操作者 |
| `manual` | 按第 4 节的步骤人工完成，用 `record` 把结果写进同一份结果文件 |

### 1.3 目标

目标由 `targets/*.toml` 中的芯片描述给出：DPIDR、可以改写的 RAM 区（scratch）、可以读出的区域（dump）、可以擦写的 Flash 区（flash_test），以及 OpenOCD、probe-rs、pyOCD 的目标名。工具把测试夹具写进 scratch，不需要专门的目标程序，目标上原有的程序照常运行；运行开始时工具保存 scratch 的原内容，结束时写回。scratch 必须选目标程序不使用的 RAM。

目标矩阵至少覆盖 Cortex-M0/M0+、M3、M4、M7 各一款，其中一款不是 ST 的芯片，DPv1 和 DPv2 各至少一款（TGT-01 记录 DP 版本）。SWD multidrop 不在支持范围内（TGT-04）。

## 2. 固定条件

### 2.1 时钟

| 用途 | 设置 |
|---|---|
| 厂商扩展、目标操作、主机工具、稳定性用例 | SWD 10 MHz（`--clock`，可改，记入结果） |
| SWD-01 扫频 | 0.1、0.5、1、2、4、8、10、15、20、30、50 MHz |
| PERF-02 吞吐 | 4 MHz、10 MHz、最高稳定时钟（同一次运行中 SWD-01 的结果，或 `--max-clock`） |

时钟一律指请求值。探针把请求值映射到自己能实现的时钟，实际值由 SWD-01 估算。

### 2.2 夹具与条目组

夹具写在 scratch 起始处（偏移相对 scratch 基址）：

| 偏移 | 内容 |
|---|---|
| 0x18 | u8 0xA5 |
| 0x1A | u16 0x1234 |
| 0x1C | u32 0x89ABCDEF |
| 0x20 | u64 0x0123456789ABCDEF |
| 0x28 | u32 0xCAFEBABE，三级指针链的终点 |
| 0x2C、0x30、0x34 | 指针链：0x34 → 0x30 → 0x2C → 0x28 |
| 0x38 | 空指针 0 |
| 0x3C | 坏指针：指向目标描述的 `fault_address`（默认 0xFFFFF000） |
| 0x40 | 读写用的字，初值 0x11111111 |
| 0x100 到 0x17F | 32 个 u32，值 0x5A000000 + k |
| 0x400 起 | 块读写区，到 scratch 末尾 |

条目组：

- N 变量组（N = 1、4、8、32）：0x100 起 N 个 u32，直接读，div 1。
- 8 条目分频组（订阅顺序）：计数器 div 1；u32 字 div 1；计数器 div 10；u64 div 5；u32 0x89ABCDEF div 100 phase 7；u16 div 100 phase 13；u8 div 100 phase 23；三级指针链 div 100 phase 41。周期 2000 µs，flags 0。

计数器取目标描述的 `counter`；没有时使用 DWT CYCCNT（工具置位 DEMCR.TRCENA 和 DWT_CTRL.CYCCNTENA）；Cortex-M0/M0+ 没有 CYCCNT 又没有 `counter` 时不检查计数器。

### 2.3 最小采样周期的查找规则

从 SUB_SET 返回的估计单帧耗时起，向上取整到 25 µs 的整数倍，依次加 25 µs。每个周期运行 3 s，上位机不间断地读 SUB_READ。第一个同时满足以下条件的周期即为最小周期：跳过节拍 0、缓冲丢帧 0、失败条目 0、节拍序号无缺口。被 SUB_SET 以 0x03 拒绝的周期直接跳过。

### 2.4 轮询

SUB_READ 的 max_bytes 为 0xFFFF（探针按包长截断）。容量测试（VND-06、VND-09）不间断地读；流测试（VND-07、VND-08、STAB-01）在剩余字节为 0 时间隔 5 ms 再读。

### 2.5 判定的通用规则

- “无错”指：DAP 应答为 OK，读回数据与写入一致（CRC32 比较），探针在下一条命令仍按一问一答响应。
- 响应超时、链路断开、工具异常记为 error，与 fail 分开统计。
- 中位数、p99 按测得的全部样本计算；吞吐按墙钟时间计算。

## 3. 用例总表

链路一栏为 `all` 时适用于 1.1 节的全部链路。

| ID | 方式 | 链路 | 内容 |
|---|---|---|---|
| SWD-01 | `auto` | all | SWD 时钟扫频：实际时钟估算、各时钟数据完整性、最高稳定时钟 |
| SWD-02 | `auto` | all | 流水深度 1 到 N：响应顺序与数据 |
| PERF-01 | `auto` | all | 往返时间：DAP_Info 和一次 DP 读，中位数 / p99 |
| PERF-02 | `auto` | all | 块读写吞吐：4 MHz、10 MHz、最高稳定时钟 |
| PERF-03 | `auto` | all | 小数据访问：随机地址 4 B、64 B 每秒次数 |
| DAP-01 | `auto` | all | DAP_Transfer：DP/AP 访问、posted 读、匹配、时间戳、FAULT、中止 |
| DAP-02 | `auto` | all | DAP_TransferBlock：AP/DP 块、计数 0、包长边界、FAULT |
| DAP-03 | `auto` | all | 包边界：USB 包长附近的请求和响应长度 |
| DAP-04 | `auto` | all | 命令集：DAP_Info、HostStatus、SWJ_Pins、Execute/QueueCommands、未知命令 |
| TOOL-01 | `auto` | all | OpenOCD：严格连接 20 次、SRAM 16 KiB、128 KiB 读出、Flash 编程校验 |
| TOOL-02 | `auto` | usb, usbip, radio | probe-rs：连接 20 次、128 KiB 读出、Flash 下载校验 |
| TOOL-03 | `auto` | usb, usbip, radio | pyOCD：连接 20 次、128 KiB 读出、Flash 下载比对 |
| TOOL-04 | `manual` | usb, radio | Keil MDK：连接、读内存、下载校验 |
| TGT-01 | `auto` | all | 识别：DPIDR、DP 版本、AP IDR、CPUID、周期计数器 |
| TGT-02 | `auto` | all | 内存：8/16/32 位访问和 4 KiB 块 |
| TGT-03 | `auto` | all | 停核、运行、复位（SYSRESETREQ；接了 nRESET 时含硬件复位） |
| TGT-04 | `manual` | all | SWD multidrop |
| VND-01 | `auto` | all | INFO 和 DAP_Info 身份 |
| VND-02 | `auto` | all | READ：1/2/4/8 字节、指针链、条目状态、上限 |
| VND-03 | `auto` | all | WRITE：1/2/4/8 字节和读回、条目状态 |
| VND-04 | `auto` | all | 参数与状态检查 |
| VND-05 | `auto` | all | READ/WRITE 延迟、每秒条目数、指针链每级开销 |
| VND-06 | `auto` | all | 采样：1/4/8/32 变量的估计耗时和实际最小周期 |
| VND-07 | `auto` | all | 分频与相位：8 条目分频组 |
| VND-08 | `auto` | all | 时间戳抖动：1 ms、10 s |
| VND-09 | `auto` | all | 跳拍与丢帧计数、满即停、部分读取、采样期间独占 |
| VND-10 | `auto` | all | 经厂商 WRITE 停核、运行、复位的延迟 |
| VND-11 | `auto` | all | AP CSW 为非特权 Prot 时访问 PPB |
| VND-12 | `auto` | all | 停止后立即重新开始：新一轮从节拍 0 开始，没有旧帧 |
| VND-13 | `auto` | usb, serial, tcp, usbip | 0x86 Wi-Fi 状态查询 |
| STAB-01 | `auto` | all | 长时间采样（默认 1 h） |
| STAB-02 | `auto` | all | OpenOCD 编程校验循环（默认 1 h） |
| STAB-03 | `assisted` | usb, serial, radio | 采样中拔插探针 |
| STAB-04 | `assisted` | all | 采样中目标断电和恢复 |
| STAB-05 | `assisted` | tcp, usbip | 热点断开：恢复可达的时间 |
| STAB-06 | `auto` | all | 上位机不停止、不 Disconnect 就退出 |
| STAB-07 | `assisted` | all | 探针重启：启动到第一次应答 DAP_Info 的时间 |
| MC-01 | `auto` | tcp | 第二个 TCP 客户端 |
| MC-02 | `auto` | all | 两条链路同时访问一块探针 |
| MC-03 | `auto` | all | 采样的发起方更换 |
| RAD-01 | `auto` | radio | 无线负载：重发率、失败数、往返时间 |
| RAD-02 | `assisted` | radio | 配对在两块板断电后保持 |
| RAD-03 | `manual` | radio | 距离与干扰 |
| RES-01 | `auto` | all | 固件 Flash 与静态 RAM 占用（ELF） |
| RES-02 | `manual` | all | 堆余量与栈峰值 |
| RES-03 | `manual` | all | SWD 所在核的 CPU 负载 |
| RES-04 | `manual` | all | 固件更新耗时与设置保留 |
| RES-05 | `manual` | all | 功耗与温度 |
| UI-01 | `manual` | all | 状态灯：链路颜色 |
| UI-02 | `manual` | all | 状态灯：活动与上位机状态 |
| UI-03 | `manual` | all | 按键：清除 Wi-Fi、无线配对 |

`run` 默认执行除 STAB-01、STAB-02 以外的全部用例（`--cases quick`）；这两项各需 1 h，用 `--cases STAB-01` 等单独执行。

## 4. 用例说明

每项给出目的、设置和通过条件。没有写设置的，按第 2 节。

### 4.1 SWD 链路

SWD-01 时钟扫频。在 2.1 节的每个请求时钟上：重新连接；用 `DAP_ExecuteCommands` 打包 N 个 256 位和 N 个 64 位的 `DAP_SWJ_Sequence`，两种包的传输开销几乎相同，取各自最快五分之一的平均耗时之差，除以 N × 192 个时钟得到实际时钟（这是 `DAP_SWJ_Sequence` 的位速率；固件的序列路径和传输路径实现不同时，两者的时钟可能不一样，传输路径的快慢由同一时钟下的吞吐反映）；随后做 `--rounds`（默认 20）轮随机数据块写入和读回（低于 1 MHz 时每块 1 KiB，1 MHz 起 4 KiB；每个时钟最多 5 s，至少一轮），用 CRC32 比较。记录每个时钟的实际时钟、出错轮数和吞吐，以及没有出错的最高请求时钟（最高稳定时钟）。通过：4 MHz 及以下各时钟 0 错误。

SWD-02 流水深度。深度 d 从 1 到 min(DAP_Info 0xFE 报告的包数, 链路允许的深度)：串口链路按协议一问一答，深度只取 1。每个深度发 256 个读字请求（写 TAR 加读 DRW），始终保持 d 个在途。通过：每个深度的响应按发送顺序到达，数值都正确。

### 4.2 标准 DAP 性能与行为

PERF-01 往返时间。`DAP_Info(0xFF)` 和一次 DP IDCODE 读各 1000 次（或 3 s 内能完成的次数），记录中位数、p99、平均值。无线板对上 DAP_Info 由本地应答，DP 读才经过射频。

PERF-02 块吞吐。在 scratch 块读写区写入、读出 16 KiB 随机数据（scratch 不足时取能放下的 KiB 整数倍），每种操作 3 遍。块按 1 KiB 地址边界切开（TAR 自增只保证在 1 KiB 内），每块不超过一个 DAP 包。同时测流水读：每块的写 TAR 和 TransferBlock 以探针允许的深度在途。通过：每个时钟读回的数据与写入一致。

PERF-03 小数据。随机 64 B 对齐地址上，各运行 2 s：4 B 读（写 TAR、读 DRW）、4 B 写、64 B 块写、64 B 块读，记录每秒次数。通过：最后一次写入的数据读回一致。

DAP-01 到 DAP-04 是 cmsis-dap-behavior-suite 分支目录在真实探针上的对应检查：

- DAP-01：DP 读写；一个包里写 TAR 后连读 4 次 DRW 再读 RDBUFF，数值按顺序；同一包里混合 DP、AP 写并中途重写 SELECT；掩码 0xFFFF0000 下匹配读命中与不命中（不命中时应答带 0x10）；探针报告测试域定时器时检查带时间戳的读和写；读 `fault_address` 得到 FAULT，写 ABORT 后恢复；计数大于实际操作数的畸形请求、读 255 次超出包长的请求都得到应答且链路仍一问一答；空闲时 `DAP_TransferAbort` 不打乱后续应答（串口上它得到一个 n 为 0 的空帧，USB 和 TCP 上没有响应）。
- DAP-02：DP 块读（IDCODE × 3）和 DP 块写（SELECT × 2）；计数 0；一个包能放下的最多字数完整返回，多一个字时响应不超过包长；从 `fault_address` 块读得到 FAULT；带匹配、时间戳位的块请求得到应答且链路同步。
- DAP-03：请求长度取 MPS−1、MPS、MPS+1、2×MPS、包长−1、包长（MPS 为 USB 端点最大包长，其他链路取 64），响应长度取 MPS−4、MPS、包长、8 字节，每种 20 次。整包长度的请求由上位机补零长度包（协议“USB 主机的注意事项”）。
- DAP-04：DAP_Info 各 ID、DAP_HostStatus、DAP_SWJ_Pins 读、DAP_Delay；ExecuteCommands 回显命令数；连续两个 QueueCommands 包加一个 ExecuteCommands 包得到三个按顺序的 0x7F 响应；未定义命令 0x3F 只回 `FF`。

behavior-suite 分支与本规范用例的对应。标 N/A 的分支或描述上位机侧的调度模型，或需要在健康目标上注入线路错误，不属于探针在线测试：

| 分支 | 用例 |
|---|---|
| `consistency.hash_match` | SWD-01 |
| `consistency.hash_mismatch` | SWD-01 |
| `consistency.repeatability_fixed_seed` | N/A：场景生成模型 |
| `consistency.result_schema_complete` | N/A：本工具以 result.json 固定格式代替 |
| `schedule.strict_homogeneous_block_only` | N/A：上位机调度模型 |
| `schedule.run_split_block_friendly` | N/A：上位机调度模型 |
| `schedule.packet_cap_transfer_only` | N/A：上位机调度模型 |
| `schedule.packet_cap_block_allowed` | N/A：上位机调度模型 |
| `schedule.threshold_lt_four_transfer` | N/A：上位机调度模型 |
| `schedule.threshold_ge_four_block` | N/A：上位机调度模型 |
| `dap.transfer.dp_write` | DAP-01 |
| `dap.transfer.dp_read` | DAP-01 |
| `dap.transfer.ap_write` | DAP-01 |
| `dap.transfer.ap_read_first_posted` | DAP-01 |
| `dap.transfer.ap_read_pipeline_continue` | DAP-01 |
| `dap.transfer.ap_read_tail_rdbuff_flush` | DAP-01 |
| `dap.transfer.write_tail_rdbuff_check` | DAP-01 |
| `dap.transfer.match_mask_write` | DAP-01 |
| `dap.transfer.match_value_read` | DAP-01 |
| `dap.transfer.timestamp_read` | DAP-01 |
| `dap.transfer.timestamp_write` | DAP-01 |
| `dap.transfer.abort_pre_packet` | DAP-01 |
| `dap.transfer.abort_mid_packet` | N/A：USB 上中止与包执行的先后不可控 |
| `dap.transfer.response_overflow_guard` | DAP-01 |
| `dap.transfer.request_underflow_guard` | DAP-01 |
| `dap.transfer.ack_wait` | N/A：需要目标持续返回 WAIT |
| `dap.transfer.ack_fault` | DAP-01 |
| `dap.transfer.ack_protocol_error` | N/A：需要注入线路错误 |
| `dap.transfer.parity_error` | N/A：需要注入线路错误 |
| `dap.transfer.mixed_ap_dp_breaks_block` | DAP-01 |
| `dap.transfer.select_churn_breaks_block` | DAP-01 |
| `dap.block.dp_write` | DAP-02 |
| `dap.block.dp_read` | DAP-02 |
| `dap.block.ap_write` | PERF-02 |
| `dap.block.ap_read_pipeline` | PERF-02 |
| `dap.block.count_zero` | DAP-02 |
| `dap.block.count_boundary_fit` | DAP-02 |
| `dap.block.count_boundary_overflow` | DAP-02 |
| `dap.block.timestamp_not_supported` | DAP-02 |
| `dap.block.match_not_supported` | DAP-02 |
| `dap.block.ack_wait` | N/A：需要目标持续返回 WAIT |
| `dap.block.ack_fault` | DAP-02 |
| `dap.block.parity_error` | N/A：需要注入线路错误 |
| `transport.usb.single_packet_out` | DAP-03 |
| `transport.usb.single_packet_in` | DAP-03 |
| `transport.usb.multi_packet_out` | DAP-03 |
| `transport.usb.multi_packet_in` | DAP-03 |
| `transport.usb.boundary_eq_mps` | DAP-03 |
| `transport.usb.boundary_gt_mps` | DAP-03 |
| `transport.usb.in_busy_deferred` | SWD-02 |
| `transport.usb.response_queue_reuse` | DAP-03 |
| `transport.usb.request_queue_pressure` | SWD-02 |

### 4.3 主机工具

主机工具 × 操作系统 × 链路组成矩阵：每次运行固定一种组合，Windows 和 Linux 各跑一遍。OpenOCD 经 USB 用 `usb_bulk` 后端并按序列号选探针；经 TCP 用 `cmsis-dap backend tcp`（Windows 上加 `cmsis-dap quirk enable` 和 `cmsis-dap tcp min_timeout 10`）；经 USB/IP 在 Linux 上由工具执行 `usbip attach`，结束后 detach；串口探针由 `xrdap share openocd` 转成 TCP。probe-rs、pyOCD 只认本机 USB 探针。

TOOL-01 OpenOCD。严格连接 20 次（`--attach-count`）：退出码 0，日志报告了内核（`Examination succeed` 或 `processor detected`），没有 `Examination failed`；传输层的重试报错不影响这一项，数据传输失败由后面几步反映。然后在 scratch 块区 `load_image`、`verify_image`、`dump_image` 16 KiB，读回与写入一致；dump 区读出两次，两次相同；有 flash_test 区时擦除、写入 16 KiB 随机数据并 `verify_image`。各步用 Tcl `clock milliseconds` 计时。

TOOL-02 probe-rs、TOOL-03 pyOCD：同样的连接次数、读出 dump 区、在 flash_test 区下载并校验（pyOCD 下载后读回比对）。

TOOL-04 Keil MDK（手工）：Debug 设置里选 CMSIS-DAP Debugger，按序列号选探针，SWD，固定时钟；确认识别到目标，在 Memory 窗口看 scratch 地址，勾选 Verify 后 Download。记录 uVision 和 pack 版本、下载时间、校验结果。Keil 只认本机 USB 探针，其他链路记 n/a。

### 4.4 目标

TGT-01 识别：DPIDR（与目标描述一致）、DP 版本、AP0 IDR、CPUID 对应的内核、DWT 有无周期计数器。

TGT-02 内存：在 RW 字上用 8 位写偏移 1、16 位写偏移 2，读回 0xCDEFAB00（检查字节通道）；4 KiB 随机块读写一致。

TGT-03 内核控制：DHCSR 停核、继续运行；置 DEMCR.VC_CORERESET 后 AIRCR.SYSRESETREQ，内核停在复位向量；`--nreset-wired` 时再用 DAP_SWJ_Pins 把 nRESET 拉低 50 ms，确认内核复位。结束时清除 VC_CORERESET、退出调试并重写夹具。

TGT-04 multidrop（手工）：XR-DAP 不实现 SWD multidrop 的目标选择，每块探针记为不支持；若用主机工具连接了 multidrop 目标（例如 RP2040），记录工具的报告。

### 4.5 XR 厂商扩展

VND-01：DAP_Info 厂商为 `XRobot`，序列号以 `XRobot-XRDAP-` 开头，有固件版本；INFO 协议版本 1、最多 32 条目、深度 8，功能位含 READ、WRITE、采样、8 字节；探针时间递增。

VND-02：一次 READ 读出 u8、u16、u32、u64 和三级指针链，数值与夹具一致；状态矩阵：空指针 0x30，读 `fault_address` 处的指针 0x10，指针地址未对齐 0x20，最终地址未按 size 对齐 0x2F，第二级指针故障 0x11，最终访问故障 0x1F，其后的正常条目 0x00 且数值正确；计数 0 返回 0x01；33 个条目返回 0x03；32 个 8 字节条目在响应放得下时成功，放不下（512 字节包）时返回 0x03。

VND-03：WRITE 1、2、8、4 字节后读回一致；空指针 0x30、第二级指针故障 0x11、最终写故障 0x1F，其后的正常写入生效；计数 0 返回 0x01。

VND-04：SUB_SET 的 period 0、flags 保留位、计数 0、div 0、phase ≥ div、size 3 返回 0x01；INFO 多一个字节、SUB_CTRL 未定义 op、SUB_CTRL 缺参数、READ 长度不符返回 0x01；运行中再次开始、运行中 SUB_SET 返回 0x02；停止时再停止返回 0x00；`DAP_Disconnect` 之后 READ、SUB_SET 返回 0x04。

VND-05：单条目 READ、WRITE 的往返中位数；1、8、32 条目 READ 的每秒条目数；指针链每级开销：8 个条目经自指针分别解引用 0、1、2、4、8 级，(8 级 − 0 级) / 64 即每级耗时。通过：8 级自指针链解析成功。

VND-06：对 1、4、8、32 变量组记录 SUB_SET 的估计耗时（周期 1 s 时返回的值）和 2.3 节规则找到的最小周期。通过：每组都找到最小周期。

VND-07：8 条目分频组运行 5 s。通过：每帧的到期条目集合等于 `(k + phase) % div == 0` 的集合；常量和指针链的值正确；没有失败条目；跳拍、丢帧、节拍缺口都为 0；计数器只增不减；各条目出现次数与 节拍数 / div 相差不超过 1。

VND-08：单个计数器条目，周期 1 ms，运行 10 s。记录时间戳偏差（帧时间戳 − 首帧时间戳 − Δk × 周期）的最小、最大和绝对值 p99；计数器为 CYCCNT 时由相邻帧的 CYCCNT 差和整段推得的目标时钟，算出目标侧实际采样间隔的偏差 p99。通过条件同 VND-07 中适用的各项。

VND-09：8 变量组，按估计耗时的两倍（至少 2 ms）设置后把 SWD 降到 100 kHz，运行 2 s：跳拍计数大于 0；覆盖模式下 2 s 不读，丢帧计数大于 0；满即停模式下 2 s 不读，读出的第一帧节拍为 0 且丢帧计数大于 0；max_bytes 20 时不返回帧且剩余字节不为 0；第一帧之前和帧之间缺的节拍数不大于跳拍计数（最后一帧之后跳过的节拍看不见，计为 skipped_not_seen）；采样期间 DAP_Transfer 只回 `FF`，DAP_Info 和 READ 正常。

VND-10：经厂商 WRITE/READ 访问 DHCSR、DEMCR、AIRCR 停核、继续运行、复位后停在复位向量，记录从发出到 DHCSR 反映状态的毫秒数。通过：三种状态都在 1 s 内观察到。

VND-11：把 AP CSW 的 Prot 设为非特权数据访问（`0x01`，部分芯片调试上电后的初值），再用厂商 READ 读 CPUID、DHCSR。通过：两项状态都为 0x00，CPUID 与特权读到的值相同。探针应自行使用特权访问，不沿用 CSW 原有的 Prot。

VND-12：4 变量组采样 0.5 s 后停止，立即用另一个 2 条目组 SUB_SET 并开始，50 ms 后读取。通过：没有丢帧，新一轮第一帧节拍为 0，所有帧只含新条目且数值正确。

VND-13（XRDAP-ESP）：0x86 op 0 的响应长度为 9 + 热点名长度，记录标志和 RSSI；未定义 op 返回 0x01。保存和清除会重启探针，归入 RES-04 和 UI-03。

### 4.6 稳定性

STAB-01：8 条目分频组运行 `--long-s`（默认 3600 s），通过条件同 VND-07。

STAB-02：在 flash_test 区循环“写入随机 16 KiB 并 verify”，持续 `--long-s`。通过：每次都校验成功；失败的日志单独保存。

STAB-03（辅助）：4 变量组 1 ms 采样中拔下探针 USB（或 USB 串口），2 s 后插回。通过：30 s 内重新应答；缓冲剩余字节不再增长（采样已停止）；重新连接后 SUB_SET 成功，1 s 内至少 500 帧。记录恢复时间。

STAB-04（辅助）：采样中给目标断电。通过：断电期间探针照常应答 SUB_READ（失败条目计数记录在结果中）；恢复供电后重新连接、采样正常。

STAB-05（辅助）：关掉热点，确认探针不可达；打开热点后立即开始计时，记录到探针重新应答 DAP_Info 的时间。通过：300 s 内可达，随后采样正常。

STAB-06：子进程连接、开始采样后直接退出，不停止、不 Disconnect、不关闭。通过：下一个客户端能停止采样，重新连接后采样正常。

STAB-07（辅助）：采样中复位或重新上电探针。工具从探针失去应答开始计时，到第一次应答 DAP_Info 为止，即启动时间（`--restart-cmd` 在重启开始后才返回时，从命令结束时算起）。通过：60 s 内应答，探针时间从头开始（确实重启过），随后采样正常。

### 4.7 多客户端

MC-01（TCP）：客户端 A 开始采样后，客户端 B 连接同一端口。通过：同一时刻只有一个客户端得到应答；A 发起的采样已停止。记录哪一方被服务。

MC-02（`--link2`）：两条链路（例如 USB 和 TCP）各自连接，同时连续 5 s 读夹具中的 u32 和指针链。通过：两边都有读数，没有一个值出错。

MC-03（`--link2`）：A 开始采样；B 读到 A 的帧，停止采样，重新连接后 SUB_SET 并采样 1 s。通过：B 的每一步状态为 0x00，至少 500 帧。

### 4.8 无线板对

链路 `radio` 下，第 3 节标 `all` 的用例都经射频执行，吞吐、往返时间即无线的性能。

RAD-01：读取无线固件的诊断统计（命令 0x9F 0x11），做 5 遍 4 KiB 块读写，再读统计。记录重发数 / 命令数、失败数、最大往返时间。通过：数据一致，失败数 0。

RAD-02（辅助）：读配对设置（0x9F 0x13），两块板都断电再上电后再读。通过：仍为已配对，link id 不变。

RAD-03（手工）：在 1 m、5 m、10 m、隔一堵墙处分别跑 PERF-02 和 RAD-01，再在 1 m 内有繁忙的 2.4 GHz Wi-Fi 时重复；记录距离、遮挡、干扰源、重发率、读吞吐。

### 4.9 固件与资源

RES-01：从探针固件 ELF 的段表统计 Flash 占用（有内容的可分配段）和静态 RAM（可写段与 NOBITS 段）。

RES-02（手工）：F103、CH32 在启动时给栈填固定值，跑完 SWD-01、PERF-02、VND-06 后用另一个调试器不停核读出被改写的最深位置；堆末端取 `__sbrk_heap_end` 或 FreeRTOS 历史最小空闲堆。ESP 在命令行读 `heap_caps_get_minimum_free_size` 和各任务的栈高水位。记录最差处的剩余字节。

RES-03（手工）：调试版固件在 DAP 命令处理和采样节拍前后翻转一个空闲 GPIO，用逻辑分析仪测 PERF-02 期间和最小周期采样期间的占空比。

RES-04（手工）：按各探针的正常途径更新固件（ESP 网页烧录或 `esptool write_flash 0x0 merged.bin`；CH32 `tools/dfu_update.py`；F103 SWD 烧录发布镜像），记录从开始到应答 DAP_Info 的时间，并确认设置保留：ESP 的热点参数（0x86 op 0 显示同一热点名），CH32 的配对和角色（0x9F 0x13）。启动时间本身由 STAB-07 测得。

RES-05（手工）：USB 电流表串在探针上，记录空闲、PERF-02 期间、最小周期采样期间的电流；PERF-02 循环 10 分钟后记录板上最热点和室温。

### 4.10 灯和按键

全部手工，逐块探针用眼睛确认，规则见本仓库 README 的“状态灯”一节。

UI-01：不接上位机上电为紫；USB 主机连接为红；只有网络客户端为蓝（ESP）；无线会话为蓝（无线板对）。

UI-02：DAP 通信时灯有活动脉冲；OpenOCD `init; halt` 后主色与紫色每 2 s 交替，`resume` 后红蓝每 1 s 交替；`DAP_Disconnect` 或 5 s 没有命令后恢复。

UI-03：带按键的 ESP 板按住 5 s，紫色快闪后常亮，热点参数清除并重启（0x86 op 0 没有热点名）；无线板对两块未配对的板长按 KEY2，紫色闪烁，完成后两块板报告同一 link id。

## 5. 结果记录

每次运行（一块探针、一条链路、一个目标）写一个目录，内容为：

| 文件 | 内容 |
|---|---|
| `result.json` | 运行信息和每个用例的状态、检查项、指标 |
| `result.md` | 运行信息一行和用例表 |
| `log.txt` | 完整过程日志 |
| `openocd/` | 主机工具的配置、日志和读出的文件 |

运行信息必须包含：探针（DAP_Info 的产品、序列号、固件版本、包长包数，厂商扩展 INFO）、链路、目标、主机（操作系统、Python 版本）、被测镜像的固件、libxr、XRDAP-Core 提交号（`--build fw=…,libxr=…,core=…`）、主机工具版本、日志路径。用例状态为 pass、fail、error（工具或链路异常）、n/a（不适用，附原因）、manual（待人工记录）。

多次运行用 `table` 合成一张表：上半部分每行一次运行，下半部分每行一个用例、每列一次运行。

| run | probe | link | target | host | firmware | libxr | core | tools | log |
|---|---|---|---|---|---|---|---|---|---|
| R1 | | | | | | | | | |

| case | R1 | R2 | … |
|---|---|---|---|
| SWD-01 | | | |

A/B 比较（`ab`）用同一组设置交替运行两个镜像（A、B、A、B），表中列出每个数值指标在 A、B 各轮的平均值和 B/A。
