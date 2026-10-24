"""Links to a probe: USB (CMSIS-DAP v2 bulk), serial (libxr Topic frame), TCP (OpenOCD
CMSIS-DAP TCP frame) and USB/IP (TCP 3240, protocol section 6).

Every link has send()/recv() so cases can keep several requests in flight, and xfer()
for one request and its response. Link specs:

    usb                 the only XR-DAP on USB
    usb:SERIAL          by USB serial number (full string or a unique suffix)
    serial:PORT[@BAUD][,keep]   keep = leave DTR/RTS alone (ESP32-C3 USB Serial/JTAG)
    tcp:HOST[:PORT]     default port 3241
    usbip:HOST[:PORT]   default port 3240, bus id 1-1
"""

import socket
import struct
import sys
import time

DEFAULT_VID_PID = (0x1D50, 0x6199)


class LinkError(RuntimeError):
    pass


class Link:
    kind = "?"
    # Requests a link may have in flight; probes report their own limit in DAP_Info 0xFE.
    max_in_flight = 1

    def send(self, request: bytes):
        raise NotImplementedError

    def recv(self, timeout=2.0) -> bytes:
        raise NotImplementedError

    def xfer(self, request: bytes, timeout=2.0) -> bytes:
        self.send(request)
        return self.recv(timeout)

    def close(self):
        pass



# ------------------------------------------------------------------------------ USB

def _usb_backend():
    if sys.platform == "win32":
        import libusb_package

        return libusb_package.get_libusb1_backend()
    return None


def _usb_serial(dev):
    import usb.core
    import usb.util

    try:
        return usb.util.get_string(dev, dev.iSerialNumber)
    except (ValueError, usb.core.USBError, NotImplementedError):
        return None


def _usb_product(dev):
    import usb.core
    import usb.util

    try:
        return usb.util.get_string(dev, dev.iProduct)
    except (ValueError, usb.core.USBError, NotImplementedError):
        return None


def list_usb(vid_pid=DEFAULT_VID_PID):
    """CMSIS-DAP probes with this VID:PID. Other devices share the VID:PID, so a device whose
    product string is readable and lacks "CMSIS-DAP" (required by the CMSIS-DAP specification)
    is left out; one whose string cannot be read (no access) is kept."""
    import usb.core

    devices = usb.core.find(find_all=True, idVendor=vid_pid[0], idProduct=vid_pid[1],
                            backend=_usb_backend())
    out = []
    for dev in devices:
        product = _usb_product(dev)
        if product is None or "CMSIS-DAP" in product:
            out.append((dev, _usb_serial(dev)))
    return out


