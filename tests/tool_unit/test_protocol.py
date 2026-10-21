"""The tool's encoders and decoders against the byte examples of docs/protocol.md."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from xrdap_test import cli, link  # noqa: E402
from xrdap_test.vendor import Item, decode_frames  # noqa: E402
from xrdap_test.vendor import Vendor as VendorClient  # noqa: E402


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
        from xrdap_test.cases import vendor_cases as V

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
        self.assertEqual(calls, [25, 50, 100, 200, 400, 200, 225, 250, 275, 300, 325, 350])


class Coverage(unittest.TestCase):
    def test_case_table_matches_registry(self):
        self.assertEqual(cli.cmd_coverage(None), 0)


if __name__ == "__main__":
    unittest.main()
