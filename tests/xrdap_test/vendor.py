"""XR vendor extension 0x80..0x86, protocol version 2 (docs/protocol.md section 3)."""

import struct
import time
from dataclasses import dataclass, field

OK, ARG, STATE, RESOURCE, NO_TARGET, BUSY = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05
PROTOCOL_VERSION = 2
WIFI_CLEAR_CONFIRM = 0xA5  # protocol 3.7: 86 02 A5


@dataclass
class Item:
    base: int
    size: int = 4
    derefs: tuple = ()
    final_off: int = 0
    id: int = 0
    ap: int = 0
    div: int = 1
    phase: int = 0
    value: int = 0  # WRITE only

    def encode(self):
        out = struct.pack("<BHBIB", self.ap, self.id, self.size, self.base & 0xFFFFFFFF,
                          len(self.derefs))
        out += b"".join(struct.pack("<h", d) for d in self.derefs)
        return out + struct.pack("<h", self.final_off)


def numbered(items):
    """Give items ids 1..n in order (ids must be unique for decoding)."""
    for i, item in enumerate(items):
        item.id = i + 1
    return items


@dataclass
class SubRead:
    status: int
    probe_time_us: int = 0
    remaining: int = 0
    dropped: int = 0
    skipped: int = 0
    frames: list = field(default_factory=list)


class Vendor:
    def __init__(self, link):
        self.link = link

    def info(self):
        r = self.link.xfer(b"\x80")
        if len(r) < 22 or r[0] != 0x80:
            return {"status": r[1] if len(r) > 1 else None, "raw": r.hex(" ")}
        return {
            "status": r[1], "version": r[2], "length": len(r),
            "features": int.from_bytes(r[3:7], "little"),
            "max_items": int.from_bytes(r[7:9], "little"), "max_depth": r[9],
            "ring_bytes": int.from_bytes(r[10:14], "little"),
            "probe_time_us": int.from_bytes(r[14:22], "little"),
        }

    def read(self, items):
        """Returns (status, [(id, item status, value)])."""
        request = bytes([0x81, len(items)]) + b"".join(i.encode() for i in items)
        r = self.link.xfer(request)
        if len(r) < 3:
            return r[1] if len(r) > 1 else None, []
        sizes = {i.id: i.size for i in items}
        pos, entries = 3, []
        for _ in range(r[2]):
            item_id, status = struct.unpack_from("<HB", r, pos)
            pos += 3
            size = sizes.get(item_id, 0)
            entries.append((item_id, status, int.from_bytes(r[pos:pos + size], "little")))
            pos += size
        return r[1], entries

    def write(self, items):
        request = bytearray([0x82, len(items)])
        for i in items:
            request += i.encode() + (i.value & (1 << 8 * i.size) - 1).to_bytes(i.size, "little")
        r = self.link.xfer(bytes(request))
        if len(r) < 3:
            return r[1] if len(r) > 1 else None, []
        return r[1], [struct.unpack_from("<HB", r, 3 + 3 * k) for k in range(r[2])]


    def sub_ctrl(self, op):
        r = self.link.xfer(bytes([0x84, op]))
        return r[1] if len(r) > 1 else None

    def sub_read(self, items, max_bytes=0xFFFF) -> SubRead:
        r = self.link.xfer(struct.pack("<BH", 0x85, max_bytes))
        if len(r) < 24:
            return SubRead(r[1] if len(r) > 1 else None)
        out = SubRead(r[1], int.from_bytes(r[2:10], "little"),
                      int.from_bytes(r[10:14], "little"), int.from_bytes(r[14:18], "little"),
                      int.from_bytes(r[18:22], "little"))
        out.frames = decode_frames(r[24:], items, int.from_bytes(r[22:24], "little"))
        return out

    def start(self, period_us, items, flags=0):
        """SUB_START (0x83): configure and start at once. Returns the status."""
        request = struct.pack("<BIBH", 0x83, period_us, flags, len(items))
        request += b"".join(i.encode() + struct.pack("<HH", i.div, i.phase) for i in items)
        r = self.link.xfer(request)
        return r[1] if len(r) > 1 else None

    def stop(self):
        return self.sub_ctrl(0)


def decode_frames(raw, items, count):
    """Frames as (tick, timestamp_us, {id: value or None if the item failed})."""
    frames, pos = [], 0
    for _ in range(count):
        tick = int.from_bytes(raw[pos:pos + 4], "little")
        stamp = int.from_bytes(raw[pos + 4:pos + 10], "little")
        pos += 10
        due = [i for i in items if (tick + i.phase) % i.div == 0]
        nbitmap = (len(due) + 7) // 8
        bitmap = int.from_bytes(raw[pos:pos + nbitmap], "little")
        pos += nbitmap
        values = {}
        for k, item in enumerate(due):
            value = int.from_bytes(raw[pos:pos + item.size], "little")
            pos += item.size
            values[item.id] = None if bitmap >> k & 1 else value
        frames.append((tick, stamp, values))
    return frames


@dataclass
class Capture:
    frames: list
    skipped: int
    dropped: int
    seconds: float
    left: int  # ring bytes still unread after the drain


def capture(vendor, items, seconds, period_us, poll_s=0.0, max_bytes=0xFFFF):
    """Start sampling, poll SUB_READ for `seconds`, stop and drain."""
    frames, last = [], SubRead(0)
    status = vendor.start(period_us, items)
    if status != OK:
        raise RuntimeError(f"start refused: {status}")
    t0 = time.monotonic()
    end = t0 + seconds
    while time.monotonic() < end:
        last = vendor.sub_read(items, max_bytes)
        if last.status != OK:
            raise RuntimeError(f"SUB_READ status {last.status}")
        frames += last.frames
        if poll_s and not last.remaining:
            time.sleep(poll_s)
    elapsed = time.monotonic() - t0
    vendor.stop()
    for _ in range(200):
        last = vendor.sub_read(items, max_bytes)
        frames += last.frames
        if not last.remaining:
            break
    return Capture(frames, last.skipped, last.dropped, elapsed, last.remaining)


def tick_gaps(frames, items):
    """Ticks that should have produced a frame but did not (sampling every tick when any
    item has div 1; otherwise counts gaps between frame ticks larger than the due pattern)."""
    if any(i.div == 1 for i in items):
        ticks = [f[0] for f in frames]
        return sum(b - a - 1 for a, b in zip(ticks, ticks[1:]) if b != a + 1)
    expected = 0
    if frames:
        for k in range(frames[0][0], frames[-1][0] + 1):
            if any((k + i.phase) % i.div == 0 for i in items):
                expected += 1
    return expected - len(frames)
