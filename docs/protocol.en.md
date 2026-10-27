# XR-DAP Link Framing and XR Vendor Extension

[中文](protocol.md)

This document describes the protocol between the XR-DAP probes (XRDAP-ESP, XRDAP-F103C8, XRDAP-CH32V307) and host tools: the frame format XRDAP-ESP uses to carry CMSIS-DAP packets over serial and TCP, the XR vendor extension commands every probe implements (protocol version 2; 0x80 to 0x85 are data commands; 0x86 is Wi-Fi configuration, XRDAP-ESP only) and the rule that one transport owns the probe at a time, the probe identity and mDNS advertisement, how the ESP32-C3 USB port separates the DAP link from the command line, the USB/IP virtual device, and the zero-length packet USB hosts must send. DAP packets on USB use the CMSIS-DAP v2 Bulk endpoints without any extra framing. These rules are implemented in `xrdap/` of this repository: serial frames in `xrdap/serial/`, the session owner in `xrdap/session/`, the vendor extension in `xrdap/vendor/`.

All integers are little-endian. Spaces in hexadecimal examples are for readability only.

Contents: 1 Serial frames; 2 TCP frames; 3 XR vendor extension (3.1 rules and session owner, 3.2 status and reply space, 3.3 items, 3.4 commands, 3.5 sampling, 3.6 SWD access, 3.7 0x86, 3.8 byte examples); 4 Probe identity and discovery (DAP_Info, mDNS); 5 The ESP32-C3 USB port; 6 USB/IP virtual device; 7 Notes for USB hosts.

## 1. Serial frames

The serial frame has the same format as libxr's Topic packed frames. On the firmware side it is implemented by `xrdap/serial/` of this repository (`Framer` assembles frames, `SerialLink` sends and receives); on the host side by the `xrdap` library of XRDAP-Host (`transport::serial`).

```
prefix 0x5A(1) + data_len(3) + topic_crc32(4) + timestamp_us(6) + version(1) + header_crc8(1) + payload(data_len) + crc8(1)
```

| Field | Content |
|---|---|
| prefix | Fixed `0x5A` |
| data_len | Payload length in bytes, 24 bits |
| topic_crc32 | CRC32 of the topic name. The request topic is `dap_req` (`0xDC8309C5`), the response topic is `dap_resp` (`0x89E1A688`) |
| timestamp_us | 48 bits. The probe fills in its own microsecond time in responses; requests carry 0 and the probe ignores it |
| version | Fixed `0x01` |
| header_crc8 | CRC8 of the first 15 bytes |
| payload | `n(2) + DAP packet(n)`, where n is the DAP packet length; no padding |
| crc8 | CRC8 of the header and payload |

