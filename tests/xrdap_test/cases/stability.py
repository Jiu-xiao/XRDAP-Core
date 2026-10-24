"""Stability (STAB-*): long runs and recovery after losing a link, the target or the host."""

import contextlib
import os
import subprocess
import sys
import time

from .. import hosttools as H
from .. import link as linkmod
from .. import runner as R
from ..vendor import OK, Vendor, capture, numbered
from .vendor_cases import check_stream, counter_item, divided_set, need_vendor


@R.case("STAB-01", "Long sampling: divided 8-item set, 2 ms, --long-s (default 1 h)")
def stab_sampling(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        counter, kind = counter_item(ctx, t, dap)
        items = divided_set(t, counter)
        cap = capture(vendor, items, ctx.args.long_s, 2000, poll_s=0.005)
        out = check_stream(ctx, cap, items, 2000, kind)
        ctx.metric("stream", out)
        ctx.metric("seconds", round(cap.seconds))
        ctx.summary(f"{out.get('frames')} frames in {cap.seconds:.0f} s, dev "
                    f"{out.get('dev_min_us')}..{out.get('dev_max_us')} us")


@R.case("STAB-02", "OpenOCD program/verify loop for --long-s (default 1 h)")
def stab_openocd(ctx):
    from .tools import openocd_ready

    t = openocd_ready(ctx)
    region = t.flash_test
    if not region:
        raise R.NotApplicable("target profile has no flash_test region")
    size = min(16384, region.size)
    end = time.monotonic() + ctx.args.long_s
    runs = fails = 0
    with H.openocd_adapter(ctx) as adapter:
        while time.monotonic() < end:
            pattern = H.tmpfile(ctx, "loop_pattern.bin", os.urandom(size))
            rc, log, _ = H.run_openocd(ctx, "loop", adapter, ctx.clock // 1000, [
                "init", "reset halt", f"flash write_image erase {pattern} {region.base:#x} bin",
                f"verify_image {pattern} {region.base:#x} bin", "reset run", "shutdown"], 600)
            runs += 1
            if rc != 0 or "verified" not in log.lower():
                fails += 1
                os.replace(os.path.join(ctx.out_dir, "openocd", "loop.log"),
                           os.path.join(ctx.out_dir, "openocd", f"loop_fail_{runs}.log"))
    ctx.metric("runs", runs)
    ctx.check("every program + verify passed", fails == 0, f"{fails}/{runs} failed")
    ctx.summary(f"{runs - fails}/{runs} program + verify")


