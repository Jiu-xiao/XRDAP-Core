"""Standard CMSIS-DAP commands over a Link, plus the SWD/MEM-AP steps the cases need."""

import struct

ACK_OK, ACK_WAIT, ACK_FAULT = 1, 2, 4

# DAP_Transfer request bits.
AP, RD = 0x01, 0x02
MATCH_VALUE, MATCH_MASK, TIMESTAMP = 0x10, 0x20, 0x80

DP_IDCODE = DP_ABORT = 0x0
DP_CTRL_STAT = 0x4
DP_SELECT = 0x8
DP_RDBUFF = 0xC
AP_CSW, AP_TAR, AP_DRW, AP_IDR = 0x00, 0x04, 0x0C, 0xFC

# 32-bit, single auto-increment, privileged data access (the value OpenOCD writes).
CSW_WORD_INC = 0x23000052
ABORT_ALL = 0x1E

# DAP_Info IDs.
INFO_VENDOR, INFO_PRODUCT, INFO_SERIAL, INFO_PROTOCOL = 0x01, 0x02, 0x03, 0x04
INFO_FW_VERSION = 0x09
INFO_CAPS = 0xF0
INFO_PACKET_COUNT, INFO_PACKET_SIZE = 0xFE, 0xFF

SWD_LINE_RESET = bytes([0x12, 51]) + b"\xFF" * 7
JTAG_TO_SWD = bytes([0x12, 16, 0x9E, 0xE7])
IDLE_8 = bytes([0x12, 8, 0x00])


class DapError(RuntimeError):
    pass


def req(apndp, rnw, addr):
    return (AP if apndp else 0) | (RD if rnw else 0) | (addr & 0x0C)