CRC8 uses the reflected polynomial `0x8C` with initial value `0xFF`. CRC32 uses the reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF` and no final inversion (the value for `"123456789"` is `0x340BC6D9`).

Example: the complete frame for the request `DAP_Info(0xFF)` (query packet size; the DAP packet is `00 FF`):

```
5A 04 00 00 C5 09 83 DC 00 00 00 00 00 00 01 B1 02 00 00 FF 32
```

The probe parses frames by these rules:

- Frames whose length is a multiple of 64 are followed by one 0x00 pad byte; receivers ignore bytes before 0x5A. A USB-serial bridge (CP210x, CH343, USB Serial/JTAG) carries data to the host in 64-byte full-speed bulk packets, and data that fills whole packets reaches the host program only when the next short packet arrives; the pad makes the last packet short. The probe pads its response frames this way.
- If the header CRC8 is wrong, version is not 1, the topic is not `dap_req`, or data_len is smaller than 2 or larger than 2 + the packet size (from DAP_Info), the current candidate frame is discarded and parsing restarts at the next `0x5A` in the buffer.
- A wrong trailing CRC8 likewise discards the frame and resynchronises.
- A frame with n larger than the packet size or n + 2 larger than data_len is discarded without a response.
- A discarded request gets no response; the host handles it as a timeout. When `xrdap share` gets no response from the probe within the response timeout (`--timeout-ms`, default 2000 ms), or a serial, USB or TCP read or write to the probe fails, it ends that debug tool's connection and reports the reason. `DAP_TransferAbort` (0x07) is forwarded to the probe as usual; it has no response under CMSIS-DAP, so `xrdap share` sends nothing back to the debug tool, and over serial the host discards the probe's n = 0 frame.

Requests and responses correspond one to one. The host sends one request at a time and sends the next after receiving the response. For a command with no response content, n in the response frame is 0.

The firmware baud rate is 4 000 000 (`kSerialBaud` in XRDAP-ESP `firmware/esp32sx/main/config.hpp` for the S2 and S3; the C3 uses USB Serial/JTAG, where the baud rate has no effect; the classic ESP32 is limited to 921 600 by its on-board USB-UART chip, see `firmware/esp32/main/config.hpp`), 8 data bits, no parity, 1 stop bit, no flow control.

A serial port has no notion of connecting or disconnecting. While serial owns the probe, 5 seconds without any request end the session, see section 3.1.

## 2. TCP frames

The probe's TCP port 3241 and the listening port of `xrdap share openocd` use the same frame, matching OpenOCD `cmsis-dap backend tcp`:

```
"DAP\0"(4) + length(2) + type(1) + reserved(1) + DAP packet
```

Type 1 is a request, type 2 is a response; the reserved byte is 0. The length is the number of DAP packet bytes that follow.

- Only one client is served at a time; requests are executed and answered in order. When the client disconnects the TCP session ends (section 3.1).
- The probe closes the connection when it receives an invalid header (wrong magic, type other than 1, length 0 or larger than 1024).
- A command with no response content (for example `DAP_TransferAbort`) gets no TCP frame back.
- `xrdap share openocd` strips the 8-byte header from a TCP frame, hands the DAP packet to the probe (wrapped in a serial frame when the link is serial), and converts responses back; it does not interpret DAP content. After the client disconnects it sends `84 00` (stop sampling) to the probe once.

## 3. XR vendor extension

This section is protocol version 2 and is not compatible with version 1: version 1's SUB_SET and SUB_CTRL start are merged into SUB_START, and INFO no longer reports an access time. The host sends INFO first and does not use the extension unless the version is 2.

### 3.1 Rules

- The commands use the CMSIS-DAP vendor command range: 0x80 to 0x85 are data commands (sections 3.2 to 3.6), 0x86 is Wi-Fi configuration (section 3.7, XRDAP-ESP only). USB, TCP, serial and radio carry the same packets.
- A command the probe does not support is answered with the single byte `0xFF`, the standard CMSIS-DAP behaviour. A host that gets `FF` for INFO knows the probe does not implement the extension.
- SWD only. The target is treated as little-endian; values are the raw bytes in target memory.
- The host resolves variable addresses (symbol table, DWARF); the probe only handles addresses.
- Before using the extension commands, the host connects with `DAP_Connect` in SWD mode (port 0 or 1) and makes sure the target is reachable. The probe does not connect on the host's behalf; without a connection in the current session READ, WRITE and SUB_START return 0x04. After `DAP_Disconnect` or the end of the session, `DAP_Connect` is needed again.
- After READ, WRITE or sampling, the host must treat DP SELECT, AP CSW and TAR as unknown and write them again. OpenOCD caches these registers, so the extension commands and OpenOCD are not mixed on one connection.

#### Session owner

At any time one transport (USB, serial, TCP, radio remote) owns the probe and SWD; that transport is the session owner. Commands fall into two classes:

| Class | Commands |
|---|---|
| No ownership needed | `DAP_Info` (0x00); `DAP_HostStatus` (0x01, status light only); the `DAP_ExecuteCommands` (0x7F) and `DAP_QueueCommands` (0x7E) containers themselves, each command inside classed on its own; vendor INFO (0x80); 0x86 op 0 Wi-Fi status query (XRDAP-ESP); 0x9F 0x13 reading this probe's radio settings, pairing and role (XRDAP-CH32V307; 0x9F 0x30, forwarded over the radio, needs ownership) |
| Ownership needed | every other command: connect, SWD, SWJ, reset and every standard command that touches the target, READ, WRITE, sampling (SUB_START, SUB_CTRL, SUB_READ), settings writes (0x86 save and clear; 0x9F role, settings, pairing and forwarding) |

Commands that need no ownership run from any transport, never make that transport the owner and never make another transport busy. When there is no owner, the first transport that runs a command needing ownership becomes the owner and starts a session. From then on, commands needing ownership from any other transport are answered busy and not run:

| Command | Reply |
|---|---|
| standard commands | `command FF` (DAP_ERROR) |
| vendor commands 0x80 to 0x9F | `command 05` (busy) |

With only 1 byte of reply space the reply is `FF` alone; with none there is no reply.

The session ends in the following cases; afterwards any transport may become the new owner:

| Case | Session that ends |
|---|---|
| The owner sends `DAP_Disconnect` | the owner's session |
| The TCP client disconnects, the serial link stops | that transport's session |
| USB bus reset (unplug or host reset) | the USB session |
| No serial request for 5 seconds | the serial session |
| The radio session id changes or is 0, or no ping within the lease | the radio remote's session (XRDAP-CH32V307) |

While serial owns the probe, the host sends a request at least every 5 seconds (SUB_READ will do).

USB attaching and detaching (the host opening or closing USB, a re-enumeration) belongs to USB's own session: the probe resets its SWD and DAP state for it only when USB is the owner or nobody is; while another transport owns the probe, that transport's SWD connection, clock and settings stay as they are.

#### Sampling

Sampling belongs to the session that started it:

- From start to stop, only `DAP_Info` (0x00), `DAP_HostStatus` (0x01) and 0x80 to 0x86 are accepted. Every other command is answered with the single byte `0xFF` and not run.
- READ and WRITE may be used while sampling; they take turns with sampling on SWD and can cause skipped ticks.
- Sampling stops on SUB_CTRL stop (`84 00`) or when the session that started it ends. The frames left in the buffer stay; the next session can still read them with SUB_READ until the next SUB_START or clear.

### 3.2 Requests, responses and status

A request is `command byte + arguments`; a response is `command byte + overall status + data`, with the command byte echoed. When the overall status is not 0 the response has only these two bytes, except for SUB_READ's 0x02 (see section 3.4).

A request whose length does not match the count field in the command returns 0x01 and is not executed.

Reply space: before a vendor command runs, the probe works out from the request how many bytes its reply needs and compares that with the space it may write this time. The space is the packet size from DAP_Info (1024 bytes on XRDAP-ESP and XRDAP-CH32V307, 512 bytes on XRDAP-F103C8); inside `DAP_ExecuteCommands` it is the part of that packet not used yet. Vendor commands may sit inside `DAP_ExecuteCommands` and `DAP_QueueCommands`, as in the CMSIS-DAP reference implementation; the commands of a container are back to back and each one's length follows from its own content (READ, WRITE and SUB_START from the item count and the items, INFO 1 byte, SUB_CTRL 2, SUB_READ 3, 0x86 by operation: query 2, save 3 + ssid_len + 1 + pw_len, clear 3); a container with a length that cannot be worked out answers `7F FF` as a whole. With less than 2 bytes of space the reply is `FF` alone; when the whole reply does not fit it is `command 03`; in both cases the command does not run and nothing is written past the given space. The READ reply is 3 + Σ(3 + size) bytes, WRITE 3 + 3 × count, INFO 22, SUB_START and SUB_CTRL 2, SUB_READ at least 24.

| Overall status | Meaning |
|---|---|
| 0x00 | Success; see the item status for each item |
| 0x01 | Invalid argument: length mismatch, count of 0, size not 1/2/4/8, more than 8 dereferences, period of 0, div of 0 or phase ≥ div, reserved flag bits not 0, undefined SUB_CTRL op |
| 0x02 | State not allowed: SUB_START while sampling; SUB_READ while not sampling (the reply still carries data) |
| 0x03 | Insufficient resources: more than 32 items, the reply does not fit the space left, more than 8 distinct APs in one sampling run, the frame with every item due does not fit the buffer |
| 0x04 | No target connected: no `DAP_Connect` in this session yet, or reading an AP's CSW failed |
| 0x05 | Busy: another transport owns the probe (section 3.1) |

Item status (one byte per item for READ and WRITE; in sample frames only 1 bit marks success or failure):

| Item status | Meaning |
|---|---|
| 0x00 | Success |
| 0x10 + k | SWD returned FAULT or timed out while reading the pointer at level k (0 to 7) |
| 0x1F | SWD failure on the final read or write (including a sticky error detected after a write) |
| 0x20 + k | The read address at level k is not 4-byte aligned |
| 0x2F | The final address is not aligned to size (4-byte alignment when size is 8) |
| 0x30 + k | The pointer read at level k is 0; stop |

### 3.3 Items

An item describes the location of one variable, through at most 8 levels of pointers.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | ap: MEM-AP index |
| 1 | 2 | id: chosen by the host, echoed in the response |
| 3 | 1 | size: 1, 2, 4 or 8 |
| 4 | 4 | base: start address |
| 8 | 1 | n: number of dereferences, 0 to 8 |
| 9 | 2xn | deref_off[i], i16, i = 0 to n-1 |
| 9+2n | 2 | final_off, i16 |

An item is 11 + 2n bytes. A WRITE item is followed by `size` bytes of value; a sampling item is followed by `div(2)` and `phase(2)`; a READ item has no extra fields.

The probe computes the final address as follows:

```
cur = base
for i in 0..n-1:
    addr = cur + sign_extend(deref_off[i])      // 32-bit wraparound
    addr not a multiple of 4  -> item status 0x20+i, stop
    p = read32(addr)                             // SWD failure -> 0x10+i, stop
    p == 0                    -> item status 0x30+i, stop
    cur = p
