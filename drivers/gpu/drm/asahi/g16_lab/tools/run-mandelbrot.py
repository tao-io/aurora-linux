#!/usr/bin/env python3
"""Run one J713 G16 dispatch after a fresh boot and preserve its evidence."""
import argparse
import collections
import datetime
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import zlib
from experiment_watchdog import ExperimentWatchdog

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
    parser.add_argument('--render-ta', action='store_true',
                        help='experimental captured vertex/tiling submission after queue checks')
    parser.add_argument('--render-loader-marker', action='store_true',
                        help='instrument the vertex state loader (implies --render-ta)')
    parser.add_argument('--firmware-ktrace', type=lambda value: int(value, 0),
                        choices=(0, 1, 2, 3), default=0,
                        help='25G83 firmware trace mask: 1 basic, 2 detail, 3 both')
    parser.add_argument('--render-fragment', action='store_true',
                        help='experimental fragment job after TA (implies --render-ta)')
    parser.add_argument('--render-generated-shaders', action='store_true',
                        help='generated vertex/red fragment bodies (implies --render-fragment)')
    parser.add_argument('--render-size', type=int, choices=(4, 32, 256), default=4,
                        help='square generated framebuffer size')
    parser.add_argument('--render-xy', action='store_true',
                        help='256x256 fragment-coordinate gradient; implies generated shaders')
    parser.add_argument('--render-triangle', action='store_true',
                        help='RGB coordinate-colored triangle; implies --render-xy')
    args = parser.parse_args()
    if args.render_triangle:
        args.render_xy = True
    if args.render_xy:
        args.render_generated_shaders = True
        args.render_size = 256
    if args.render_generated_shaders:
        args.render_fragment = True
        if args.render_loader_marker:
            parser.error('generated shaders and loader instrumentation are separate tests')
    elif args.render_size != 4:
        parser.error('larger targets require --render-generated-shaders')
    if args.render_fragment:
        args.render_ta = True
    if args.render_loader_marker:
        args.render_ta = True
    if args.render_ta:
        args.render_queues = True
    params = PARAMS + (['render_queue_probe=1'] if args.render_queues else [])
    if args.render_ta:
        params += ['render_ta_probe=1']
    if args.render_fragment:
        params += ['render_fragment_probe=1']
    if args.render_generated_shaders:
        params += ['render_generated_shaders=1', f'render_size={args.render_size}']
    if args.render_xy:
        params += ['render_xy=1']
    if args.render_triangle:
        params += ['render_triangle=1']
    if args.render_loader_marker:
        params += ['render_loader_marker=1']
    if args.firmware_ktrace:
        params += [f'firmware_ktrace_mask={args.firmware_ktrace}']
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
    result = args.results / (datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + ('-render-ta' if args.render_ta else '-render-queues' if args.render_queues else '-mandelbrot'))
    result.mkdir(parents=True, exist_ok=False)
    metadata = {
        'kernel': release,
        'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest(),
        'parameters': params,
    }
    os.sync()
    watchdog = ExperimentWatchdog()
    metadata['watchdog'] = watchdog.arm()
    marker.write_text(str(result) + '\n')
    with (result / 'manifest.json').open('w') as manifest:
        manifest.write(json.dumps(metadata, indent=2) + '\n')
        manifest.flush()
        os.fsync(manifest.fileno())
    run = subprocess.run(['insmod', str(args.module), *params], capture_output=True, text=True)
    (result / 'insmod.txt').write_text(run.stdout + run.stderr)
    after = output('dmesg')
    (result / 'dmesg.log').write_text(after)
    print(after[len(before):] if after.startswith(before) else after, end='')
    metadata['insmod_rc'] = run.returncode
    status = Path('/sys/module/g16_lab/parameters/stage_result')
    metadata['stage_result'] = int(status.read_text()) if status.exists() else None
    for name, dest in [('g16_lab_mandelbrot_rgba8', 'mandelbrot.rgba'), ('g16_lab_crashlog', 'crashlog.bin'),
                       ('g16_lab_render_prepared', 'render-graph.bin'),
                       ('g16_lab_ktrace', 'firmware-ktrace.bin')]:
        p = debug / name
        if p.exists():
            (result / dest).write_bytes(p.read_bytes())
    if args.firmware_ktrace:
        from g16ktrace import decode
        trace = result / 'firmware-ktrace.bin'
        metadata['firmware_ktrace_valid'] = False
        if trace.exists():
            try:
                decoded = decode(trace.read_bytes())
                (result / 'firmware-ktrace.json').write_text(json.dumps(decoded, indent=2) + '\n')
                metadata['firmware_ktrace'] = {key: value for key, value in decoded.items()
                                               if key != 'records'}
                summary = re.findall(r'KTrace captured=(\d+) host-dropped=(\d+) drain-result=(-?\d+)', after)
                if summary:
                    captured, dropped, drain = map(int, summary[-1])
                    metadata['firmware_ktrace']['host_dropped_records'] = dropped
                    metadata['firmware_ktrace']['drain_result'] = drain
                    metadata['firmware_ktrace_valid'] = (
                        captured == decoded['record_count'] and captured > 0
                        and dropped == 0 and drain == 0
                        and decoded['firmware_reported_lost_records'] == 0)
            except ValueError as error:
                metadata['firmware_ktrace_error'] = str(error)
    pixels = result / 'mandelbrot.rgba'
    if args.render_queues and (debug / 'g16_lab_render').is_dir():
        snapshots = result / 'render-resources'
        snapshots.mkdir()
        for source in (debug / 'g16_lab_render').glob('*.bin'):
            (snapshots / source.name).write_bytes(source.read_bytes())
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
    if args.firmware_ktrace:
        passed = passed and metadata['firmware_ktrace_valid']
    if args.render_queues:
        current_log = after[len(before):] if after.startswith(before) else after
        queue_evidence = all(message in current_log for message in (
            'render: TA barrier retired, queue idle for 50ms; no shader submitted',
            'render: fragment barrier retired, queue idle for 50ms; no shader submitted',
            'render: removed context-6 and root-3 mappings after firmware stop',
        ))
        graph = result / 'render-graph.bin'
        metadata['render_queues_passed'] = queue_evidence and graph.exists() and graph.stat().st_size == 0x80000
        metadata['render_shaders_submitted'] = 'render: submitted InitPB + captured vertex/TA job' in current_log
        if args.render_ta:
            metadata['ta_retired_with_writes'] = 'render: TA retired with GPU-written pool/tilemap' in current_log
            if not args.render_fragment:
                passed = passed and metadata['ta_retired_with_writes']
        metadata['fragment_job_submitted'] = 'render: submitted fragment diagnostic after retired TA' in current_log
        metadata['fragment_job_retired'] = 'render: fragment diagnostic retired stamp=0x100 ring=2' in current_log
        metadata['vertex_fragment_pixels_verified'] = False
        if args.render_fragment:
            metadata['generated_shader_bodies'] = args.render_generated_shaders
            metadata['render_size'] = args.render_size
            metadata['render_xy'] = args.render_xy
            metadata['render_triangle'] = args.render_triangle
            target_va = 0x10000058000 if args.render_size == 4 else 0x10001000000
            target_offset = 0x100 if args.render_size == 4 else 0x4000
            pixel_bytes = args.render_size * args.render_size * 4
            expected_bytes = 0x20000 if args.render_size == 4 else ((target_offset + pixel_bytes + 0x3fff) & ~0x3fff)
            target = result / 'render-resources' / f'resource-{target_va:x}.bin'
            if target.exists():
                raw_target = target.read_bytes()
                # Constant color is independent of the tiled pixel ordering.
                rgba = raw_target[target_offset:target_offset + pixel_bytes]
                (result / 'render-tiled.rgba').write_bytes(rgba)
                if args.render_xy:
                    from g16image import detile_rgba8
                    rgba = detile_rgba8(rgba, args.render_size)
                (result / 'render.rgba').write_bytes(rgba)
                metadata['render_target_sha256'] = hashlib.sha256(raw_target).hexdigest()
                metadata['render_rgba_sha256'] = hashlib.sha256(rgba).hexdigest()
                expected_rgba = bytes.fromhex('ff0000ff' if args.render_generated_shaders else '4080bfff')
                expected_image = expected_rgba * (pixel_bytes // 4)
                if args.render_xy:
                    expected_image = bytes(v for y in range(256) for x in range(256)
                                           for v in ((x * 255 + 128) // 256, (y * 255 + 128) // 256, 0, 255))
                if args.render_triangle:
                    expected_image = bytes(v for y in range(256) for x in range(256)
                                           for v in ((((y - x) * 255 + 128) // 256,
                                                      (x * 255 + 128) // 256,
                                                      ((256 - y) * 255 + 128) // 256, 255)
                                                     if x < y else (0, 0, 0, 0)))
                from g16image import write_png
                write_png(result / 'render.png', rgba, args.render_size)
                metadata['vertex_fragment_pixels_verified'] = (
                    len(raw_target) == expected_bytes and rgba == expected_image
                    and not any(raw_target[:target_offset])
                    and not any(raw_target[target_offset + pixel_bytes:]))
            # Pixel evidence replaces the earlier nonzero-tilemap heuristic.
            # A firmware crash or incomplete queue still fails the outer gate.
            passed = (passed and metadata['fragment_job_retired']
                      and metadata['vertex_fragment_pixels_verified'])
        passed = passed and metadata['render_queues_passed']
    if passed:
        unload = subprocess.run(['rmmod', 'g16_lab'], capture_output=True, text=True)
        metadata['rmmod_rc'] = unload.returncode
        (result / 'rmmod.txt').write_text(unload.stdout + unload.stderr)
        passed = unload.returncode == 0
    metadata['passed'] = passed
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    os.sync()
    watchdog.disarm()
    metadata['watchdog']['armed'] = False
    metadata['watchdog']['disarmed_after_evidence_collection'] = True
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))
    print('Results:', result)
    return 0 if passed else 1

if __name__ == '__main__':
    raise SystemExit(main())