def wait_probe(ctx, gone, timeout):
    """Poll until the probe stops answering (gone=True) or answers DAP_Info again."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            with ctx.link() as lk:
                lk.xfer(b"\x00\xFF", timeout=0.5)
            if not gone:
                return time.monotonic() - t0
        except Exception:
            if gone:
                return time.monotonic() - t0
        time.sleep(0.05)
    return None


def start_sampling(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        need_vendor(vendor)
        items = numbered(t.word_items(4))
        vendor.start(1000, items)
        return items


def after_recovery(ctx):
    """The probe after an interruption: standard commands work and sampling can restart."""
    t = ctx.target
    with ctx.session() as (lk, dap, vendor):
        ctx.check("DPIDR readable", dap.dp_read(0) != 0)
        items = numbered(t.word_items(4))
        cap = capture(vendor, items, 1.0, 1000)
        ctx.check("sampling runs again", len(cap.frames) > 500, len(cap.frames))


@R.case("STAB-03", "Replug the probe while sampling: sampling stops, the link comes back",
        mode=R.ASSISTED, links=("usb", "serial", "radio"))
def stab_replug(ctx):
    if not ctx.args.replug_cmd:
        ctx.need_operator()
    start_sampling(ctx)
    if ctx.args.replug_cmd:
        subprocess.run(ctx.args.replug_cmd, shell=True, check=True)
    else:
        ctx.ask("unplug the probe's USB cable, wait 2 s, plug it back")
    back = wait_probe(ctx, False, 30)
    ctx.check("probe answers again within 30 s", back is not None)
    with ctx.link() as lk:
        v = Vendor(lk)
        first = v.sub_read([], 0).remaining
        time.sleep(0.5)
        second = v.sub_read([], 0).remaining
        ctx.check("sampling stopped: the ring no longer grows", second <= first, f"{first} -> {second}")
        v.stop()
    after_recovery(ctx)
    ctx.metric("back_s_after_replug", round(back, 2) if back else None)
    ctx.summary(f"back after {back:.1f} s" if back else "did not come back")


@R.case("STAB-04", "Target power loss during sampling and after", mode=R.ASSISTED)
def stab_target_power(ctx):
    ctx.need_operator()
    t = ctx.need_target()
    with ctx.session() as (lk, dap, vendor):
        items = numbered(t.word_items(4))
        vendor.start(1000, items)
        ctx.ask("remove the target's power (keep the probe powered), wait 2 s")
        r = vendor.sub_read(items)
        failed = sum(1 for f in r.frames for v in f[2].values() if v is None)
        ctx.check("probe answers while the target is off", r.status == OK)
        ctx.metric("failed_items_seen", failed)
        vendor.stop()
        ctx.ask("power the target again")
    after_recovery(ctx)
    ctx.summary("probe kept answering, recovered")


@R.case("STAB-05", "Wi-Fi access point loss: time until the probe is reachable again",
        mode=R.ASSISTED, links=("tcp", "usbip"))
def stab_wifi(ctx):
    ctx.need_operator()
    ctx.ask("switch the access point off")
    gone = wait_probe(ctx, True, 60)
    ctx.check("probe unreachable while the AP is off", gone is not None)
    ctx.ask("switch the access point on, then press Enter at once")
    back = wait_probe(ctx, False, 300)
    ctx.check("probe reachable again within 300 s", back is not None)
    ctx.metric("reconnect_s", round(back, 1) if back else None)
    if back:
        after_recovery(ctx)
    ctx.summary(f"reachable {back:.1f} s after the AP came back" if back else "not back")


CHILD = """
import os, sys
sys.path.insert(0, {root!r})
from xrdap_test import link
from xrdap_test.dap import Dap
from xrdap_test.vendor import Vendor, numbered, Item
lk = link.open_link({spec!r})
v = Vendor(lk); v.stop()
d = Dap(lk); d.query_packet(); d.attach({clock})
v.start(1000, numbered([Item({addr})]))
os._exit(0)  # no stop, no DAP_Disconnect, no close
"""


@R.case("STAB-06", "Host exits without stop or DAP_Disconnect: the next client recovers")
def stab_host_exit(ctx):
    t = ctx.need_target()
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    code = CHILD.format(root=root, spec=ctx.spec, clock=ctx.clock, addr=t.at(t.V_U32))
    p = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=60)
    ctx.check("client started sampling and exited", p.returncode == 0, p.stderr[-300:])
    t0 = time.monotonic()
    with ctx.link() as lk:
        v = Vendor(lk)
        ctx.check("next client can stop the sampling", v.stop() == OK)
    after_recovery(ctx)
    ctx.metric("recover_s", round(time.monotonic() - t0, 2))
    ctx.summary("next client recovered")


@R.case("STAB-07", "Probe restart: boot time to the first DAP_Info answer, sampling works again", mode=R.ASSISTED)
def stab_restart(ctx):
    if not ctx.args.restart_cmd:
        ctx.need_operator()
    start_sampling(ctx)
    with ctx.link() as lk:
        before = Vendor(lk).info()["probe_time_us"]
    if ctx.args.restart_cmd:
        subprocess.run(ctx.args.restart_cmd, shell=True, check=True)
    else:
        ctx.ask("after Enter, press the probe's reset button (or power-cycle it) within 10 s")
    # Measured from the moment the probe stops answering; a restart command that returns
    # after the reboot has begun leaves nothing to see, so it is timed from its end.
    gone = wait_probe(ctx, True, 10)
    back = wait_probe(ctx, False, 60)
    ctx.check("probe answers again within 60 s", back is not None)
    ctx.metric("down_seen", gone is not None)
    ctx.metric("ready_s", round(back, 2) if back else None)
    if back:
        with ctx.link() as lk:
            after = Vendor(lk).info()["probe_time_us"]
        ctx.check("the probe restarted (its clock started again)", after < before,
                  f"{before // 1000} ms -> {after // 1000} ms")
        after_recovery(ctx)
    ctx.summary(f"ready {back:.1f} s after the restart was seen" if back else "not back")


def wifi_and_boot(spec, timeout=20.0):
    """Saved SSID, last clear source and reset reason (0x86 op 0, protocol 3.7) and the
    probe's boot moment (wall time minus probe clock), read over another link; retried while
    the session under test still owns the probe (answers 86 05 / 80 05)."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            with contextlib.closing(linkmod.open_link(spec)) as lk:
                r = lk.xfer(b"\x86\x00")
                t0 = time.time()
                info = Vendor(lk).info()
                rtt = time.time() - t0
                lk.xfer(b"\x03")  # end the checker's session so the next serial step can own the probe
                if r[:2] == b"\x86\x00" and info.get("status") == OK:
                    n = r[8]
                    ssid = bytes(r[9:9 + n])
                    why = {"clear_source": r[9 + n], "reset_reason": r[10 + n]} if len(r) >= 11 + n else {}
                    return ssid, t0 + rtt / 2 - info["probe_time_us"] / 1e6, why
        except Exception:
            pass
        if time.monotonic() > deadline:
            raise RuntimeError(f"no Wi-Fi status over {spec}")
        time.sleep(1.0)