addr = cur + sign_extend(final_off)
addr not a multiple of size (4 when size is 8) -> 0x2F, stop
read or write size bytes                         // SWD failure -> 0x1F
```

With n equal to 0 the item accesses `base + final_off` directly. A base of 0 is legal (address 0 is valid on Cortex-M); only a pointer read back as 0 counts as null. Array indexing and linked-list traversal are expanded by the host into multiple items.

### 3.4 Commands

#### 0x80 INFO

Request: `80`

The response is always 22 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | 0x80 |
| 1 | 1 | Overall status |
| 2 | 1 | Protocol version, currently 2 |
| 3 | 4 | Feature bits: bit0 READ, bit1 WRITE, bit2 sampling, bit3 8-byte access, bit4 Wi-Fi configuration (0x86); 0x1F on XRDAP-ESP, 0x0F on the others |
| 7 | 2 | Maximum items per request or sampling run: 32 |
| 9 | 1 | Maximum dereference depth: 8 |
| 10 | 4 | Ring buffer size in bytes: 16384 on XRDAP-ESP, 4096 on XRDAP-F103C8 and XRDAP-CH32V307 |
| 14 | 8 | Current probe time, µs |

#### 0x81 READ

Request: `81 count(1) item…`, count from 1 to 32.

Response: `81 status count(1)`, then per item `id(2) item_status(1) value(size)`. A failed item's value is all zeros.

#### 0x82 WRITE

Request: `82 count(1) (item value)…`, where value is size bytes in target byte order.

Response: `82 status count(1)`, then per item `id(2) item_status(1)`.

Items are executed in order; what has been written is not rolled back, and a failing item does not affect later ones. An 8-byte write is two 32-bit writes and is not atomic.

#### 0x83 SUB_START

One command carries the whole configuration, and sampling starts once it passes the checks. The probe keeps no "configured but not started" state.

Request: `83 period_us(4) flags(1) count(2) (item div(2) phase(2))…`

- period_us: tick period, greater than 0.
- flags: bit0 = 0 overwrites the oldest frame when the buffer is full, bit0 = 1 drops the new frame; the other bits must be 0.
- count: 1 to 32.

Response: `83 status`

- SUB_START while sampling returns 0x02 and does not affect the running sampling.
- Invalid arguments return 0x01; more than 32 items, more than 8 APs, or a frame with every item due that with its 2-byte length head is larger than the buffer return 0x03.
- No `DAP_Connect` in this session, or a failed CSW read of an AP, returns 0x04.
- On success the buffer and both counters are cleared, the tick index starts at 0, and tick 0 is sampled at once.

The probe does not estimate the frame time and does not refuse a period as too short. Whether sampling keeps up is judged at run time only: skipped ticks, dropped frames and the bytes left in the buffer, all in the head of every SUB_READ response (section 3.5).

#### 0x84 SUB_CTRL

Request: `84 op(1)`; op 0 stops, op 2 clears the buffer and the counters. Other ops return 0x01.

Response: `84 status`

- Stop: replies after the tick in progress has been read, and returns 0x00 also when not sampling. Frames left in the buffer can still be read with SUB_READ.
- Clear: also allowed while sampling; the counters count again from 0.

#### 0x85 SUB_READ

Request: `85 max_bytes(2)`

A 24-byte response head, followed by frames:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | 0x85 |
| 1 | 1 | Overall status: 0x00 sampling, 0x02 not sampling |
| 2 | 8 | Current probe time, µs |
| 10 | 4 | Frame bytes left unread in the buffer (length heads excluded) |
| 14 | 4 | Accumulated dropped frames |
| 18 | 4 | Accumulated skipped ticks |
| 22 | 2 | Number of frames returned |
| 24 | … | Frames |

Only whole frames are returned; their total length does not exceed `min(max_bytes, reply space - 24)`. The frame count is 0 when the buffer is empty. With status 0x02 the response still carries the full head and frames: the host learns that sampling has stopped (for example because its session ended) and keeps reading until no bytes are left. Both counters are cleared on a successful SUB_START and on op 2.

### 3.5 Sampling

#### Ticks

- The tick timer is the only clock. Its interrupt adds 1 to the tick count every period and wakes sampling only when the count reaches the next tick to sample; on waking, sampling reads the count, and the newest tick is the count value k. The time base only stamps frames.
- After a successful SUB_START the count is cleared, tick 0 is sampled at once, and one more tick comes every period.
- An item is sampled on ticks where `(k + phase) % div == 0`. A div of 1 samples every tick; phase staggers slow items. Items due in one tick are read in the order given in SUB_START.
- A tick with no item due produces no frame.

#### Frame format

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Tick index k |
| 4 | 6 | Timestamp, µs, taken when the tick starts reading |
| 10 | ceil(m/8) | Status bitmap, m is the number of items due; due item i maps to bit i (low bit first), 1 means failed |
| … | … | Values of the due items in due order, size bytes each, all zeros on failure |

The items due are derived from k and each item's div and phase, so they are not listed in the frame.

- The timestamp comes from libxr's Timebase: the probe's monotonic microsecond time, 48 bits, the same clock as the probe time in INFO and SUB_READ. The time axis is not reset when sampling starts; the tick index restarts from 0. After a probe restart the time axis starts from 0.
- Items in one frame are read one after another, so variables in the same frame are not sampled at the same instant.
- Aligning to the host clock: record the host time before and after an INFO or SUB_READ request, pair the midpoint with the probe time in the response, and take the offset; the error is about half a round trip. Every SUB_READ can refresh the offset.

#### Keeping up and skipped ticks

- When sampling wakes, every tick between the last one sampled and the current count that was not sampled counts as skipped and accumulates in the skipped tick count; the tick index then has a gap. The probe does not make up ticks and does not lower the rate or drop items by itself.
- When the next tick arrives while a tick is still being read, that tick is not made up; sampling waits for the one after it.
- After each frame, the next tick to sample is at least `busy + gap` after this frame's tick, with gap = `max(15 µs, period / 8, busy / 4)` and busy counted, with a sampling task, from the moment the tick interrupt woke sampling to the end of this frame's read, the wake, task switch and link lock included (a single-loop probe counts from the start of the read, since the wait after the interrupt is the main loop running commands); the ticks in between count as skipped.
- Sampling yields to waiting commands: a command waits from the moment it queues to run until it has run. While a command waits, sampling reads at most one more frame (the one being read when the command started waiting counts); every tick due after that is given up and counted as skipped, and once the last command has run, sampling carries on from the next tick. A command in the executor therefore waits for at most one frame, whatever the period and however tasks sit on the cores. Without overlap nothing changes; periodic SUB_READs add no skips.
- So the probe answers commands at any period: between two frames the other tasks on the same core (DAP commands, serial, TCP) get at least that gap, about 1/5 of the core or more. A period shorter than a frame only adds skipped ticks; commands still get the CPU.

#### Reading too slowly

Frames are first written to the ring buffer (its size is in INFO; each frame takes another 2 bytes for its length). When the buffer is full, the oldest frame is overwritten or the new frame is dropped according to flags.bit0; the number of frames overwritten or dropped accumulates in the dropped frame count, and gaps in the tick index also show which ticks were lost. When the unread byte count is not 0, the host should read again immediately.

Each tick takes 10 + bitmap + the sizes of the due items; averaged over div and compared with the real link speed, this estimates whether the bandwidth is enough.

The host judges keeping up from these three values and gives advice:

| Value | Meaning | Advice |
|---|---|---|
| Skipped ticks | growing: SWD cannot keep up | longer period, larger div, fewer items |
| Dropped frames | growing: reading too slowly or not enough link bandwidth | read more often, larger div, fewer items |
| Bytes left | staying close to the buffer size: frames are about to drop | as above |

### 3.6 SWD access

- DP SELECT: `ap << 24` when accessing a MEM-AP; CSW, TAR and DRW are in bank 0. The AP's IDR is read with bank 0xF (register 0xFC).
- CSW: the CSW of an AP is read once the first time a READ or WRITE command uses that AP, and at SUB_START; it is not read again within the same command or the same sampling run. While sampling is active, READ commands may reuse a CSW base successfully read at SUB_START or by an earlier READ; access width and increment mode are still programmed again, and target data is read on every request. A transfer failure, resynchronization, or a new sampling run invalidates this reuse. WRITE always clears the command cache first; outside active sampling, READ also continues to read CSW for every command. When HPROT1 (bit 25) of the CSW read is 0, the AP's IDR is read too: when the IDR class (bits 16:13) is 8 (MEM-AP) and its type (bits 3:0) is 1, 5 or 8 (AHB3, AHB5, AHB5 with enhanced HPROT), bit 24 (HPROT0, data access), bit 25 (HPROT1, privileged) and bit 29 (debug master) of Prot are set, the same bits as OpenOCD's `CSW_AHB_DEFAULT`; the other Prot bits (cache, buffer, secure attributes) keep the value read. Other AP types (for example AXI-AP, APB-AP) use these bits differently and keep them. Accesses write `(that value & ~0x37) | Size`, i.e. Size (bits 2:0) and AddrInc (bits 5:4) cleared, the access width set and auto-increment off. After one write HPROT1 in the AP is 1, so the IDR is not read again.
- Reading a word: write TAR, read DRW, then read DP RDBUFF for the result. Items are accessed one by one in order.
- Writing a word: write TAR, write DRW, then read DP CTRL/STAT and check STICKYORUN, STICKYERR and WDATAERR.
- 1- and 2-byte accesses: CSW Size is set to 8 or 16 bits; the value sits on the DRW byte lanes selected by the low address bits, and the probe moves it to the low bytes of value.
- 8 bytes: two 32-bit accesses, low address first, not atomic; a running target may yield an inconsistent value. Aligned accesses of 4 bytes or less are single accesses.
- WAIT responses: retried up to the count set by `DAP_TransferConfigure`; exceeding it is a failure.
- After any access with FAULT, a timeout or a sticky error, DP ABORT = `0x0000001E` (STKCMPCLR, STKERRCLR, WDERRCLR, ORUNERRCLR) is written before continuing with the next item.
- Memory is read and written through the MEM-AP; the core does not need to be halted.

### 3.7 0x86 Wi-Fi configuration

XRDAP-ESP only. Query, save and clear the Wi-Fi credentials through DAP itself (USB, serial or TCP), without a separate serial terminal. Supported only when INFO feature bit 4 is 1. The credentials are stored in NVS (namespace `rmlink`, key `wifi`), the same data that the serial console command `wifi set` saves.

The command does not touch the target and does not require `DAP_Connect`; it also works during sampling. A request is `86 op(1) parameters`; byte 1 of the response echoes `86` and byte 2 is the status.

| Status | Meaning |
|---|---|
| 0x00 | Success |
| 0x01 | Invalid argument: undefined op, request length does not match the op, SSID empty or longer than 32 bytes, password non-empty and not between 8 and 63 bytes (WPA password length) |
| 0x03 | Insufficient resources: NVS read or write failed, or the response buffer cannot hold the status response |

#### op 0 query

Request: `86 00` (the length must be 2 bytes)

Successful response:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | 0x86 |
| 1 | 1 | Status 0x00 |
| 2 | 1 | Flags: bit0 Wi-Fi started (it starts only when credentials are saved), bit1 IP obtained, bit2 NVS was damaged at this boot and was erased and rebuilt |
| 3 | 4 | IPv4 address in network byte order (`C0 A8 01 14` is 192.168.1.20); 0 when not connected |
| 7 | 1 | Signal strength RSSI, i8, in dBm; 0 when not connected |
| 8 | 1 | SSID length n |
| 9 | n | Saved SSID; n is 0 when none is saved; the password is not returned |
| 9+n | 1 | What last cleared the credentials (kept in NVS): 0 never or unknown, 1 board button, 2 console `wifi clear`, 3 0x86 clear, 4 NVS repair |
| 10+n | 1 | Reset reason of this boot: 0 unknown, 1 power-on, 2 software restart, 3 watchdog, 4 panic, 5 brown-out, 6 other |

On failure the response is just `86` and the status.

#### op 1 save

Request: `86 01 ssid_len(1) ssid(ssid_len) pw_len(1) password(pw_len)`

The total request length must equal `4 + ssid_len + pw_len`. For an open network pw_len is 0.

Response: `86 status`. With status 0x00 the credentials have been written to NVS, and about 0.5 seconds after the response is sent the probe restarts and connects to the access point with the new credentials. During the restart the USB device re-enumerates and serial and TCP connections are lost, so the host has to reconnect.

#### op 2 clear

Request: `86 02 A5` (the length must be 3 bytes; the third byte is the confirm byte 0xA5). A missing or wrong confirm byte returns 0x01 and does nothing, so a stray short command cannot clear the credentials.

Response: `86 status`. With status 0x00 the credentials have been cleared, and about 0.5 seconds after the response is sent the probe restarts; Wi-Fi does not start after the restart.

### 3.8 Byte examples

The values below are examples only.

#### INFO

Request: `80`

Response: `80 00 02 1F000000 2000 08 00400000 40420F0000000000`

Meaning: version 2, feature bits 0x1F, at most 32 items, depth 8, 16384-byte buffer, probe time 1 000 000 µs.

#### READ, one item

Read `*(uint32_t*)(*(uint32_t*)0x20000008 + 4)`:

Item: `00 0100 04 00000020 01 0800 0400` (ap 0, id 1, size 4, base 0x20000000, n 1, deref_off 8, final_off 4)

Request: `81 01 00 0100 04 00000020 01 0800 0400`

Response (the pointer reads 0x20001000 and the value there is 0x12345678): `81 00 01 0100 00 78563412`

Response when the pointer is 0: `81 00 01 0100 30 00000000`

#### WRITE, one item

Write the 16-bit value 0x1234 to 0x20000010:

Item: `00 0200 02 10000020 00 0000`, followed by the value `3412`

Request: `82 01 00 0200 02 10000020 00 0000 3412`

Response: `82 00 01 0200 00`

#### SUB_START, two items

Period 1000 µs, flags 0, 2 items:

- Item 1: id 0x10, size 4, base 0x20000100, n 0, div 1, phase 0: `00 1000 04 00010020 00 0000 0100 0000`
- Item 2: id 0x11, size 2, base 0x20000104, n 0, div 10, phase 3: `00 1100 02 04010020 00 0000 0A00 0300`

Request: `83 E8030000 00 0200`, followed by item 1 and item 2

Response: `83 00`; sampling has started.

Item 2 is due at k = 7, 17, 27, … (`(k + 3) % 10 == 0`).

Sent again while sampling: `83 02`; without `DAP_Connect` in this session: `83 04`

#### SUB_READ

Request: `85 0004` (at most 1024 bytes, on a probe with 1024-byte packets)

A response with two frames: at k = 6 only item 1 is due, at k = 7 both items are due.

```
85 00                      command, overall status (sampling)
80610F0000000000           current probe time 1 008 000 µs
00000000                   0 bytes left unread
00000000                   0 dropped frames
00000000                   0 skipped ticks
0200                       2 frames
06000000 B0590F000000 00 07000000              k=6, timestamp 1 006 000 µs, bitmap 00, item 1 value 7
07000000 985D0F000000 00 08000000 3412         k=7, timestamp 1 007 000 µs, bitmap 00, item 1 value 8, item 2 value 0x1234
```

#### Stop

Request: `84 00`, response: `84 00`

#### Not enough reply space

Inside `DAP_ExecuteCommands` the commands before used up the packet, and 5 bytes are left when a READ whose reply needs 10 bytes comes: this READ's reply is `81 03`, and it does not run. With 1 byte left it is `FF`.

#### Another transport owns the probe

USB is the owner; a READ over serial: `81 05`; a `DAP_Transfer` over serial: `05 FF`; a `DAP_Info` or `DAP_HostStatus` over serial: answered as usual, and USB stays the owner.

#### 0x86 query

Request: `86 00`

Response (connected to access point `lab`, IP 192.168.1.20, signal -52 dBm, credentials never cleared, power-on boot): `86 00 03 C0A80114 CC 03 6C6162 00 01`

After a 0x86 clear and the software restart: `86 00 00 00000000 00 00 03 02`

#### 0x86 save

Save SSID `lab` and password `12345678`:

Request: `86 01 03 6C6162 08 3132333435363738`

Response: `86 00`; the probe restarts about 0.5 seconds later.

#### 0x86 clear

Request: `86 02 A5`, response: `86 00`; the probe restarts about 0.5 seconds later.

Without the confirm byte: request `86 02`, response `86 01`; the credentials stay.

#### Standard command during sampling

Request: `05 …` (`DAP_Transfer`), response: `FF`

## 4. Probe identity and discovery

### 4.1 DAP_Info strings

The firmware returns these strings in `DAP_Info` (`probe/probe_identity.hpp` in this repository). On probes with USB the USB device descriptor uses the same vendor and serial number.

| Info ID | Meaning | Content |
|---|---|---|
| 0x01 | Vendor | `XRobot` |
| 0x02 | Product | ESP32-S3: `XR-DAP-S3`; ESP32-S2: `XR-DAP-S2`; ESP32-C3: `XR-DAP-C3`; ESP32: `XR-DAP-ESP32`; STM32F103: `XR-DAP-F103`; CH32V307: `XR-DAP-V307`; CH32V305: `XR-DAP-V305` |
| 0x03 | Serial number | `XRobot-XRDAP-` followed by the chip's unique id in upper-case hexadecimal: the factory MAC on ESP (6 bytes from eFuse, 12 digits), e.g. `XRobot-XRDAP-0123456789AB`; the 12-byte UID on STM32F103, CH32V307 and CH32V305 (24 digits) |
| 0x04 | CMSIS-DAP protocol version | `2.0.0` |
| 0x05, 0x06 | Target device vendor, name | Not provided (empty) |
| 0x07 | Board vendor | `XRobot` |
| 0x08 | Board name | XRDAP-ESP: same as the product; XRDAP-F103C8: `STLINK-V2`; XRDAP-CH32V307: the board, e.g. `XR-DAP-WDAP`; the CH32V305 board is `CH32V305-LINKE` |
| 0x09 | Firmware version | The version of each firmware |

The serial number is the same over every link (USB, serial, TCP, ...), which lets host tools recognise one probe across them.

### 4.2 mDNS advertisement

Once the probe has an IP address it advertises itself on the LAN:

| Item | Content |
|---|---|
| Host name | `xrdap-` followed by the last 3 MAC bytes in lower-case hexadecimal, e.g. `xrdap-6789ab.local` |
| Service | `_xrdap._tcp`, on the TCP DAP port 3241 |
| Instance name | The serial number, e.g. `XRobot-XRDAP-0123456789AB` |
| TXT `serial` | The serial number |
| TXT `chip` | `esp32s3`, `esp32s2`, `esp32c3` or `esp32` |
| TXT `fw` | Firmware version, e.g. `0.5.5` |

OpenOCD accepts the host name directly in `cmsis-dap tcp host`. Host tools browse the `_xrdap._tcp` service to list the probes on the LAN and match them with the same probe on USB or serial through the TXT `serial`. An mDNS startup failure is only logged and does not affect the TCP service.

## 5. The ESP32-C3 USB port

The USB port of the ESP32-C3 is USB Serial/JTAG, which appears on the computer as one CDC serial port. The DAP link (the serial frames of section 1) and the Wi-Fi command line share this port without any mode switch. For each chunk of received data the firmware (XRDAP-ESP `firmware/common/serial/serial_mux.hpp`) does the following:

1. The chunk is first given to the DAP link. The link finds the frame header `0x5A` and checks the CRC itself; bytes that are not part of a frame are discarded.
2. If the chunk executed at least one DAP frame, or a DAP frame was executed within the last 2 seconds, the data is not given to the command line.
3. Any other data goes to the command line byte by byte. The command line produces output only after a line typed by a person ends with Enter, and a line containing control characters is dropped as a whole.

As a result DAP frames and command-line output never mix: the debug tool always receives DAP response frames, and commands such as `help` and `wifi set` keep working in a terminal. The terminal must not change DTR/RTS, otherwise USB Serial/JTAG may reset the chip. The serial link of the ESP32-S3 is on UART0 and does not use this separation; its command line is on the USB CDC virtual serial port.

## 6. USB/IP virtual device

The probe serves USB/IP on TCP port 3240 (protocol in the Linux kernel document `usb/usbip_protocol.rst`, fields big-endian). The implementation is in XRDAP-ESP `firmware/common/net/usbip_device.hpp`, and the `usbip` module of the host xrdap library is the same device model.

| Operation | Handling |
|---|---|
| `OP_REQ_DEVLIST` (0x8005) | Replies with one device, bus ID `1-1`, then closes the connection |
| `OP_REQ_IMPORT` (0x8003) | Replies with the device information when the bus ID is `1-1`, followed by URBs; any other bus ID gets status 1 |
| `USBIP_CMD_SUBMIT` (1) | Endpoint 0: standard requests and descriptors; endpoint 1: Bulk OUT executes a DAP command, Bulk IN fetches the response |
| `USBIP_CMD_UNLINK` (2) | Cancels an IN that is still waiting for data; replies -ECONNRESET when found, otherwise 0 |

Device:

| Item | Value |
|---|---|
| VID:PID, bcdDevice | 1D50:6199, 0x0200 (the real USB device of the S3 uses 0x0100; Windows caches descriptors by these three items) |
| bcdUSB, speed | 2.10 (with BOS), high speed |
| Strings | 1 `XRobot`, 2 `XR-DAP-<chip> CMSIS-DAP` (`XR-DAP-S3`, `XR-DAP-S2`, `XR-DAP-C3`, `XR-DAP-ESP32`), 3 serial number (same as DAP_Info), 4 `CMSIS-DAP v2` |
| Interface | One, vendor class 0xFF, interface name is string 4 |
| Endpoints | Bulk OUT 0x01, Bulk IN 0x81, 512 bytes |
| BOS | MS OS 2.0 platform capability, vendor request code 0x20; descriptor set of 162 bytes: compatible ID `WINUSB`, `DeviceInterfaceGUIDs` = `{CDB3B5AD-293B-4663-AA36-1AAE46463776}` |

Descriptor bytes (the ESP firmware and the `usbip` module of XRDAP-Host both follow these):

```
Device descriptor (18 bytes)
12 01 10 02 00 00 00 40 50 1D 99 61 00 02 01 02 03 01

