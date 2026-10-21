"""Several clients (MC-*): TCP single-client rule, USB and network at once, sampling owner."""

import threading
import time

from .. import link as linkmod
from .. import runner as R
from ..dap import Dap
from ..vendor import OK, Item, Vendor, capture, numbered


def second_link(ctx):
    if not ctx.args.link2:
        raise R.NotApplicable("needs --link2 (a second path to the same probe)")
    return ctx.args.link2


@R.case("MC-01", "Second TCP client: one client at a time, sampling of the first stops",
        links=("tcp",))
def mc_second_tcp(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        items = numbered(t.word_items(2))
        ctx.check("first client samples", vendor.start(1000, items) == OK)
        b = linkmod.open_link(ctx.spec)
        try:
            served_b = b.xfer(b"\x00\xFF", timeout=2.0)[:1] == b"\x00"
        except Exception:
            served_b = False
        try:
            served_a = lk.xfer(b"\x00\xFF", timeout=2.0)[:1] == b"\x00"
        except Exception:
            served_a = False
        ctx.metric("served", {"first": served_a, "second": served_b})
        ctx.check("exactly one client is served", served_a != served_b)
        survivor = b if served_b else lk
        vs = Vendor(survivor)
        r1 = vs.sub_read([], 0).remaining
        time.sleep(0.3)
        r2 = vs.sub_read([], 0).remaining
        ctx.check("sampling of the first client stopped", r2 <= r1, f"{r1} -> {r2}")
        b.close()
    ctx.summary("second client " + ("takes over" if served_b else "refused"))


@R.case("MC-02", "Two links to one probe at once: interleaved READs stay correct")
def mc_two_links(ctx):
    t = ctx.need_target()
    spec2 = second_link(ctx)
    errors, counts = [], {}

    def worker(spec, name):
        lk = linkmod.open_link(spec)
        try:
            d = Dap(lk)
            d.query_packet()
            d.attach(ctx.clock)
            v = Vendor(lk)
            items = numbered([Item(t.at(t.V_U32)), Item(t.at(t.CHAIN_P1), 4, (0, 0, 0))])
            n, end = 0, time.monotonic() + 5
            while time.monotonic() < end:
                status, entries = v.read(items)
                if status != OK or [e[2] for e in entries] != [0x89ABCDEF, 0xCAFEBABE]:
                    errors.append((name, status, entries))
                n += 1
            counts[name] = n
        except Exception as exc:
            errors.append((name, repr(exc)))
        finally:
            lk.close()

    with ctx.session():
        pass  # writes the fixture
    threads = [threading.Thread(target=worker, args=(s, n)) for s, n in ((ctx.spec, "a"), (spec2, "b"))]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    ctx.metric("reads", counts)
    ctx.check("both links served, no wrong value", not errors and len(counts) == 2, errors[:3])
    ctx.summary(f"reads a {counts.get('a')} b {counts.get('b')}, {len(errors)} errors")


@R.case("MC-03", "Sampling owner change: link A samples, link B stops and takes over")
def mc_owner(ctx):
    t = ctx.need_target()
    spec2 = second_link(ctx)
    items = numbered(t.word_items(2))
    with ctx.session() as (lk, dap, vendor):
        ctx.check("A starts sampling", vendor.start(1000, items) == OK)
        lk2 = linkmod.open_link(spec2)
        try:
            d2 = Dap(lk2)
            d2.query_packet()
            v2 = Vendor(lk2)
            r = v2.sub_read(items)
            ctx.check("B reads A's frames", r.status == OK and len(r.frames) > 0, len(r.frames))
            ctx.check("B stops", v2.stop() == OK)
            d2.attach(ctx.clock)
            cap = capture(v2, items, 1.0, 1000)
            ctx.check("B samples", len(cap.frames) > 500, len(cap.frames))
        finally:
            lk2.close()
    ctx.summary("owner change works")
