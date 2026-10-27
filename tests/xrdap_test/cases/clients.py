"""Several clients (MC-*): one TCP client at a time."""

from .. import link as linkmod
from .. import runner as R
from ..vendor import OK, numbered


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