class Dap:
    def __init__(self, link):
        self.link = link
        self.packet_size = None
        self.packet_count = None

    # ------------------------------------------------------------------ info
    def info(self, info_id) -> bytes:
        r = self.link.xfer(bytes([0x00, info_id]))
        if len(r) < 2 or r[0] != 0x00:
            raise DapError(f"DAP_Info {info_id:#x}: {r.hex(' ')}")
        return r[2:2 + r[1]]

    def info_str(self, info_id):
        return self.info(info_id).split(b"\0")[0].decode("ascii", "replace")

    def query_packet(self):
        self.packet_size = int.from_bytes(self.info(INFO_PACKET_SIZE)[:2], "little")
        self.packet_count = self.info(INFO_PACKET_COUNT)[0]
        return self.packet_size, self.packet_count

    def identity(self):
        out = {}
        for key, info_id in (("vendor", INFO_VENDOR), ("product", INFO_PRODUCT),
                             ("serial", INFO_SERIAL), ("protocol", INFO_PROTOCOL),
                             ("firmware", INFO_FW_VERSION)):
            try:
                out[key] = self.info_str(info_id)
            except DapError:
                out[key] = None
        out["packet_size"], out["packet_count"] = self.query_packet()
        out["capabilities"] = self.info(INFO_CAPS)[0]
        return out

    # --------------------------------------------------------------- general
    def swj_clock(self, hz):
        r = self.link.xfer(struct.pack("<BI", 0x11, int(hz)))
        if r[:2] != b"\x11\x00":
            raise DapError(f"SWJ_Clock {hz}: {r.hex(' ')}")

    def connect(self):
        r = self.link.xfer(bytes([0x02, 0x01]))
        if r[:2] != b"\x02\x01":
            raise DapError(f"DAP_Connect SWD: {r.hex(' ')}")

    def transfer_configure(self, idle=0, wait_retry=100, match_retry=100):
        self.link.xfer(struct.pack("<BBHH", 0x04, idle, wait_retry, match_retry))

    def swj_sequence(self, seq: bytes):
        self.link.xfer(seq)

    # -------------------------------------------------------------- transfer
    def transfer(self, ops, ap_index=0):
        """ops: list of (request byte, value or None). Returns (done, ack, words); a
        timestamped read puts its timestamp word before its data word."""
        body = bytearray([0x05, ap_index, len(ops)])
        for request, value in ops:
            body.append(request)
            if not request & RD or request & MATCH_VALUE:
                body += struct.pack("<I", value or 0)
        r = self.link.xfer(bytes(body))
        if len(r) < 3 or r[0] != 0x05:
            raise DapError(f"DAP_Transfer response {r.hex(' ')}")
        words = [int.from_bytes(r[3 + 4 * i:7 + 4 * i], "little")
                 for i in range((len(r) - 3) // 4)]
        return r[1], r[2], words

    def transfer_ok(self, ops):
        done, ack, words = self.transfer(ops)
        if done != len(ops) or ack != ACK_OK:
            raise DapError(f"DAP_Transfer {done}/{len(ops)} ack {ack:#x}")
        return words

    def dp_read(self, addr):
        return self.transfer_ok([(req(0, 1, addr), None)])[0]

    def dp_write(self, addr, value):
        self.transfer_ok([(req(0, 0, addr), value)])

    def ap_read(self, reg, ap=0):
        self.dp_write(DP_SELECT, ap << 24 | (reg & 0xF0))
        return self.transfer_ok([(req(1, 1, reg), None)])[0]

    def ap_write(self, reg, value, ap=0):
        self.dp_write(DP_SELECT, ap << 24 | (reg & 0xF0))
        self.transfer_ok([(req(1, 0, reg), value)])

    def clear_errors(self):
        self.transfer([(req(0, 0, DP_ABORT), ABORT_ALL)])

    # ------------------------------------------------------------------ SWD
    def attach(self, clock_hz, csw=CSW_WORD_INC):
        """Connect SWD, power up the debug domain and set the AP CSW. Returns DPIDR."""
        self.swj_clock(clock_hz)
        self.connect()
        self.transfer_configure()
        self.link.xfer(bytes([0x13, 0x00]))  # SWD_Configure: 1 turnaround clock, no data phase
        for seq in (SWD_LINE_RESET, JTAG_TO_SWD, SWD_LINE_RESET, IDLE_8):
            self.swj_sequence(seq)
        # After a line reset the first DP access must be an IDCODE read.
        dpidr = self.dp_read(DP_IDCODE)
        self.dp_write(DP_ABORT, ABORT_ALL)
        self.dp_write(DP_SELECT, 0)
        self.dp_write(DP_CTRL_STAT, 0x50000000)
        for _ in range(100):
            if self.dp_read(DP_CTRL_STAT) & 0xA0000000 == 0xA0000000:
                break
        else:
            raise DapError("debug power-up not acknowledged")
        if csw is not None:
            self.ap_write(AP_CSW, csw)
        return dpidr

    # --------------------------------------------------------------- memory
    def read32(self, addr):
        self.dp_write(DP_SELECT, 0)
        return self.transfer_ok([(req(1, 0, AP_TAR), addr), (req(1, 1, AP_DRW), None)])[0]

    def write32(self, addr, value):
        self.dp_write(DP_SELECT, 0)
        self.transfer_ok([(req(1, 0, AP_TAR), addr), (req(1, 0, AP_DRW), value),
                          (req(0, 1, DP_RDBUFF), None)])

    def block_words(self, write):
        """Most words one TransferBlock carries in this probe's packet."""
        return (self.packet_size - (5 if write else 4)) // 4

    def chunks(self, addr, nbytes, write, max_words=None):
        """Split [addr, addr+nbytes) at 1 KiB TAR auto-increment boundaries."""
        limit = min(self.block_words(write), max_words or 1 << 16)
        while nbytes > 0:
            room = (1024 - (addr & 0x3FF)) // 4
            words = min(limit, room, nbytes // 4)
            yield addr, words
            addr += words * 4
            nbytes -= words * 4

    def block_read(self, addr, nbytes, max_words=None):
        self.dp_write(DP_SELECT, 0)
        out = bytearray()
        for a, words in self.chunks(addr, nbytes, False, max_words):
            self.transfer_ok([(req(1, 0, AP_TAR), a)])
            r = self.link.xfer(struct.pack("<BBHB", 0x06, 0, words, req(1, 1, AP_DRW)))
            if len(r) < 4 or r[3] != ACK_OK or int.from_bytes(r[1:3], "little") != words:
                raise DapError(f"TransferBlock read at {a:#x}: {r[:4].hex(' ')}")
            out += r[4:4 + 4 * words]
        return bytes(out)

    def block_write(self, addr, data, max_words=None):
        self.dp_write(DP_SELECT, 0)
        pos = 0
        for a, words in self.chunks(addr, len(data), True, max_words):
            self.transfer_ok([(req(1, 0, AP_TAR), a)])
            r = self.link.xfer(struct.pack("<BBHB", 0x06, 0, words, req(1, 0, AP_DRW))
                               + data[pos:pos + 4 * words])
            if len(r) < 4 or r[3] != ACK_OK or int.from_bytes(r[1:3], "little") != words:
                raise DapError(f"TransferBlock write at {a:#x}: {r[:4].hex(' ')}")
            pos += 4 * words
