"""The tool's encoders and decoders against the byte examples of docs/protocol.md."""

import contextlib
import unittest
from types import SimpleNamespace
from unittest import mock

from xrdap_test import link
from xrdap_test import runner as R
from xrdap_test.cases import vendor_cases as V
from xrdap_test.cases.dap_behavior import dap_response_lengths
from xrdap_test.target import Region, Target
from xrdap_test.vendor import OK, STATE, Item, capture, decode_frames, numbered
from xrdap_test.vendor import Vendor as VendorClient


def hexb(text):
    return bytes.fromhex(text.replace(" ", ""))


class Framing(unittest.TestCase):
    def test_crc32_check_value(self):
        self.assertEqual(link.crc32(b"123456789"), 0x340BC6D9)
        self.assertEqual(link.TOPIC_REQ, 0xDC8309C5)
        self.assertEqual(link.TOPIC_RESP, 0x89E1A688)

    def test_serial_frame_example(self):
        frame = link.pack_serial_request(b"\x00\xFF")
        self.assertEqual(frame, hexb("5A 04 00 00 C5 09 83 DC 00 00 00 00 00 00 01 B1 02 00 00 FF 32"))

    def test_parser_resynchronises(self):
        response = link.pack_topic(link.TOPIC_RESP, b"\x02\x00\x00\x02")
        parser = link.TopicParser()
        self.assertEqual(parser.feed(b"\x5A\x00garbage" + response[:5]), [])
        self.assertEqual(parser.feed(response[5:]), [b"\x00\x02"])

    def test_tcp_frame(self):
        self.assertEqual(link.pack_tcp_request(b"\x00\xFF"), b"DAP\x00\x02\x00\x01\x00\x00\xFF")

    def test_link_specs(self):
        self.assertEqual(link.parse_spec("serial:PORT@921600,keep"),
                         {"kind": "serial", "port": "PORT", "baud": 921600, "keep_lines": True})
        self.assertEqual(link.parse_spec("tcp:xrdap-0a1b2c.local")["port"], 3241)
        self.assertEqual(link.parse_spec("usbip:probe.local:3250")["port"], 3250)


class Vendor(unittest.TestCase):
    def test_read_item_example(self):
        item = Item(0x20000000, 4, (8,), 4, id=1)
        self.assertEqual(item.encode(), hexb("00 0100 04 00000020 01 0800 0400"))

    def test_sub_read_example(self):
        items = [Item(0x20000100, 4, id=0x10), Item(0x20000104, 2, id=0x11, div=10, phase=3)]
        raw = hexb("06000000 B0590F000000 00 07000000"
                   "07000000 985D0F000000 00 08000000 3412")
        frames = decode_frames(raw, items, 2)
        self.assertEqual(frames, [(6, 1006000, {0x10: 7}), (7, 1007000, {0x10: 8, 0x11: 0x1234})])


class FakeLink:
    """Records the request and returns a canned answer (encoder/decoder checks only)."""

    def __init__(self, answer):
        self.answer, self.sent = answer, None

    def xfer(self, request, timeout=2.0):
        self.sent = request
        return self.answer


class ProtocolV2(unittest.TestCase):
    def test_info_example(self):
        info = VendorClient(FakeLink(hexb("80 00 02 1F000000 2000 08 00400000 40420F0000000000"))).info()
        self.assertEqual((info["version"], info["length"], info["features"], info["max_items"],
                          info["max_depth"], info["ring_bytes"], info["probe_time_us"]),
                         (2, 22, 0x1F, 32, 8, 16384, 1_000_000))

    def test_sub_start_example(self):
        items = [Item(0x20000100, 4, id=0x10), Item(0x20000104, 2, id=0x11, div=10, phase=3)]
        lk = FakeLink(hexb("83 00"))
        self.assertEqual(VendorClient(lk).start(1000, items), 0)
        self.assertEqual(lk.sent, hexb("83 E8030000 00 0200"
                                       "00 1000 04 00010020 00 0000 0100 0000"
                                       "00 1100 02 04010020 00 0000 0A00 0300"))


