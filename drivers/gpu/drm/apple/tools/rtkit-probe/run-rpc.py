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
p.add_argument('--test-nested-query', action='store_true')
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
properties = {}
dictionary_properties = {}
chunked_property = None
chunked_length = 0
def property_key(data, object_prefix=True):
    start = 4 if object_prefix else 0
    name = data[start:start+64].split(b'\0', 1)[0].decode('ascii')
    return (data[:4].hex() if object_prefix else 'root', name)

def startup_callback(dcp, tag, data, output_size):
    global chunked_property, chunked_length
    # M4 call sites 0x1dce18 and 0x1dced0; no callback output payload.
    if a.swap_completion is not None and output_size == 0:
        if (tag == 'D588' and not data) or (tag == 'D590' and len(data) == 0x730):
            return b''
    # J713 __TEXT 0x1d7760..0x1d7778: D599, no input or output.
    # This is the first callback observed from A401 on the inherited display.
    if tag == 'D599' and not data and output_size == 0:
        if a.test_nested_query and dcp.call('A412', output_size=4, nested=True) != b'\x01\0\0\0':
            raise RuntimeError('nested main-display query failed')
        return b''
    # M4 wrappers 0x265e20, 0x2700b0, 0x260c74, 0x26d708:
    # create PROV/PROD/PMUS/IOMF services. Startup sites 0x26e4dc/0x26e57c
    # add D112/D113; each returns a four-byte slot with boolean byte 0.
    if tag in ('D108', 'D109', 'D110', 'D111', 'D112', 'D113') and not data and output_size == 4:
        return b'\x01\0\0\0'
    # M4 0x1dcae8 marshals an eight-byte connection value, 75-byte tile
    # data, one extra flag, then a null byte at +84. Preserve all 76 bytes.
    if tag == 'D575' and len(data) == 88 and output_size == 76:
        log({'event': 'hotplug', 'connected': struct.unpack_from('<Q', data)[0]})
        return data[8:84] if not data[84] & 1 else bytes(76)
    # M4 0x1dcbf0 publishes two byte-sized power state flags in a u32 slot.
    if tag == 'D576' and len(data) == 4 and output_size == 0:
        log({'event': 'powerstate', 'flags': list(data[:2])})
        return b''
    # M4 0x1ca55c: property ID, parameter and u64 publication value.
    if tag == 'D300' and len(data) == 16 and output_size == 0:
        ident, param, value = struct.unpack('<IIQ', data)
        log({'event': 'pr_publish', 'id': ident, 'parameter': param, 'value': value})
        return b''
    # M4 0x257c78..0x257ddc: length + 1, chunks, then property key.
    if tag == 'D127' and len(data) == 4 and output_size == 4:
        length = struct.unpack('<I', data)[0]
        if chunked_property is not None or not 1 <= length <= 0x100001:
            raise RuntimeError('invalid chunked property start')
        chunked_length, chunked_property = length - 1, bytearray()
        return b'\x01\0\0\0'
    if tag == 'D128' and len(data) == 0x1008 and output_size == 4:
        offset, length = struct.unpack_from('<II', data, 0x1000)
        if chunked_property is None or offset != len(chunked_property) or length > 4096 or offset+length > chunked_length:
            raise RuntimeError('invalid chunked property range')
        chunked_property.extend(data[:length])
        return b'\x01\0\0\0'
    if tag == 'D129' and len(data) == 64 and output_size == 4:
        if chunked_property is None or len(chunked_property) != chunked_length:
            raise RuntimeError('incomplete chunked property')
        key = property_key(data, False)
        dictionary_properties[key] = bytes(chunked_property)
        log({'event': 'chunked_property', 'key': key, 'bytes': chunked_length})
        # Keep the serialized property as evidence, without trusting the name as a path.
        (a.output / ('property-%d.bin' % len(dictionary_properties))).write_bytes(chunked_property)
        chunked_property = None
        return b'\x01\0\0\0'
    # Hash-pinned M4 property wrappers: 0x2397f0, 0x23a004, 0x23a2e4,
    # 0x23a388, 0x2722e4, 0x272424 and 0x27254c. These exchange host metadata.
    if tag == 'D401' and len(data) == 80 and output_size == 12:
        key = property_key(data)
        return struct.pack('<QB3x', properties.get(key, 0), key in properties)
    # M4 dictionary publishers 0x23a224, 0x1db450, 0x1dbed4.
    if tag in ('D413', 'D552', 'D561') and len(data) == (0x1048 if tag == 'D413' else 0x1044) and output_size == 4:
        prefix = 4 if tag == 'D413' else 0
        if not data[prefix+0x1040] & 1:
            key = property_key(data, bool(prefix))
            dictionary_properties[key] = data[prefix+64:prefix+0x1040]
            log({'event': 'dictionary_property', 'key': key, 'bytes': 4096})
        return b'\x01\0\0\0'
    if tag in ('D563', 'D565') and len(data) == (76 if tag == 'D563' else 72) and output_size == 4:
        null_offset = 72 if tag == 'D563' else 68
        if not data[null_offset] & 1:
            properties[property_key(data, False)] = struct.unpack_from('<Q' if tag == 'D563' else '<I', data, 64)[0]
        return b'\x01\0\0\0'
    if tag == 'D567' and len(data) == 128 and output_size == 4:
        log({'event': 'string_property', 'key': property_key(data, False),
             'value': data[64:].split(b'\0', 1)[0].decode('ascii')})
        return b'\x01\0\0\0'
    if tag in ('D414', 'D415') and len(data) == (80 if tag == 'D414' else 76) and output_size == 4:
        null_offset = 76 if tag == 'D414' else 72
        if not data[null_offset] & 1:
            properties[property_key(data)] = struct.unpack_from('<Q' if tag == 'D414' else '<I', data, 68)[0]
        return b'\x01\0\0\0'
    if tag == 'D406' and len(data) == 72 and output_size == 0:
        log({'event': 'fixed_property', 'key': property_key(data), 'value': struct.unpack_from('<I', data, 68)[0]})
        return b''
    if tag in ('D102', 'D104') and len(data) == 68 and output_size == 0:
        properties[property_key(data, False)] = struct.unpack_from('<I', data, 64)[0] if tag == 'D102' else data[64]
        return b''
    if tag == 'D107' and len(data) == 64 and output_size == 0:
        properties.pop(property_key(data, False), None)
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
