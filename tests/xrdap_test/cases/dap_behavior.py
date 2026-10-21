"""Standard CMSIS-DAP command behaviour (DAP-*), live counterparts of the behaviour-suite
branch catalogue (SPEC section 4.2)."""

import struct

from .. import runner as R
from ..dap import (ACK_FAULT, ACK_OK, AP_DRW, AP_TAR, DP_CTRL_STAT, DP_RDBUFF, DP_SELECT,
                   MATCH_MASK, MATCH_VALUE, TIMESTAMP, req)

VALUE_MISMATCH = 0x10


def in_sync(lk):
    """The link still pairs requests and responses one to one."""
    r = lk.xfer(b"\x00\xFF")
    return r[:1] == b"\x00"


@R.case("DAP-01", "DAP_Transfer: DP/AP access, posted reads, match, timestamp, FAULT, abort")
def dap_transfer(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        words = t.at(t.WORDS)
        dap.dp_write(DP_SELECT, 0)
        ctx.check("DP read CTRL/STAT powered", dap.dp_read(DP_CTRL_STAT) & 0xA0000000 == 0xA0000000)
        # AP read pipeline: TAR, four DRW reads with auto-increment, then a DP read in one packet.
        done, ack, vals = dap.transfer([(req(1, 0, AP_TAR), words)] + [(req(1, 1, AP_DRW), None)] * 4
                                       + [(req(0, 1, DP_RDBUFF), None)])
        ctx.check("posted AP reads return in order", ack == ACK_OK and vals[:4] == [0x5A000000 | k for k in range(4)],
                  [hex(v) for v in vals])
        # Writes followed by the trailing check; AP and DP mixed and SELECT rewritten mid-packet.
        done, ack, _ = dap.transfer([(req(0, 0, DP_SELECT), 0), (req(1, 0, AP_TAR), t.at(t.RW)),
                                     (req(1, 0, AP_DRW), 0x11223344), (req(0, 0, DP_SELECT), 0),
                                     (req(1, 0, AP_DRW), 0x55667788)])
        ctx.check("AP/DP mixed writes", ack == ACK_OK and done == 5)
        ctx.check("write data landed, TAR auto-incremented",
                  (dap.read32(t.at(t.RW)), dap.read32(t.at(t.RW) + 4)) == (0x11223344, 0x55667788))
        # Value match: DRW of v_u32 (0x89ABCDEF) under mask 0xFFFF0000.
        dap.transfer_configure(match_retry=5)
        dap.transfer_ok([(req(1, 0, AP_TAR), t.at(t.V_U32))])
        done, ack, _ = dap.transfer([(MATCH_MASK, 0xFFFF0000), (req(1, 1, AP_DRW) | MATCH_VALUE, 0x89AB0000)])
        ctx.check("value match hit", ack == ACK_OK and done == 2, f"ack {ack:#x}")
        dap.transfer_ok([(req(1, 0, AP_TAR), t.at(t.V_U32))])
        done, ack, _ = dap.transfer([(MATCH_MASK, 0xFFFF0000), (req(1, 1, AP_DRW) | MATCH_VALUE, 0x12340000)])
        ctx.check("value mismatch reported", ack & VALUE_MISMATCH, f"ack {ack:#x}")
        dap.transfer_configure()
        # Timestamped read when the probe has the test domain timer (DAP_Info 0xF0 bit 5).
        if dap.info(0xF0)[0] & 0x20:
            dap.transfer_ok([(req(1, 0, AP_TAR), words)])
            _, ack, v1 = dap.transfer([(req(1, 1, AP_DRW) | TIMESTAMP, None)])
            _, ack2, v2 = dap.transfer([(req(1, 1, AP_DRW) | TIMESTAMP, None)])
            ctx.check("timestamped reads", ack == ack2 == ACK_OK and v2[0] != v1[0], (v1, v2))
            dap.transfer_ok([(req(1, 0, AP_TAR), words)])
            _, ack, v3 = dap.transfer([(req(1, 0, AP_DRW) | TIMESTAMP, 0x5A000000)])
            ctx.check("timestamped write", ack == ACK_OK and len(v3) == 1, v3)
        else:
            ctx.metric("timestamp", "not supported (DAP_Info 0xF0 bit 5 clear)")
        # FAULT: read an unmapped address, then ABORT clears the sticky error.
        done, ack, _ = dap.transfer([(req(1, 0, AP_TAR), t.fault_address), (req(1, 1, AP_DRW), None),
                                     (req(0, 1, DP_RDBUFF), None)])
        ctx.check("FAULT acknowledged", ack == ACK_FAULT, f"ack {ack:#x}")
        dap.clear_errors()
        ctx.check("access works after ABORT", dap.read32(t.at(t.V_U32)) == 0x89ABCDEF)
        # Malformed: count 3 with one operation; the probe must answer and stay in sync.
        r = lk.xfer(bytes([0x05, 0, 3, req(0, 1, 0)]))
        ctx.check("short request answered, link in sync", r[:1] == b"\x05" and in_sync(lk), r.hex(" "))
        # More reads than fit in a response: the answer must stay within one packet.
        r = lk.xfer(bytes([0x05, 0, 255]) + bytes([req(0, 1, DP_RDBUFF)]) * 255)
        ctx.check("oversized read count clipped to the packet", len(r) <= dap.packet_size and in_sync(lk),
                  f"{len(r)} bytes, done {r[1]}")
        # DAP_TransferAbort has no response; the next command must still pair correctly.
        # Over serial it gets an empty frame (n = 0), protocol section 1.
        lk.send(b"\x07")
        if lk.kind == "serial":
            ctx.check("TransferAbort: empty serial frame", lk.recv() == b"")
        ctx.check("TransferAbort while idle", in_sync(lk))
        ctx.summary("transfer semantics as expected")


@R.case("DAP-02", "DAP_TransferBlock: AP/DP blocks, count 0, packet boundary, FAULT")
def dap_block(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        dpidr = dap.dp_read(0)
        r = lk.xfer(struct.pack("<BBHB", 0x06, 0, 3, req(0, 1, 0)))
        ctx.check("DP block read (IDCODE x3)", r[3] == ACK_OK and r[4:16] == struct.pack("<I", dpidr) * 3, r[:16].hex(" "))
        r = lk.xfer(struct.pack("<BBHB", 0x06, 0, 2, req(0, 0, DP_SELECT)) + b"\0" * 8)
        ctx.check("DP block write (SELECT x2)", r[3] == ACK_OK and r[1] == 2)
        r = lk.xfer(struct.pack("<BBHB", 0x06, 0, 0, req(1, 1, AP_DRW)))
        ctx.check("count 0: nothing transferred, in sync", int.from_bytes(r[1:3], "little") == 0 and in_sync(lk), r.hex(" "))
        fit = dap.block_words(False)
        base = t.at(t.BLOCK) & ~0x3FF
        dap.transfer_ok([(req(1, 0, AP_TAR), base)])
        r = lk.xfer(struct.pack("<BBHB", 0x06, 0, fit, req(1, 1, AP_DRW)))
        ctx.check(f"{fit} words (largest that fits) complete", len(r) == 4 + 4 * fit and r[3] == ACK_OK,
                  f"{len(r)} bytes")
        dap.transfer_ok([(req(1, 0, AP_TAR), base)])
        r = lk.xfer(struct.pack("<BBHB", 0x06, 0, fit + 1, req(1, 1, AP_DRW)))
        ctx.check("one word too many: answer stays within the packet", len(r) <= dap.packet_size and in_sync(lk),
                  f"{len(r)} bytes, done {int.from_bytes(r[1:3], 'little')}")
        dap.clear_errors()
        dap.transfer_ok([(req(1, 0, AP_TAR), t.fault_address)])
        r = lk.xfer(struct.pack("<BBHB", 0x06, 0, 4, req(1, 1, AP_DRW)))
        ctx.check("block FAULT acknowledged", r[3] == ACK_FAULT, r[:4].hex(" "))
        dap.clear_errors()
        r = lk.xfer(struct.pack("<BBHB", 0x06, 0, 2, req(1, 1, AP_DRW) | MATCH_VALUE | TIMESTAMP))
        ctx.check("match/timestamp bits in a block request: answered, in sync", r[:1] == b"\x06" and in_sync(lk))
        dap.clear_errors()
        ctx.check("access works afterwards", dap.read32(t.at(t.V_U32)) == 0x89ABCDEF)
        ctx.summary(f"block semantics as expected, {fit} words per packet")


def transfer_of_length(n):
    """A DAP_Transfer request exactly n bytes long: SELECT writes plus RDBUFF reads."""
    w, r = divmod(n - 3, 5)
    writes = b"".join(bytes([req(0, 0, DP_SELECT)]) + b"\0" * 4 for _ in range(w))
    return bytes([0x05, 0, w + r]) + writes + bytes([req(0, 1, DP_RDBUFF)]) * r


@R.case("DAP-03", "Packet boundaries: request and response lengths around the USB packet")
def dap_lengths(ctx):
    t = ctx.need_target()
    with ctx.session() as (lk, dap, _):
        mps = getattr(lk, "mps", 64)
        p = dap.packet_size
        lengths = sorted({mps - 1, mps, mps + 1, 2 * mps, p - 1, p} - {0})
        bad = []
        for n in lengths:
            for _ in range(20):
                r = lk.xfer(transfer_of_length(n))
                if r[:1] != b"\x05" or r[2] != ACK_OK:
                    bad.append(("request", n, r[:3].hex(" ")))
                    break
        ctx.check("requests of every length answered", not bad, bad)
        base = t.at(t.BLOCK) & ~0x3FF
        bad = []
        for words in sorted({(mps - 4) // 4, mps // 4, (p - 4) // 4, 1}):
            for _ in range(20):
                dap.transfer_ok([(req(1, 0, AP_TAR), base)])
                r = lk.xfer(struct.pack("<BBHB", 0x06, 0, words, req(1, 1, AP_DRW)))
                if len(r) != 4 + 4 * words or r[3] != ACK_OK:
                    bad.append(("response", 4 + 4 * words, len(r)))
                    break
        ctx.check("responses of every length complete", not bad, bad)
        ctx.metric("request_lengths", lengths)
        ctx.summary(f"MPS {mps}, packet {p}: all lengths ok")


@R.case("DAP-04", "Command set: DAP_Info IDs, HostStatus, SWJ_Pins, Execute/QueueCommands, unknown -> FF")
def dap_commands(ctx):
    with ctx.session(fixture=False) as (lk, dap, _):
        ident = dap.identity()
        ctx.metric("identity", ident)
        ctx.check("packet size and count", ident["packet_size"] >= 64 and ident["packet_count"] >= 1)
        ctx.check("SWD capability", ident["capabilities"] & 1)
        ctx.check("HostStatus", lk.xfer(b"\x01\x00\x01")[:2] == b"\x01\x00")
        lk.xfer(b"\x01\x00\x00")
        r = lk.xfer(struct.pack("<BBBI", 0x10, 0, 0, 0))  # read pins, change nothing
        ctx.check("SWJ_Pins read", r[:1] == b"\x10" and len(r) == 2)
        ctx.check("DAP_Delay", lk.xfer(struct.pack("<BH", 0x09, 10))[:2] == b"\x09\x00")
        inner = bytes([0x00, 0xFF]) + bytes([0x05, 0, 1, req(0, 1, 0)])
        r = lk.xfer(bytes([0x7F, 2]) + inner)
        ctx.check("ExecuteCommands: count echoed, both answers", r[:2] == b"\x7F\x02" and r[2] == 0x00, r[:8].hex(" "))
        for _ in range(2):
            lk.send(bytes([0x7E, 1, 0x00, 0xFF]))
        lk.send(bytes([0x7F, 1, 0x00, 0xFF]))
        answers = [lk.recv() for _ in range(3)]
        ctx.check("QueueCommands: one answer per packet, in order", all(a[:2] == b"\x7F\x01" for a in answers),
                  [a[:4].hex(" ") for a in answers])
        r = lk.xfer(b"\x3F")
        ctx.check("unknown command -> FF", r == b"\xFF", r.hex(" "))
        ctx.summary("command set as expected")
