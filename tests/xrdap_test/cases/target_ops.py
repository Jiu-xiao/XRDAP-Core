"""Target matrix (TGT-*): identify, memory access, halt/run/reset with raw DAP."""

import os
import struct
import time

from .. import runner as R
from ..dap import AP_CSW, AP_DRW, AP_IDR, AP_TAR, req
from ..target import CORES, CPUID, DBGKEY, DEMCR, DHCSR, AIRCR, DWT_CTRL, S_HALT, S_RESET_ST


@R.case("TGT-01", "Identify: DPIDR, DP version, AP IDR, CPUID, cycle counter")
def tgt_identify(ctx):
    t = ctx.need_target()
    with ctx.session(fixture=False) as (lk, dap, _):
        dpidr = dap.dp_read(0)
        idr = dap.ap_read(AP_IDR)
        cpuid = dap.read32(CPUID)
        out = {"dpidr": hex(dpidr), "dp_version": dpidr >> 12 & 0xF, "ap0_idr": hex(idr),
               "cpuid": hex(cpuid), "core": CORES.get(cpuid >> 4 & 0xFFF, "unknown"),
               "cyccnt": not dap.read32(DWT_CTRL) >> 25 & 1}
        for k, v in out.items():
            ctx.metric(k, v)
        if t.dpidr is not None:
            ctx.check("DPIDR as in the profile", dpidr == t.dpidr, f"{dpidr:#x} vs {t.dpidr:#x}")
        ctx.check("CPUID readable", cpuid != 0, hex(cpuid))
        ctx.summary(f"{out['core']} DPv{out['dp_version']} DPIDR {out['dpidr']}")


@R.case("TGT-02", "Memory: 8/16/32-bit access and a 4 KiB block in scratch RAM")
def tgt_memory(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        rw = t.at(t.RW)
        dap.write32(rw, 0)
        csw = dap.ap_read(AP_CSW) & ~0x37
        dap.dp_write(0x8, 0)
        # Byte write to rw+1 and halfword write to rw+2: data sits in its byte lane.
        dap.transfer_ok([(req(1, 0, AP_CSW), csw | 0x0), (req(1, 0, AP_TAR), rw + 1),
                         (req(1, 0, AP_DRW), 0xAB << 8),
                         (req(1, 0, AP_CSW), csw | 0x1), (req(1, 0, AP_TAR), rw + 2),
                         (req(1, 0, AP_DRW), 0xCDEF << 16),
                         (req(1, 0, AP_CSW), csw | 0x12)])
        ctx.check("byte and halfword lanes", dap.read32(rw) == 0xCDEFAB00, hex(dap.read32(rw)))
        data = os.urandom(4096)
        dap.block_write(t.at(t.BLOCK), data)
        ctx.check("4 KiB block", dap.block_read(t.at(t.BLOCK), 4096) == data)
        ctx.summary("8/16/32-bit and block access")


def dhcsr_wait(dap, mask, want, timeout=1.0):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        try:
            if dap.read32(DHCSR) & mask == want:
                return round((time.perf_counter() - t0) * 1e3, 2)
        except Exception:
            dap.clear_errors()
    return None


@R.case("TGT-03", "Halt, run, reset (SYSRESETREQ, and nRESET when wired)")
def tgt_core(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        dap.write32(DHCSR, DBGKEY | 0x3)
        ctx.check("halt", dhcsr_wait(dap, S_HALT, S_HALT) is not None)
        dap.write32(DHCSR, DBGKEY | 0x1)
        ctx.check("run", dhcsr_wait(dap, S_HALT, 0) is not None)
        dap.read32(DHCSR)
        dap.write32(DEMCR, dap.read32(DEMCR) | 1)
        dap.write32(AIRCR, 0x05FA0004)
        ctx.check("SYSRESETREQ stops at the reset vector", dhcsr_wait(dap, S_HALT, S_HALT) is not None)
        if ctx.args.nreset_wired:
            dap.read32(DHCSR)
            # DAP_SWJ_Pins: nRESET (bit 7) low for 50 ms, then high.
            lk.xfer(struct.pack("<BBBI", 0x10, 0x00, 0x80, 0))
            time.sleep(0.05)
            lk.xfer(struct.pack("<BBBI", 0x10, 0x80, 0x80, 0))
            dap.clear_errors()
            ctx.check("nRESET pin resets the core",
                      dap.read32(DHCSR) & S_RESET_ST or dhcsr_wait(dap, S_HALT, S_HALT) is not None)
        dap.write32(DEMCR, dap.read32(DEMCR) & ~1)
        dap.write32(DHCSR, DBGKEY)
        dap.block_write(t.scratch.base, t.fixture_bytes())
        ctx.summary("halt, run, reset" + (", nRESET" if ctx.args.nreset_wired else ""))


R.manual("TGT-04", "Multidrop SWD (DPv2 targets with TARGETSEL)", """
XR-DAP does not implement SWD multidrop (DAP_SWD_Sequence based TARGETSEL selection is not used
by the firmware). Record the case as unsupported for every probe; if a host tool is pointed at a
multidrop target (RP2040 and similar), record what it reports.""")
