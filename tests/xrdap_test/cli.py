"""Command line: run cases against a probe, record manual results, build tables, A/B, coverage."""

import argparse
import json
import os
import re
import subprocess
import time

from . import cases  # noqa: F401  (registers every case)
from . import link as linkmod
from . import report
from . import runner as R
from .dap import Dap
from .target import list_targets, load_target
from .vendor import Vendor

LONG = ("STAB-01", "STAB-02")


def hz(text):
    text = text.strip().lower()
    scale = {"k": 1e3, "m": 1e6}.get(text[-1], 1)
    return int(float(text.rstrip("km")) * scale)


def select(spec):
    """'all', 'quick' (all but the 1 h runs) or a comma list of ids and id prefixes."""
    ids = list(R.CASES)
    if spec in ("all", "quick"):
        return [i for i in ids if spec == "all" or i not in LONG]
    chosen = []
    for part in spec.split(","):
        part = part.strip().upper()
        match = [i for i in ids if i == part or i.startswith(part + "-") or i.startswith(part)]
        if not match:
            raise SystemExit(f"no case matches {part!r}")
        chosen += [i for i in match if i not in chosen]
    return chosen


def add_run_args(p):
    p.add_argument("--link", required=True, help="usb[:SERIAL] | serial:PORT[@BAUD][,keep] | "
                   "tcp:HOST[:PORT] | usbip:HOST[:PORT]")
    p.add_argument("--target", help="profile name in tests/targets or a .toml path")
    p.add_argument("--cases", default="quick", help="quick (default), all, or ids/prefixes: SWD,VND-06")
    p.add_argument("--out", default="xrdap-results", help="results root; each run gets a subdirectory")
    p.add_argument("--build", default="", help="fw=SHA,libxr=SHA,core=SHA of the image under test")
    p.add_argument("--clock", type=hz, default=R.CLOCK_HZ, help="SWD clock for vendor/target cases")
    p.add_argument("--max-clock", type=hz, help="max stable clock when SWD-01 is not in the run")
    p.add_argument("--via-radio", action="store_true", help="the link reaches the target over the radio pair")
    p.add_argument("--link2", help="second path to the same probe (MC-02, MC-03)")
    p.add_argument("--rounds", type=int, default=20, help="SWD-01 write/readback rounds per clock")
    p.add_argument("--attach-count", type=int, default=20, help="TOOL-* attach repetitions")
    p.add_argument("--long-s", type=float, default=3600.0, help="STAB-01/02 duration")
    p.add_argument("--elf", help="probe firmware ELF (RES-01)")
    p.add_argument("--nreset-wired", action="store_true", help="probe nRESET reaches the target (TGT-03)")
    p.add_argument("--replug-cmd", help="shell command that drops and restores the probe USB (STAB-03)")
    p.add_argument("--restart-cmd", help="shell command that restarts the probe (STAB-07)")
    p.add_argument("--no-prompt", action="store_true", help="skip assisted cases instead of asking")
    p.add_argument("--openocd", help="OpenOCD command (default: openocd on PATH)")
    p.add_argument("--probe-rs", help="probe-rs command")
    p.add_argument("--pyocd", help="pyOCD command")
    p.add_argument("--xrdap", help="xrdap command, used to share a serial probe with OpenOCD")
    p.add_argument("--scratch", help="override the profile scratch region BASE:SIZE")


