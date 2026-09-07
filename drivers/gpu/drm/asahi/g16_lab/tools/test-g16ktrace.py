#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the M4 record ABI, loss reports and malformed input handling."""
import gzip
from pathlib import Path
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

    def test_captured_m4_compute_and_ta(self):
        fixtures = Path(__file__).resolve().parent.parent / 'render' / 'validation'
        compute = decode(gzip.decompress((fixtures /
            '20260907T195911Z-mandelbrot-ktrace.bin.gz').read_bytes()))
        self.assertEqual(compute['record_count'], 976)
        self.assertEqual(compute['firmware_reported_lost_records'], 0)
        self.assertEqual(compute['event_counts']['FinishCompute'], 1)
        self.assertNotIn('StartTA', compute['event_counts'])
        ta = decode(gzip.decompress((fixtures /
            '20260907T200100Z-render-ta-ktrace.bin.gz').read_bytes()))
        self.assertEqual(ta['record_count'], 1458)
        self.assertEqual(ta['firmware_reported_lost_records'], 0)
        stamp = next(r for r in ta['records'] if r['name'] == 'StampUpdateTA')
        self.assertEqual([int(a, 16) for a in stamp['args']],
                         [0x100, 112, 0xfffffc300000c900, 0x16000001])
        self.assertNotIn('Start3D', ta['event_counts'])

    def test_reject_truncation_and_legacy_stride(self):
        for length in (1, 56, 71, 73):
            with self.subTest(length=length), self.assertRaises(ValueError):
                decode(bytes(length))
        with self.assertRaises(ValueError):
            decode(bytes(72))
        self.assertEqual(decode(b'')['record_count'], 0)


if __name__ == '__main__':
    unittest.main()