@R.case("STAB-08", "Serial session hygiene: Wi-Fi credentials and probe uptime survive xrdap share "
        "runs, OpenOCD attaches and a killed share", links=("serial",))
def stab_serial_hygiene(ctx):
    from .tools import openocd_ready

    if not ctx.args.link2 or linkmod.parse_spec(ctx.args.link2)["kind"] == "serial":
        raise R.NotApplicable("needs --link2 over another transport (tcp:HOST or usb), not serial")
    t = openocd_ready(ctx)
    ssid0, boot0, why0 = wifi_and_boot(ctx.args.link2)
    ctx.metric("before", why0)
    if not ssid0:
        raise R.NotApplicable("needs saved Wi-Fi credentials on the probe")
    hold = ctx.args.share_hold_s

    def python_session():
        with ctx.link() as lk:
            end = time.monotonic() + hold
            while time.monotonic() < end:
                lk.xfer(b"\x00\xFF")
                time.sleep(1.0)

    def share_idle(kill):
        def step():
            proc = H.start_share(ctx, 3299)
            time.sleep(hold)
            H.stop_share(proc, kill=kill)
        return step

    def share_openocd():
        with H.openocd_adapter(ctx) as adapter:
            for i in range(ctx.args.attach_count):
                H.run_openocd(ctx, f"hygiene_attach_{i:02d}", adapter, ctx.clock // 1000,
                              ["init", "shutdown"], 60)
            n = min(16384, (t.scratch.size - t.BLOCK) // 1024 * 1024)
            src = H.tmpfile(ctx, "hygiene_src.bin", os.urandom(n))
            a = t.at(t.BLOCK)
            H.run_openocd(ctx, "hygiene_sram", adapter, ctx.clock // 1000, [
                "init", "halt", f"load_image {src} {a:#x} bin",
                f"dump_image {H.tmpfile(ctx, 'hygiene_back.bin')} {a:#x} {n}", "resume", "shutdown"])

    steps = [("python serial session", python_session),
             ("xrdap share idle, Ctrl-C", share_idle(False)),
             ("xrdap share with OpenOCD attaches and SRAM, Ctrl-C", share_openocd),
             ("xrdap share idle, process killed", share_idle(True))]
    rows = []
    for name, step in steps:
        step()
        ssid, boot, why = wifi_and_boot(ctx.args.link2)
        restarted = abs(boot - boot0) > 2.0
        rows.append({"step": name, "ssid_kept": ssid == ssid0, "restarted": restarted, **why})
        ctx.check(f"{name}: credentials kept", ssid == ssid0, "SSID gone" if not ssid else "")
        ctx.check(f"{name}: no restart", not restarted, f"boot moved {boot - boot0:+.1f} s")
        if restarted:
            boot0 = boot
    ctx.metric("steps", rows)
    ctx.summary(", ".join(f"{r['step']}: {'ok' if r['ssid_kept'] and not r['restarted'] else 'FAIL'}"
                          for r in rows))
