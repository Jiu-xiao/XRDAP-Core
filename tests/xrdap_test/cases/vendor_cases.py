"""XR vendor extension (VND-*)."""

import statistics
import struct
import time

from .. import runner as R
from ..dap import AP_CSW, AP_IDR
from ..target import CPUID, DEMCR, DHCSR, AIRCR, DBGKEY, DWT_CTRL, DWT_CYCCNT, S_HALT
from ..vendor import (ARG, NO_TARGET, OK, PROTOCOL_VERSION, RESOURCE, STATE, WIFI_CLEAR_CONFIRM, Item,
                      capture, numbered, tick_gaps)


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
        ctx.check("protocol 2, 22-byte INFO, 32 items, depth 8",
                  (info["version"], info["length"], info["max_items"], info["max_depth"])
                  == (PROTOCOL_VERSION, 22, 32, 8), info)
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
        ctx.check("32 x 8 B (355-byte response) fits every packet size", vendor.read(big)[0] == OK)
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
                ("size 3", 2000, 0, [Item(base.base, size=3)], ARG),
                ("33 items", 2000, 0, [Item(base.base) for _ in range(33)], RESOURCE)):
            ctx.check(f"SUB_START {name} -> {want:#x}", vendor.start(period, numbered(items), flags) == want)
        ctx.check("INFO with extra byte -> ARG", lk.xfer(b"\x80\x00")[1] == ARG)
        ctx.check("SUB_CTRL op 1 (start, removed) -> ARG", vendor.sub_ctrl(1) == ARG)
        ctx.check("SUB_CTRL bad op -> ARG", vendor.sub_ctrl(3) == ARG)
        ctx.check("SUB_CTRL short -> ARG", lk.xfer(b"\x84")[1] == ARG)
        ctx.check("READ length mismatch -> ARG", lk.xfer(b"\x81\x01\x00")[1] == ARG)
        idle = vendor.sub_read([base])
        ctx.check("SUB_READ when not sampling -> 0x02 with the full head", idle.status == STATE
                  and idle.probe_time_us > 0, idle.status)
        ctx.check("SUB_START", vendor.start(100000, numbered([base])) == OK)
        ctx.check("SUB_READ while sampling -> OK", vendor.sub_read([base]).status == OK)
        ctx.check("SUB_START while running -> STATE", vendor.start(100000, numbered([base])) == STATE)
        vendor.stop()
        ctx.check("stop when stopped -> OK", vendor.stop() == OK)
    with ctx.link() as lk:  # fresh link: no DAP_Connect yet
        from ..vendor import Vendor

        v = Vendor(lk)
        lk.xfer(bytes([0x03]))  # DAP_Disconnect
        ctx.check("READ before DAP_Connect -> NO_TARGET", v.read(numbered([Item(t.at(t.V_U32))]))[0] == NO_TARGET)
        ctx.check("SUB_START before DAP_Connect -> NO_TARGET",
                  v.start(100000, numbered([Item(t.at(t.V_U32))])) == NO_TARGET)
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


def keeps_up(vendor, items, period, tried):
    """SPEC 2.3 (decision B9): 3 s with 0 skipped, 0 dropped, 0 failed items, no tick gap,
    and the ring read empty after the stop."""
    try:
        cap = capture(vendor, items, R.SEARCH_RUN_S, period)
    except RuntimeError as exc:
        tried.append((period, str(exc)))
        return False
    failed = sum(1 for f in cap.frames for v in f[2].values() if v is None)
    gaps = tick_gaps(cap.frames, items)
    tried.append((period, cap.skipped, cap.dropped, failed, gaps, cap.left))
    return bool(cap.frames) and not (cap.skipped or cap.dropped or failed or gaps or cap.left)


