# XR-DAP Test Tool

[中文](README.md)

`xrdap_test` runs the tests of [SPEC.en.md](SPEC.en.md) on any probe of the XR-DAP family: the same set of cases runs over USB, serial, Wi-Fi TCP, USB/IP or a wireless board pair, and the results are written as JSON and Markdown tables. The tool sends and receives CMSIS-DAP packets directly (protocol in [docs/protocol.en.md](../docs/protocol.en.md)), and the host tool cases call OpenOCD, probe-rs and pyOCD.

## Installation

Python 3.11 or newer is required, with pyusb and pyserial; on Windows pyusb uses the libusb bundled with libusb-package:

```bash
python -m pip install pyusb pyserial libusb-package
```

On Windows the probe needs the WinUSB driver (the MS OS 2.0 descriptor of XR-DAP makes the system install it automatically); on Linux a udev rule is needed, or run with `sudo`. All commands are run in this directory (`tests/`).

## Usage

List the cases, target profiles and probes on USB:

```bash
python -m xrdap_test list
```

Run the default case set (everything except the two 1 h cases) for one probe, one link and one target:

```bash
python -m xrdap_test run --link usb:5744 --target stm32f302cb
```

Notation of `--link`:

| Notation | Link |
|---|---|
| `usb`, `usb:SERIAL` | USB; SERIAL can be the last few characters of the serial number |
| `serial:COM5`, `serial:/dev/ttyACM0@921600`, `serial:COM7,keep` | Serial frames; default 4000000 baud; `keep` leaves DTR/RTS unchanged (USB port of the ESP32-C3) |
| `tcp:192.168.1.20`, `tcp:xrdap-93ef1c.local:3241` | TCP frames, default port 3241 |
| `usbip:192.168.1.20` | USB/IP, default port 3240 |

To test through a wireless board pair, `--link` points to the board attached to the computer, with `--via-radio` added.

To run only some cases use `--cases` with case IDs or prefixes:

```bash
python -m xrdap_test run --link tcp:192.168.1.20 --target stm32h723 --cases VND-06,VND-08
python -m xrdap_test run --link usb --target stm32f302cb --cases STAB-01 --long-s 3600
```

Cases that need a second link (MC-02, MC-03) take another path to the same probe with `--link2`; when an assisted case reaches an operator step it prompts and waits for Enter, and with `--no-prompt` it is recorded as n/a.

The commit IDs of the image under test are written into the result:

```bash
python -m xrdap_test run --link usb --target stm32f302cb --build fw=1a2b3c4,libxr=5d6e7f8,core=9a0b1c2
```

## Results

Each run creates a new directory under `--out` (default `xrdap-results/`), named from the time, link and target, containing `result.json`, `result.md`, `log.txt` and the logs of the host tools. The fields are described in section 5 of SPEC.en.md.

Manual cases and results that need to be added are written into the same run with `record`:

```bash
python -m xrdap_test record --run xrdap-results/20260101-120000-usb-stm32f302cb UI-01 pass --note "purple / red as expected"
```

Several runs are combined into one table:

```bash
python -m xrdap_test table xrdap-results/*-usb-stm32f302cb xrdap-results/*-tcp-stm32h723 --output matrix.md
```

## A/B comparison

`ab` alternately flashes two images, runs the same set of cases with the same settings after each flash, and finally lists the A and B means and the ratio of each numeric metric. The flash commands are given by the caller:

```bash
python -m xrdap_test ab --link usb --target stm32f302cb --cases PERF,VND-06 \
    --a-cmd "./flash.sh old.elf" --b-cmd "./flash.sh new.elf" --ab-rounds 2
```

## Target profiles

`targets/*.toml` describes one chip:

```toml
name = "stm32f302cb"
dpidr = 0x2BA01477
openocd_target = "target/stm32f3x.cfg"
probe_rs_chip = "STM32F302CBTx"
pyocd_target = "stm32f302cb"
scratch = { base = 0x20001000, size = 0x4400 }     # RAM the tool may overwrite, restored at the end of the run
dump = { base = 0x08000000, size = 0x20000 }       # region read out by TOOL-*
flash_test = { base = 0x0801C000, size = 0x4000 }  # Flash region erased and written by TOOL-01 and STAB-02
```

Optional fields: `counter` (address of a u32 in the target program that only increases, used for the counter check when a Cortex-M0/M0+ has no CYCCNT), `fault_address` (an address whose read gives FAULT, default 0xFFFFF000). Scratch must be RAM the target program does not use; to use another region temporarily, use `--scratch BASE:SIZE`.

## Coverage check

`coverage` checks that every case in section 3 of SPEC.md and SPEC.en.md is implemented, that the mode and links agree with the code, and that every branch of the behavior-suite maps to a case or is marked N/A:

```bash
python -m xrdap_test coverage
python -m unittest discover -s unit
```

Output of `coverage`:

```
SPEC.md: 51 cases, 52 behaviour-suite branches mapped
SPEC.en.md: 51 cases, 52 behaviour-suite branches mapped
code: 51 cases ({'auto': 36, 'assisted': 5, 'manual': 10})
coverage OK
```

The unit tests use the byte examples in the protocol document to check frames, item encoding and SUB_READ decoding.
