# XR-DAP Test Specification

[中文](SPEC.md)

This document defines the test cases, fixed conditions and result format shared by the XR-DAP probe family (XRDAP-ESP, XRDAP-F103C8, XRDAP-CH32V307). All three probes and all links use the same cases and the same settings, so the measured numbers can be compared directly. The cases are run by the test tool `xrdap_test` in this directory; usage is in [README.en.md](README.en.md); protocol details are in [docs/protocol.en.md](../docs/protocol.en.md).

Contents: 1 Scope; 2 Fixed conditions; 3 Case table; 4 Case descriptions; 5 Result records.

## 1. Scope

### 1.1 Probes and links

| Link | Notation | Applicable probes |
|---|---|---|
| `usb` | CMSIS-DAP v2 Bulk | All (ESP32-S3, F103, CH32V307; ESP32-C3 and the original ESP32 have no USB DAP) |
| `serial` | libxr Topic serial frames (protocol section 1) | XRDAP-ESP |
| `tcp` | OpenOCD CMSIS-DAP TCP frames (protocol section 2) | XRDAP-ESP; any probe shared with `xrdap share openocd` |
| `usbip` | USB/IP, TCP 3240 (protocol section 6) | XRDAP-ESP; any probe shared with `xrdap share usbip` |
| `radio` | To the target through the wireless peer; the host side is USB | XRDAP-CH32V307 wireless board pair |

Section 3 lists the applicable links of each case. If a probe does not have a link, that cell is recorded as n/a.

### 1.2 Execution modes

| Mode | Meaning |
|---|---|
| `auto` | The tool completes and judges the case on its own |
| `assisted` | The tool judges, but the operator has to act in between (plug and unplug, power off, turn off the hotspot); the tool prompts and waits for Enter; when `--replug-cmd` or `--restart-cmd` is given, the command replaces the operator |
| `manual` | Performed by hand following the steps in section 4; the result is written to the same result file with `record` |

### 1.3 Target

The target is given by a chip description file (TOML), kept in the user's own directory and specified with `--target PATH`; `targets/example.toml` in this directory is only a format example. The description contains: DPIDR, the RAM region that may be overwritten (scratch), the region that may be read out (dump), the Flash region that may be erased and written (flash_test), and the target names for OpenOCD, probe-rs and pyOCD. The tool writes the test fixture into scratch and needs no dedicated target program; the program already on the target keeps running; at the start of a run the tool saves the original contents of scratch and writes them back at the end. Scratch must be RAM that the target program does not use.

The target matrix covers at least one each of Cortex-M0/M0+, M3, M4 and M7, one of which is not an ST chip, and at least one each of DPv1 and DPv2 (TGT-01 records the DP version). SWD multidrop is not supported (TGT-04).

### 1.4 Oracle sources

The pass criteria of each case may come only from the sources below; the "Oracle" column of section 3 names them case by case; the existing behaviour of the firmware or of a simulated probe cannot serve as an oracle:

| Notation | Source |
|---|---|
| `cmsis-dap` | The Arm CMSIS-DAP specification (command format, responses, packet size and packet count) |
| `arm` | The Arm debug architecture (ADIv5/ADIv6; DHCSR, DEMCR, AIRCR and so on of ARMv7-M/ARMv8-M), which is what CMSIS-DAP accesses |
| `protocol X` | Section X of `docs/protocol.en.md` in this repository |
| `decision X` | A decision of the user, numbered X (`status-light` is the light colour rule of the whole family) |
| `hardware` | Hardware evidence: the host tool's own verification and a byte-by-byte comparison |
| `measure` | Numbers that are only recorded and compared A/B; they do not decide pass or fail by themselves |

## 2. Fixed conditions

### 2.1 Clock

| Purpose | Setting |
|---|---|
| Vendor extension, target operations, host tools, stability cases | SWD 10 MHz (`--clock`, changeable, recorded in the result) |
| SWD-01 sweep | 0.1, 0.5, 1, 2, 4, 8, 10, 15, 20, 30, 50 MHz |
| PERF-02 throughput | 4 MHz, 10 MHz, highest stable clock (the SWD-01 result of the same run, or `--max-clock`) |

A clock always means the requested value. The probe maps the requested value to a clock it can implement; the actual value is estimated by SWD-01.

### 2.2 Fixture and item groups

The fixture is written at the start of scratch (offsets are relative to the scratch base):

| Offset | Content |
|---|---|
| 0x18 | u8 0xA5 |
| 0x1A | u16 0x1234 |
| 0x1C | u32 0x89ABCDEF |
| 0x20 | u64 0x0123456789ABCDEF |
| 0x28 | u32 0xCAFEBABE, the end of the three-level pointer chain |
| 0x2C, 0x30, 0x34 | Pointer chain: 0x34 → 0x30 → 0x2C → 0x28 |
| 0x38 | Null pointer 0 |
| 0x3C | Bad pointer: points to the `fault_address` of the target profile (default 0xFFFFF000) |
| 0x40 | Word for read/write, initial value 0x11111111 |
| 0x100 to 0x17F | 32 u32 values, value 0x5A000000 + k |
| From 0x400 | Block read/write area, up to the end of scratch |