class UsbLink(Link):
    kind = "usb"
    max_in_flight = 4

    def __init__(self, serial=None, vid_pid=DEFAULT_VID_PID):
        import usb.util

        self._util = usb.util
        found = [(d, s) for d, s in list_usb(vid_pid)
                 if serial is None or (s is not None and s.endswith(serial))]
        if not found:
            raise LinkError(f"no USB probe {vid_pid[0]:04x}:{vid_pid[1]:04x}"
                            + (f" with serial ...{serial}" if serial else ""))
        if len(found) > 1:
            raise LinkError("several USB probes match: "
                            + ", ".join(str(s) for _, s in found) + "; give usb:SERIAL")
        self.dev = found[0][0]
        try:
            self.dev.set_configuration()
        except Exception:
            pass  # already configured, or the OS does it (Windows WinUSB)
        cfg = self.dev.get_active_configuration()
        intf = next(i for i in cfg if i.bInterfaceClass == 0xFF and i.bNumEndpoints >= 2)
        self.intf = intf.bInterfaceNumber
        usb.util.claim_interface(self.dev, self.intf)
        direction = usb.util.endpoint_direction
        self.ep_out = next(e for e in intf if direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.ep_in = next(e for e in intf if direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        self.mps = self.ep_out.wMaxPacketSize
        self.drain()
        # IN reads ask for exactly the DAP packet size: a response that fills it ends without
        # a short packet, so a longer read would wait for the timeout (protocol, USB notes).
        self.read_size = 4096
        r = self.xfer(bytes([0x00, 0xFF]))
        size = int.from_bytes(r[2:4], "little") if len(r) >= 4 else 64
        self.read_size = -(-size // self.mps) * self.mps

    def drain(self):
        """Drop responses an earlier client left unread."""
        for _ in range(16):
            try:
                self.ep_in.read(4096, 20)
            except Exception:
                break

    def send(self, request):
        self.ep_out.write(request, 2000)
        if request and len(request) % self.mps == 0:
            # A request of whole packets has no short packet to end it (protocol, USB notes).
            self.ep_out.write(b"", 2000)

    def recv(self, timeout=2.0):
        try:
            return bytes(self.ep_in.read(self.read_size, int(timeout * 1000)))
        except Exception as exc:
            raise TimeoutError(f"USB response timeout: {exc}") from exc

    def close(self):
        try:
            self._util.release_interface(self.dev, self.intf)
        finally:
            self._util.dispose_resources(self.dev)



# --------------------------------------------------------------------------- serial

def crc8(data: bytes) -> int:
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8C if crc & 1 else crc >> 1
    return crc


def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc


TOPIC_REQ = crc32(b"dap_req")
TOPIC_RESP = crc32(b"dap_resp")


def pack_topic(topic: int, payload: bytes) -> bytes:
    header = bytearray(16)
    header[0] = 0x5A
    header[1:4] = len(payload).to_bytes(3, "little")
    header[4:8] = topic.to_bytes(4, "little")
    header[14] = 1
    header[15] = crc8(header[:15])
    frame = bytes(header) + payload
    return frame + bytes([crc8(frame)])


def pack_serial_request(dap_packet: bytes) -> bytes:
    return pack_topic(TOPIC_REQ, len(dap_packet).to_bytes(2, "little") + dap_packet)


class TopicParser:
    """Resynchronising parser for libxr Topic frames; yields DAP packets of dap_resp."""

    def __init__(self, topic=TOPIC_RESP):
        self.buf = bytearray()
        self.topic = topic
        # Diagnostics: bytes dropped while searching for a frame (pad bytes included), headers
        # rejected after a 0x5A, and frames whose data CRC failed.
        self.skipped = self.bad_headers = self.crc_errors = 0

    def feed(self, data: bytes):
        self.buf.extend(data)
        out = []
        while True:
            start = self.buf.find(b"\x5A")
            if start < 0:
                self.skipped += len(self.buf)
                self.buf.clear()
                return out
            self.skipped += start
            del self.buf[:start]
            if len(self.buf) < 16:
                return out
            data_len = int.from_bytes(self.buf[1:4], "little")
            if self.buf[14] != 1 or data_len > 2 + 4096 or crc8(self.buf[:15]) != self.buf[15]:
                self.bad_headers += 1
                self.skipped += 1
                del self.buf[:1]
                continue
            total = 16 + data_len + 1
            if len(self.buf) < total:
                return out
            if crc8(self.buf[:16 + data_len]) != self.buf[16 + data_len]:
                self.crc_errors += 1
                self.skipped += 1
                del self.buf[:1]
                continue
            topic = int.from_bytes(self.buf[4:8], "little")
            payload = bytes(self.buf[16:16 + data_len])
            del self.buf[:total]
            if topic == self.topic and len(payload) >= 2:
                n = int.from_bytes(payload[:2], "little")
                out.append(payload[2:2 + n])


class SerialLink(Link):
    kind = "serial"

    def __init__(self, port, baud=4_000_000, keep_lines=False):
        import serial

        self.port = port
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.005
        if not keep_lines:
            # Run state of the ESP32 auto-reset circuit: DTR asserted, RTS released.
            self.ser.dtr = True
            self.ser.rts = False
        self.ser.open()
        self.parser = TopicParser()
        self.pending = []
        # Opening the port may reset the probe; wait until DAP_Info answers.
        deadline = time.monotonic() + 10.0
        while True:
            try:
                self.xfer(bytes([0x00, 0xFF]), timeout=0.3)
                break
            except TimeoutError:
                if time.monotonic() > deadline:
                    self.ser.close()
                    raise LinkError(f"no DAP answer on {port}")
        # A late answer to an earlier client's request may still arrive; let it land and drop
        # it so that every later response pairs with its own request.
        time.sleep(0.2)
        self.pending.clear()
        self.parser.feed(self.ser.read(self.ser.in_waiting or 1))
        self.pending.clear()

    def send(self, request):
        self.ser.write(pack_serial_request(request))

    def recv(self, timeout=2.0):
        deadline = time.monotonic() + timeout
        while not self.pending:
            if time.monotonic() > deadline:
                raise TimeoutError("serial response timeout")
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if chunk:
                self.pending.extend(self.parser.feed(chunk))
        return self.pending.pop(0)

    def close(self):
        self.ser.close()



# ------------------------------------------------------------------------------ TCP

def pack_tcp_request(dap_packet: bytes) -> bytes:
    return b"DAP\0" + len(dap_packet).to_bytes(2, "little") + b"\x01\x00" + dap_packet


def _recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed by the probe")
        data.extend(chunk)
    return bytes(data)


class TcpLink(Link):
    kind = "tcp"
    max_in_flight = 4

    def __init__(self, host, port=3241, timeout=3.0):
        self.addr = (host, port)
        self.sock = socket.create_connection(self.addr, timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send(self, request):
        self.sock.sendall(pack_tcp_request(request))

    def recv(self, timeout=2.0):
        self.sock.settimeout(timeout)
        try:
            header = _recv_exact(self.sock, 8)
        except socket.timeout as exc:
            raise TimeoutError("TCP response timeout") from exc
        if header[:4] != b"DAP\0" or header[6] != 2:
            raise LinkError(f"bad TCP frame header {header.hex()}")
        return _recv_exact(self.sock, int.from_bytes(header[4:6], "little"))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------- USB/IP

class UsbipLink(Link):
    """Minimal USB/IP client: imports bus 1-1 and runs bulk OUT/IN on endpoint 1."""

    kind = "usbip"
    max_in_flight = 4

    def __init__(self, host, port=3240, busid=b"1-1", timeout=3.0):
        self.addr = (host, port)
        self.sock = socket.create_connection(self.addr, timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.sendall(struct.pack(">HHI", 0x0111, 0x8003, 0) + busid.ljust(32, b"\0"))
        reply = _recv_exact(self.sock, 8)
        if struct.unpack(">I", reply[4:8])[0] != 0:
            raise LinkError("USB/IP import refused")
        device = _recv_exact(self.sock, 312)
        busnum, devnum = struct.unpack(">II", device[288:296])
        self.devid = busnum << 16 | devnum
        self.seq = 0
        self.in_seqs = []  # bulk IN URBs waiting for a response, oldest first
        self.done = {}
        self.is_in = set()  # RET_SUBMIT has direction 0, the URB's own direction counts
        config = self._submit(0, 0, b"", setup=bytes([0x00, 0x09, 0x01, 0, 0, 0, 0, 0]))
        self._wait(config)  # SET_CONFIGURATION 1

    def _submit(self, direction, ep, data, length=None, setup=b"\0" * 8):
        self.seq += 1
        length = len(data) if length is None else length
        header = struct.pack(">IIIII", 1, self.seq, self.devid, direction, ep)
        header += struct.pack(">IiiiI", 0, length, 0, 0, 0) + setup
        self.sock.sendall(header + data)
        if direction == 1:
            self.is_in.add(self.seq)
        return self.seq

    def _wait(self, seq, timeout=2.0):
        self.sock.settimeout(timeout)
        while seq not in self.done:
            try:
                header = _recv_exact(self.sock, 48)
            except socket.timeout as exc:
                raise TimeoutError("USB/IP response timeout") from exc
            command, got = struct.unpack(">II", header[:8])
            status, actual = struct.unpack(">ii", header[20:28])
            if command != 3:
                raise LinkError(f"unexpected USB/IP reply {command}")
            data = _recv_exact(self.sock, actual) if got in self.is_in and actual > 0 else b""
            self.is_in.discard(got)
            self.done[got] = (status, data)
        status, data = self.done.pop(seq)
        if status != 0:
            raise LinkError(f"USB/IP URB {seq} status {status}")
        return data

    def send(self, request):
        # OUT executes the command; the IN behind it collects the response.
        self._submit(0, 1, request)
        self.in_seqs.append(self._submit(1, 1, b"", length=4096))

    def recv(self, timeout=2.0):
        data = self._wait(self.in_seqs.pop(0), timeout)
        for seq in [s for s in self.done if s not in self.in_seqs]:
            status, _ = self.done.pop(seq)  # completed OUT URBs
            if status != 0:
                raise LinkError(f"USB/IP OUT URB {seq} status {status}")
        return data

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass



# ------------------------------------------------------------------------------ open

def parse_spec(spec: str):
    kind, _, rest = spec.partition(":")
    if kind == "usb":
        return {"kind": "usb", "serial": rest or None}
    if kind == "serial":
        port, _, opts = rest.partition(",")
        port, _, baud = port.partition("@")
        return {"kind": "serial", "port": port, "baud": int(baud) if baud else 4_000_000,
                "keep_lines": opts == "keep"}
    if kind in ("tcp", "usbip"):
        host, _, port = rest.rpartition(":") if rest.count(":") == 1 else (rest, "", "")
        return {"kind": kind, "host": host,
                "port": int(port) if port else (3241 if kind == "tcp" else 3240)}
    raise ValueError(f"unknown link spec {spec!r}")


def open_link(spec: str) -> Link:
    p = parse_spec(spec)
    if p["kind"] == "usb":
        return UsbLink(p["serial"])
    if p["kind"] == "serial":
        return SerialLink(p["port"], p["baud"], p["keep_lines"])
    if p["kind"] == "tcp":
        return TcpLink(p["host"], p["port"])
    return UsbipLink(p["host"], p["port"])
