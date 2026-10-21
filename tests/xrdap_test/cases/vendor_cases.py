"""XR vendor extension (VND-*)."""

import statistics
import struct
import time

from .. import runner as R
from ..dap import AP_CSW
from ..target import CPUID, DEMCR, DHCSR, AIRCR, DBGKEY, DWT_CTRL, DWT_CYCCNT, S_HALT
from ..vendor import ARG, NO_TARGET, OK, RESOURCE, STATE, Item, capture, numbered, tick_gaps


def need_vendor(vendor):
    info = vendor.info()
    if info.get("status") != OK:
        raise R.NotApplicable(f"probe has no XR vendor extension (INFO {info})")
    return info


@R.case("VND-01", "INFO and DAP_Info identity")
def vnd_info(ctx):
    with ctx.session(fixture=False) as (lk, dap, vendor):
        ident = dap.identity()
        info = need_vendor(vendor)
        ctx.metric("identity", ident)
        ctx.metric("info", info)
        ctx.check("vendor string XRobot", ident["vendor"] == "XRobot", ident["vendor"])
        ctx.check("serial XRobot-XRDAP-<hex>", (ident["serial"] or "").startswith("XRobot-XRDAP-"),
                  ident["serial"])
        ctx.check("firmware version present", bool(ident["firmware"]))
        ctx.check("protocol 1, 32 items, depth 8",
                  (info["version"], info["max_items"], info["max_depth"]) == (1, 32, 8))
        ctx.check("features READ/WRITE/sampling/8-byte", info["features"] & 0x0F == 0x0F,
                  hex(info["features"]))
        t1 = vendor.info()["probe_time_us"]
        ctx.check("probe time advances", t1 > info["probe_time_us"])
        ctx.summary(f"{ident['product']} {ident['firmware']}, ring {info['ring_bytes']} B, "
                    f"features {info['features']:#x}")


