"""Host tool matrix (TOOL-*): OpenOCD, pyOCD, probe-rs, Keil."""

import os
import re
import subprocess
import time

from .. import hosttools as H
from .. import runner as R

USB_LIKE = ("usb", "usbip", "radio")


def openocd_ready(ctx):
    t = ctx.need_target()
    if not t.openocd_target:
        raise R.NotApplicable("target profile has no openocd_target")
    if not H.tool_cmd(ctx, "openocd"):
        raise R.NotApplicable("openocd not found (--openocd)")
    ctx.metric("openocd", H.tool_version(H.tool_cmd(ctx, "openocd")))
    return t


@R.case("TOOL-01", "OpenOCD: strict attach x20, SRAM 16 KiB, 128 KiB dump, flash program + verify")
def tool_openocd(ctx):
    t = openocd_ready(ctx)
    khz = ctx.clock // 1000
    with H.openocd_adapter(ctx) as adapter:
        ok, times = 0, []
        for i in range(ctx.args.attach_count):
            rc, log, sec = H.run_openocd(ctx, f"attach_{i:02d}", adapter, khz, ["init", "shutdown"], 60)
            ok += H.strict_examine(rc, log)
            times.append(sec)
        ctx.check(f"strict attach {ctx.args.attach_count}/{ctx.args.attach_count}",
                  ok == ctx.args.attach_count, f"{ok} ok")
        ctx.metric("attach_s_mean", round(sum(times) / len(times), 3))

        n = min(16384, (t.scratch.size - t.BLOCK) // 1024 * 1024)
        src = H.tmpfile(ctx, "sram_src.bin", os.urandom(n))
        back = H.tmpfile(ctx, "sram_back.bin")
        a = t.at(t.BLOCK)
        rc, log, _ = H.run_openocd(ctx, "sram", adapter, khz, ["init", "halt"]
                                   + H.tcl_timed("load", f"load_image {src} {a:#x} bin")
                                   + H.tcl_timed("verify", f"verify_image {src} {a:#x} bin")
                                   + H.tcl_timed("dump", f"dump_image {back} {a:#x} {n}")
                                   + ["resume", "shutdown"])
        ms = H.timings(log)
        same = os.path.exists(back) and open(back, "rb").read() == open(src, "rb").read()
        ctx.check("SRAM load / verify / dump", rc == 0 and same and "dump" in ms, ms)
        ctx.metric("sram_ms", ms)

        if t.dump:
            files = [H.tmpfile(ctx, f"dump{k}.bin") for k in (1, 2)]
            rc, log, _ = H.run_openocd(ctx, "dump", adapter, khz, ["init"] + sum(
                (H.tcl_timed(f"dump{k}", f"dump_image {f} {t.dump.base:#x} {t.dump.size}")
                 for k, f in zip((1, 2), files)), []) + ["shutdown"])
            ms = H.timings(log)
            same = all(os.path.exists(f) for f in files) and \
                open(files[0], "rb").read() == open(files[1], "rb").read()
            ctx.check(f"dump {t.dump.size // 1024} KiB twice, identical", rc == 0 and same, ms)
            ctx.metric("dump_ms", ms)
        if t.flash_test:
            size = min(16384, t.flash_test.size)
            pattern = H.tmpfile(ctx, "flash_pattern.bin", os.urandom(size))
            rc, log, _ = H.run_openocd(ctx, "flash", adapter, khz, ["init", "reset halt"]
                                       + H.tcl_timed("erase", f"flash erase_address {t.flash_test.base:#x} {t.flash_test.size:#x}")
                                       + H.tcl_timed("program", f"flash write_image {pattern} {t.flash_test.base:#x} bin")
                                       + H.tcl_timed("verify", f"verify_image {pattern} {t.flash_test.base:#x} bin")
                                       + ["reset run", "shutdown"], 600)
            ms = H.timings(log)
            ctx.check(f"flash {size // 1024} KiB program + verify", rc == 0 and "verify" in ms
                      and not re.search(r"^Error", log, re.M), ms)
            ctx.metric("flash_ms", ms)
    ctx.summary(f"attach {ok}/{ctx.args.attach_count}, " + ", ".join(
        f"{k} {v}" for k, v in ctx.result.metrics.items() if k.endswith("_ms")))


def usb_tool(ctx, name):
    if ctx.link_kind not in USB_LIKE:
        raise R.NotApplicable(f"{name} talks to USB probes only")
    argv = H.tool_cmd(ctx, name)
    if not argv:
        raise R.NotApplicable(f"{name} not found (--{name})")
    ctx.metric(name, H.tool_version(argv))
    return argv


def run_tool(ctx, label, argv, timeout=300):
    t0 = time.monotonic()
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                           encoding="utf-8", errors="replace")
        rc, log = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        rc, log = -1, "TIMEOUT"
    sec = time.monotonic() - t0
    with open(H.tmpfile(ctx, f"{label}.log"), "w", encoding="utf-8") as f:
        f.write(" ".join(argv) + "\n" + log)
    return rc, log, sec