class MinimumPeriodSearch(unittest.TestCase):
    def test_doubling_then_25_us_steps(self):
        calls = []

        def keeps_up(vendor, items, period, tried):
            calls.append(period)
            return period >= 330

        real, V.keeps_up = V.keeps_up, keeps_up
        try:
            best, _ = V.search_min_period(None, [])
        finally:
            V.keeps_up = real
        self.assertEqual(best, 350)
        self.assertEqual(calls, [25, 25, 50, 50, 100, 100, 200, 200, 400, 200, 200, 225, 225,
                                 250, 250, 275, 275, 300, 300, 325, 325, 350])

    def test_one_failed_run_is_retried(self):
        flaky = {200: [False, True]}  # 200 us: a burst of drops once, then fine

        def keeps_up(vendor, items, period, tried):
            if period in flaky and flaky[period]:
                return flaky[period].pop(0)
            return period >= 150

        real, V.keeps_up = V.keeps_up, keeps_up
        try:
            best, _ = V.search_min_period(None, [])
        finally:
            V.keeps_up = real
        self.assertEqual(best, 150)


class SamplingProbe:
    """SUB_START, SUB_CTRL stop and SUB_READ of protocol 3.4, one frame per read while sampling.
    The first `lost` SUB_READs answer 85 FF, the host board's answer to a failed forward."""

    def __init__(self, lost):
        self.lost, self.sampling, self.tick = lost, False, 0

    def xfer(self, request, timeout=2.0):
        command = request[0]
        if command == 0x83:
            refused, self.sampling = self.sampling, True
            return bytes([command, STATE if refused else OK])
        if command == 0x84:
            self.sampling = self.sampling and request[1] != 0
            return bytes([command, OK])
        if self.lost:
            self.lost -= 1
            return bytes([command, 0xFF])
        if not self.sampling:
            return bytes([command, STATE]) + bytes(22)
        self.tick += 1
        head = bytes(20) + (1).to_bytes(2, "little")  # time, remaining, dropped, skipped, frames
        return bytes([command, OK]) + head + self.tick.to_bytes(4, "little") + bytes(11)


class LostForward(unittest.TestCase):
    def test_period_search_goes_on_after_one_failed_read(self):
        vendor = VendorClient(SamplingProbe(lost=1))
        with mock.patch.object(R, "SEARCH_RUN_S", 0.01):
            best, _ = V.search_min_period(vendor, numbered([Item(0x20000000)]))
        self.assertEqual(best, R.SEARCH_STEP_US)


class LossyProbe(SamplingProbe):
    """A SamplingProbe whose answers can get lost. A lost stop did not reach the probe, so sampling
    goes on. Failed forwards answer 84 FF or 85 FF, and a link without an answer raises."""

    def __init__(self, lost_stops=0, stop_timeouts=0, lost_drains=0, read_timeouts=0):
        super().__init__(lost=0)
        self.lost_stops, self.stop_timeouts = lost_stops, stop_timeouts
        self.lost_drains, self.read_timeouts = lost_drains, read_timeouts

    def xfer(self, request, timeout=2.0):
        command = request[0]
        if command == 0x84 and self.stop_timeouts:
            self.stop_timeouts -= 1
            raise TimeoutError("no answer")
        if command == 0x84 and self.lost_stops:
            self.lost_stops -= 1
            return bytes([command, 0xFF])
        if command == 0x85 and self.read_timeouts:
            self.read_timeouts -= 1
            raise TimeoutError("no answer")
        if command == 0x85 and self.lost_drains and not self.sampling:
            self.lost_drains -= 1
            return bytes([command, 0xFF])
        return super().xfer(request, timeout)


