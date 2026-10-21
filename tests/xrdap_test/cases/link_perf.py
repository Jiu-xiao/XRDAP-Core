"""SWD link (SWD-*) and standard DAP performance (PERF-*)."""

import os
import random
import statistics
import struct
import time
import zlib

from .. import runner as R
from ..dap import AP_DRW, AP_TAR, DapError, req


def pipeline(link, requests, depth):
    """Send requests keeping up to `depth` in flight; responses in order."""
    out, sent = [], 0
    while len(out) < len(requests):
        while sent < len(requests) and sent - len(out) < depth:
            link.send(requests[sent])
            sent += 1
        out.append(link.recv())
    return out


def read_word_request(addr):
    return bytes([0x05, 0, 2, req(1, 0, AP_TAR)]) + struct.pack("<I", addr) + bytes([req(1, 1, AP_DRW)])


def max_depth(link, dap):
    return 1 if link.kind == "serial" else min(dap.packet_count, link.max_in_flight)


def swclk_estimate(link, dap, budget_s=0.6):
    """Actual SWCLK from DAP_ExecuteCommands of N SWJ_Sequence: 256-bit minus 64-bit runs.
    Both packets cost about the same to move, the difference is N x 192 clocks."""
    n = min(20, (dap.packet_size - 2) // 34)

    def best(bits):
        sub = bytes([0x12, 0 if bits == 256 else bits]) + b"\x00" * (bits // 8)
        packet = bytes([0x7F, n]) + sub * n
        times = []
        end = time.monotonic() + budget_s
        while time.monotonic() < end or len(times) < 5:
            t = time.perf_counter()
            r = link.xfer(packet)
            times.append(time.perf_counter() - t)
            if r[:2] != bytes([0x7F, n]):
                raise DapError(f"ExecuteCommands answer {r[:4].hex(' ')}")
        times.sort()
        return statistics.mean(times[:max(3, len(times) // 5)])  # fastest fifth

    diff = best(256) - best(64)
    return n * 192 / diff if diff > 0 else None


def integrity_rounds(dap, base, nbytes, rounds, budget_s=5.0):
    """Random block write + readback, compared by CRC32, `rounds` times or until the time
    budget is spent (at least one round). Returns (rounds done, errors, KiB/s)."""
    errors, moved, done = 0, 0, 0
    t0 = time.perf_counter()
    while done < rounds and (done == 0 or time.perf_counter() - t0 < budget_s):
        done += 1
        data = os.urandom(nbytes)
        try:
            dap.block_write(base, data)
            back = dap.block_read(base, nbytes)
            if zlib.crc32(back) != zlib.crc32(data):
                errors += 1
            moved += 2 * nbytes
        except (DapError, TimeoutError):
            errors += 1
            dap.clear_errors()
    return done, errors, moved / 1024 / (time.perf_counter() - t0)


@R.case("SWD-01", "SWD clock sweep: actual clock, data integrity, max stable clock")
def swd_clock_sweep(ctx):
    t = ctx.need_target()
    base = t.at(t.BLOCK)
    rows, best = [], None
    for hz in R.SWEEP_CLOCKS:
        row = {"request_hz": hz}
        # 1 KiB blocks below 1 MHz keep the slow clocks within the time budget.
        nbytes = min(4096 if hz >= 1_000_000 else 1024, t.scratch.size - t.BLOCK)
        try:
            with ctx.session(hz, fixture=False) as (lk, dap, _):
                est = swclk_estimate(lk, dap)
                row["actual_mhz"] = round(est / 1e6, 2) if est else None
                row["rounds"], row["errors"], kib = integrity_rounds(dap, base, nbytes, ctx.args.rounds)
                row["block_bytes"], row["kib_s"] = nbytes, round(kib, 1)
        except (DapError, TimeoutError) as exc:
            row["rounds"], row["errors"], row["attach"] = 0, 1, str(exc)
        rows.append(row)
        ctx.log(f"  {hz / 1e6:6.1f} MHz -> actual {row.get('actual_mhz')} MHz, "
                f"{row['errors']}/{row['rounds']} bad rounds of {nbytes} B, {row.get('kib_s')} KiB/s")
        if row["errors"] == 0:
            best = hz
    ctx.metric("sweep", rows)
    ctx.metric("max_stable_hz", best)
    ctx.shared["max_clock"] = best
    # CMSIS-DAP: whatever clock the probe sets for a request, transfers must stay correct.
    ctx.check("zero errors at every requested clock", all(r["errors"] == 0 for r in rows),
              [r["request_hz"] for r in rows if r["errors"]])
    ctx.summary(f"max stable {best / 1e6 if best else 0:g} MHz request")


@R.case("SWD-02", "Pipelining depth 1..N: response order and data")
def swd_pipelining(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        top = max_depth(lk, dap)
        ctx.metric("max_depth", top)
        rates = {}
        for depth in range(1, top + 1):
            requests = [read_word_request(t.at(t.WORDS + 4 * (i % 32))) for i in range(256)]
            t0 = time.perf_counter()
            responses = pipeline(lk, requests, depth)
            rates[depth] = round(len(requests) / (time.perf_counter() - t0))
            bad = [i for i, r in enumerate(responses)
                   if len(r) < 7 or r[2] != 1 or int.from_bytes(r[3:7], "little") != 0x5A000000 | i % 32]
            ctx.check(f"depth {depth}: 256 responses in order", not bad,
                      f"first bad {bad[:3]}" if bad else "")
        ctx.metric("requests_per_s", rates)
        ctx.summary(f"depth 1..{top} ok, {rates[top]} req/s at {top}")


@R.case("PERF-01", "Round trip: DAP_Info and one DP read, median / p99")
def perf_rtt(ctx):
    with ctx.session(fixture=False) as (lk, dap, _):
        out = {}
        for name, packet in (("dap_info", bytes([0x00, 0xFF])),
                             ("dp_read", bytes([0x05, 0, 1, req(0, 1, 0)]))):
            times = []
            end = time.monotonic() + 3.0
            while len(times) < 1000 and time.monotonic() < end:
                t0 = time.perf_counter()
                lk.xfer(packet)
                times.append((time.perf_counter() - t0) * 1e6)
            times.sort()
            out[name] = {"n": len(times), "median_us": round(statistics.median(times), 1),
                         "p99_us": round(times[int(len(times) * 0.99) - 1], 1),
                         "mean_us": round(statistics.mean(times), 1)}
            ctx.metric(name, out[name])
        ctx.check("1000 round trips or 3 s without error", True)
        ctx.summary(f"DAP_Info {out['dap_info']['median_us']}/{out['dap_info']['p99_us']} us, "
                    f"DP read {out['dp_read']['median_us']}/{out['dp_read']['p99_us']} us")


def block_rates(ctx, lk, dap, base, nbytes, passes=3):
    data = os.urandom(nbytes)
    t0 = time.perf_counter()
    for _ in range(passes):
        dap.block_write(base, data)
    write = nbytes * passes / 1024 / (time.perf_counter() - t0)
    t0 = time.perf_counter()
    for _ in range(passes):
        back = dap.block_read(base, nbytes)
    read = nbytes * passes / 1024 / (time.perf_counter() - t0)
    # Pipelined read: TAR write and TransferBlock for each chunk, up to the probe depth.
    requests = []
    for a, words in dap.chunks(base, nbytes, False):
        requests.append(bytes([0x05, 0, 1, req(1, 0, AP_TAR)]) + struct.pack("<I", a))
        requests.append(struct.pack("<BBHB", 0x06, 0, words, req(1, 1, AP_DRW)))
    depth = max_depth(lk, dap)
    t0 = time.perf_counter()
    for _ in range(passes):
        responses = pipeline(lk, requests, depth)
    piped = nbytes * passes / 1024 / (time.perf_counter() - t0)
    piped_data = b"".join(r[4:] for r in responses[1::2])
    return {"write_kib_s": round(write, 1), "read_kib_s": round(read, 1),
            "read_pipelined_kib_s": round(piped, 1), "depth": depth,
            "ok": back == data and piped_data == data}


@R.case("PERF-02", "Block read/write throughput at 4 MHz, 10 MHz and the max stable clock")
def perf_block(ctx):
    t = ctx.need_target()
    nbytes = min(16384, (t.scratch.size - t.BLOCK) // 1024 * 1024)
    clocks = list(R.PERF_CLOCKS)
    top = ctx.shared.get("max_clock") or ctx.args.max_clock
    if top and top not in clocks:
        clocks.append(top)
    rows = {}
    for hz in clocks:
        with ctx.session(hz, fixture=False) as (lk, dap, _):
            rows[hz] = block_rates(ctx, lk, dap, t.at(t.BLOCK), nbytes)
            ctx.log(f"  {hz / 1e6:g} MHz: {rows[hz]}")
            ctx.check(f"{hz / 1e6:g} MHz data intact", rows[hz]["ok"])
    ctx.metric("bytes", nbytes)
    ctx.metric("block", {str(k): v for k, v in rows.items()})
    last = rows[clocks[-1]]
    ctx.summary(f"@{clocks[-1] / 1e6:g} MHz W {last['write_kib_s']} / R {last['read_kib_s']}"
                f" / R pipelined {last['read_pipelined_kib_s']} KiB/s")


@R.case("PERF-03", "Small transfers: 4 B and 64 B at random addresses, operations/s")
def perf_small(ctx):
    t = ctx.need_target()
    span = (t.scratch.size - t.BLOCK) // 64
    rnd = random.Random(1)
    with ctx.session() as (lk, dap, _):
        dap.dp_write(0x8, 0)
        out = {}

        def rate(fn):
            n, end = 0, time.monotonic() + 2.0
            t0 = time.perf_counter()
            while time.monotonic() < end:
                fn(t.at(t.BLOCK) + 64 * rnd.randrange(span))
                n += 1
            return round(n / (time.perf_counter() - t0))

        pattern = bytes(range(64))
        out["read4_per_s"] = rate(lambda a: dap.read32(a))
        out["write4_per_s"] = rate(lambda a: dap.write32(a, a))
        out["write64_per_s"] = rate(lambda a: dap.block_write(a, pattern))
        out["read64_per_s"] = rate(lambda a: dap.block_read(a, 64))
        a = t.at(t.BLOCK)
        dap.block_write(a, pattern)
        dap.write32(a + 64, 0x600DF00D)
        ctx.check("values read back", dap.block_read(a, 64) == pattern and dap.read32(a + 64) == 0x600DF00D)
        for k, v in out.items():
            ctx.metric(k, v)
        ctx.summary(f"4 B R/W {out['read4_per_s']}/{out['write4_per_s']}/s, "
                    f"64 B R/W {out['read64_per_s']}/{out['write64_per_s']}/s")