@R.case("TOOL-02", "probe-rs: attach x20, 128 KiB read, flash download + verify", links=USB_LIKE)
def tool_probe_rs(ctx):
    t = ctx.need_target()
    argv = usb_tool(ctx, "probe-rs")
    if not t.probe_rs_chip:
        raise R.NotApplicable("target profile has no probe_rs_chip")
    khz = str(ctx.clock // 1000)
    with H.usb_view(ctx) as serial:
        common = ["--chip", t.probe_rs_chip, "--probe", f"1d50:6199:{serial}", "--speed", khz]
        ok = 0
        for i in range(ctx.args.attach_count):
            rc, log, _ = run_tool(ctx, f"probe-rs_attach_{i:02d}",
                                  argv + ["read"] + common + ["b32", hex(t.scratch.base), "1"], 60)
            ok += rc == 0
        ctx.check(f"attach {ctx.args.attach_count}/{ctx.args.attach_count}", ok == ctx.args.attach_count, ok)
        if t.dump:
            rc, log, sec = run_tool(ctx, "probe-rs_dump", argv + ["read"] + common +
                                    ["b32", hex(t.dump.base), str(t.dump.size // 4)])
            words = len(re.findall(r"\b[0-9a-fA-F]{8}\b", log))
            ctx.check("dump read", rc == 0 and words >= t.dump.size // 4, f"{words} words")
            ctx.metric("dump_s", round(sec, 2))
        if t.flash_test:
            pattern = H.tmpfile(ctx, "probe-rs_pattern.bin", os.urandom(min(16384, t.flash_test.size)))
            rc, log, sec = run_tool(ctx, "probe-rs_flash", argv + ["download"] + common +
                                    ["--binary-format", "bin", "--base-address", hex(t.flash_test.base),
                                     "--verify", pattern], 600)
            ctx.check("flash download + verify", rc == 0, log.strip().splitlines()[-1:] if log else "")
            ctx.metric("flash_s", round(sec, 2))
    ctx.summary(f"attach {ok}/{ctx.args.attach_count}")


@R.case("TOOL-03", "pyOCD: attach x20, 128 KiB read, flash load + compare", links=USB_LIKE)
def tool_pyocd(ctx):
    t = ctx.need_target()
    argv = usb_tool(ctx, "pyocd")
    if not t.pyocd_target:
        raise R.NotApplicable("target profile has no pyocd_target")
    with H.usb_view(ctx) as serial:
        common = ["-u", serial, "-t", t.pyocd_target, "-f", str(ctx.clock)]
        ok = 0
        for i in range(ctx.args.attach_count):
            rc, _, _ = run_tool(ctx, f"pyocd_attach_{i:02d}", argv + ["commander"] + common +
                                ["-c", f"read32 {t.scratch.base:#x}", "-c", "exit"], 60)
            ok += rc == 0
        ctx.check(f"attach {ctx.args.attach_count}/{ctx.args.attach_count}", ok == ctx.args.attach_count, ok)
        if t.dump:
            out = H.tmpfile(ctx, "pyocd_dump.bin")
            rc, _, sec = run_tool(ctx, "pyocd_dump", argv + ["commander"] + common +
                                  ["-c", f"savemem {t.dump.base:#x} {t.dump.size} {out}", "-c", "exit"])
            ctx.check("dump read", rc == 0 and os.path.exists(out) and os.path.getsize(out) == t.dump.size)
            ctx.metric("dump_s", round(sec, 2))
        if t.flash_test:
            data = os.urandom(min(16384, t.flash_test.size))
            pattern = H.tmpfile(ctx, "pyocd_pattern.bin", data)
            back = H.tmpfile(ctx, "pyocd_back.bin")
            rc, _, sec = run_tool(ctx, "pyocd_flash", argv + ["load"] + common +
                                  ["--base-address", hex(t.flash_test.base), pattern], 600)
            run_tool(ctx, "pyocd_flash_back", argv + ["commander"] + common +
                     ["-c", f"savemem {t.flash_test.base:#x} {len(data)} {back}", "-c", "exit"])
            same = os.path.exists(back) and open(back, "rb").read() == data
            ctx.check("flash load, read back equal", rc == 0 and same)
            ctx.metric("flash_s", round(sec, 2))
    ctx.summary(f"attach {ok}/{ctx.args.attach_count}")


R.manual("TOOL-04", "Keil MDK: connect, read memory, download + verify", """
Keil uVision, Options for Target > Debug > CMSIS-DAP Debugger > Settings: select the probe by
serial, SWD, the fixed clock. Check that the target is detected (IDCODE, device name), open a
Memory window at the scratch address, then Flash > Download with Verify enabled. Record the
probe serial, uVision and pack versions, Download time and the Verify result. Keil sees only
local USB probes; for other links record n/a.""", links=("usb", "radio"))