class LostReplies(unittest.TestCase):
    ITEMS = numbered([Item(0x20000000)])

    def run_capture(self, probe):
        return capture(VendorClient(probe), self.ITEMS, 0.01, 1000)

    def test_nothing_lost(self):
        probe = LossyProbe()
        result = self.run_capture(probe)
        self.assertFalse(probe.sampling)
        self.assertTrue(result.frames)
        self.assertEqual(result.left, 0)

    def test_stop_with_a_lost_answer_is_sent_again(self):
        for probe in (LossyProbe(lost_stops=2), LossyProbe(stop_timeouts=2)):
            self.assertTrue(self.run_capture(probe).frames)
            self.assertFalse(probe.sampling)
            self.assertEqual(VendorClient(probe).start(1000, self.ITEMS), OK)

    def test_stop_that_never_gets_through_raises(self):
        probe = LossyProbe(lost_stops=1000)
        with self.assertRaises(RuntimeError):
            self.run_capture(probe)

    def test_lost_read_in_the_drain_raises_and_the_next_run_starts(self):
        probe = LossyProbe(lost_drains=1)
        with self.assertRaises(RuntimeError):
            self.run_capture(probe)
        self.assertFalse(probe.sampling)
        self.assertTrue(self.run_capture(probe).frames)

    def test_read_without_an_answer_stops_sampling(self):
        probe = LossyProbe(read_timeouts=1)
        with self.assertRaises(TimeoutError):
            self.run_capture(probe)
        self.assertFalse(probe.sampling)
        self.assertTrue(self.run_capture(probe).frames)


class ResponseLengthScan(unittest.TestCase):
    @staticmethod
    def response_length(request):
        """Response length of an ExecuteCommands built from DAP_Transfer and DAP_HostStatus."""
        pos, total = 2, 2
        for _ in range(request[1]):
            if request[pos] == 0x05:
                pos, reads = pos + 3, 0
                for _ in range(request[pos - 1]):
                    reads += bool(request[pos] & 0x02)
                    pos += 1 if request[pos] & 0x02 else 5
                total += 3 + 4 * reads
            else:
                pos, total = pos + 3, total + 2
        return total

    def test_every_length_up_to_1024(self):
        from xrdap_test.cases.dap_behavior import execute_of_length

        for n in range(12, 1025):
            request = execute_of_length(n, 0x20000000)
            self.assertEqual(self.response_length(request), n)
            self.assertLessEqual(request[2 + 2], 255)  # DAP_Transfer count
            self.assertLessEqual(len(request), 1024)


class SlowLink:
    """Every answer to DAP-05's requests takes `delay` seconds."""

    kind = "usb"
    mps = 64

    def __init__(self, delay):
        self.delay = delay

    def xfer(self, request, timeout=2.0):
        if timeout < self.delay:
            raise TimeoutError
        return b"\x7F" + bytes(ResponseLengthScan.response_length(request) - 1)

    def send(self, request):
        pass

    def recv(self, timeout=2.0):
        return b"\x00"


class ScanContext:
    """The part of runner.Context DAP-05 uses."""

    def __init__(self, link_kind, delay):
        self.link_kind, self.delay, self.metrics = link_kind, delay, {}

    def need_target(self):
        return Target("fake", Region(0x20000000, 0x4400))

    @contextlib.contextmanager
    def session(self):
        yield SlowLink(self.delay), SimpleNamespace(packet_size=64), None

    def check(self, name, ok, detail=""):
        pass

    def metric(self, name, value):
        self.metrics[name] = value

    def summary(self, text):
        pass


class ResponseWait(unittest.TestCase):
    @staticmethod
    def missing(link_kind, delay):
        ctx = ScanContext(link_kind, delay)
        dap_response_lengths(ctx)
        return ctx.metrics["missing"]

    def test_radio_answer_at_the_end_of_the_host_boards_wait_counts(self):
        # Protocol 8.2 and forwarding.cpp: 1 s + 0.5 s of execution + 200 ms after the last send.
        self.assertEqual(self.missing("radio", 1.7), {})

    def test_radio_answer_after_the_host_boards_wait_is_missing(self):
        self.assertTrue(self.missing("radio", 1.71))

    def test_other_links_keep_one_second(self):
        self.assertEqual(self.missing("usb", 1.0), {})
        self.assertTrue(self.missing("usb", 1.01))