def do_run(args, out_dir):
    overrides = {}
    if args.scratch:
        base, _, size = args.scratch.partition(":")
        overrides["scratch"] = {"base": int(base, 0), "size": int(size, 0)}
    target = load_target(args.target, overrides) if args.target else None
    os.makedirs(out_dir, exist_ok=True)
    ctx = R.Context(args, target, out_dir)
    lk = linkmod.open_link(args.link)
    try:
        dap = Dap(lk)
        identity = dap.identity()
        identity["vendor_info"] = Vendor(lk).info()
        saved = None
        if target:
            Vendor(lk).stop()
            dap.attach(args.clock)
            saved = dap.block_read(target.scratch.base, target.scratch.size)
    finally:
        lk.close()
    ctx.shared["serial"] = identity.get("serial")
    meta = R.run_meta(ctx, identity, {"cases_selected": args.cases})
    ctx.log(f"probe {identity.get('product')} {identity.get('serial')} fw {identity.get('firmware')}"
            f" link {ctx.link_kind} target {target.name if target else '-'} -> {out_dir}")
    try:
        results = R.run_cases(ctx, select(args.cases), meta)
    finally:
        if saved is not None:
            try:
                with ctx.session(fixture=False) as (_, d, _v):
                    d.block_write(target.scratch.base, saved)
                ctx.log("scratch RAM restored")
            except Exception as exc:
                ctx.log(f"scratch RAM NOT restored: {exc}")
    meta["tools"] = {k: v for r in results for k, v in r.metrics.items()
                     if k in ("openocd", "probe-rs", "pyocd")}
    R.write_result(out_dir, meta, results)
    counts = {s: sum(r.status == s for r in results) for s in (R.PASS, R.FAIL, R.ERROR, R.NA, R.TODO)}
    ctx.log("summary " + " ".join(f"{k} {v}" for k, v in counts.items()))
    return counts


def run_dir(args, label=""):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    kind = "radio" if args.via_radio else linkmod.parse_spec(args.link)["kind"]
    name = "-".join(filter(None, [stamp, kind, args.target or "notarget", label]))
    return os.path.join(args.out, name)


def cmd_run(args):
    counts = do_run(args, run_dir(args))
    return 1 if counts[R.FAIL] or counts[R.ERROR] else 0


def cmd_ab(args):
    """Flash image A, run; flash B, run; repeat; then compare the numeric metrics."""
    dirs = {"A": [], "B": []}
    for k in range(args.ab_rounds):
        for label, flash in (("A", args.a_cmd), ("B", args.b_cmd)):
            print(f"== round {k + 1} image {label}: {flash}", flush=True)
            subprocess.run(flash, shell=True, check=True)
            deadline = time.monotonic() + 60
            while True:
                try:
                    linkmod.open_link(args.link).close()
                    break
                except Exception:
                    if time.monotonic() > deadline:
                        raise SystemExit("probe did not come back after flashing")
                    time.sleep(1)
            d = run_dir(args, f"{label}{k + 1}")
            do_run(args, d)
            dirs[label].append(d)
    table = report.compare(dirs["A"], dirs["B"])
    path = os.path.join(args.out, time.strftime("ab-%Y%m%d-%H%M%S.md"))
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"A: {args.a_cmd}\n\nB: {args.b_cmd}\n\nruns A: {dirs['A']}\n\nruns B: {dirs['B']}\n\n{table}")
    print(table)
    print(f"written {path}")
    return 0


def cmd_list(args):
    for cid, c in R.CASES.items():
        links = "all" if set(c.links) == set(R.LINKS) else ",".join(c.links)
        print(f"{cid:8s} {c.mode:8s} {links:22s} {c.title}")
    print("\ntargets:", ", ".join(list_targets()))
    try:
        found = linkmod.list_usb()
        print("USB probes:", ", ".join(str(s) for _, s in found) or "none")
    except Exception as exc:
        print("USB probes: cannot enumerate:", exc)
    return 0


