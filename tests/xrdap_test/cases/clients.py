"""Several clients (MC-*): one TCP client at a time, one session owner across links."""

import time

from .. import link as linkmod
from .. import runner as R
from ..dap import Dap, req
from ..vendor import BUSY, OK, STATE, Item, Vendor, capture, numbered


def second_link(ctx):
    if not ctx.args.link2:
        raise R.NotApplicable("needs --link2 (a second path to the same probe)")
    return ctx.args.link2


def check_busy(ctx, lk, who):
    """Protocol 3.1: a link that does not own the probe still gets DAP_Info; a standard
    command answers `cmd FF`, a vendor command `cmd 05`, nothing is executed."""
    t = ctx.target
    ctx.check(f"{who}: DAP_Info runs", lk.xfer(b"\x00\xFF")[:1] == b"\x00")
    r = lk.xfer(bytes([0x05, 0, 1, req(0, 1, 0)]))
    ctx.check(f"{who}: DAP_Transfer -> 05 FF", r == b"\x05\xFF", r.hex(" "))
    st, _ = Vendor(lk).read(numbered([Item(t.at(t.V_U32))]))
    ctx.check(f"{who}: READ -> 81 05", st == BUSY, st)


@R.case("MC-01", "Second TCP client: one client at a time, the first keeps its session",
        links=("tcp",))
def mc_second_tcp(ctx):
    """Protocol 2: the probe serves one TCP client at a time."""
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        items = numbered(t.word_items(2))
        ctx.check("first client samples", vendor.start(1000, items) == OK)
        try:
            b = linkmod.open_link(ctx.spec)
            try:
                served_b = b.xfer(b"\x00\xFF", timeout=2.0)[:1] == b"\x00"
            finally:
                b.close()
        except Exception:
            served_b = False
        ctx.check("second client is not served", not served_b)
        r = vendor.sub_read(items)
        ctx.check("first client still served, still sampling", r.status == OK and r.frames, r.status)
        vendor.stop()
    ctx.summary("second client refused, first unaffected")


@R.case("MC-02", "Two links to one probe: the second gets busy, the owner is unaffected")
def mc_two_links(ctx):
    """Decision B2, protocol 3.1."""
    t = ctx.need_target()
    spec2 = second_link(ctx)
    with ctx.session() as (lk, dap, vendor):  # link A owns the probe from here
        lk2 = linkmod.open_link(spec2)
        try:
            check_busy(ctx, lk2, "B")
            items = numbered([Item(t.at(t.V_U32)), Item(t.at(t.CHAIN_P1), 4, (0, 0, 0))])
            bad, n, end = 0, 0, time.monotonic() + 5
            while time.monotonic() < end:
                st, entries = vendor.read(items)
                bad += st != OK or [e[2] for e in entries] != [0x89ABCDEF, 0xCAFEBABE]
                n += 1
                if n % 50 == 0:
                    lk2.xfer(bytes([0x05, 0, 1, req(0, 1, 0)]))  # refused traffic in between
            ctx.check("A's reads all correct while B is refused", bad == 0, f"{bad}/{n} wrong")
        finally:
            lk2.close()
    ctx.summary(f"B busy, A {n} reads")


@R.case("MC-03", "Owner change: B is busy while A samples, takes over after A's session ends")
def mc_owner(ctx):
    """Decision B2, protocol 3.1: the session ends with the owner's DAP_Disconnect; frames left
    in the ring stay readable by the next session."""
    t = ctx.need_target()
    spec2 = second_link(ctx)
    items = numbered(t.word_items(2))
    lk2 = linkmod.open_link(spec2)
    try:
        with ctx.session() as (lk, dap, vendor):
            ctx.check("A starts sampling", vendor.start(1000, items) == OK)
            st, _ = Vendor(lk2).read(numbered([Item(t.at(t.V_U32))]))
            ctx.check("B is busy while A samples", st == BUSY, st)
            ctx.check("A stops", vendor.stop() == OK)
            ctx.check("A disconnects", lk.xfer(b"\x03")[:2] == b"\x03\x00")
        v2 = Vendor(lk2)
        left = v2.sub_read(items)
        ctx.check("B reads A's frames (not sampling: 0x02 with data)", left.status == STATE and left.frames,
                  f"status {left.status} frames {len(left.frames)}")
        d2 = Dap(lk2)
        d2.query_packet()
        d2.attach(ctx.clock)
        cap = capture(v2, items, 1.0, 1000)
        ctx.check("B samples", len(cap.frames) > 500, len(cap.frames))
        v2.stop()
        lk2.xfer(b"\x03")
    finally:
        lk2.close()
    ctx.summary("busy, then owner change works")
