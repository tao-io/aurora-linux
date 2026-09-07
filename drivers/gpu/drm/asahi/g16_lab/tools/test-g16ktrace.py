#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the M4 record ABI, loss reports and malformed input handling."""
import unittest
from g16ktrace import RECORD, decode


class DecodeTests(unittest.TestCase):
    def test_unaligned_fields_and_unknown_tail(self):
        tail = bytes(range(20))
        raw = RECORD.pack(5, 123456789, 0x16000001, 2, 3, 0xfffffc3000010000,
                          0x05000000, 7, tail)
        record = decode(raw)['records'][0]
        self.assertEqual(len(raw), 72)
        self.assertEqual(record['ticks'], 123456789)
        self.assertEqual(record['thread'], 5)
        self.assertEqual(record['name'], 'StartTA')
        self.assertEqual(record['phase'], 7)
        self.assertEqual(record['args'][3], '0xfffffc3000010000')
        self.assertEqual(record['unknown_tail_hex'], tail.hex())

    def test_multiple_records_and_firmware_loss(self):
        raw = b''.join(RECORD.pack(5, 100 + i, arg, 0, 0, 0, event, 0, bytes(20))
                       for i, (event, arg) in enumerate(((7, 42), (0x11c, 9), (0xabcdef, 1))))
        result = decode(raw)
        self.assertEqual(result['record_count'], 3)
        self.assertEqual(result['firmware_reported_lost_records'], 9)
        self.assertEqual(result['records'][2]['name'], 'event_abcdef')
        self.assertEqual(result['records'][2]['relative_ticks'], 2)

    def test_reject_truncation_and_legacy_stride(self):
        for length in (1, 56, 71, 73):
            with self.subTest(length=length), self.assertRaises(ValueError):
                decode(bytes(length))
        with self.assertRaises(ValueError):
            decode(bytes(72))
        self.assertEqual(decode(b'')['record_count'], 0)


if __name__ == '__main__':
    unittest.main()