def cmd_record(args):
    path = os.path.join(args.run, "result.json")
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    c = next((c for c in data["cases"] if c["id"] == args.case), None)
    if c is None:
        cdef = R.CASES[args.case]
        c = R.CaseResult(cdef.id, cdef.title, cdef.mode).__dict__
        data["cases"].append(c)
    c["status"] = args.status
    c["summary"] = args.note or c["summary"]
    for kv in args.metric or []:
        k, _, v = kv.partition("=")
        try:
            c["metrics"][k] = float(v)
        except ValueError:
            c["metrics"][k] = v
    c["recorded"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
    with open(os.path.join(args.run, "result.md"), "w", encoding="utf-8") as f:
        f.write(report.run_markdown(data, args.run))
    print(f"{args.case} -> {args.status}")
    return 0


def cmd_table(args):
    text = report.matrix(args.runs)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    print(text)
    return 0


SPEC_ROW = re.compile(r"^\| ([A-Z]+-\d\d) \| `?(auto|assisted|manual)`? \| ([a-z, ]+) \|", re.M)
MAP_ROW = re.compile(r"^\| `((?:consistency|schedule|dap|transport)\.[a-z_.]+)` \| ([^|]+)\|", re.M)


def cmd_coverage(args):
    """Every case in SPEC.md and SPEC.en.md is registered with the same mode and links, every
    registered case is in both, and every behaviour-suite branch maps to a case or to N/A."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    problems = []
    for name in ("SPEC.md", "SPEC.en.md"):
        with open(os.path.join(root, name), encoding="utf-8") as f:
            text = f.read()
        rows = {m[0]: (m[1], m[2].strip()) for m in SPEC_ROW.findall(text)}
        for cid, c in R.CASES.items():
            links = "all" if set(c.links) == set(R.LINKS) else ", ".join(c.links)
            if cid not in rows:
                problems.append(f"{name}: {cid} missing")
            elif rows[cid] != (c.mode, links):
                problems.append(f"{name}: {cid} says {rows[cid]}, code has {(c.mode, links)}")
        problems += [f"{name}: {cid} has no implementation" for cid in rows if cid not in R.CASES]
        mapped = MAP_ROW.findall(text)
        if not mapped:
            problems.append(f"{name}: behaviour-suite map missing")
        for branch, target in mapped:
            target = target.strip()
            ok = target.startswith("N/A") or all(t.strip() in R.CASES for t in target.split(","))
            if not ok:
                problems.append(f"{name}: {branch} -> {target} is neither a case nor N/A")
        print(f"{name}: {len(rows)} cases, {len(mapped)} behaviour-suite branches mapped")
    modes = {m: sum(c.mode == m for c in R.CASES.values()) for m in (R.AUTO, R.ASSISTED, R.MANUAL)}
    print(f"code: {len(R.CASES)} cases ({modes})")
    for p in problems:
        print("PROBLEM", p)
    print("coverage OK" if not problems else f"{len(problems)} problems")
    return 1 if problems else 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m xrdap_test", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run", help="run cases against one probe, link and target")
    add_run_args(p)
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("ab", help="A/B: flash A, run, flash B, run, repeat, compare")
    add_run_args(p)
    p.add_argument("--a-cmd", required=True, help="shell command that flashes image A")
    p.add_argument("--b-cmd", required=True, help="shell command that flashes image B")
    p.add_argument("--ab-rounds", type=int, default=2)
    p.set_defaults(fn=cmd_ab)
    p = sub.add_parser("list", help="cases, target profiles, USB probes")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("record", help="record the result of a manual or assisted case")
    p.add_argument("--run", required=True, help="run directory")
    p.add_argument("case")
    p.add_argument("status", choices=(R.PASS, R.FAIL, R.NA))
    p.add_argument("--note", help="short result text for the table")
    p.add_argument("--metric", action="append", help="name=value, repeatable")
    p.set_defaults(fn=cmd_record)
    p = sub.add_parser("table", help="results matrix from several run directories")
    p.add_argument("runs", nargs="+")
    p.add_argument("--output")
    p.set_defaults(fn=cmd_table)
    p = sub.add_parser("coverage", help="check SPEC.md / SPEC.en.md against the implemented cases")
    p.set_defaults(fn=cmd_coverage)
    args = ap.parse_args(argv)
    return args.fn(args)