Configuration descriptor (wTotalLength 32 = configuration 9 + interface 9 + endpoint 7 + endpoint 7)
09 02 20 00 01 01 00 80 32
09 04 00 00 02 FF 00 00 04
07 05 01 02 00 02 00
07 05 81 02 00 02 00

Device qualifier descriptor (10 bytes)
0A 06 00 02 00 00 00 40 01 00

BOS (wTotalLength 33 = BOS header 5 + MS OS 2.0 platform capability 28)
05 0F 21 00 01
1C 10 05 00 DF 60 DD D8 89 45 C7 4C 9C D2 65 9D 9E 64 8A 9F 00 00 03 06 A2 00 20 00

MS OS 2.0 descriptor set (162 bytes = set header 10 + compatible ID 20 + registry property 132)
0A 00 00 00 00 00 03 06 A2 00
14 00 03 00 57 49 4E 55 53 42 00 00 00 00 00 00 00 00 00 00
84 00 04 00 07 00 2A 00
44 00 65 00 76 00 69 00 63 00 65 00 49 00 6E 00 74 00 65 00 72 00 66 00 61 00 63 00
65 00 47 00 55 00 49 00 44 00 73 00 00 00
50 00
7B 00 43 00 44 00 42 00 33 00 42 00 35 00 41 00 44 00 2D 00 32 00 39 00 33 00 42 00
2D 00 34 00 36 00 36 00 33 00 2D 00 41 00 41 00 33 00 36 00 2D 00 31 00 41 00 41 00
45 00 34 00 36 00 34 00 36 00 33 00 37 00 37 00 36 00 7D 00 00 00 00 00
```

- Device: bcdUSB 0x0210 (at least 2.01 when a BOS is present), class 0/0/0, EP0 packet size 64, 1D50:6199, bcdDevice 0x0200, string indexes 1/2/3, one configuration.
- Configuration: one interface, bus-powered, 100 mA; the interface is vendor class 0xFF/0x00/0x00 and its name is string 4 (CMSIS-DAP v2 requires an interface name containing `CMSIS-DAP`); Bulk OUT 0x01 first, then Bulk IN 0x81, 512 bytes each.
- The BOS platform capability UUID is MS OS 2.0's `{D8DD60DF-4589-4CC7-9CD2-659D9E648A9F}`, Windows version 0x06030000, descriptor set of 162 bytes, vendor request code 0x20. The host fetches the set with a `GET` vendor request (bRequest 0x20, wIndex 7).
- The set holds only the device-level compatible ID `WINUSB` and the registry property `DeviceInterfaceGUIDs` (REG_MULTI_SZ, ending in two zeros) with the GUID CMSIS-DAP v2 prescribes, `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`.
- String 0 is `04 03 09 04` (English, United States); the other strings are UTF-16LE.
- `usbip_usb_device`: bus ID `1-1`, busnum 1, devnum 1, speed 3 (high), device class 0/0/0, configuration value 1, 1 configuration, 1 interface (FF 00 00).

DAP command pairing: every Bulk OUT is executed immediately and its response is queued (depth 4, equal to the packet count reported by DAP_Info) and handed to the waiting Bulk IN in order of arrival; an IN that arrives first waits in the queue for a response. A command without a response (DAP_TransferAbort) does not take an IN. When the queue is full the URB is answered with -EPIPE.

The replies caused by one command (the reply to the OUT and the response paired with an IN) are combined into a single `send()`. When sent in two parts, the second part waits for the peer's delayed ACK, which makes every round trip much slower.

## 7. Notes for USB hosts

A full-speed USB Bulk packet is 64 bytes. When a request length is an exact multiple of 64 (128, 192 bytes, ...) and smaller than the DAP packet size, the OUT transfer has no short packet to mark its end, so the probe keeps waiting: the request gets no response and later responses are misaligned. After sending such a request the host must send a zero-length packet. A SUB_START with 8 items is exactly 128 bytes and is the most likely case. `xrdap` handles this; clients written with libusb or pyusb must add it. For reception, the IN request length must be a multiple of the endpoint maximum packet size (and not smaller than the DAP packet size).
