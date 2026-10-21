"""Wireless pair (RAD-*), firmware resources (RES-*), lights and keys (UI-*)."""

import struct
import time

from .. import runner as R
from .link_perf import block_rates

RADIO = ("radio",)
# Diagnostic command 0x9F of the wireless (SX1281) firmware: 0x11 statistics, 0x13 settings.
RADIO_FIELDS = ["radio_tx", "radio_rx", "crc_errors", "tx_timeouts", "commands", "retries",
                "failures", "duplicates", "last_rtt_us", "max_rtt_us"]


def radio_stats(lk):
    r = lk.xfer(b"\x9F\x11")
    if len(r) < 3 or r[1] != 0:
        raise R.NotApplicable(f"no radio statistics command (0x9F 0x11 -> {r.hex(' ')})")
    count = min(len(RADIO_FIELDS), (len(r) - 3) // 4)
    return dict(zip(RADIO_FIELDS, struct.unpack_from(f"<{count}I", r, 3)))


@R.case("RAD-01", "Radio link under load: retry and failure rate, RTT", links=RADIO)
def rad_load(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        before = radio_stats(lk)
        rates = block_rates(ctx, lk, dap, t.at(t.BLOCK), 4096, passes=5)
        after = radio_stats(lk)
        delta = {k: after[k] - before[k] for k in before if k not in ("last_rtt_us", "max_rtt_us")}
        cmds = max(1, delta["commands"])
        ctx.metric("block", rates)
        ctx.metric("delta", delta)
        ctx.metric("retry_rate", round(delta["retries"] / cmds, 4))
        ctx.metric("max_rtt_us", after.get("max_rtt_us"))
        ctx.check("data intact over the radio", rates["ok"])
        ctx.check("no failed radio command", delta["failures"] == 0, delta["failures"])
        ctx.summary(f"retries {delta['retries']}/{cmds}, R {rates['read_kib_s']} KiB/s")


@R.case("RAD-02", "Pairing survives a power cycle of both boards", mode=R.ASSISTED, links=RADIO)
def rad_pairing(ctx):
    def settings():
        with ctx.link() as lk:
            r = lk.xfer(b"\x9F\x13")
            if len(r) < 12 or r[1] != 0:
                raise R.NotApplicable("no radio settings command (0x9F 0x13)")
            return r[4], struct.unpack_from("<I", r, 8)[0]

    ctx.need_operator()
    paired, link_id = settings()
    ctx.check("paired before", paired == 1)
    ctx.ask("power-cycle both radio boards (unplug both, plug both back)")
    time.sleep(2)
    paired2, link_id2 = settings()
    ctx.check("still paired, same link id", paired2 == 1 and link_id2 == link_id,
              f"{link_id:08x} -> {link_id2:08x}")
    ctx.summary(f"link id {link_id2:08x}")


R.manual("RAD-03", "Range and interference", """
Run PERF-02 and RAD-01 over the radio pair at 1 m, 5 m, 10 m and through one wall, then with
a 2.4 GHz Wi-Fi link busy next to the pair (for example a file copy on an access point
within 1 m). Record distance, wall, interferer, retry rate and read KiB/s per point.""",
         links=RADIO)


def elf_sizes(path):
    """Flash and RAM use from ELF section headers (ALLOC sections; SHT_NOBITS is RAM only)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise ValueError("not a 32-bit ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    flash = ram = 0
    for i in range(shnum):
        _, sh_type, flags, addr, _, size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
        if not flags & 0x2 or size == 0:  # SHF_ALLOC
            continue
        if sh_type == 8:  # NOBITS: .bss, heap, stack reserves
            ram += size
        else:
            flash += size
            if flags & 0x1:  # SHF_WRITE: .data is copied to RAM as well
                ram += size
    return flash, ram


@R.case("RES-01", "Firmware image size: flash and static RAM from the ELF (--elf)")
def res_size(ctx):
    if not ctx.args.elf:
        raise R.NotApplicable("needs --elf of the probe firmware")
    flash, ram = elf_sizes(ctx.args.elf)
    ctx.metric("flash_bytes", flash)
    ctx.metric("ram_bytes", ram)
    ctx.check("ELF parsed", flash > 0)
    ctx.summary(f"flash {flash} B, RAM {ram} B")


R.manual("RES-02", "Heap headroom and stack peak", """
F103 / CH32: fill the stack with a pattern at boot (or use the linker's stack fill), run
SWD-01, PERF-02 and VND-06, then read the deepest overwritten word with a second debugger
(J-Link / WCH-Link) without halting; heap end from `__sbrk_heap_end` (newlib) or FreeRTOS
xPortGetMinimumEverFreeHeapSize. ESP: `heap_caps_get_minimum_free_size` and
uxTaskGetStackHighWaterMark of the DAP, sampling and network tasks from the console.
Record bytes free at the worst point.""")

R.manual("RES-03", "CPU load of the core that runs SWD", """
Toggle a spare GPIO around the DAP command and sampling tick handlers (debug build) and
measure the duty cycle with a logic analyser during PERF-02 and during VND-06 at the minimum
period. Record busy percent for both loads.""")


R.manual("RES-04", "Firmware update: DFU / web flasher time, settings kept", """
Update the probe with its normal path (ESP: web flasher or `esptool write_flash 0x0 merged.bin`;
CH32: `tools/dfu_update.py`; F103: SWD flash of the release image). Record the time from start
to the probe answering DAP_Info, and check that settings survive: ESP Wi-Fi credentials (0x86
op 0 shows the same SSID), CH32 radio pairing and role (0x9F 0x13).""")

R.manual("RES-05", "Power and temperature", """
USB power meter in line with the probe: record mA idle, during PERF-02 and during sampling at
the minimum period. After 10 minutes of PERF-02 in a loop, record the hottest point of the
board (IR thermometer or thermocouple) and the room temperature.""")

R.manual("UI-01", "Status light: link colours", """
Family rule: red = wired (USB configured or a USB/serial DAP command within 5 s), blue =
wireless (network client or radio session), purple = no link or a setting change. Check by
eye on each probe: power on without host (purple), USB host (red), network client only
(blue, ESP), radio session (blue, wireless pair).""")

R.manual("UI-02", "Status light: activity and host status", """
DAP traffic (PERF-02 loop) makes the light pulse; DAP_HostStatus connected alternates the
main colour with purple every 2 s, running alternates red and blue every 1 s; both clear on
DAP_Disconnect or 5 s without commands. Check each state by eye while OpenOCD runs
`init; halt` then `resume`.""")

R.manual("UI-03", "Keys: Wi-Fi clear (ESP Lite) and pairing (wireless pair)", """
ESP boards with a key: hold it 5 s -> fast purple blink, then solid purple, Wi-Fi credentials
cleared and the probe restarts without Wi-Fi (0x86 op 0 shows no SSID). Wireless pair: long
press KEY2 on both unpaired boards -> purple blink, both report paired with the same link id
(RAD-02 settings). Record pass/fail per board.""")
