"""The tool's encoders and decoders against the byte examples of docs/protocol.md."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from xrdap_test import cli, link  # noqa: E402
from xrdap_test.vendor import Item, decode_frames  # noqa: E402


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
        self.assertEqual(link.parse_spec("serial:COM5@921600,keep"),
                         {"kind": "serial", "port": "COM5", "baud": 921600, "keep_lines": True})
        self.assertEqual(link.parse_spec("tcp:xrdap-93ef1c.local")["port"], 3241)
        self.assertEqual(link.parse_spec("usbip:192.168.1.20:3250")["port"], 3250)


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


class Coverage(unittest.TestCase):
    def test_spec_matches_code(self):
        self.assertEqual(cli.cmd_coverage(None), 0)


if __name__ == "__main__":
    unittest.main()
