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
python -m xrdap_test run --link usb:SERIAL --target my-target.toml
```

Notation of `--link`:

| Notation | Link |
|---|---|
| `usb`, `usb:SERIAL` | USB; SERIAL can be the last few characters of the serial number |
| `serial:PORT`, `serial:PORT@921600`, `serial:PORT,keep` | Serial frames; default 4000000 baud; `keep` leaves DTR/RTS unchanged (USB port of the ESP32-C3) |
| `tcp:HOST`, `tcp:xrdap-xxxxxx.local:3241` | TCP frames, default port 3241 |
| `usbip:HOST` | USB/IP, default port 3240 |

To test through a wireless board pair, `--link` points to the board attached to the computer, with `--via-radio` added.

To run only some cases use `--cases` with case IDs or prefixes:

```bash
python -m xrdap_test run --link tcp:HOST --target my-target.toml --cases VND-06,VND-08
python -m xrdap_test run --link usb --target my-target.toml --cases STAB-01 --long-s 3600
```

Cases that need a second link (STAB-08) take another path to the same probe with `--link2`; when an assisted case reaches an operator step it prompts and waits for Enter, and with `--no-prompt` it is recorded as n/a.

The commit IDs of the image under test are written into the result:

```bash
python -m xrdap_test run --link usb --target my-target.toml --build fw=1a2b3c4,libxr=5d6e7f8,core=9a0b1c2
```

## Results

Each run creates a new directory under `--out` (default `xrdap-results/`), named from the time, link and target, containing `result.json`, `result.md`, `log.txt` and the logs of the host tools. The fields are described in section 5 of SPEC.en.md.

Manual cases and results that need to be added are written into the same run with `record`:

```bash
python -m xrdap_test record --run xrdap-results/RUN UI-01 pass --note "purple / red as expected"
```

Several runs are combined into one table:

```bash
python -m xrdap_test table xrdap-results/RUN-A xrdap-results/RUN-B --output matrix.md
```

## A/B comparison

`ab` alternately flashes two images, runs the same set of cases with the same settings after each flash, and finally lists the A and B means and the ratio of each numeric metric. The flash commands are given by the caller:

```bash
python -m xrdap_test ab --link usb --target my-target.toml --cases PERF,VND-06 \
    --a-cmd "./flash.sh old.elf" --b-cmd "./flash.sh new.elf" --ab-rounds 2
```

## Target profile

A target profile is a TOML file that states the RAM the tool may overwrite (scratch, restored at the end of the run), the region read out by the host tools (dump), the Flash region that may be erased and written (flash_test) and the target name for each host tool; the format is in [targets/example.toml](targets/example.toml). Target profiles are kept in the user's own directory and given with `--target PATH`; this repository only carries that example. Scratch must be RAM the target program does not use; to use another region temporarily, use `--scratch BASE:SIZE`.

## Serial session hygiene (STAB-08)

STAB-08 checks whether a serial session makes the probe restart or lose the Wi-Fi credentials: on the serial link it runs `xrdap share` and OpenOCD, and at the same time reads the Wi-Fi credentials and the probe clock over another link (TCP or USB), so both the serial port and that link must be given, together with the locations of `xrdap` and OpenOCD:

```bash
python -m xrdap_test run --link serial:PORT --link2 tcp:HOST --target my-target.toml --cases STAB-08 --xrdap PATH/xrdap --openocd PATH/openocd
```

When the probe has no saved Wi-Fi credentials, this case is recorded as n/a.

## Coverage check

`coverage` checks that every case in section 3 of SPEC.md and SPEC.en.md is implemented, that the mode, links and oracle agree with the code, that the oracle uses only the kinds allowed in section 1.4 of the SPEC, and that every branch of the behavior-suite maps to a case or is marked N/A:

```bash
python -m xrdap_test coverage
python -m unittest discover -s tool_unit
```

Output of `coverage`:

```
SPEC.md: 55 cases, 52 behaviour-suite branches mapped
SPEC.en.md: 55 cases, 52 behaviour-suite branches mapped
code: 55 cases ({'auto': 39, 'assisted': 5, 'manual': 11})
coverage OK
```

The unit tests use the byte examples in the protocol document to check frames, item encoding and SUB_READ decoding.
