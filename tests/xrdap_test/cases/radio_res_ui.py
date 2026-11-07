"""Wireless pair (RAD-*), firmware resources (RES-*), lights and keys (UI-*)."""

import struct
import time

from .. import runner as R
from .link_perf import block_rates

RADIO = ("radio",)
# Control port operations of a radio probe (protocol 8.3): 0x11 statistics, 0x13 settings.
RADIO_FIELDS = ["radio_tx", "radio_rx", "crc_errors", "tx_timeouts", "commands", "retries",
                "failures", "duplicates", "last_rtt_us", "max_rtt_us", "channel", "rearms",
                "restarts"]


def radio_control(lk, request, what):
    try:
        r = lk.control(request)
    except (NotImplementedError, TimeoutError) as exc:
        raise R.NotApplicable(f"no radio {what} (control {request.hex(' ')}): {exc}")
    if len(r) < 3 or r[1] != 0:
        raise R.NotApplicable(f"no radio {what} (control {request.hex(' ')} -> {r.hex(' ')})")
    return r


def radio_stats(lk):
    r = radio_control(lk, b"\x11", "statistics")
    count = min(len(RADIO_FIELDS), r[2], (len(r) - 3) // 4)
    return dict(zip(RADIO_FIELDS, struct.unpack_from(f"<{count}I", r, 3)))


@R.case("RAD-01", "Radio link under load: retry and failure rate, RTT", links=RADIO)
def rad_load(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        before = radio_stats(lk)
        rates = block_rates(ctx, lk, dap, t.at(t.BLOCK), 4096, passes=5)
        after = radio_stats(lk)
        delta = {k: after[k] - before[k] for k in before
                 if k not in ("last_rtt_us", "max_rtt_us", "channel")}
        cmds = max(1, delta["commands"])
        ctx.metric("block", rates)
        ctx.metric("delta", delta)
        ctx.metric("retry_rate", round(delta["retries"] / cmds, 4))
        ctx.metric("max_rtt_us", after.get("max_rtt_us"))
        ctx.metric("channel", after.get("channel"))
        ctx.check("data intact over the radio", rates["ok"])
        ctx.check("no failed radio command", delta["failures"] == 0, delta["failures"])
        ctx.summary(f"retries {delta['retries']}/{cmds}, R {rates['read_kib_s']} KiB/s")


@R.case("RAD-02", "Pairing survives a power cycle of both boards", mode=R.ASSISTED, links=RADIO)
def rad_pairing(ctx):
    def settings():
        with ctx.link() as lk:
            r = radio_control(lk, b"\x13", "settings")
            if len(r) < 12:
                raise R.NotApplicable(f"short radio settings answer {r.hex(' ')}")
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


R.manual("RAD-04", "Radio session end: new session id, id 0 or no ping stops remote sampling",
         links=RADIO)

R.manual("RAD-03", "Range and interference", links=RADIO)


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


R.manual("RES-02", "Heap headroom and stack peak")

R.manual("RES-03", "CPU load of the core that runs SWD")


R.manual("RES-04", "Firmware update: DFU / web flasher time, settings kept")

R.manual("RES-05", "Power and temperature")

R.manual("UI-01", "Status light: link colours")

R.manual("UI-02", "Status light: activity and host status")

R.manual("UI-03", "Keys: Wi-Fi clear (ESP Lite) and pairing (wireless pair)")