Item groups:

- N-variable group (N = 1, 4, 8, 32): N u32 values starting at 0x100, read directly, div 1.
- 8-item divider group (in subscription order): counter div 1; u32 word div 1; counter div 10; u64 div 5; u32 0x89ABCDEF div 100 phase 7; u16 div 100 phase 13; u8 div 100 phase 23; three-level pointer chain div 100 phase 41. Period 2000 µs, flags 0.

The counter is the `counter` of the target profile; if there is none, DWT CYCCNT is used (the tool sets DEMCR.TRCENA and DWT_CTRL.CYCCNTENA); on Cortex-M0/M0+, which have no CYCCNT and no `counter`, the counter is not checked.

### 2.3 Rule for finding the minimum sampling period

The probe does not estimate the time needed (decision B9); whether it keeps up is judged only from the result of a run. A period "keeps up" when: the run lasts 3 s with the host reading frames without pause, skipped ticks 0, buffer dropped frames 0, failed items 0, no gap in the tick sequence, and after stopping the remaining bytes of the buffer are read empty in one read. A period that fails once is run again at once and counts as not keeping up only when both runs fail (one burst of dropped frames from a pause in the host's reads does not change the result). Starting from 25 µs, the period is doubled each time until the first period P that keeps up; then, starting from P/2, 25 µs is added each time, and the first period that keeps up is the minimum period. The same rule is used for all probes and links.

### 2.4 Polling

The max_bytes of SUB_READ is 0xFFFF (the probe truncates to the packet length). Capacity tests (VND-06, VND-09) read without pause; stream tests (VND-07, VND-08, STAB-01) read again after 5 ms when the remaining bytes are 0.

### 2.5 General rules for judging

- "No error" means: the DAP response is OK, the data read back matches the data written (CRC32 comparison), and the probe still responds in request-response fashion to the next command.
- Response timeouts, link disconnects and tool exceptions are recorded as error, counted separately from fail.
- Median and p99 are computed over all measured samples; throughput is computed from wall-clock time.

## 3. Case table

When the link column is `all`, the case applies to all links of section 1.1.

| ID | Mode | Links | Oracle | Description |
|---|---|---|---|---|
| SWD-01 | `auto` | all | cmsis-dap, measure | SWD clock sweep: actual clock estimate, data integrity at each clock, highest stable clock |
| SWD-02 | `auto` | all | cmsis-dap | Pipelining depth 1 to N: response order and data |
| PERF-01 | `auto` | all | measure | Round-trip time: DAP_Info and one DP read, median / p99 |
| PERF-02 | `auto` | all | measure, cmsis-dap | Block read/write throughput: 4 MHz, 10 MHz, highest stable clock |
| PERF-03 | `auto` | all | measure, cmsis-dap | Small-data access: 4 B and 64 B operations per second at random addresses |
| DAP-01 | `auto` | all | cmsis-dap, protocol 1, protocol 2, decision B10 | DAP_Transfer: DP/AP access, posted read, match, timestamp, FAULT, abort |
| DAP-02 | `auto` | all | cmsis-dap | DAP_TransferBlock: AP/DP block, count 0, packet size boundary, FAULT |
| DAP-03 | `auto` | all | cmsis-dap, protocol 2 | Packet boundary: request and response lengths around the USB packet size |
| DAP-04 | `auto` | all | cmsis-dap | Command set: DAP_Info, HostStatus, SWJ_Pins, Execute/QueueCommands, unknown command |
| DAP-05 | `auto` | all | cmsis-dap, protocol 1, protocol 2, hardware | Response length scan: every length from 12 bytes to the packet size arrives at once, including link frames that are an exact multiple of the packet size |
| TOOL-01 | `auto` | all | hardware, measure | OpenOCD: 20 strict attaches, 16 KiB SRAM, 128 KiB readout, Flash program and verify |
| TOOL-02 | `auto` | usb, usbip, radio | hardware, measure | probe-rs: 20 attaches, 128 KiB readout, Flash download and verify |
| TOOL-03 | `auto` | usb, usbip, radio | hardware, measure | pyOCD: 20 attaches, 128 KiB readout, Flash download and compare |
| TOOL-04 | `manual` | usb, radio | hardware, measure | Keil MDK: connect, read memory, download and verify |
| TGT-01 | `auto` | all | arm | Identification: DPIDR, DP version, AP IDR, CPUID, cycle counter |
| TGT-02 | `auto` | all | arm | Memory: 8/16/32-bit access and 4 KiB block |
| TGT-03 | `auto` | all | arm | Halt, run, reset (SYSRESETREQ; including hardware reset when nRESET is wired) |
| TGT-04 | `manual` | all | arm | SWD multidrop |
| VND-01 | `auto` | all | protocol 3.4, protocol 4.1 | INFO and DAP_Info identity |
| VND-02 | `auto` | all | protocol 3.2, protocol 3.3 | READ: 1/2/4/8 bytes, pointer chain, item status, limits |
| VND-03 | `auto` | all | protocol 3.2, protocol 3.3 | WRITE: 1/2/4/8 bytes with read-back, item status |
| VND-04 | `auto` | all | protocol 3.2, protocol 3.4, decision B9 | Parameter and state checks |
| VND-05 | `auto` | all | measure, protocol 3.3 | READ/WRITE latency, items per second, cost per pointer-chain level |
| VND-06 | `auto` | all | decision B9, measure | Sampling: minimum period for 1/4/8/32 variables |
| VND-07 | `auto` | all | protocol 3.5 | Divider and phase: 8-item divider group |
| VND-08 | `auto` | all | protocol 3.5, measure | Timestamp jitter: 1 ms, 10 s |
| VND-09 | `auto` | all | protocol 3.5, decision B9 | Skipped-tick and dropped-frame counts, stop when full, partial read, exclusive use during sampling |
| VND-10 | `auto` | all | arm, protocol 3.1, measure | Latency of halt, run and reset through vendor WRITE |
| VND-11 | `auto` | all | protocol 3.6, decision C5 | PPB access when the AP CSW has an unprivileged Prot |
| VND-12 | `auto` | all | protocol 3.4, protocol 3.5 | Restart right after stop: the new run starts at tick 0, no stale frames |
| VND-13 | `auto` | usb, serial, tcp, usbip | protocol 3.7, decision A4 | 0x86 Wi-Fi status query |
| VND-14 | `auto` | all | protocol 3.2, decision B8 | Status 0x03 when the response space is not enough |
| VND-15 | `auto` | serial | protocol 3.1 | Serial lease: sampling started over serial stops after 5 s without requests |
| STAB-01 | `auto` | all | protocol 3.5 | Long sampling run (default 1 h) |
| STAB-02 | `auto` | all | hardware | OpenOCD program-and-verify loop (default 1 h) |
| STAB-03 | `assisted` | usb, serial, radio | protocol 3.1 | Unplug and replug the probe during sampling |
| STAB-04 | `assisted` | all | protocol 3.5 | Target power loss and recovery during sampling |
| STAB-05 | `assisted` | tcp, usbip | measure | Hotspot disconnect: time to become reachable again |
| STAB-06 | `auto` | all | protocol 3.1, decision B2 | Host exits without stopping or Disconnect |
| STAB-07 | `assisted` | all | measure | Probe restart: time from boot to the first DAP_Info response |
| STAB-08 | `auto` | serial | decision A4, protocol 3.7, hardware | Serial session hygiene: Wi-Fi credentials and probe uptime unchanged after a long-running xrdap share, an OpenOCD attach and a killed process |
| MC-01 | `auto` | tcp | protocol 2 | Second TCP client is not served |
| MC-02 | `auto` | all | protocol 3.1, decision B2 | Second link answers "busy", session owner unaffected |
| MC-03 | `auto` | all | protocol 3.1, decision B2 | Change of the session owner |
| MC-04 | `auto` | all | protocol 3.1, decision B2 | Free commands: status queries from a second link need no session and take none |
| RAD-01 | `auto` | radio | measure | Wireless load: retransmission rate, failure count, round-trip time |
| RAD-02 | `assisted` | radio | decision B3 | Pairing persists after both boards are powered off |
| RAD-03 | `manual` | radio | measure | Distance and interference |
| RAD-04 | `manual` | radio | decision B6 | Wireless session end: the remote side ends the session and stops sampling when the session number changes, is 0, or no ping arrives for 3 s |
| RES-01 | `auto` | all | measure | Firmware Flash and static RAM usage (ELF) |
| RES-02 | `manual` | all | measure | Heap margin and peak stack usage |
| RES-03 | `manual` | all | measure | CPU load of the core running SWD |
| RES-04 | `manual` | all | decision A4, measure | Firmware update time and settings retention |
| RES-05 | `manual` | all | measure | Power consumption and temperature |
| UI-01 | `manual` | all | decision status-light | Status light: link colour |
| UI-02 | `manual` | all | decision status-light | Status light: activity and host state |
| UI-03 | `manual` | all | decision A4, decision B3 | Button: clear Wi-Fi, wireless pairing |

`run` executes all cases except STAB-01 and STAB-02 by default (`--cases quick`); these two take 1 h each and are run individually, for example with `--cases STAB-01`.

## 4. Case descriptions

Each entry gives the purpose, the setup and the pass criteria. Where no setup is written, section 2 applies.

### 4.1 SWD link

SWD-01 Clock sweep. At each requested clock of section 2.1: reconnect; use `DAP_ExecuteCommands` to pack N 256-bit and N 64-bit `DAP_SWJ_Sequence` commands (the two packet kinds have almost the same transfer overhead); take the difference of the mean times of the fastest fifth of each, and divide by N × 192 clocks to get the actual clock (this is the bit rate of `DAP_SWJ_Sequence`; when the firmware implements the sequence path and the transfer path differently, the two clocks may differ, and the speed of the transfer path is reflected by the throughput at the same clock); then run `--rounds` (default 20) rounds of random-data block write and read-back (1 KiB per block below 1 MHz, 4 KiB from 1 MHz up; at most 5 s per clock, at least one round), compared by CRC32. For each clock the actual clock, the number of failed rounds and the throughput are recorded, together with the highest requested clock without errors (the highest stable clock). Pass (`cmsis-dap`: at every clock the probe accepts, the transfers must be correct): 0 errors at every clock; the actual clock and the highest stable clock are only recorded.

SWD-02 Pipelining depth. Depth d runs from 1 to min(the packet count reported by DAP_Info 0xFE, the depth the link allows): the serial link is request-response by protocol, so the depth is only 1. At each depth, 256 word-read requests (write TAR plus read DRW) are sent, always keeping d in flight. Pass: at each depth the responses arrive in send order and all values are correct.

### 4.2 Standard DAP performance and behaviour

PERF-01 Round-trip time. `DAP_Info(0xFF)` and one DP IDCODE read, 1000 times each (or as many as can be completed within 3 s); the median, p99 and mean are recorded. On the wireless board pair DAP_Info is answered locally, and only the DP read goes over the radio.

PERF-02 Block throughput. 16 KiB of random data is written to and read from the scratch block read/write area (an integer number of KiB that fits when scratch is smaller), 3 passes for each operation. Blocks are split at 1 KiB address boundaries (TAR auto-increment is only guaranteed within 1 KiB), and each block is no larger than one DAP packet. Pipelined reads are measured as well: for each block, the TAR write and the TransferBlock are in flight at the depth the probe allows. Pass: at each clock the data read back matches the data written.

PERF-03 Small data. At random 64 B-aligned addresses, each runs for 2 s: 4 B read (write TAR, read DRW), 4 B write, 64 B block write, 64 B block read; operations per second are recorded. Pass: the data read back matches the last write.

DAP-01 to DAP-04 are the checks on a real probe that correspond to the branch catalogue of cmsis-dap-behavior-suite:

- DAP-01: DP read and write; in one packet, write TAR followed by 4 DRW reads and an RDBUFF read, with values in order; in one packet, mixed DP and AP writes with SELECT rewritten midway; match reads that hit and miss under the mask 0xFFFF0000 (a miss answers with 0x10); when the probe reports a test-domain timer, reads and writes with timestamps; reading `fault_address` gives FAULT, and writing ABORT recovers; malformed requests with a count larger than the actual number of operations, and a request reading 255 times that exceeds the packet length, both get a response and the link stays request-response; `DAP_TransferAbort` has no response content (CMSIS-DAP specification; decision B10): on USB and TCP there is no response (protocol section 2), and on serial it is an empty frame with n equal to 0 (protocol section 1); when sent while idle, the response to the next command is still paired as usual.
- DAP-02: DP block read (IDCODE × 3) and DP block write (SELECT × 2); count 0; the maximum number of words that fit in one packet is returned in full, and with one more word the response does not exceed the packet length; a block read from `fault_address` gives FAULT; block requests with the match and timestamp bits get a response and the link stays in sync.
- DAP-03: request lengths MPS−1, MPS, MPS+1, 2×MPS, packet size−1, packet size (MPS is the USB endpoint maximum packet size; 64 for other links), response lengths MPS−4, MPS, packet size, 8 bytes, 20 times each. A request of exactly the packet length is followed by a zero-length packet from the host (protocol "Notes for USB hosts").
- DAP-04: each DAP_Info ID, DAP_HostStatus, DAP_SWJ_Pins read, DAP_Delay; ExecuteCommands echoes the command count; two consecutive QueueCommands packets plus one ExecuteCommands packet give three 0x7F responses in order; the undefined command 0x3F answers only `FF`.
- DAP-05: ExecuteCommands builds a response of exactly n bytes (one DAP_Transfer that writes TAR and reads DRW, a second DAP_Transfer with one DP read for even lengths, and HostStatus for the rest); every n from 12 to the packet size is requested once, and the lengths whose frame on the link is one less than, equal to or one more than a multiple of the USB packet size (64 for links without a USB endpoint) are requested 5 times. On serial the frame is the response plus the 19-byte Topic header and checksum. Every response must arrive complete within 1 s; a missing length is recorded, and one DAP_Info realigns the link before the scan goes on. This checks the whole link: a USB-to-serial bridge may hold a frame that is an exact multiple of 64 bytes until the next short packet, which only a scan over every length finds.

Mapping from behavior-suite branches to the cases of this specification. A branch marked N/A either describes the host-side scheduling model or needs a wire error injected on a healthy target, and is not an online probe test:

| Branch | Case |
|---|---|
| `consistency.hash_match` | SWD-01 |
| `consistency.hash_mismatch` | SWD-01 |
| `consistency.repeatability_fixed_seed` | N/A: scenario generation model |
| `consistency.result_schema_complete` | N/A: this tool uses the fixed result.json format instead |
| `schedule.strict_homogeneous_block_only` | N/A: host scheduling model |
| `schedule.run_split_block_friendly` | N/A: host scheduling model |
| `schedule.packet_cap_transfer_only` | N/A: host scheduling model |
| `schedule.packet_cap_block_allowed` | N/A: host scheduling model |
| `schedule.threshold_lt_four_transfer` | N/A: host scheduling model |
| `schedule.threshold_ge_four_block` | N/A: host scheduling model |
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
| `dap.transfer.abort_mid_packet` | N/A: on USB the order of abort and packet execution cannot be controlled |
| `dap.transfer.response_overflow_guard` | DAP-01 |
| `dap.transfer.request_underflow_guard` | DAP-01 |
| `dap.transfer.ack_wait` | N/A: needs the target to keep returning WAIT |
| `dap.transfer.ack_fault` | DAP-01 |
| `dap.transfer.ack_protocol_error` | N/A: needs wire error injection |
| `dap.transfer.parity_error` | N/A: needs wire error injection |
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
| `dap.block.ack_wait` | N/A: needs the target to keep returning WAIT |
| `dap.block.ack_fault` | DAP-02 |
| `dap.block.parity_error` | N/A: needs wire error injection |
| `transport.usb.single_packet_out` | DAP-03 |
| `transport.usb.single_packet_in` | DAP-03 |
| `transport.usb.multi_packet_out` | DAP-03 |
| `transport.usb.multi_packet_in` | DAP-03 |
| `transport.usb.boundary_eq_mps` | DAP-03 |
| `transport.usb.boundary_gt_mps` | DAP-03 |
| `transport.usb.in_busy_deferred` | SWD-02 |
| `transport.usb.response_queue_reuse` | DAP-03 |
| `transport.usb.request_queue_pressure` | SWD-02 |

### 4.3 Host tools

The host tools × operating system × link form a matrix: each run fixes one combination, and Windows and Linux are each run once. OpenOCD uses the `usb_bulk` backend over USB and selects the probe by serial number; over TCP it uses `cmsis-dap backend tcp` (on Windows with `cmsis-dap quirk enable` and `cmsis-dap tcp min_timeout 10` added); over USB/IP on Linux the tool runs `usbip attach` and detaches afterwards; a serial probe is converted to TCP by `xrdap share openocd`. probe-rs and pyOCD only accept a local USB probe.

TOOL-01 OpenOCD. 20 strict attaches (`--attach-count`): exit code 0, the log reports the core (`Examination succeed` or `processor detected`), and there is no `Examination failed`; retry errors of the transport do not affect this item, failed data transfers show in the following steps. Then `load_image`, `verify_image` and `dump_image` of 16 KiB in the scratch block area, with the read-back matching the written data; the dump region is read twice and the two reads are identical; when there is a flash_test region, 16 KiB of random data is erased, written and checked with `verify_image`. Each step is timed with Tcl `clock milliseconds`.

TOOL-02 probe-rs, TOOL-03 pyOCD: the same number of attaches, readout of the dump region, download to the flash_test region and verification (pyOCD compares by reading back after the download).

TOOL-04 Keil MDK (manual): in the Debug settings select CMSIS-DAP Debugger, select the probe by serial number, SWD, fixed clock; confirm that the target is recognised, view the scratch address in the Memory window, and Download with Verify checked. Record the uVision and pack versions, the download time and the verification result. Keil only accepts a local USB probe; other links are recorded as n/a.

### 4.4 Target

TGT-01 Identification: DPIDR (matching the target profile), DP version, AP0 IDR, the core indicated by CPUID, whether DWT has a cycle counter.

TGT-02 Memory: on the RW word, an 8-bit write at offset 1 and a 16-bit write at offset 2, reading back 0xCDEFAB00 (checks the byte lanes); a 4 KiB random block reads back identical to what was written.

TGT-03 Core control: halt through DHCSR and resume; set DEMCR.VC_CORERESET and then AIRCR.SYSRESETREQ, the core stops at the reset vector; with `--nreset-wired`, additionally pull nRESET low for 50 ms with DAP_SWJ_Pins and confirm that the core resets. At the end VC_CORERESET is cleared, debug is exited and the fixture is rewritten.

TGT-04 multidrop (manual): XR-DAP does not implement SWD multidrop target selection, and every probe is recorded as unsupported; if a multidrop target (for example RP2040) is connected with a host tool, the report of the tool is recorded.

### 4.5 XR vendor extension

VND-01: the DAP_Info vendor is `XRobot`, the serial starts with `XRobot-XRDAP-`, and there is a firmware version; INFO is 22 bytes, protocol version 2, at most 32 items, depth 8, feature bits include READ, WRITE, sampling and 8-byte; the probe time increases.

VND-02: one READ reads u8, u16, u32, u64 and the three-level pointer chain, with values matching the fixture; status matrix: null pointer 0x30, pointer read at `fault_address` 0x10, pointer address unaligned 0x20, final address not aligned to size 0x2F, second-level pointer fault 0x11, final access fault 0x1F, and the normal item after them 0x00 with the correct value; count 0 returns 0x01; 33 items return 0x03; 32 items of 8 bytes (a 355-byte response, which fits in any packet size) succeed. The case where the response space is not enough is checked by VND-14.

VND-03: WRITE of 1, 2, 8 and 4 bytes reads back identical; null pointer 0x30, second-level pointer fault 0x11, final write fault 0x1F, and the normal write after them takes effect; count 0 returns 0x01.

VND-04: SUB_START with period 0, reserved flag bits, count 0, div 0, phase ≥ div, or size 3 returns 0x01, and 33 items returns 0x03; INFO with one extra byte, SUB_CTRL op 1 (removed in version 2), an undefined op, a missing parameter, and READ with a wrong length return 0x01; SUB_READ while not sampling returns 0x02 with a complete response header; SUB_READ while sampling returns 0x00, and a further SUB_START returns 0x02; stopping while stopped returns 0x00; after `DAP_Disconnect`, READ and SUB_START return 0x04.

VND-05: the median round trip of single-item READ and WRITE; items per second of 1, 8 and 32 item READ; cost per pointer-chain level: 8 items each dereferenced through a self pointer 0, 1, 2, 4 and 8 levels, with (8 levels − 0 levels) / 64 giving the time per level. Pass: the 8-level self-pointer chain resolves successfully.

VND-06: for the 1, 4, 8 and 32 variable groups, the minimum period is found by the rule of section 2.3 (decision B9: the probe does not estimate the time needed, and only whether it keeps up is looked at). Pass: a minimum period is found for every group; the minimum period is recorded in the result for A/B comparison.

VND-07: the 8-item divider group runs for 5 s. Pass: the set of due items in each frame equals the set where `(k + phase) % div == 0`; the constants and the pointer chain have correct values; no failed items; skipped ticks, dropped frames and tick gaps are all 0; the counter only increases; the number of occurrences of each item differs from tick count / div by no more than 1.

VND-08: a single counter item, period 1 ms, run for 10 s. The minimum, maximum and absolute p99 of the timestamp deviation (frame timestamp − first frame timestamp − Δk × period) are recorded; when the counter is CYCCNT, the p99 of the deviation of the actual target-side sampling interval is computed from the CYCCNT difference of adjacent frames and the target clock derived from the whole run. The pass criteria are the applicable ones of VND-07.

VND-09: 8-variable group, with a 2 ms period started and then SWD lowered to 100 kHz, run for 2 s: the skipped-tick count is greater than 0; the number of ticks missing before the first frame and between frames is not greater than the skipped-tick count (ticks skipped after the last frame cannot be seen and are counted as skipped_not_seen); in overwrite mode with no reads for 2 s, the dropped-frame count is greater than 0; in stop-when-full mode with no reads for 2 s, the tick of the first frame read is 0 and the dropped-frame count is greater than 0; with max_bytes 20 no frame is returned and the remaining bytes are not 0; during sampling DAP_Transfer answers only `FF`, and DAP_Info and READ work normally. These counters are the basis of the "keeps up" judgement prescribed by decision B9.

VND-10: through vendor WRITE/READ access to DHCSR, DEMCR and AIRCR: halt, resume, and reset with the core stopping at the reset vector; the milliseconds from issuing to DHCSR reflecting the state are recorded. Pass: all three states are observed within 1 s.

VND-11 (protocol 3.6, decision C5): set the Prot of the AP CSW to unprivileged data access (`0x01`, the initial value on some chips after debug power-up), then read CPUID and DHCSR with vendor READ. Pass: both statuses are 0x00 and CPUID equals the value read with privileged access. Basis: when HPROT1 of the CSW is 0, the probe sets HPROT0, HPROT1 and the debug master bit on an AHB MEM-AP (the same as OpenOCD's CSW_AHB_DEFAULT) and keeps other AP types unchanged; the tool records the CSW written and the IDR of the AP.

VND-12: the 4-variable group samples for 0.5 s and stops; immediately SUB_START another 2-item group, and read after 50 ms. Pass: no dropped frames, the first frame of the new run is at tick 0, and all frames contain only the new items with correct values.

VND-13 (XRDAP-ESP): the response length of 0x86 op 0 is 11 + hotspot name length, and the flags, RSSI, source of the last clear and reset reason are recorded; an undefined op returns 0x01; `86 02` without the confirmation byte, or with a confirmation byte other than 0xA5, returns `86 01` and the Wi-Fi credentials stay unchanged (decision A4, protocol 3.7). Clear and save with the confirmation byte change the settings and restart the probe, and fall under RES-04 and the manual steps.

VND-14 (protocol 3.2, decision B8): in a `DAP_ExecuteCommands`, several `DAP_Info(0xFF)` are placed first to take up response space, and when only about 50 bytes remain, a READ that needs a 91-byte response (8 items of 8 bytes) is placed. Pass: the answer to the READ is `81 03`, the whole response does not exceed the packet length, and the link afterwards stays request-response. Then the remaining space is made exactly 1 byte (using a `DAP_Info` whose response length is odd, adding `DAP_HostStatus` if necessary): the answer to the READ is a single `FF`, and the whole response fills exactly one packet.

VND-15 (protocol 3.1, serial): after sampling with a 100 ms period is started over serial, the port is kept open but no request is sent for 7 s. Pass: afterwards the remaining bytes of the buffer no longer grow within 1 s (sampling has stopped because of the 5 s lease), and the frames in the buffer can still be read.

### 4.6 Stability

STAB-01: the 8-item divider group runs for `--long-s` (default 3600 s), with the pass criteria of VND-07.

STAB-02: in the flash_test region, loop "write random 16 KiB and verify" for `--long-s`. Pass: every verification succeeds; logs of failures are saved separately.

STAB-03 (assisted): during 1 ms sampling of the 4-variable group, unplug the probe USB (or USB serial), and plug it back after 2 s. Pass: it responds again within 30 s; the remaining bytes of the buffer no longer grow (sampling has stopped); after reconnecting, SUB_SET succeeds and at least 500 frames arrive within 1 s. The recovery time is recorded.

STAB-04 (assisted): power off the target during sampling. Pass: while the power is off the probe still answers SUB_READ (the failed-item count is recorded in the result); after power is restored, reconnecting and sampling work normally.

STAB-05 (assisted): turn off the hotspot and confirm that the probe is unreachable; start timing immediately after turning the hotspot on and record the time until the probe answers DAP_Info again. Pass: reachable within 300 s, and sampling works normally afterwards.

STAB-06 (protocol 3.1, decision B2): a child process connects on the same link, starts sampling and exits directly, without stopping, Disconnect or closing. Pass: the next client on the same link can stop sampling, and sampling works normally after reconnecting.

STAB-07 (assisted): reset or power-cycle the probe during sampling. The tool starts timing when the probe stops responding and stops at the first DAP_Info response, which is the boot time (when `--restart-cmd` returns only after the restart has begun, timing starts at the end of the command). Pass: it responds within 60 s, the probe time starts again from zero (the probe really restarted), and sampling works normally afterwards.

STAB-08 (decision A4, serial + `--link2` over TCP or USB): a serial session must not restart the probe or lose the Wi-Fi credentials. The steps are executed in order: a Python serial session is held for `--share-hold-s` (default 30 s); `xrdap share openocd` is left idle for the same time and then ended with Ctrl-C; `xrdap share` with OpenOCD connecting `--attach-count` times and one SRAM read and write, then ended with Ctrl-C; `xrdap share` left idle and then the process is killed directly. After each step, 0x86 op 0 (hotspot name, source of the last clear, reset reason) and the probe clock are read over the second link (TCP or USB, never the serial port; wall-clock time minus probe time gives the boot instant); while the serial session has not ended, the second link gets "busy", the tool waits for it to end before reading, and afterwards ends its own session with `DAP_Disconnect`. Pass: after each step the hotspot name is unchanged and the boot instant changes by no more than 2 s. If the probe has no saved Wi-Fi credentials, this is recorded as n/a.

### 4.7 Multiple clients

MC-01 (protocol section 2, TCP): after client A connects and starts sampling, client B connects to the same port. Pass: B is not served (connection refused or no response), A is still answered, and sampling continues.

MC-02 (protocol 3.1, decision B2, `--link2`): two links A and B (for example USB and TCP) connect to the same probe, A connecting first and becoming the session owner. Pass: the DAP_Info of B is answered as usual, the `DAP_Transfer` of B gets `05 FF`, and the READ of B gets `81 05`; A continuously reads the u32 and the pointer chain in the fixture for 5 s (while B keeps sending commands), and not a single value is wrong.

MC-04 (protocol 3.1, extension of decision B2, `--link2`): after A connects and owns the session, B sends the free commands: DAP_HostStatus answers `01 00`, INFO answers 22 bytes with status 0, and when the probe has the Wi-Fi feature bit (INFO features 0x10) `86 00` has status 0; when the DAP_Info product is `XR-DAP-V307` (the radio probe, protocol section 4) `9F 13` has status 0, while B's `9F 30 9F 13` (read the peer's settings, which uses the radio link that may carry another session) and `9F 14 xx` (write settings) get `9F 05`; A's read still runs; B's `DAP_Transfer` gets `05 FF` and its READ `81 05`; one 0x7F container from B (DAP_Info, HostStatus, INFO, DAP_Transfer) answers 4 commands, the first three normally and DAP_Transfer with `05 FF`, and A is still the owner afterwards. After A's `DAP_Disconnect` there is no owner; B sends the free commands again, and then A's `DAP_Connect` answers `02 01` (B did not become the owner).

MC-03 (protocol 3.1, decision B2, `--link2`): A starts sampling, and the READ of B gets `81 05`; A stops sampling and sends `DAP_Disconnect` (the session ends); B reads the frames left by A with SUB_READ (status 0x02, with data), then connects and starts sampling for 1 s. Pass: every step has the status above, and B samples at least 500 frames.

### 4.8 Wireless board pair

On the `radio` link, the cases marked `all` in section 3 are all executed over the radio, and throughput and round-trip time are the wireless performance.

RAD-01: read the diagnostic statistics of the wireless firmware (command 0x9F 0x11), run 5 passes of 4 KiB block read/write, then read the statistics again. Retransmissions / commands, failure count and maximum round-trip time are recorded. Pass: data matches and the failure count is 0.

RAD-02 (assisted): read the pairing settings (0x9F 0x13), power both boards off and on again, then read again. Pass: still paired, and the link id is unchanged.

RAD-04 (manual, decision B6): after sampling is started over the radio, in turn have the host change the session number (reopen the session), send session number 0, and unplug the host board for more than 3 s, and read frames through the remote side each time. Pass: in all three cases the remote side ends the session and stops sampling, and the cached response is cleared. The tool becomes automatic once the wireless firmware publishes how to read the session number.

RAD-03 (manual): run PERF-02 and RAD-01 at 1 m, 5 m, 10 m and behind a wall, then repeat with busy 2.4 GHz Wi-Fi within 1 m; distance, obstruction, interference source, retransmission rate and read throughput are recorded.

### 4.9 Firmware and resources

RES-01: from the section table of the probe firmware ELF, count the Flash usage (allocatable sections with content) and the static RAM (writable sections and NOBITS sections).

RES-02 (manual): on F103 and CH32, fill the stack with a fixed value at boot, and after SWD-01, PERF-02 and VND-06 have run, read the deepest overwritten position with another debugger without halting the core; the heap end is taken from `__sbrk_heap_end` or the FreeRTOS minimum ever free heap. On ESP, read `heap_caps_get_minimum_free_size` and the stack high-water mark of each task on the command line. The remaining bytes at the worst point are recorded.

RES-03 (manual): the debug firmware toggles an idle GPIO before and after DAP command handling and the sampling tick, and a logic analyser measures the duty cycle during PERF-02 and during minimum-period sampling.

RES-04 (manual): update the firmware by the normal route of each probe (ESP web flashing or `esptool write_flash 0x0 merged.bin`; CH32 `tools/dfu_update.py`; F103 SWD flashing of the release image); the time from start to a DAP_Info response is recorded, and the retention of settings is confirmed: on ESP the Wi-Fi credentials (0x86 op 0 shows the same hotspot name), and the clear source and the reset reason can be read (decision A4); on CH32 the pairing and role (0x9F 0x13). The boot time itself is measured by STAB-07.

RES-05 (manual): a USB ammeter in series with the probe records the current when idle, during PERF-02 and during minimum-period sampling; after PERF-02 has looped for 10 minutes, the hottest point on the board and the room temperature are recorded.

### 4.10 Lights and buttons

All manual, confirmed by eye on each probe, with the rules in the "Status light" section of this repository's README.

UI-01: purple when powered up with no host connected; red when a USB host is connected; blue with only a network client (ESP); blue for a wireless session (wireless board pair).

UI-02: the light shows an activity pulse during DAP communication; after OpenOCD `init; halt`, the main colour and purple alternate every 2 s, and after `resume`, red and blue alternate every 1 s; it returns to normal after `DAP_Disconnect` or 5 s without commands.

UI-03: on the Lite board, holding the GPIO21 button for 5 s gives a fast purple blink and then solid purple, the Wi-Fi credentials are cleared and the board restarts (0x86 op 0 has no hotspot name); the BOOT button (GPIO0) of the default S3 board does not clear any setting while running (decision A4); on a wireless board pair, long-pressing KEY2 on two unpaired boards gives a purple blink, and when done both boards report the same link id, and an unpaired board is not taken as the remote side (decision B3).

## 5. Result records

Each run (one probe, one link, one target) writes one directory with the following contents:

| File | Content |
|---|---|
| `result.json` | Run information and, for each case, the status, checks and metrics |
| `result.md` | One line of run information and the case table |
| `log.txt` | Complete process log |
| `openocd/` | Configuration and logs of the host tools, and the files read out |

The run information must include: the probe (DAP_Info product, serial, firmware version, packet size and count, vendor extension INFO), the link, the target, the host (operating system, Python version), the firmware, libxr and XRDAP-Core commit of the image under test (`--build fw=…,libxr=…,core=…`), the host tool versions, and the log path. The case status is pass, fail, error (tool or link exception), n/a (not applicable, with the reason) or manual (to be recorded by hand).

Several runs are combined into one table with `table`: the upper part has one row per run, and the lower part has one row per case and one column per run.

| run | probe | link | target | host | firmware | libxr | core | tools | log |
|---|---|---|---|---|---|---|---|---|---|
| R1 | | | | | | | | | |

| case | R1 | R2 | … |
|---|---|---|---|
| SWD-01 | | | |

The A/B comparison (`ab`) alternately runs two images (A, B, A, B) with the same settings, and the table lists, for each numeric metric, the mean over the rounds of A and of B and B/A.