@R.case("VND-02", "READ: 1/2/4/8 bytes, pointer chain, item status matrix, limits")
def vnd_read(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        pairs = t.constant_items()
        items = numbered([i for i, _ in pairs])
        status, entries = vendor.read(items)
        ctx.check("overall OK", status == OK, status)
        for (item, want), (_, st, value) in zip(pairs, entries):
            ctx.check(f"value size {item.size} derefs {len(item.derefs)}", st == OK and value == want,
                      f"status {st:#x} value {value:#x}")
        cases = [  # item, expected item status
            (Item(t.at(t.NULL_P1), 4, (0,)), 0x30),
            (Item(t.fault_address, 4, (0,)), 0x10),
            (Item(t.at(t.BAD_P1), 4, (1,)), 0x20),
            (Item(t.at(t.V_U32), 4, (), 1), 0x2F),
            (Item(t.at(t.BAD_P1), 4, (0, 0)), 0x11),
            (Item(t.at(t.BAD_P1), 4, (0,)), 0x1F),
            (Item(t.at(t.V_U32), 4), 0x00),  # recovery after the faults
        ]
        status, entries = vendor.read(numbered([i for i, _ in cases]))
        ctx.check("status matrix overall OK", status == OK, status)
        for (item, want), (_, st, value) in zip(cases, entries):
            ctx.check(f"item status {want:#04x}", st == want, f"got {st:#04x}")
        ctx.check("recovered value", entries[-1][2] == 0x89ABCDEF, hex(entries[-1][2]))
        ctx.check("count 0 -> ARG", lk.xfer(b"\x81\x00")[1] == ARG)
        many = numbered([Item(t.at(t.V_U8), 1) for _ in range(33)])
        ctx.check("33 items -> RESOURCE", vendor.read(many)[0] == RESOURCE)
        big = numbered([Item(t.at(t.V_U64), 8) for _ in range(32)])
        fits = 3 + 32 * 11 <= dap.packet_size
        status = vendor.read(big)[0]
        ctx.check("32 x 8 B: OK if the response fits, else RESOURCE",
                  status == (OK if fits else RESOURCE), f"status {status} packet {dap.packet_size}")
        ctx.summary("values, chain, statuses and limits as specified")


@R.case("VND-03", "WRITE: 1/2/4/8 bytes with readback, item status matrix")
def vnd_write(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        writes = numbered([Item(t.at(t.V_U8), 1, value=0x5A), Item(t.at(t.V_U16), 2, value=0xBEAF),
                           Item(t.at(t.V_U64), 8, value=0x8877665544332211),
                           Item(t.at(t.RW), 4, value=0xDEADBEEF)])
        status, entries = vendor.write(writes)
        ctx.check("write OK", status == OK and all(st == OK for _, st in entries), entries)
        back = numbered([Item(w.base, w.size) for w in writes])
        _, entries = vendor.read(back)
        for w, (_, st, value) in zip(writes, entries):
            ctx.check(f"readback size {w.size}", value == w.value, hex(value))
        cases = [(Item(t.at(t.NULL_P1), 4, (0,), value=1), 0x30),
                 (Item(t.at(t.BAD_P1), 4, (0, 0), value=2), 0x11),
                 (Item(t.fault_address, 4, value=3), 0x1F),
                 (Item(t.at(t.RW), 4, value=0xA5A5A5A5), 0x00)]
        status, entries = vendor.write(numbered([i for i, _ in cases]))
        for (item, want), (_, st) in zip(cases, entries):
            ctx.check(f"write item status {want:#04x}", st == want, f"got {st:#04x}")
        ctx.check("recovery write landed", dap.read32(t.at(t.RW)) == 0xA5A5A5A5)
        ctx.check("count 0 -> ARG", lk.xfer(b"\x82\x00")[1] == ARG)
        ctx.summary("writes, readback and statuses as specified")


@R.case("VND-04", "Argument and state checks")
def vnd_args(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        base = Item(t.at(t.V_U32), 4)
        vendor.stop()
        for name, period, flags, items, want in (
                ("period 0", 0, 0, [base], ARG), ("flags reserved", 2000, 2, [base], ARG),
                ("count 0", 2000, 0, [], ARG), ("div 0", 2000, 0, [Item(base.base, div=0)], ARG),
                ("phase >= div", 2000, 0, [Item(base.base, div=10, phase=10)], ARG),
                ("size 3", 2000, 0, [Item(base.base, size=3)], ARG)):
            ctx.check(f"SUB_SET {name} -> {want:#x}", vendor.sub_set(period, numbered(items), flags)[0] == want)
        ctx.check("INFO with extra byte -> ARG", lk.xfer(b"\x80\x00")[1] == ARG)
        ctx.check("SUB_CTRL bad op -> ARG", vendor.sub_ctrl(3) == ARG)
        ctx.check("SUB_CTRL short -> ARG", lk.xfer(b"\x84")[1] == ARG)
        ctx.check("READ length mismatch -> ARG", lk.xfer(b"\x81\x01\x00")[1] == ARG)
        ctx.check("SUB_SET accepted", vendor.sub_set(100000, numbered([base]))[0] == OK)
        ctx.check("start", vendor.sub_ctrl(1) == OK)
        ctx.check("start again -> STATE", vendor.sub_ctrl(1) == STATE)
        ctx.check("SUB_SET while running -> STATE", vendor.sub_set(100000, [base])[0] == STATE)
        vendor.stop()
        ctx.check("stop when stopped -> OK", vendor.stop() == OK)
    with ctx.link() as lk:  # fresh link: no DAP_Connect yet
        from ..vendor import Vendor

        v = Vendor(lk)
        lk.xfer(bytes([0x03]))  # DAP_Disconnect
        ctx.check("READ before DAP_Connect -> NO_TARGET", v.read(numbered([Item(t.at(t.V_U32))]))[0] == NO_TARGET)
        ctx.check("SUB_SET before DAP_Connect -> NO_TARGET",
                  v.sub_set(100000, numbered([Item(t.at(t.V_U32))]))[0] == NO_TARGET)
    ctx.summary("all argument and state errors as specified")


@R.case("VND-05", "READ/WRITE latency, items/s, pointer-chain cost")
def vnd_latency(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)

        def median_us(fn, n=300):
            times = []
            end = time.monotonic() + 3.0
            while len(times) < n and time.monotonic() < end:
                t0 = time.perf_counter()
                fn()
                times.append((time.perf_counter() - t0) * 1e6)
            return round(statistics.median(times), 1)

        one = numbered([Item(t.at(t.V_U32))])
        w = numbered([Item(t.at(t.RW), value=1)])
        ctx.metric("read1_us", median_us(lambda: vendor.read(one)))
        ctx.metric("write1_us", median_us(lambda: vendor.write(w)))
        rates = {}
        for n in (1, 8, 32):
            items = numbered(t.word_items(n))
            rates[n] = round(n / median_us(lambda: vendor.read(items)) * 1e6)
        ctx.metric("items_per_s", rates)
        # Chain of k levels through a self-pointer: each level is one more SWD read.
        dap.write32(t.at(t.RW), t.at(t.RW))
        chain = {}
        for k in (0, 1, 2, 4, 8):
            items = numbered([Item(t.at(t.RW), 4, (0,) * k) for _ in range(8)])
            chain[k] = median_us(lambda: vendor.read(items))
        ctx.metric("chain8_us_by_depth", chain)
        per_level = round((chain[8] - chain[0]) / 64, 2)
        ctx.metric("per_level_us", per_level)
        status, entries = vendor.read(numbered([Item(t.at(t.RW), 4, (0,) * 8)]))
        ctx.check("8-level self chain resolves", entries and entries[0][1] == OK, entries)
        ctx.summary(f"READ1 {ctx.result.metrics['read1_us']} us, 32 items {rates[32]}/s, "
                    f"+{per_level} us per level")


def search_min_period(ctx, vendor, items, start_us):
    """SPEC 2.3: from the estimate up in 25 us steps; first period that runs 3 s with
    0 skipped, 0 dropped, 0 failed items and no tick gap."""
    period = max(R.SEARCH_STEP_US, -(-start_us // R.SEARCH_STEP_US) * R.SEARCH_STEP_US)
    tried = []
    for _ in range(400):
        status, _, est = vendor.sub_set(period, items)
        if status == OK:
            cap = capture(vendor, items, R.SEARCH_RUN_S)
            failed = sum(1 for f in cap.frames for v in f[2].values() if v is None)
            gaps = tick_gaps(cap.frames, items)
            tried.append((period, cap.skipped, cap.dropped, failed, gaps))
            if not (cap.skipped or cap.dropped or failed or gaps) and cap.frames:
                return period, tried
        else:
            tried.append((period, f"rejected {status:#x}"))
        period += R.SEARCH_STEP_US
    return None, tried


@R.case("VND-06", "Sampling: estimate and real minimum period for 1/4/8/32 variables")
def vnd_capacity(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        out = {}
        for n in R.VAR_COUNTS:
            items = numbered(t.word_items(n))
            status, max_frame, est = vendor.sub_set(1_000_000, items)
            best, tried = search_min_period(ctx, vendor, items, est or 25)
            out[n] = {"est_us": est, "max_frame": max_frame, "min_period_us": best}
            ctx.log(f"  {n} vars: estimate {est} us, min period {best} us, tried {tried[-3:]}")
            ctx.check(f"{n} vars: a clean period exists", best is not None)
        ctx.metric("capacity", out)
        ctx.summary(" ".join(f"{n}:{v['est_us']}/{v['min_period_us']}" for n, v in out.items())
                    + " (vars:est/min us)")


def counter_item(ctx, t, dap):
    """A u32 that only counts up: the target's own counter, else DWT CYCCNT."""
    if t.counter is not None:
        return t.counter, "target"
    if dap.read32(DWT_CTRL) >> 25 & 1:
        return None, None  # no cycle counter (Cortex-M0/M0+)
    dap.write32(DEMCR, dap.read32(DEMCR) | 1 << 24)
    dap.write32(DWT_CTRL, dap.read32(DWT_CTRL) | 1)
    return DWT_CYCCNT, "cyccnt"


def divided_set(t, counter):
    """The 8-item divided subscription of SPEC 2.2."""
    c = counter if counter is not None else t.at(t.WORDS)
    return numbered([
        Item(c, 4, div=1), Item(t.at(t.WORDS), 4, div=1), Item(c, 4, div=10),
        Item(t.at(t.V_U64), 8, div=5), Item(t.at(t.V_U32), 4, div=100, phase=7),
        Item(t.at(t.V_U16), 2, div=100, phase=13), Item(t.at(t.V_U8), 1, div=100, phase=23),
        Item(t.at(t.CHAIN_P1), 4, (0, 0, 0), div=100, phase=41)])


DIVIDED_CONST = {2: 0x5A000000, 4: 0x0123456789ABCDEF, 5: 0x89ABCDEF, 6: 0x1234, 7: 0xA5, 8: 0xCAFEBABE}


def check_stream(ctx, cap, items, period, counter_kind):
    """Divider/phase, values, counters and timestamps of a captured stream."""
    frames = cap.frames
    ctx.check("frames received", len(frames) > 0, len(frames))
    if not frames:
        return {}
    wrong_due = sum(1 for k, _, v in frames
                    if set(v) != {i.id for i in items if (k + i.phase) % i.div == 0})
    ctx.check("due set of every frame matches (k + phase) % div", wrong_due == 0, f"{wrong_due} frames")
    failed = sum(1 for f in frames for v in f[2].values() if v is None)
    bad_const = sum(1 for f in frames for i, v in f[2].items()
                    if i in DIVIDED_CONST and v is not None and v != DIVIDED_CONST[i])
    ctx.check("no failed items", failed == 0, failed)
    ctx.check("constants and 3-level chain", bad_const == 0, f"{bad_const} wrong values")
    gaps = tick_gaps(frames, items)
    ctx.check("no tick gaps, 0 skipped, 0 dropped", not (gaps or cap.skipped or cap.dropped),
              f"gaps {gaps} skipped {cap.skipped} dropped {cap.dropped}")
    base_k, base_t = frames[0][0], frames[0][1]
    dev = [ts - base_t - (k - base_k) * period for k, ts, _ in frames]
    out = {"frames": len(frames), "dev_min_us": min(dev), "dev_max_us": max(dev),
           "dev_p99_abs_us": sorted(abs(d) for d in dev)[int(len(dev) * 0.99) - 1]}
    if counter_kind:
        ticks = [(k, v[1]) for k, _, v in frames if v.get(1) is not None]
        deltas = [(b - a) & 0xFFFFFFFF for (_, a), (_, b) in zip(ticks, ticks[1:])]
        ctx.check("counter only counts up", all(0 < d < 1 << 31 for d in deltas) if counter_kind == "cyccnt"
                  else all(d < 1 << 31 for d in deltas))
        if counter_kind == "cyccnt" and len(ticks) > 2:
            span_k = ticks[-1][0] - ticks[0][0]
            span_c = sum(deltas)
            hz = span_c / (span_k * period * 1e-6)
            real = [d / hz * 1e6 - period for d in deltas]
            out["target_hz"] = round(hz)
            out["target_jitter_p99_us"] = round(sorted(abs(x) for x in real)[int(len(real) * 0.99) - 1], 1)
        slow = [v[3] for _, _, v in frames if 3 in v and v[3] is not None]
        ctx.check("div-10 counter item advances", len(slow) < 2 or all(
            (b - a) & 0xFFFFFFFF < 1 << 31 for a, b in zip(slow, slow[1:])))
    return out


@R.case("VND-07", "Divider and phase: 8-item divided set, values and due pattern")
def vnd_divider(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        counter, kind = counter_item(ctx, t, dap)
        items = divided_set(t, counter)
        status, _, est = vendor.sub_set(2000, items)
        ctx.check("SUB_SET 2000 us accepted", status == OK, f"{status} est {est}")
        cap = capture(vendor, items, 5.0, poll_s=0.005)
        out = check_stream(ctx, cap, items, 2000, kind)
        per_item = {i.id: sum(1 for f in cap.frames if i.id in f[2]) for i in items}
        ticks = cap.frames[-1][0] - cap.frames[0][0] + 1 if cap.frames else 0
        ctx.check("item counts follow div", all(abs(per_item[i.id] - ticks / i.div) <= 1 for i in items),
                  per_item)
        ctx.metric("stream", out)
        ctx.summary(f"{len(cap.frames)} frames, counter {kind}")


@R.case("VND-08", "Timestamp jitter at 1 ms, 10 s")
def vnd_jitter(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        counter, kind = counter_item(ctx, t, dap)
        items = numbered([Item(counter if counter is not None else t.at(t.WORDS), 4)])
        ctx.check("SUB_SET 1000 us", vendor.sub_set(1000, items)[0] == OK)
        cap = capture(vendor, items, 10.0, poll_s=0.005)
        out = check_stream(ctx, cap, items, 1000, kind)
        ctx.metric("jitter", out)
        ctx.summary(f"probe dev {out.get('dev_min_us')}..{out.get('dev_max_us')} us"
                    + (f", target p99 {out['target_jitter_p99_us']} us" if "target_jitter_p99_us" in out else ""))


@R.case("VND-09", "Skip and drop accounting, stop-on-full, partial reads, exclusivity")
def vnd_overload(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        items = numbered(t.word_items(8))
        # Overrun: configure at the normal clock, then slow SWD so every tick is late.
        est = vendor.sub_set(1_000_000, items)[2] or 0
        ctx.check("SUB_SET accepted", vendor.sub_set(max(2000, 2 * est), items)[0] == OK)
        dap.swj_clock(100_000)
        cap = capture(vendor, items, 2.0)
        dap.swj_clock(ctx.clock)
        # Missing ticks visible in the stream: before the first frame and between frames.
        # Ticks skipped after the last frame are counted but cannot be seen.
        gaps = tick_gaps(cap.frames, items) + (cap.frames[0][0] if cap.frames else 0)
        ctx.check("overrun counts skipped ticks", cap.skipped > 0, f"skipped {cap.skipped}")
        ctx.check("every missing tick is counted as skipped", gaps <= cap.skipped,
                  f"missing {gaps}, skipped {cap.skipped}")
        ctx.metric("skipped_not_seen", cap.skipped - gaps)
        # Overflow, overwrite mode: no reads for 2 s.
        vendor.sub_set(1000, items)
        vendor.sub_ctrl(1)
        time.sleep(2.0)
        r = vendor.sub_read(items)
        vendor.stop()
        ctx.check("no reads: dropped counted (overwrite)", r.dropped > 0, f"dropped {r.dropped}")
        # Stop-on-full: oldest frames kept, new ones dropped.
        vendor.sub_set(1000, items, flags=1)
        vendor.sub_ctrl(1)
        time.sleep(2.0)
        r = vendor.sub_read(items)
        ctx.check("stop-on-full keeps the first frames", r.frames and r.frames[0][0] == 0,
                  r.frames[0][0] if r.frames else None)
        ctx.check("stop-on-full counts dropped", r.dropped > 0, r.dropped)
        vendor.stop()
        # max_bytes below one frame: no partial frame, data stays queued.
        vendor.sub_set(1000, items)
        vendor.sub_ctrl(1)
        time.sleep(0.1)
        small = vendor.sub_read(items, 20)
        ctx.check("max_bytes 20: no frame, data kept", not small.frames and small.remaining > 0,
                  f"frames {len(small.frames)} remaining {small.remaining}")
        r = lk.xfer(bytes([0x05, 0, 1, 0x02]))
        ctx.check("standard DAP_Transfer while sampling -> FF", r == b"\xFF", r.hex(" "))
        ctx.check("DAP_Info while sampling", lk.xfer(b"\x00\xFF")[:1] == b"\x00")
        _, entries = vendor.read(numbered([Item(t.at(t.V_U32))]))
        ctx.check("READ while sampling", entries and entries[0][2] == 0x89ABCDEF)
        vendor.stop()
        ctx.summary(f"skipped {cap.skipped} (seen {gaps}), overflow and stop-on-full counted")


@R.case("VND-10", "Halt, run and reset through vendor WRITE (works while sampling)")
def vnd_core(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)

        def wr(addr, value):
            return vendor.write(numbered([Item(addr, 4, value=value)]))[1][0][1]

        def rd(addr):
            return vendor.read(numbered([Item(addr, 4)]))[1][0]

        def wait(mask, want, timeout=1.0):
            t0 = time.perf_counter()
            while time.perf_counter() - t0 < timeout:
                _, st, v = rd(DHCSR)
                if st == OK and v & mask == want:
                    return round((time.perf_counter() - t0) * 1e3, 2)
            return None

        out = {}
        ctx.check("DHCSR readable", rd(DHCSR)[1] == OK, rd(DHCSR))
        wr(DHCSR, DBGKEY | 0x3)
        out["halt_ms"] = wait(S_HALT, S_HALT)
        wr(DHCSR, DBGKEY | 0x1)
        out["run_ms"] = wait(S_HALT, 0)
        rd(DHCSR)  # clear the sticky S_RESET_ST
        wr(DEMCR, rd(DEMCR)[2] | 1)  # VC_CORERESET: stop at the reset vector
        wr(AIRCR, 0x05FA0004)
        out["reset_halt_ms"] = wait(S_HALT, S_HALT)
        wr(DEMCR, rd(DEMCR)[2] & ~1)
        wr(DHCSR, DBGKEY)  # run, debug off
        for k, v in out.items():
            ctx.check(f"{k} observed", v is not None, v)
        ctx.metric("latency_ms", out)
        dap.block_write(t.scratch.base, t.fixture_bytes())  # the reset may have run startup code
        ctx.summary(", ".join(f"{k} {v}" for k, v in out.items()))


@R.case("VND-11", "PPB access when the AP CSW holds an unprivileged Prot value")
def vnd_ppb(ctx):
    with ctx.session(fixture=False) as (lk, dap, vendor):
        need_vendor(vendor)
        cpuid = dap.read32(CPUID)
        csw = dap.ap_read(AP_CSW)
        dap.ap_write(AP_CSW, csw & ~(0x7F << 24) | 0x01 << 24)  # Prot = unprivileged data
        found = dap.ap_read(AP_CSW)
        _, entries = vendor.read(numbered([Item(CPUID), Item(DHCSR)]))
        dap.ap_write(AP_CSW, csw)
        ctx.metric("csw_set", hex(found))
        ctx.check("vendor READ CPUID", entries[0][1] == OK and entries[0][2] == cpuid,
                  f"status {entries[0][1]:#x} value {entries[0][2]:#x} want {cpuid:#x}")
        ctx.check("vendor READ DHCSR", entries[1][1] == OK, f"status {entries[1][1]:#x}")
        ctx.summary(f"CSW {found:#x}: CPUID status {entries[0][1]:#x}")


@R.case("VND-12", "Stop and restart at once: new run starts at tick 0, no stale frame")
def vnd_restart(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        a = numbered(t.word_items(4))
        b = numbered([Item(t.at(t.V_U16), 2), Item(t.at(t.V_U8), 1)])
        vendor.sub_set(1000, a)
        first = capture(vendor, a, 0.5)
        vendor.sub_set(1000, b)
        vendor.sub_ctrl(1)
        time.sleep(0.05)  # well inside the smallest ring (4 KiB)
        r = vendor.sub_read(b)
        vendor.stop()
        ctx.check("first run produced frames", len(first.frames) > 0)
        ctx.check("nothing dropped before the read", r.dropped == 0, r.dropped)
        ctx.check("second run starts at tick 0", r.frames and r.frames[0][0] == 0,
                  r.frames[0][0] if r.frames else None)
        ctx.check("second run frames carry only the new items",
                  all(v == {1: 0x1234, 2: 0xA5} for _, _, v in r.frames), r.frames[:2])
        ctx.summary(f"{len(first.frames)} then {len(r.frames)} frames")


@R.case("VND-13", "0x86 Wi-Fi status query (op 0)", links=("usb", "serial", "tcp", "usbip"))
def vnd_wifi(ctx):
    with ctx.link() as lk:
        from ..vendor import Vendor

        info = Vendor(lk).info()
        if info.get("status") != OK or not info["features"] & 0x10:
            raise R.NotApplicable("probe has no 0x86 (feature bit 4)")
        r = lk.xfer(b"\x86\x00")
        ok = len(r) >= 9 and r[:2] == b"\x86\x00" and len(r) == 9 + r[8]
        ctx.check("status response well formed", ok, r.hex(" "))
        if ok:
            ctx.metric("flags", r[2])
            ctx.metric("rssi_dbm", struct.unpack("b", r[7:8])[0])
        ctx.check("bad op -> ARG", lk.xfer(b"\x86\x07")[1] == ARG)
        ctx.summary(f"flags {r[2]:#x}" if ok else "malformed")
