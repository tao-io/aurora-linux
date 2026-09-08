#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run bounded DCP RPC diagnostics with watchdog and thermal monitoring."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import time

from rpc import DCP

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--guards', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--call', default='A412')
p.add_argument('--input-hex', default='')
p.add_argument('--input-file', type=Path)
p.add_argument('--output-size', type=int, default=4)
p.add_argument('--repeat', type=int, default=1)
p.add_argument('--expect-hex')
p.add_argument('--startup-callbacks', action='store_true')
p.add_argument('--swap-completion', type=int)
p.add_argument('--noop-swap', action='store_true')
p.add_argument('--background-swap', type=lambda s: int(s, 0))
a = p.parse_args()
if not 1 <= a.repeat <= 100 or not 0 <= a.output_size <= 0x7000:
    p.error('invalid repeat count/output size')
if a.background_swap is not None and not 0 <= a.background_swap <= 0xffffffff:
    p.error('background color must fit u32')
swapping = a.noop_swap or a.background_swap is not None
if a.input_file and a.input_hex:
    p.error('choose input file or hex')
payload = a.input_file.read_bytes() if a.input_file else bytes.fromhex(a.input_hex)
sys.path.insert(0, str(a.guards))
from experiment_watchdog import ExperimentWatchdog
from experiment_thermals import ExperimentThermals

a.output.mkdir(parents=True, exist_ok=False)
w = ExperimentWatchdog()
r = {'watchdog': w.arm(), 'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
t = ExperimentThermals(a.output)
try:
    t.start()
except BaseException:
    w.disarm()
    raise
since = str(int(time.time()))
events = []
def log(event):
    events.append(event)
    with (a.output / 'rpc.jsonl').open('a') as f:
        f.write(json.dumps(event) + '\n')
        f.flush()
        os.fsync(f.fileno())
    print(json.dumps(event), flush=True)

dcp = None
def startup_callback(dcp, tag, data, output_size):
    # M4 call sites 0x1dce18 and 0x1dced0; no callback output payload.
    if a.swap_completion is not None and output_size == 0:
        if (tag == 'D588' and not data) or (tag == 'D590' and len(data) == 0x730):
            return b''
    # J713 __TEXT 0x1d7760..0x1d7778: D599, no input or output.
    # This is the first callback observed from A401 on the inherited display.
    if tag == 'D599' and not data and output_size == 0:
        return b''
    # M4 wrappers 0x265e20, 0x2700b0, 0x260c74, 0x26d708:
    # create PROV/PROD/PMUS/IOMF services; a four-byte slot with boolean byte 0.
    if tag in ('D108', 'D109', 'D110', 'D111') and not data and output_size == 4:
        return b'\x01\0\0\0'
    # Observed M4 publication: IOMF / InitialPanelTemperature / fixed-point value.
    # The host records this property; the firmware requests no reply payload.
    if (tag == 'D406' and len(data) == 72 and output_size == 0
            and data[:4] == b'FMOI'
            and data[4:68].split(b'\0', 1)[0] == b'InitialPanelTemperature'):
        return b''
    if tag == 'D100' and not data and output_size == 0:
        # 0x272d3c is a no-input, four-byte-output flag query on this firmware.
        dcp.call('A390', output_size=4, nested=True)
        return b''
    raise RuntimeError(f'unhandled startup callback {tag}, {len(data)}/{output_size} bytes')

try:
    dcp = DCP(log, t.check, startup_callback if a.startup_callbacks or swapping else None)
    start = time.monotonic()
    for i in range(a.repeat):
        if time.monotonic() - start > 20:
            raise TimeoutError('RPC test exceeded 20 seconds')
        if swapping:
            reply = dcp.call('A406', bytes(16), 8)
            swap_id, status = struct.unpack('<II', reply)
            if status:
                raise RuntimeError(f'A406 failed: {status:#x}')
            # M4 0x1d50b4/0x1d50c8 read swap ID at +0x98 from input x1.
            # A407 wrapper qualification establishes the null-array positions.
            payload = bytearray(0x1bd8)
            struct.pack_into('<I', payload, 0x98, swap_id)
            if a.background_swap is not None:
                struct.pack_into('<III', payload, 0x14c, 1 << 31, 1 << 31, a.background_swap)
            payload[0x1bcb:0x1bd5] = bytes([1])*10
            payload[0x1bd6:0x1bd8] = b'\x01\x01'
            a.call, a.output_size, a.swap_completion = 'A407', 12, swap_id
        result = dcp.call(a.call, payload, a.output_size,
                          completion=('D590', a.swap_completion) if a.swap_completion is not None else None)
        if a.expect_hex is not None and result != bytes.fromhex(a.expect_hex):
            raise RuntimeError('RPC result differs from expected value')
    t.check()
    r.update(passed=True, calls=a.repeat, elapsed_seconds=time.monotonic()-start)
except BaseException as error:
    r.update(passed=False, failure=repr(error))
    raise
finally:
    if dcp:
        dcp.close()
    t.close()
    r['thermal'] = t.summary()
    logtext = subprocess.check_output(['journalctl', '-k', '-b', '--since', '@'+since,
                                       '--no-pager', '-o', 'cat'], text=True, timeout=5)
    (a.output / 'kernel.log').write_text(logtext)
    (a.output / 'result.json').write_text(json.dumps(r, indent=2)+'\n')
    os.sync()
w.disarm()
print(json.dumps(r, indent=2))
