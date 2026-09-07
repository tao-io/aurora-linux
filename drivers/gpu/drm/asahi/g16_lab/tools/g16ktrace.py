#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode the J713 25G83 firmware execution records exported by g16_lab."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct

RECORD = struct.Struct('<I5QII20s')
# Names checked at the same-build firmware's emitter call sites. Unknown events
# remain numeric; older m1n1 names are not assumed to describe the M4 ABI.
EVENTS = {
    0x000: 'StartTA', 0x001: 'FinishTA',
    0x004: 'Start3D', 0x006: 'Finish3D',
    0x007: 'StartCompute', 0x008: 'FinishCompute',
    0x00a: 'StampUpdateTA', 0x00c: 'StampUpdate3D',
    0x00e: 'StampUpdateCompute', 0x028: 'EventNotify',
    0x11c: 'FirmwareTraceOverflow',
}


def decode(data):
    if len(data) % RECORD.size:
        raise ValueError(f'truncated trace: {len(data)} bytes is not a multiple of {RECORD.size}')
    records = []
    counts = collections.Counter()
    first_tick = None
    lost = 0
    for index, values in enumerate(RECORD.iter_unpack(data)):
        kind, ticks, *rest = values
        args = rest[:4]
        packed_event, phase, tail = rest[4:]
        if kind != 5:
            raise ValueError(f'record {index}: type {kind}, expected 5')
        event = packed_event & 0xffffff
        thread = packed_event >> 24
        if first_tick is None:
            first_tick = ticks
        name = EVENTS.get(event, f'event_{event:06x}')
        counts[name] += 1
        if event == 0x11c:
            lost += args[0]
        records.append({
            'index': index, 'ticks': ticks, 'relative_ticks': ticks - first_tick,
            'event': f'0x{event:06x}', 'name': name, 'thread': thread,
            'phase': phase, 'args': [f'0x{x:016x}' for x in args],
            'unknown_tail_hex': tail.hex(),
        })
    return {
        'platform': 'J713/T8132/G16G', 'firmware_build': '25G83',
        'record_size': RECORD.size, 'record_count': len(records),
        'sha256': hashlib.sha256(data).hexdigest(),
        'firmware_reported_lost_records': lost,
        'event_counts': dict(sorted(counts.items())), 'records': records,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--timeline', action='store_true')
    args = parser.parse_args()
    try:
        result = decode(args.trace.read_bytes())
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.timeline:
        for record in result['records']:
            print(f"{record['index']:5d} {record['relative_ticks']:12d} "
                  f"thread={record['thread']} {record['name']} phase={record['phase']} "
                  + ' '.join(record['args']))
    else:
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
