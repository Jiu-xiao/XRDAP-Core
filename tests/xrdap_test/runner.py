"""Case registry, run context and result files."""

import contextlib
import json
import os
import platform
import sys
import time
import traceback
from dataclasses import dataclass, field

from . import link as linkmod
from .dap import Dap
from .vendor import Vendor

AUTO, ASSISTED, MANUAL = "auto", "assisted", "manual"
LINKS = ("usb", "serial", "tcp", "usbip", "radio")
PASS, FAIL, NA, ERROR, TODO = "pass", "fail", "n/a", "error", "manual"

# Fixed settings shared by every probe (SPEC section 2).
CLOCK_HZ = 10_000_000  # vendor and target cases
PERF_CLOCKS = (4_000_000, 10_000_000)  # plus the max stable clock
SWEEP_CLOCKS = (100_000, 500_000, 1_000_000, 2_000_000, 4_000_000, 8_000_000, 10_000_000,
                15_000_000, 20_000_000, 30_000_000, 50_000_000)
SEARCH_STEP_US = 25
SEARCH_RUN_S = 3.0
VAR_COUNTS = (1, 4, 8, 32)


@dataclass
class CaseDef:
    id: str
    title: str
    mode: str
    links: tuple
    fn: object = None
    procedure: str = ""


CASES = {}


def case(case_id, title, mode=AUTO, links=LINKS):
    def register(fn):
        CASES[case_id] = CaseDef(case_id, title, mode, tuple(links), fn,
                                 (fn.__doc__ or "").strip())
        return fn
    return register


def manual(case_id, title, procedure, links=LINKS):
    CASES[case_id] = CaseDef(case_id, title, MANUAL, tuple(links), None, procedure.strip())


class NotApplicable(Exception):
    pass


@dataclass
class CaseResult:
    id: str
    title: str
    mode: str
    status: str = PASS
    summary: str = ""
    checks: list = field(default_factory=list)
    metrics: dict = field(default_factory=dict)
    seconds: float = 0.0


class Context:
    def __init__(self, args, target, out_dir):
        self.args = args
        self.spec = args.link
        self.link_kind = "radio" if args.via_radio else linkmod.parse_spec(args.link)["kind"]
        self.target = target
        self.out_dir = out_dir
        self.clock = args.clock
        self.prompt = not args.no_prompt
        self.result = None
        self.shared = {}  # values one case leaves for later ones (max stable clock)
        self._log = open(os.path.join(out_dir, "log.txt"), "a", encoding="utf-8")

    # -------------------------------------------------------------- logging
    def log(self, *parts):
        line = " ".join(str(p) for p in parts)
        print(line, flush=True)
        self._log.write(line + "\n")
        self._log.flush()

    def check(self, name, ok, detail=""):
        self.result.checks.append({"name": name, "ok": bool(ok), "detail": str(detail)})
        self.log(f"  {'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else ""))
        return ok

    def metric(self, name, value):
        self.result.metrics[name] = value
        self.log(f"  {name} = {value}")
        return value

    def summary(self, text):
        self.result.summary = text

    def ask(self, text):
        """Operator step of an assisted case; returns False when prompts are off."""
        if not self.prompt:
            raise NotApplicable("needs an operator (run without --no-prompt)")
        self.log(f"  >>> {text}")
        input("      press Enter when done ")
        return True

    # ------------------------------------------------------------- sessions
    @contextlib.contextmanager
    def link(self):
        lk = linkmod.open_link(self.spec)
        try:
            yield lk
        finally:
            lk.close()

    @contextlib.contextmanager
    def session(self, clock=None, fixture=True):
        """Open the link, attach SWD at `clock` and (re)write the scratch fixture."""
        with self.link() as lk:
            dap = Dap(lk)
            dap.query_packet()
            vendor = Vendor(lk)
            vendor.stop()  # a previous client may have left sampling running
            dap.attach(clock or self.clock)
            if fixture and self.target:
                dap.block_write(self.target.scratch.base, self.target.fixture_bytes())
            yield lk, dap, vendor

    def need_target(self):
        if not self.target:
            raise NotApplicable("needs --target")
        return self.target


def build_info(text):
    out = {}
    for part in filter(None, (text or "").split(",")):
        key, _, value = part.partition("=")
        out[key.strip()] = value.strip()
    return out


def run_meta(ctx, identity, extra):
    a = ctx.args
    return {
        "probe": identity,
        "link": ctx.link_kind,
        "link_spec": ctx.spec,
        "target": ctx.target.name if ctx.target else None,
        "host": {"os": platform.platform(), "python": sys.version.split()[0],
                 "node": platform.node()},
        "build": build_info(a.build),
        "clock_hz": ctx.clock,
        "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "argv": sys.argv[1:],
        **extra,
    }


def run_cases(ctx, ids, meta):
    results = []
    for cid in ids:
        cdef = CASES[cid]
        res = CaseResult(cid, cdef.title, cdef.mode)
        ctx.result = res
        ctx.log(f"== {cid} {cdef.title}")
        t0 = time.monotonic()
        try:
            if cdef.mode == MANUAL:
                res.status = TODO
                res.summary = "manual: record with `record`"
                ctx.log("  manual case, procedure:\n    " + cdef.procedure.replace("\n", "\n    "))
            elif ctx.link_kind not in cdef.links:
                raise NotApplicable(f"not for link {ctx.link_kind}")
            else:
                cdef.fn(ctx)
                if any(not c["ok"] for c in res.checks):
                    res.status = FAIL
        except NotApplicable as exc:
            res.status, res.summary = NA, str(exc)
            ctx.log(f"  n/a: {exc}")
        except KeyboardInterrupt:
            raise
        except Exception as exc:  # the case is broken or the probe stopped answering
            res.status, res.summary = ERROR, f"{type(exc).__name__}: {exc}"
            ctx.log("  ERROR " + traceback.format_exc().rstrip().replace("\n", "\n  "))
        res.seconds = round(time.monotonic() - t0, 2)
        ctx.log(f"   -> {res.status.upper()} {res.summary}")
        results.append(res)
        write_result(ctx.out_dir, meta, results)
    return results


def write_result(out_dir, meta, results):
    data = {"meta": meta, "cases": [r.__dict__ for r in results]}
    with open(os.path.join(out_dir, "result.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
    from .report import run_markdown

    with open(os.path.join(out_dir, "result.md"), "w", encoding="utf-8") as f:
        f.write(run_markdown(data, out_dir))
