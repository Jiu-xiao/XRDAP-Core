"""Running OpenOCD, pyOCD and probe-rs against the probe under test."""

import contextlib
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time

from . import link as linkmod
from .dap import Dap


def probe_serial(ctx):
    if "serial" not in ctx.shared:
        with ctx.link() as lk:
            ctx.shared["serial"] = Dap(lk).info_str(0x03)
    return ctx.shared["serial"]


def tool_cmd(ctx, name):
    """Command list for a host tool from --openocd/--pyocd/--probe-rs, or None if missing."""
    text = getattr(ctx.args, name.replace("-", "_")) or name
    argv = shlex.split(text, posix=sys.platform != "win32")
    return argv if shutil.which(argv[0]) or os.path.exists(argv[0]) else None


def tool_version(argv, flag="--version"):
    try:
        p = subprocess.run(argv + [flag], capture_output=True, text=True, timeout=20)
        text = (p.stdout + p.stderr).strip().splitlines()
        return text[0] if text else "?"
    except (OSError, subprocess.TimeoutExpired):
        return None


@contextlib.contextmanager
def usbip_attached(ctx, spec):
    """Linux: attach the probe's USB/IP export with the kernel driver, detach afterwards."""
    if not sys.platform.startswith("linux"):
        raise RuntimeError("USB/IP attach is done with the Linux vhci-hcd driver")
    sudo = [] if os.geteuid() == 0 else ["sudo", "-n"]
    serial = probe_serial(ctx)
    subprocess.run(sudo + ["usbip", "attach", "-r", spec["host"], "-b", "1-1"] +
                   (["--tcp-port", str(spec["port"])] if spec["port"] != 3240 else []),
                   check=True, capture_output=True, text=True, timeout=20)
    try:
        deadline = time.monotonic() + 10
        while not any(s == serial for _, s in linkmod.list_usb()):
            if time.monotonic() > deadline:
                raise RuntimeError("attached device did not appear")
            time.sleep(0.2)
        yield serial
    finally:
        ports = subprocess.run(sudo + ["usbip", "port"], capture_output=True, text=True).stdout
        for port in re.findall(r"Port (\d+):", ports):
            subprocess.run(sudo + ["usbip", "detach", "-p", port], capture_output=True)


@contextlib.contextmanager
def usb_view(ctx):
    """Serial number of the probe as a local USB device (USB, USB/IP attached, radio host)."""
    spec = linkmod.parse_spec(ctx.spec)
    if spec["kind"] == "usb":
        yield probe_serial(ctx)
    elif spec["kind"] == "usbip":
        with usbip_attached(ctx, spec) as serial:
            yield serial
    else:
        yield None


@contextlib.contextmanager
def openocd_adapter(ctx):
    """OpenOCD adapter lines for the link under test."""
    spec = linkmod.parse_spec(ctx.spec)
    win = sys.platform == "win32"
    tcp_quirks = ["cmsis-dap quirk enable", "cmsis-dap tcp min_timeout 10"] if win else []
    head = ["adapter driver cmsis-dap"]
    if spec["kind"] == "tcp":
        yield head + ["cmsis-dap backend tcp", f"cmsis-dap tcp host {spec['host']}",
                      f"cmsis-dap tcp port {spec['port']}"] + tcp_quirks
    elif spec["kind"] == "serial":
        xrdap = shlex.split(ctx.args.xrdap or "xrdap", posix=not win)
        port = 3299
        cmd = xrdap + ["-p", spec["port"], "share", "openocd", "--listen", f"127.0.0.1:{port}"]
        if spec["keep_lines"]:
            cmd.insert(len(xrdap) + 2, "--keep-lines")
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
                    break
                except OSError:
                    if time.monotonic() > deadline or proc.poll() is not None:
                        raise RuntimeError("xrdap share openocd did not start")
                    time.sleep(0.2)
            yield head + ["cmsis-dap backend tcp", "cmsis-dap tcp host 127.0.0.1",
                          f"cmsis-dap tcp port {port}"] + tcp_quirks
        finally:
            proc.terminate()
            proc.wait(10)
    else:
        with usb_view(ctx) as serial:
            yield head + ["cmsis-dap backend usb_bulk", f"adapter serial {serial}"]


def run_openocd(ctx, name, adapter, khz, commands, timeout=300):
    t = ctx.target
    work = os.path.join(ctx.out_dir, "openocd")
    os.makedirs(work, exist_ok=True)
    lines = adapter + ["transport select swd", f"source [find {t.openocd_target}]",
                       f"adapter speed {khz}", "gdb_port disabled", "tcl_port disabled",
                       "telnet_port disabled"]
    cfg = os.path.join(work, f"{name}.cfg")
    with open(cfg, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    argv = tool_cmd(ctx, "openocd") + ["-f", cfg]
    for c in commands:
        argv += ["-c", c]
    t0 = time.monotonic()
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                           encoding="utf-8", errors="replace")
        rc, log = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as exc:
        rc, log = -1, f"TIMEOUT after {timeout} s\n{exc.stdout or ''}{exc.stderr or ''}"
    seconds = time.monotonic() - t0
    with open(os.path.join(work, f"{name}.log"), "w", encoding="utf-8") as f:
        f.write(log)
    return rc, log, seconds


def strict_examine(rc, log):
    """Strict OpenOCD attach: exit 0, examination reported, no examination failure. Other
    error lines (transport retries) are tolerated; a failed transfer shows in the later steps."""
    examined = "Examination succeed" in log or re.search(r"processor detected", log)
    return rc == 0 and bool(examined) and "Examination failed" not in log


def timings(log):
    return {k: int(v) for k, v in re.findall(r"XRT (\w+) (\d+)", log)}


def tcl_timed(label, command):
    return ["set _t [clock milliseconds]", command,
            f'echo "XRT {label} [expr {{[clock milliseconds] - $_t}}]"']


def tmpfile(ctx, name, data=None):
    path = os.path.join(ctx.out_dir, "openocd", name).replace("\\", "/")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if data is not None:
        with open(path, "wb") as f:
            f.write(data)
    return path

