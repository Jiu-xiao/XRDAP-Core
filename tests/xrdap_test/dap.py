"""Standard CMSIS-DAP commands over a Link, plus the SWD/MEM-AP steps the cases need."""

import struct

ACK_OK, ACK_FAULT = 1, 4

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
# Out of the dormant state: selection alert, four low clocks, SWD activation code.
DORMANT_TO_SWD = (bytes([0x12, 8, 0xFF]),
                  bytes([0x12, 128]) + bytes.fromhex("92f30962952d8586e9afdde3a20ebc19"),
                  bytes([0x12, 4, 0x00]), bytes([0x12, 8, 0x1A]))
# ADIv6: the MEM-AP registers, at their ADIv5 offsets, from the base address of the AP.
ADIV6_AP_REGS = 0xD00


class DapError(RuntimeError):
    pass


def req(apndp, rnw, addr):
    return (AP if apndp else 0) | (RD if rnw else 0) | (addr & 0x0C)


class Dap:
    def __init__(self, link, targetsel=None, ap_base=None):
        self.link = link
        self.targetsel = targetsel  # SWD multidrop: the value of TARGETSEL
        self.ap_base = ap_base  # ADIv6: the base address of the MEM-AP
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

    def ap_select(self, reg=0):
        """DP SELECT for an AP register: ADIv5 the bank, ADIv6 the address of the 16 bytes
        around the register."""
        if self.ap_base is None:
            return reg & 0xF0
        return self.ap_base + ADIV6_AP_REGS + (reg & 0xF0)

    def ap_read(self, reg):
        self.dp_write(DP_SELECT, self.ap_select(reg))
        return self.transfer_ok([(req(1, 1, reg), None)])[0]

    def ap_write(self, reg, value):
        self.dp_write(DP_SELECT, self.ap_select(reg))
        self.transfer_ok([(req(1, 0, reg), value)])

    def clear_errors(self):
        self.transfer([(req(0, 0, DP_ABORT), ABORT_ALL)])

    # ------------------------------------------------------------------ SWD
    def select_target(self):
        """SWD multidrop: wake the SW-DP and select it. Nobody drives the acknowledge of
        TARGETSEL, so it goes out as a sequence: request, 5 clocks released, data, parity."""
        for seq in DORMANT_TO_SWD + (SWD_LINE_RESET, IDLE_8):
            self.swj_sequence(seq)
        parity = bin(self.targetsel).count("1") & 1
        r = self.link.xfer(bytes([0x1D, 3, 0x08, 0x99, 0x85, 0x21]) + struct.pack("<IB", self.targetsel, parity))
        if r[:2] != b"\x1D\x00":
            raise DapError(f"TARGETSEL: {r.hex(' ')}")

    def attach(self, clock_hz):
        """Connect SWD, power up the debug domain and set the AP CSW. Returns DPIDR."""
        self.swj_clock(clock_hz)
        self.connect()
        self.transfer_configure()
        self.link.xfer(bytes([0x13, 0x00]))  # SWD_Configure: 1 turnaround clock, no data phase
        if self.targetsel is None:
            for seq in (SWD_LINE_RESET, JTAG_TO_SWD, SWD_LINE_RESET, IDLE_8):
                self.swj_sequence(seq)
        else:
            self.select_target()
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
        self.ap_write(AP_CSW, CSW_WORD_INC)
        return dpidr

    # --------------------------------------------------------------- memory
    def read32(self, addr):
        self.dp_write(DP_SELECT, self.ap_select())
        return self.transfer_ok([(req(1, 0, AP_TAR), addr), (req(1, 1, AP_DRW), None)])[0]

    def write32(self, addr, value):
        self.dp_write(DP_SELECT, self.ap_select())
        self.transfer_ok([(req(1, 0, AP_TAR), addr), (req(1, 0, AP_DRW), value),
                          (req(0, 1, DP_RDBUFF), None)])

    def block_words(self, write):
        """Most words one TransferBlock carries in this probe's packet."""
        return (self.packet_size - (5 if write else 4)) // 4

    def chunks(self, addr, nbytes, write):
        """Split [addr, addr+nbytes) at 1 KiB TAR auto-increment boundaries."""
        limit = self.block_words(write)
        while nbytes > 0:
            room = (1024 - (addr & 0x3FF)) // 4
            words = min(limit, room, nbytes // 4)
            yield addr, words
            addr += words * 4
            nbytes -= words * 4

    def block_read(self, addr, nbytes):
        self.dp_write(DP_SELECT, self.ap_select())
        out = bytearray()
        for a, words in self.chunks(addr, nbytes, False):
            self.transfer_ok([(req(1, 0, AP_TAR), a)])
            r = self.link.xfer(struct.pack("<BBHB", 0x06, 0, words, req(1, 1, AP_DRW)))
            if len(r) < 4 or r[3] != ACK_OK or int.from_bytes(r[1:3], "little") != words:
                raise DapError(f"TransferBlock read at {a:#x}: {r[:4].hex(' ')}")
            out += r[4:4 + 4 * words]
        return bytes(out)

    def block_write(self, addr, data):
        self.dp_write(DP_SELECT, self.ap_select())
        pos = 0
        for a, words in self.chunks(addr, len(data), True):
            self.transfer_ok([(req(1, 0, AP_TAR), a)])
            r = self.link.xfer(struct.pack("<BBHB", 0x06, 0, words, req(1, 0, AP_DRW))
                               + data[pos:pos + 4 * words])
            if len(r) < 4 or r[3] != ACK_OK or int.from_bytes(r[1:3], "little") != words:
                raise DapError(f"TransferBlock write at {a:#x}: {r[:4].hex(' ')}")
            pos += 4 * words
