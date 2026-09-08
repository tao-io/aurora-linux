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
p.add_argument('--hardware-boot', action='store_true')
p.add_argument('--decline-bandwidth', action='store_true')
p.add_argument('--decline-default-surface', action='store_true')
p.add_argument('--initialize', action='store_true',
               help='complete A401 startup and A457 first-client-open before the requested call')
p.add_argument('--swap-completion', type=int)
p.add_argument('--noop-swap', action='store_true')
p.add_argument('--background-swap', type=lambda s: int(s, 0))
p.add_argument('--scanout-pattern', action='store_true')
a = p.parse_args()
if not 1 <= a.repeat <= 100 or not 0 <= a.output_size <= 0x7000:
    p.error('invalid repeat count/output size')
if a.background_swap is not None and not 0 <= a.background_swap <= 0xffffffff:
    p.error('background color must fit u32')
swapping = a.noop_swap or a.background_swap is not None or a.scanout_pattern
if a.input_file and a.input_hex:
    p.error('choose input file or hex')
if a.initialize and not (a.startup_callbacks and a.hardware_boot):
    p.error('--initialize requires --startup-callbacks and --hardware-boot')
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
    # M4 0x2394a4: service ID and clock index. Its built-in host-less path
    # at 0x2394f8 returns 48 MHz for index 0 and 24 MHz for index 1.
    if tag == 'D408' and len(data) == 8 and output_size == 8:
        service, index = struct.unpack('<II', data)
        if data[:4] != b'VORP' or index not in (0, 1):
            raise RuntimeError(f'unsupported M4 display clock {service:#x}/{index}')
        rate = (48000000, 24000000)[index]
        log({'event': 'clock_rate', 'index': index, 'hz': rate})
        return struct.pack('<Q', rate)
    # M4 0x272a34..0x272abc reads up to eight EDT u32 values, preserving
    # caller defaults when the host has no override for the requested key.
    if tag == 'D125' and len(data) == 100 and output_size == 36:
        count = struct.unpack_from('<I', data, 64)[0]
        if count > 8:
            raise RuntimeError('invalid EDT element count')
        log({'event': 'edt_default', 'key': property_key(data, False), 'count': count})
        return data[68:68+count*4] + bytes(36-count*4)
    # M4 0x2dfb24/0x2dfb3c: host boot/power-on notifications.
    if tag in ('D000', 'D001') and not data and output_size == 4:
        return b'\x01\0\0\0'
    # M4 0x2e47e4..0x2e482c returns the frame sync properties in place.
    if tag == 'D006' and len(data) == 84 and output_size == 80:
        return data[:80] if not data[80] & 1 else bytes(80)
    # M4 0x2728e8 and startup 0x26eb98: ordinary interactive cold boot,
    # with no dark-boot or hibernation resume requested by this host.
    if tag in ('D122', 'D123') and not data and output_size == 4:
        return bytes(4)
    # M4 startup 0x26e920 explicitly diagnoses false as start_hardware_boot
    # failure. The sequence follows the existing host protocol, using M4 IDs.
    if a.hardware_boot and tag == 'D121' and not data and output_size == 4:
        dcp.call('A389', output_size=0, nested=True)
        # A446 reads the existing default-framebuffer enable flag; it does not
        # allocate a host buffer. Keep inherited framebuffer ownership here.
        dcp.call('A446', output_size=4, nested=True)
        for method, payload, output in (('A029', b'', 0),
                                        ('A468', b'\x01\0\0\0', 0),
                                        ('A000', b'\x01\0\0\0', 4),
                                        ('A465', b'', 4)):
            reply = dcp.call(method, payload, output, nested=True)
            if output and not reply[0] & 1:
                raise RuntimeError(f'{method}: hardware boot step returned false')
        return b'\x01\0\0\0'
    # M4 0x2df504 publishes the reply as DisplayPipePlaneBaseAlignment's
    # DefaultStride. Describe the inherited simple-framebuffer exactly.
    if tag == 'D101' and not data and output_size == 4:
        nodes = [n for n in Path('/sys/firmware/devicetree/base/chosen').glob('framebuffer*')
                 if b'simple-framebuffer' in (n/'compatible').read_bytes().split(b'\0')]
        if len(nodes) != 1:
            raise RuntimeError('cannot identify inherited framebuffer stride')
        stride = struct.unpack('>I', (nodes[0]/'stride').read_bytes())[0]
        if not 0 < stride <= 0x100000:
            raise RuntimeError('invalid inherited framebuffer stride')
        log({'event': 'default_stride', 'bytes': stride})
        return struct.pack('<I', stride)
    # M4 0x2cf460 requests a 56-byte bandwidth configuration plus status.
    # Status 1 maps to 0x8000000a and exits before consuming any registers.
    # This diagnostic tests whether inherited display state permits declining
    # the service; it never reports bandwidth setup success with fake addresses.
    if a.decline_bandwidth and tag == 'D003' and len(data) == 4 and output_size == 60:
        log({'event': 'bandwidth_declined', 'status': 1})
        return bytes(56) + struct.pack('<I', 1)
    # M4 0x238270..0x2382d8: flags, size, mode, three null flags;
    # reply contains physical/DCP addresses, mapped size and a nonzero handle.
    if tag == 'D451' and len(data) == 20 and output_size == 28:
        flags, size, mode = struct.unpack_from('<IQI', data)
        log({'event': 'allocate_request', 'flags': hex(flags), 'size': size, 'mode': mode})
        if flags != 0x703 or mode != 4 or data[16:19] != bytes(3):
            raise RuntimeError('unqualified M4 DMA allocation flags')
        return dcp.allocate_buffer(size)
    # M4 destructor 0x237cb8 releases a descriptor; defer physical freeing
    # until reboot because this diagnostic has no display retirement fence.
    if tag == 'D454' and len(data) == 4 and output_size == 4:
        dcp.retire_buffer(struct.unpack('<I', data)[0])
        return b'\x01\0\0\0'
    # M4 0x251018 has a 32-bit mapping cookie, u64 DVA and u32 status.
    # PIODMA uses DISP0 SID16, not the DCP coprocessor's SID23 mapping.
    if tag == 'D201' and len(data) == 12 and output_size == 16:
        ident = struct.unpack_from('<Q', data)[0]
        if data[8:12] != b'\x01\0\0\0':
            raise RuntimeError('unqualified PIODMA map flags')
        return dcp.map_piodma(ident)
    # M4 0x1d0268 checks D582's boolean. False logs allocation failure,
    # releases the optional default surface and returns to startup. This
    # diagnostic has not supplied a native scanout surface yet.
    if a.decline_default_surface and tag == 'D582' and len(data) == 8 and output_size == 4:
        width, height = struct.unpack('<II', data)
        log({'event': 'default_surface_declined', 'width': width, 'height': height})
        return bytes(4)
    # Hash-pinned M4 property wrappers: 0x2397f0, 0x23a004, 0x23a2e4,
    # 0x23a388, 0x2722e4, 0x272424 and 0x27254c. These exchange host metadata.
    # M4 0x239b28..0x239bd0 returns up to 3072 property bytes and a u32
    # length. Zero length is the wrapper's missing-property path. The J713
    # ADT has no power-lut-data-x property; do not invent calibration data.
    if tag == 'D400' and len(data) == 76 and output_size == 0xc04:
        capacity = struct.unpack_from('<I', data, 68)[0]
        if capacity > 0xc00 or data[72] & 1:
            raise RuntimeError('unsupported raw property request')
        key = property_key(data)
        value = dictionary_properties.get(key, b'')
        if len(value) > capacity:
            raise RuntimeError('raw property exceeds caller capacity')
        log({'event': 'raw_property', 'key': key, 'bytes': len(value)})
        return value + bytes(0xc00-len(value)) + struct.pack('<I', len(value))
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
    # M4 callback sites 0x2511f4/0x251250, matching wrappers
    # 0x2514e0/0x251394 return translated IOMFB status (zero succeeds).
    if tag in ('D206', 'D207') and not data and output_size == 4:
        method = 'A131' if tag == 'D206' else 'A132'
        status = dcp.call(method, output_size=4, nested=True)
        if status != bytes(4):
            raise RuntimeError(f'{method}: service match failed: {status.hex()}')
        return b'\x01\0\0\0'
    if tag == 'D100' and not data and output_size == 0:
        # 0x272d3c is a no-input, four-byte-output flag query on this firmware.
        dcp.call('A390', output_size=4, nested=True)
        return b''
    raise RuntimeError(f'unhandled startup callback {tag}, {len(data)}/{output_size} bytes')

try:
    dcp = DCP(log, t.check, startup_callback if a.startup_callbacks or swapping else None)
    start = time.monotonic()
    if a.initialize:
        # M4 0x1e33bc converts internal status zero to output boolean one.
        if dcp.call('A401', output_size=4) != b'\x01\0\0\0':
            raise RuntimeError('A401 startup returned false')
        dcp.call('A457', output_size=0)
        log({'event': 'initialized', 'first_client_open': True})
    scanout = dcp.allocate_scanout() if a.scanout_pattern else None
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
            if scanout:
                from surface import scanout_surface
                payload[0x588:0x7b4] = scanout_surface(scanout)
                struct.pack_into('<Q', payload, 0xe38, scanout['dva'])
                struct.pack_into('<I', payload, 0x9c, 1)
                for offset in (0xac, 0x10c):
                    struct.pack_into('<IIII', payload, offset, 0, 0, scanout['width'], scanout['height'])
                # Clear inherited layers and set opaque black below layer 0.
                struct.pack_into('<III', payload, 0x14c, 0x80000007, 0x80000007, 0xff000000)
                payload[0x1bcb] = 0
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