def search_min_period(vendor, items):
    """SPEC 2.3: double from 25 us to the first period that keeps up (P), then from P/2 up in
    25 us steps; the first period that keeps up is the minimum."""
    tried, period = [], R.SEARCH_STEP_US
    while not keeps_up(vendor, items, period, tried):
        period *= 2
        if period > 1_000_000:
            return None, tried
    top, period = period, max(R.SEARCH_STEP_US, period // 2)
    while period < top and not keeps_up(vendor, items, period, tried):
        period += R.SEARCH_STEP_US
    return min(period, top), tried


@R.case("VND-06", "Sampling: minimum period that keeps up for 1/4/8/32 variables")
def vnd_capacity(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        out = {}
        for n in R.VAR_COUNTS:
            items = numbered(t.word_items(n))
            best, tried = search_min_period(vendor, items)
            out[n] = {"min_period_us": best}
            ctx.log(f"  {n} vars: min period {best} us, last tries {tried[-3:]}")
            ctx.check(f"{n} vars: a period that keeps up exists", best is not None)
        ctx.metric("capacity", out)
        ctx.summary(" ".join(f"{n}:{v['min_period_us']}" for n, v in out.items())
                    + " (vars:min period us)")


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


def distribution(values):
    """Percentiles, extremes and a coarse histogram (us) of a list of numbers."""
    if not values:
        return {}
    v = sorted(values)

    def pct(p):
        return v[min(len(v) - 1, int(len(v) * p))]

    edges = (5, 10, 20, 50, 100, 200, 500)
    hist = {f"<={e}": sum(1 for x in v if abs(x) <= e) for e in edges}
    hist[">500"] = sum(1 for x in v if abs(x) > 500)
    return {"n": len(v), "min": v[0], "p50": pct(0.5), "p90": pct(0.9), "p99": pct(0.99),
            "p999": pct(0.999), "max": v[-1], "hist_abs_cumulative": hist}


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
    per_item = {i.id: sum(1 for f in frames if i.id in f[2] and f[2][i.id] is None) for i in items}
    first = next(((k, ts) for k, ts, v in frames if None in v.values()), None)
    ctx.check("no failed items", failed == 0,
              f"{failed} failed, per item id {per_item}, first at tick {first[0]}" if first else "")
    if first:
        ctx.metric("failed_items", {"per_item": per_item, "first_tick": first[0],
                                    "first_after_s": round((first[1] - frames[0][1]) / 1e6, 1)})
    ctx.check("constants and 3-level chain", bad_const == 0, f"{bad_const} wrong values")
    gaps = tick_gaps(frames, items)
    ctx.check("no tick gaps, 0 skipped, 0 dropped", not (gaps or cap.skipped or cap.dropped),
              f"gaps {gaps} skipped {cap.skipped} dropped {cap.dropped}")
    base_k, base_t = frames[0][0], frames[0][1]
    dev = [ts - base_t - (k - base_k) * period for k, ts, _ in frames]
    out = {"frames": len(frames), "dev_min_us": min(dev), "dev_max_us": max(dev),
           "dev_p99_abs_us": sorted(abs(d) for d in dev)[int(len(dev) * 0.99) - 1],
           "dev_abs_us": distribution([abs(d) for d in dev]),
           # Time between consecutive frames minus the ticks they are apart x period.
           "interval_error_us": distribution(
               [(b[1] - a[1]) - (b[0] - a[0]) * period for a, b in zip(frames, frames[1:])])}
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
            # Sampling instants seen by the target's own cycle counter.
            out["target_interval_error_us"] = distribution([round(x, 1) for x in real])
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
        cap = capture(vendor, items, 5.0, 2000, poll_s=0.005)
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
        cap = capture(vendor, items, 10.0, 1000, poll_s=0.005)
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
        # Start at 2 ms with SWD slowed to 100 kHz: every tick is late.
        dap.swj_clock(100_000)
        cap = capture(vendor, items, 2.0, 2000)
        dap.swj_clock(ctx.clock)
        # Missing ticks visible in the stream: before the first frame and between frames.
        # Ticks skipped after the last frame are counted but cannot be seen.
        gaps = tick_gaps(cap.frames, items) + (cap.frames[0][0] if cap.frames else 0)
        ctx.check("overrun counts skipped ticks", cap.skipped > 0, f"skipped {cap.skipped}")
        ctx.check("every missing tick is counted as skipped", gaps <= cap.skipped,
                  f"missing {gaps}, skipped {cap.skipped}")
        ctx.metric("skipped_not_seen", cap.skipped - gaps)
        # Overflow, overwrite mode: no reads for 2 s.
        vendor.start(1000, items)
        time.sleep(2.0)
        r = vendor.sub_read(items)
        vendor.stop()
        ctx.check("no reads: dropped counted (overwrite)", r.dropped > 0, f"dropped {r.dropped}")
        # Stop-on-full: oldest frames kept, new ones dropped.
        vendor.start(1000, items, flags=1)
        time.sleep(2.0)
        r = vendor.sub_read(items)
        ctx.check("stop-on-full keeps the first frames", r.frames and r.frames[0][0] == 0,
                  r.frames[0][0] if r.frames else None)
        ctx.check("stop-on-full counts dropped", r.dropped > 0, r.dropped)
        vendor.stop()
        # max_bytes below one frame: no partial frame, data stays queued.
        vendor.start(1000, items)
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
        ctx.metric("ap_idr", hex(dap.ap_read(AP_IDR)))
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
        first = capture(vendor, a, 0.5, 1000)
        vendor.start(1000, b)
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
        ok = len(r) >= 11 and r[:2] == b"\x86\x00" and len(r) == 11 + r[8]
        ctx.check("status response well formed (11 + SSID length bytes)", ok, r.hex(" "))
        if ok:
            n = r[8]
            ctx.metric("flags", r[2])
            ctx.metric("rssi_dbm", struct.unpack("b", r[7:8])[0])
            ctx.metric("last_clear_source", r[9 + n])
            ctx.metric("reset_reason", r[10 + n])
        ctx.check("bad op -> ARG", lk.xfer(b"\x86\x07")[1] == ARG)
        # Decision A4: a clear without the confirm byte (or with a wrong one) changes nothing.
        ctx.check("86 02 without confirm byte -> ARG", lk.xfer(b"\x86\x02") == b"\x86\x01")
        wrong = bytes([0x86, 0x02, WIFI_CLEAR_CONFIRM ^ 0xFF])
        ctx.check("86 02 with a wrong confirm byte -> ARG", lk.xfer(wrong) == b"\x86\x01")
        after = lk.xfer(b"\x86\x00")
        ctx.check("credentials unchanged", ok and after[8:9 + after[8]] == r[8:9 + r[8]],
                  "SSID changed" if ok else "")
        ctx.summary(f"flags {r[2]:#x}, unconfirmed clear refused" if ok else "malformed")


@R.case("VND-14", "Response space: a vendor command that does not fit the space left answers 0x03")
def vnd_space(ctx):
    """B8: inside DAP_ExecuteCommands, fillers use up the response buffer until about 50 bytes
    are left; a READ needing 91 bytes must answer [81 03] and the packet must stay in bounds."""
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        items = numbered([Item(t.at(t.V_U64), 8) for _ in range(8)])  # response 3 + 8 x 11 = 91
        read = bytes([0x81, len(items)]) + b"".join(i.encode() for i in items)
        k = (dap.packet_size - 2 - 50) // 4  # DAP_Info(0xFF) answers 4 bytes
        request = bytes([0x7F, k + 1]) + bytes([0x00, 0xFF]) * k + read
        r = lk.xfer(request)
        tail = r[2 + 4 * k:]
        ctx.metric("space_left", dap.packet_size - 2 - 4 * k)
        ctx.check("answer within the packet", len(r) <= dap.packet_size, f"{len(r)} bytes")
        ctx.check("READ answers 81 03", tail[:2] == b"\x81\x03", tail[:4].hex(" "))
        ctx.check("link still in sync", lk.xfer(b"\x00\xFF")[:1] == b"\x00")
        # One byte left: the READ answers a single FF (protocol 3.2). An odd-length filler
        # (DAP_Info vendor string) and 2-byte HostStatus answers bring the space to exactly 1.
        odd = len(lk.xfer(b"\x00\x01"))
        h = next((h for h in (0, 1) if (dap.packet_size - 3 - odd - 2 * h) % 4 == 0), None)
        if odd % 2 == 0 or h is None:
            ctx.metric("one_byte_case", "skipped: vendor string answer has even length")
        else:
            k1 = (dap.packet_size - 3 - odd - 2 * h) // 4
            fill = bytes([0x00, 0xFF]) * k1 + bytes([0x00, 0x01]) + bytes([0x01, 0x00, 0x00]) * h
            r = lk.xfer(bytes([0x7F, k1 + 1 + h + 1]) + fill + read)
            ctx.check("one byte left: READ answers FF, packet exactly full",
                      len(r) == dap.packet_size and r[-1:] == b"\xFF", f"{len(r)} bytes, last {r[-1:].hex()}")
            lk.xfer(b"\x01\x00\x00")
        ctx.summary(f"{dap.packet_size - 2 - 4 * k} bytes left: {tail[:2].hex(' ')}")


@R.case("VND-15", "Serial lease: serial-started sampling stops 5 s after the last request",
        links=("serial",))
def vnd_lease(ctx):
    """Protocol 3.1 stop table: no serial request for 5 s stops sampling started over serial;
    the frames stay readable."""
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        items = numbered(t.word_items(1))
        # 100 ms period: 7 s of frames stay far below the smallest ring, so a full ring
        # cannot fake a stopped one.
        ctx.check("start", vendor.start(100_000, items) == OK)
        time.sleep(7.0)  # port open, no request
        first = vendor.sub_read(items, 0).remaining
        time.sleep(1.0)
        second = vendor.sub_read(items, 0).remaining
        ctx.check("ring stopped growing after the lease", second == first, f"{first} -> {second}")
        ctx.check("frames still readable", first > 0, first)
        vendor.stop()
        ctx.summary(f"stopped, {first} bytes kept")
