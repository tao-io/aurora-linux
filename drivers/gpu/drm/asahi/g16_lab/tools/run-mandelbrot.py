#!/usr/bin/env python3
"""Run one J713 G16 dispatch after a fresh boot and preserve its evidence."""
import argparse
import collections
import datetime
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib

EXPECTED = '792314d0bce27c82b5777d559020eba427b687d8fc4d6b1c031d9c915fc7ffc1'
PARAMS = [
    'stage=rtkit-initdata-exact-offscreen-store',
    'compute_macos_iadd=1', 'compute_mandelbrot=1',
    'compute_macos_iadd_capture_context=1', 'compute_macos_iadd_ap2_leaves=1',
    'compute_exact_loader_iomap_all=1',
    'compute_macos_iadd_aligned_uma_hardware_table=1',
    'compute_macos_iadd_populated_usc_private_memory=1',
]

def output(*args):
    return subprocess.check_output(args, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, default=Path('/root/g16_lab.ko'))
    parser.add_argument('--results', type=Path, default=Path('/root/g16-results'))
    parser.add_argument('--render-queues', action='store_true',
                        help='after Mandelbrot, test TA/fragment queue barriers (no render shaders)')
    args = parser.parse_args()
    params = PARAMS + (['render_queue_probe=1'] if args.render_queues else [])
    if os.geteuid() != 0:
        parser.error('run as root')
    compatible = Path('/sys/firmware/devicetree/base/compatible').read_bytes().split(b'\0')
    if b'apple,j713' not in compatible or b'apple,t8132' not in compatible:
        parser.error('requires J713/T8132')
    if os.sysconf('SC_PAGE_SIZE') != 16384:
        parser.error('requires a 16 KiB page-size kernel')
    handoff = Path('/sys/firmware/devicetree/base/chosen')
    regions = {
        'gfx-handoff': (0x103fff70000, 0x4000),
        'gfx-shared-l2-region': (0x103fff74000, 0x4000),
        'gfx-shared-region': (0x103fff78000, 0x40000),
        'gpu-region': (0x103fffb8000, 0x4000),
    }
    try:
        # These chosen properties preserve the ADT's little-endian encoding.
        for name, expected in regions.items():
            actual = tuple(struct.unpack('<Q', (handoff / 'g16-lab-sgx' / (name + '-' + field)).read_bytes())[0]
                           for field in ('base', 'size'))
            if actual != expected:
                parser.error('GPU shared-memory layout differs from the qualified J713')
        segments = (handoff / 'g16-lab-gfx-asc' / 'segment-ranges').read_bytes()
        expected_segments = (
            (0x10000cc0000, 0xfffffc0000000000, 0x10000cc0000, 0x60000, 1),
            (0x10001df4000, 0xfffffc0000060000, 0x10001df4000, 0x13c000, 0),
        )
        if tuple(struct.iter_unpack('<QQQII', segments)) != expected_segments:
            parser.error('GPU firmware segment layout differs from the qualified J713')
    except (OSError, struct.error):
        parser.error('missing or malformed m1n1 G16 lab handoff metadata')
    if 'apple_j713_reserve_stage2=1' not in Path('/proc/cmdline').read_text().split():
        parser.error('boot with apple_j713_reserve_stage2=1')
    release = os.uname().release
    if output('modinfo', '-F', 'vermagic', str(args.module)).split()[0] != release:
        parser.error('module does not match the running kernel')
    if args.render_queues:
        resource = Path('/lib/firmware/g16/render-reference.bin')
        expected_resource = '43e630484f7b5de294911fb6e5f78dc38b833d3d7d409c03fa3cdc4da98b27bf'
        if not resource.exists() or hashlib.sha256(resource.read_bytes()).hexdigest() != expected_resource:
            parser.error('install the checksummed encoded M4 render-reference.bin first')
    before = output('dmesg')
    marker = Path('/run/g16-lab-firmware-used')
    if marker.exists() or 'GFX RTKit' in before or Path('/sys/module/g16_lab').exists():
        parser.error('GPU firmware already used this boot; reboot before a new dispatch')
    debug = Path('/sys/kernel/debug')
    if subprocess.run(['mountpoint', '-q', str(debug)]).returncode:
        subprocess.run(['mount', '-t', 'debugfs', 'debugfs', str(debug)], check=True)
    result = args.results / (datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + ('-render-queues' if args.render_queues else '-mandelbrot'))
    result.mkdir(parents=True, exist_ok=False)
    marker.write_text(str(result) + '\n')
    metadata = {
        'kernel': release,
        'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest(),
        'parameters': params,
    }
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    run = subprocess.run(['insmod', str(args.module), *params], capture_output=True, text=True)
    (result / 'insmod.txt').write_text(run.stdout + run.stderr)
    after = output('dmesg')
    (result / 'dmesg.log').write_text(after)
    print(after[len(before):] if after.startswith(before) else after, end='')
    metadata['insmod_rc'] = run.returncode
    status = Path('/sys/module/g16_lab/parameters/stage_result')
    metadata['stage_result'] = int(status.read_text()) if status.exists() else None
    for name, dest in [('g16_lab_mandelbrot_rgba8', 'mandelbrot.rgba'), ('g16_lab_crashlog', 'crashlog.bin'),
                       ('g16_lab_render_prepared', 'render-graph.bin')]:
        p = debug / name
        if p.exists():
            (result / dest).write_bytes(p.read_bytes())
    pixels = result / 'mandelbrot.rgba'
    valid = False
    if pixels.exists():
        raw = pixels.read_bytes()
        metadata['output_bytes'] = len(raw)
        metadata['output_sha256'] = hashlib.sha256(raw).hexdigest()
        if len(raw) == 64 * 64 * 4:
            colors = collections.Counter(struct.unpack('<4096I', raw))
            metadata['colors'] = {f'{key:08x}': count for key, count in colors.items()}
            metadata['symmetric_rows'] = all(raw[y*256:(y+1)*256] == raw[(63-y)*256:(64-y)*256] for y in range(64))
            valid = metadata['output_sha256'] == EXPECTED
            # Container encoding only: the CPU does not calculate the fractal.
            def chunk(kind, data):
                return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
            png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 6, 0, 0, 0))
            png += chunk(b'IDAT', zlib.compress(b''.join(b'\0' + raw[y*256:(y+1)*256] for y in range(64))))
            (result / 'mandelbrot.png').write_bytes(png + chunk(b'IEND', b''))
    passed = run.returncode == 0 and metadata['stage_result'] == 0 and valid and not (result / 'crashlog.bin').exists()
    if args.render_queues:
        current_log = after[len(before):] if after.startswith(before) else after
        queue_evidence = all(message in current_log for message in (
            'render: TA barrier retired, queue idle for 50ms; no shader submitted',
            'render: fragment barrier retired, queue idle for 50ms; no shader submitted',
            'render: independent queue gate result=0',
            'render: removed context-6 and root-3 mappings after firmware stop',
        ))
        graph = result / 'render-graph.bin'
        metadata['render_queues_passed'] = queue_evidence and graph.exists() and graph.stat().st_size == 0x40000
        metadata['render_shaders_submitted'] = False
        passed = passed and metadata['render_queues_passed']
    if passed:
        unload = subprocess.run(['rmmod', 'g16_lab'], capture_output=True, text=True)
        metadata['rmmod_rc'] = unload.returncode
        (result / 'rmmod.txt').write_text(unload.stdout + unload.stderr)
        passed = unload.returncode == 0
    metadata['passed'] = passed
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))
    print('Results:', result)
    return 0 if passed else 1

if __name__ == '__main__':
    raise SystemExit(main())
