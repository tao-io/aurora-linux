#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run one J713 G16 dispatch after a fresh boot and preserve its evidence."""
import argparse
import collections
import datetime
import errno
import time
import hashlib
import json
import os
import re
import pwd
from pathlib import Path
import struct
import subprocess
import zlib
from experiment_watchdog import ExperimentWatchdog
from experiment_thermals import ExperimentThermals

EXPECTED = '792314d0bce27c82b5777d559020eba427b687d8fc4d6b1c031d9c915fc7ffc1'
ACTIVE_RESULT = None
DMESG_READER = None
THERMALS = None
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


def captured_dmesg():
    # A compositor can wrap printk's ring before the end of the experiment.
    # Retain a continuously drained copy rather than infer missing events.
    if DMESG_READER is not None:
        if DMESG_READER.poll() is not None:
            raise RuntimeError('continuous kernel log reader exited')
        time.sleep(0.05)
        return (ACTIVE_RESULT / 'dmesg-live.log').read_text()
    return output('dmesg')


def run_interactive_client(client, env, result, watchdog, seconds):
    import selectors
    summary = None
    checks = 0
    deadline = time.monotonic() + seconds + 25
    progress_deadline = time.monotonic() + 30
    with (result / 'interactive.stderr').open('w') as errors, (result / 'interactive-health.jsonl').open('w') as history:
        process = subprocess.Popen(client, env=env, stdout=subprocess.PIPE,
                                   stderr=errors, text=True, bufsize=1)
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        try:
            while True:
                THERMALS.check()
                now = time.monotonic()
                if now > deadline or now > progress_deadline:
                    raise RuntimeError('interactive session stopped providing verified GPU progress')
                if not selector.select(timeout=1):
                    if process.poll() is not None:
                        break
                    continue
                line = process.stdout.readline()
                if not line:
                    break
                record = json.loads(line)
                history.write(json.dumps(record) + '\n')
                history.flush()
                os.fsync(history.fileno())
                if record.get('event') == 'gpu-progress':
                    THERMALS.check()
                    watchdog.keepalive_after_gpu_check(record['check'])
                    checks += 1
                    progress_deadline = time.monotonic() + 30
                elif record.get('event') == 'ready':
                    print('Interactive M4 Hyprland ready: ' + json.dumps(record['result']), flush=True)
                elif record.get('passed') is True:
                    summary = record
                else:
                    raise RuntimeError('unrecognized interactive supervisor record')
            rc = process.wait(timeout=5)
            if rc or summary is None or not checks:
                raise RuntimeError(f'interactive client failed: exit={rc}, checks={checks}')
            summary['verified_watchdog_keepalives'] = checks
            return subprocess.CompletedProcess(client, rc, json.dumps(summary), '')
        finally:
            selector.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
            process.stdout.close()


def main():
    global ACTIVE_RESULT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', type=Path, default=Path('/root/g16_lab.ko'))
    parser.add_argument('--results', type=Path, default=Path('/root/g16-results'))
    parser.add_argument('--render-queues', action='store_true',
                        help='after Mandelbrot, test TA/fragment queue barriers (no render shaders)')
    parser.add_argument('--render-ta', action='store_true',
                        help='experimental captured vertex/tiling submission after queue checks')
    parser.add_argument('--render-loader-marker', action='store_true',
                        help='instrument the vertex state loader (implies --render-ta)')
    parser.add_argument('--render-full-tlbi', action='store_true',
                        help='diagnose shared-context translation with a full outer-shareable TLBI')
    parser.add_argument('--render-rebuild-job-tables', action='store_true',
                        help='diagnose the VM cache with the original per-job mapping builder')
    parser.add_argument('--render-submit-delay-us', type=int, choices=range(5001),
                        metavar='0..5000', default=0,
                        help='diagnose submission timing with a bounded pre-publication delay')
    parser.add_argument('--render-power-controller', action='store_true',
                        help='initialize the qualified J713 power-controller data for the render experiment')
    parser.add_argument('--firmware-ktrace', type=lambda value: int(value, 0),
                        choices=(0, 1, 2, 3, 0x11, 0x31), default=0,
                        help='25G83 firmware trace mask: 1 basic, 2 detail, 3 both, 0x11/0x31 basic + power')
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
    parser.add_argument('--render-frames', type=int, choices=range(1, 9), default=1,
                        help='sequential frames with alternating geometry; requires triangle')
    parser.add_argument('--render-bindings', action='store_true',
                        help='generated relocated loaders and vertex bindings; implies triangle')
    parser.add_argument('--render-stream', action='store_true',
                        help='generated relocated direct draws; implies generated bindings')
    parser.add_argument('--render-varying', action='store_true',
                        help='experimental linear vertex color; implies generated stream')
    parser.add_argument('--render-drm-target', action='store_true',
                        help='fenced GEM target read by a separate unprivileged process')
    parser.add_argument('--render-drm-submit', action='store_true',
                        help='Asahi command submissions using client-created GEM resources')
    parser.add_argument('--interactive-seconds', type=int, default=0,
                        help='30..3600 seconds of interactive Hyprland with GPU-verified watchdog progress')
    parser.add_argument('--desktop-user', help='explicit trusted local user for the supervised physical desktop')
    parser.add_argument('--drm-frame-budget', type=int, choices=range(8, 129), default=8)
    parser.add_argument('--drm-client-mode', choices=('legacy', 'composite', 'local-composite'), default='legacy')
    parser.add_argument('--drm-submit-test', type=Path, default=Path('/root/g16submittest'))
    parser.add_argument('--mesa-device-probe', type=Path,
                        help='optional Vulkan device/queue initialization probe before the client draws')
    parser.add_argument('--drm-target-test', type=Path, default=Path('/root/g16targettest'))
    parser.add_argument('--render-service', action='store_true',
                        help='request eight RGB frames from a retained firmware worker')
    parser.add_argument('--render-mesa-fragment', action='store_true',
                        help='Mesa NIR-compiled fragment body; implies RGB')
    parser.add_argument('--render-mesa-vertex', action='store_true',
                        help='Mesa NIR-compiled vertex body; implies RGB')
    parser.add_argument('--render-varying-rgb', action='store_true',
                        help='three linear vertex color components')
    parser.add_argument('--render-varying-uniform', action='store_true',
                        help='equal vertex colors to isolate varying transport')
    parser.add_argument('--render-varying-rhw', action='store_true',
                        help='interpolate reciprocal W to isolate coefficient setup')
    args = parser.parse_args()
    desktop_account = None
    if args.desktop_user:
        desktop_account = pwd.getpwnam(args.desktop_user)
        if not args.interactive_seconds or not desktop_account.pw_uid:
            parser.error('--desktop-user requires an interactive session and a non-root account')
    if args.interactive_seconds:
        if not 30 <= args.interactive_seconds <= 3600 or not args.render_drm_submit or args.drm_client_mode != 'composite':
            parser.error('interactive mode requires DRM composite and 30..3600 seconds')
        if args.firmware_ktrace and args.interactive_seconds > 120:
            parser.error('interactive KTrace diagnostics are limited to 120 seconds')
        args.drm_frame_budget = 65535
    if args.render_drm_submit and args.render_drm_target:
        parser.error('client submission and lab target export are separate tests')
    if args.mesa_device_probe and not args.render_drm_submit:
        parser.error('--mesa-device-probe requires --render-drm-submit')
    if args.render_drm_target or args.render_drm_submit:
        args.render_service = True
    if args.render_service:
        args.render_mesa_fragment = args.render_mesa_vertex = True
        args.render_frames = args.drm_frame_budget if args.render_drm_submit else 8
    if args.render_mesa_fragment or args.render_mesa_vertex:
        args.render_varying_rgb = True
    if args.render_varying_rgb and (args.render_varying_uniform or args.render_varying_rhw):
        parser.error('RGB and scalar varying diagnostics are separate tests')
    if args.render_varying_rgb or args.render_varying_uniform or args.render_varying_rhw:
        args.render_varying = True
    if args.render_varying:
        args.render_stream = True
    if args.render_stream:
        args.render_bindings = True
    if args.render_bindings:
        args.render_triangle = True
    if args.render_frames > 1 and not args.render_triangle:
        parser.error('--render-frames requires --render-triangle')
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
    params = PARAMS + ([f'render_service_seconds={args.interactive_seconds + 20}'] if args.interactive_seconds else []) + (['render_queue_probe=1'] if args.render_queues else [])
    if args.render_ta:
        params += ['render_ta_probe=1']
    if args.render_fragment:
        params += ['render_fragment_probe=1']
    if args.render_generated_shaders:
        params += ['render_generated_shaders=1', f'render_size={args.render_size}']
    if args.render_xy:
        params += ['render_xy=1']
    if args.render_bindings:
        params += ['render_bindings=1']
    if args.render_stream:
        params += ['render_stream=1']
    if args.render_varying:
        params += ['render_varying=1']
    if args.render_service:
        params += ['render_service=1']
    if args.render_drm_target:
        params += ['render_drm_target=1']
    if args.render_drm_submit:
        params += ['render_drm_submit=1']
    if desktop_account:
        params += [f'render_trusted_uid={desktop_account.pw_uid}']
    if args.render_mesa_fragment:
        params += ['render_mesa_fragment=1']
    if args.render_mesa_vertex:
        params += ['render_mesa_vertex=1']
    if args.render_varying_rgb:
        params += ['render_varying_rgb=1']
    if args.render_varying_uniform:
        params += ['render_varying_uniform=1']
    if args.render_varying_rhw:
        params += ['render_varying_rhw=1']
    if args.render_triangle:
        params += ['render_triangle=1', f'render_frames={args.render_frames}']
    if args.render_loader_marker:
        params += ['render_loader_marker=1']
    if args.render_full_tlbi:
        params += ['render_full_tlbi=1']
    if args.render_rebuild_job_tables:
        params += ['render_rebuild_job_tables=1']
    if args.render_submit_delay_us:
        params += [f'render_submit_delay_us={args.render_submit_delay_us}']
    if args.render_power_controller:
        params += ['render_power_controller=1']
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
    result = args.results / (datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + ('-render-ta' if args.render_ta else '-render-queues' if args.render_queues else '-mandelbrot'))
    result.mkdir(parents=True, exist_ok=False)
    ACTIVE_RESULT = result
    global DMESG_READER
    with (result / 'dmesg-live.log').open('w') as stream:
        DMESG_READER = subprocess.Popen(['stdbuf', '-oL', 'dmesg', '--follow'], stdout=stream)
    before = captured_dmesg()
    initial_render_nodes = set(Path('/dev/dri').glob('renderD*'))
    marker = Path('/run/g16-lab-firmware-used')
    if marker.exists() or 'GFX RTKit' in before or Path('/sys/module/g16_lab').exists():
        parser.error('GPU firmware already used this boot; reboot before a new dispatch')
    debug = Path('/sys/kernel/debug')
    if subprocess.run(['mountpoint', '-q', str(debug)]).returncode:
        subprocess.run(['mount', '-t', 'debugfs', 'debugfs', str(debug)], check=True)
    metadata = {
        'kernel': release,
        'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest(),
        'parameters': params,
    }
    if args.render_drm_submit:
        metadata['drm_submit_test_sha256'] = hashlib.sha256(args.drm_submit_test.read_bytes()).hexdigest()
    if args.mesa_device_probe:
        metadata['mesa_probe_sha256'] = hashlib.sha256(args.mesa_device_probe.read_bytes()).hexdigest()
        metadata['mesa_environment'] = {name: os.environ.get(name) for name in
                                        ('ASAHI_M4_LAB', 'VK_DRIVER_FILES', 'LD_LIBRARY_PATH')}
    global THERMALS
    THERMALS = ExperimentThermals(result)
    metadata['thermals'] = THERMALS.start()
    os.sync()
    watchdog = ExperimentWatchdog()
    metadata['watchdog'] = watchdog.arm()
    marker.write_text(str(result) + '\n')
    with (result / 'manifest.json').open('w') as manifest:
        manifest.write(json.dumps(metadata, indent=2) + '\n')
        manifest.flush()
        os.fsync(manifest.fileno())
    run = subprocess.run(['insmod', str(args.module), *params], capture_output=True, text=True)
    THERMALS.check()
    (result / 'insmod.txt').write_text(run.stdout + run.stderr)
    if args.render_service and run.returncode == 0:
        service = debug / 'g16_lab_render_submit'
        deadline = time.monotonic() + 25
        events = []

        def status():
            return {k: int(v) for k, v in (word.split('=') for word in service.read_text().split())}

        while not status()['ready']:
            if status()['stopped'] or time.monotonic() >= deadline:
                raise RuntimeError('render service failed before becoming ready')
            time.sleep(0.02)
        initial = status()
        time.sleep(0.5)
        idle = status()
        if initial['completed'] or idle['completed'] or idle['pending']:
            raise RuntimeError('GPU rendered without a userspace request')
        for payload in (b'0\n', b'2\n', b'bad\n', b'9\n'):
            try:
                with service.open('wb', buffering=0) as control:
                    control.write(payload)
            except OSError as error:
                if error.errno != errno.EINVAL:
                    raise
            else:
                raise RuntimeError('service accepted an invalid sequence')
        if args.render_drm_submit:
            if args.mesa_device_probe:
                probe = subprocess.run([str(args.mesa_device_probe)], capture_output=True,
                                       text=True, timeout=5)
                (result / 'mesa-device.stdout').write_text(probe.stdout)
                (result / 'mesa-device.stderr').write_text(probe.stderr)
                metadata['mesa_probe_returncode'] = probe.returncode
                if probe.returncode == 0:
                    # Compiler diagnostics may precede the probe's final JSON.
                    metadata['mesa_device'] = json.loads(probe.stdout.strip().splitlines()[-1])
            nodes = list(Path('/dev/dri').glob('renderD*'))
            if len(nodes) != 1:
                raise RuntimeError('expected one lab render node')
            if desktop_account:
                subprocess.run(['setfacl', '-m', f'u:{desktop_account.pw_uid}:rw', str(nodes[0])], check=True)
            client = [str(args.drm_submit_test)]
            if args.drm_client_mode == 'legacy':
                client += [str(nodes[0]), str(debug / 'g16_lab_render'), str(result)]
            elif args.drm_client_mode == 'local-composite':
                client += ['--local-textures']
            client_env = {**os.environ, 'M4_RENDER_OUTPUT': str(result),
                          'M4_HYPR_INTERACTIVE_SECONDS': str(args.interactive_seconds)}
            if desktop_account:
                client_env['M4_HYPR_USER'] = desktop_account.pw_name
            if args.interactive_seconds:
                target = run_interactive_client(client, client_env, result, watchdog, args.interactive_seconds)
                metadata['watchdog']['progress_verified_keepalives'] = json.loads(target.stdout)['verified_watchdog_keepalives']
            else:
                target = subprocess.run(client, capture_output=True, text=True, timeout=20, env=client_env)
            (result / 'drm-submit.stdout').write_text(target.stdout)
            (result / 'drm-submit.stderr').write_text(target.stderr)
            target.check_returncode()
            metadata['drm_submit'] = json.loads(target.stdout)
            if not metadata['drm_submit'].get('passed'):
                raise RuntimeError('DRM submit check failed')
            if args.drm_frame_budget != 8:
                with service.open('wb', buffering=0) as control:
                    control.write(b'stop\n')
        elif args.render_drm_target:
            nodes = list(Path('/dev/dri').glob('renderD*'))
            if len(nodes) != 1:
                raise RuntimeError('expected one lab render node')
            target = subprocess.run([str(args.drm_target_test), str(nodes[0]), str(service)],
                                    capture_output=True, text=True, timeout=15)
            (result / 'drm-target.stdout').write_text(target.stdout)
            (result / 'drm-target.stderr').write_text(target.stderr)
            target.check_returncode()
            metadata['drm_target'] = json.loads(target.stdout)
            if not metadata['drm_target'].get('passed'):
                raise RuntimeError('DRM target check failed')
        else:
            for sequence in range(1, 9):
                start = time.monotonic()
                with service.open('wb', buffering=0) as control:
                    control.write(f'{sequence}\n'.encode())
                current = status()
                if current['completed'] != sequence or current['result'] != 0:
                    raise RuntimeError(f'render service failed: {current}')
                events.append({'sequence': sequence, 'seconds': time.monotonic() - start,
                               'status': current})
                if sequence < 8:
                    time.sleep(0.2)
        # Shutdown gets its own bound; the startup deadline has elapsed
        # after a healthy interactive session lasting minutes.
        deadline = time.monotonic() + 5
        while not status()['stopped']:
            if time.monotonic() >= deadline:
                raise RuntimeError('render worker did not stop')
            time.sleep(0.02)
        metadata['render_service'] = {'idle_verified': True, 'invalid_requests_rejected': 4,
                                      'events': events, 'final_status': status()}
        (result / 'render-service.json').write_text(json.dumps(metadata['render_service'], indent=2) + '\n')
    after = captured_dmesg()
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
        metadata['render_queues_passed'] = queue_evidence and graph.exists() and graph.stat().st_size == 0xf0000
        metadata['render_shaders_submitted'] = 'render: submitted InitPB + captured vertex/TA job' in current_log
        if args.render_ta:
            metadata['ta_retired_with_writes'] = 'render: TA retired with GPU-written pool/tilemap' in current_log
            if not args.render_fragment:
                passed = passed and metadata['ta_retired_with_writes']
        metadata['fragment_job_submitted'] = 'render: submitted fragment diagnostic after retired TA' in current_log
        metadata['fragment_job_retired'] = 'render: fragment diagnostic retired stamp=0x100 ring=2' in current_log
        metadata['vertex_fragment_pixels_verified'] = False
        if args.render_fragment and not args.render_drm_submit:
            metadata['generated_shader_bodies'] = args.render_generated_shaders
            metadata['render_size'] = args.render_size
            metadata['render_xy'] = args.render_xy
            metadata['render_triangle'] = args.render_triangle
            metadata['render_frames'] = args.render_frames
            metadata['generated_bindings'] = args.render_bindings
            metadata['generated_stream'] = args.render_stream
            metadata['linear_vertex_varying'] = args.render_varying
            metadata['rgb_vertex_varying'] = args.render_varying_rgb
            metadata['mesa_nir_fragment'] = args.render_mesa_fragment
            metadata['mesa_nir_vertex'] = args.render_mesa_vertex
            metadata['uniform_vertex_colors'] = args.render_varying_uniform
            metadata['coefficient_source_rhw'] = args.render_varying_rhw
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
                    from g16image import triangle_reference
                    expected_image = triangle_reference(bool((args.render_frames - 1) & 1))
                if args.render_varying:
                    from g16image import linear_varying_reference
                    expected_image = linear_varying_reference(bool((args.render_frames - 1) & 1), args.render_varying_uniform)
                    if args.render_varying_rhw:
                        expected_image = linear_varying_reference(False, True)
                if args.render_varying_rgb:
                    from g16image import rgb_varying_reference
                    expected_image = rgb_varying_reference(bool((args.render_frames - 1) & 1))
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
        if args.render_bindings and not args.render_drm_submit:
            old_ranges = ((0x10000148000, 0, 164), (0x10000158000, 0x640, 84),
                          (0x10000128000, 0xa0, 8), (0x10000090000, 0, 24))
            erased = True
            for va, offset, count in old_ranges:
                path = result / 'render-resources' / f'resource-{va:x}.bin'
                erased = erased and path.exists()
                if path.exists():
                    data = path.read_bytes()
                    erased = erased and len(data) >= offset + count and not any(data[offset:offset + count])
            metadata['old_bindings_erased'] = erased
            passed = passed and erased and 'generated relocated loaders and vertex bindings; old entries erased' in current_log
        if args.render_stream and not args.render_drm_submit:
            stream_file = result / 'render-resources/resource-1000018000.bin'
            vertices_file = result / 'render-resources/resource-10000148000.bin'
            valid_stream = False
            if stream_file.exists() and vertices_file.exists():
                stream, vertices = stream_file.read_bytes(), vertices_file.read_bytes()
                expected_vertices = struct.pack('<12f', -1, -1, 1, -1, -1, 1, -1, -1, 3, -1, -1, 3)
                first = ((args.render_frames - 1) & 1) * 3
                if args.render_varying:
                    flip = bool((args.render_frames - 1) & 1)
                    edge = float(not flip) if args.render_varying_uniform else float(flip)
                    expected_vertices = struct.pack('<12f', -1, -1, edge, 0,
                        1, -1, float(not flip), 0, -1, 1, edge, 0)
                    if args.render_varying_rgb:
                        colors = ((0,1), (0,0), (1,0)) if flip else ((1,0), (0,1), (0,0))
                        expected_vertices = struct.pack('<12f',
                            *(v for xy, rg in zip(((-1,-1), (1,-1), (-1,1)), colors) for v in (*xy, *rg)))
                    first = 0
                valid_stream = (len(stream) >= 0x4078 and len(vertices) >= 0x5130
                                and not any(stream[:120])
                                and struct.unpack_from('<I', stream, 0x4070)[0] == first
                                and vertices[0x5100:0x5130] == expected_vertices)
            metadata['old_stream_erased_and_inputs_verified'] = valid_stream
            passed = passed and valid_stream
        if args.render_varying and not args.render_drm_submit:
            trace_path = result / 'render-resources/resource-100000a8000.bin'
            reads = None
            if trace_path.exists() and trace_path.stat().st_size >= 0x100c:
                reads = list(struct.unpack_from('<3I', trace_path.read_bytes(), 0x1000))
            flip = bool((args.render_frames - 1) & 1)
            wanted = [0x3f800000 if ((args.render_varying_uniform or i == 1) != flip) else 0
                      for i in range(3)]
            if args.render_varying_rgb:
                wanted = [0, 0, 0x3f800000] if flip else [0x3f800000, 0, 0]
            metadata['vertex_attribute_readback'] = reads
            metadata['vertex_attribute_reads_verified'] = reads == wanted
            if args.render_mesa_vertex:
                # Compiler output has no diagnostic store. Require the initial
                # sentinel so an earlier traced shader cannot satisfy this test.
                metadata['vertex_attribute_reads_verified'] = None
                metadata['vertex_trace_absent'] = reads == [0xcdcdcdcd] * 3
                passed = passed and metadata['vertex_trace_absent']
            else:
                passed = passed and reads == wanted
            if not args.render_varying_rgb and trace_path.exists() and trace_path.stat().st_size >= 0x2400:
                coefficients = list(struct.unpack_from('<256I', trace_path.read_bytes(), 0x2000))
                metadata['fragment_coefficient_readback'] = coefficients
        if args.render_triangle and not args.render_drm_submit:
            from g16image import detile_rgba8, triangle_reference, linear_varying_reference, rgb_varying_reference, write_png
            frames = []
            for frame in range(args.render_frames):
                source = result / 'render-resources' / f'frame-{frame:02d}.bin'
                info = {'frame': frame, 'verified': False}
                if source.exists() and source.stat().st_size == 256 * 256 * 4:
                    rgba = detile_rgba8(source.read_bytes(), 256)
                    info['sha256'] = hashlib.sha256(rgba).hexdigest()
                    reference = (lambda flip: linear_varying_reference(flip, args.render_varying_uniform)) if args.render_varying else triangle_reference
                    if args.render_varying_rhw:
                        reference = lambda flip: linear_varying_reference(False, True)
                    if args.render_varying_rgb:
                        reference = rgb_varying_reference
                    info['verified'] = (rgba == reference(bool(frame & 1)) and
                        f'render: verified frame={frame} pixels=65536 stamp={((frame + 1) << 8):#x}' in current_log)
                    write_png(result / f'frame-{frame:02d}.png', rgba, 256)
                frames.append(info)
            metadata['frames'] = frames
            passed = passed and all(frame['verified'] for frame in frames)
        if args.render_drm_submit:
            metadata['vertex_fragment_pixels_verified'] = metadata.get('drm_submit', {}).get('passed', False)
            metadata['client_vm_publications'] = current_log.count('DRM submit: publishing ')
            # Successful publication logs are sampled during long sessions.
            # The stopped worker's completion counter is the job authority.
            metadata['client_jobs_completed'] = metadata['render_service']['final_status']['completed']
            passed = (passed and metadata['vertex_fragment_pixels_verified']
                      and (metadata['client_jobs_completed'] == 8 if args.drm_frame_budget == 8
                           else 0 < metadata['client_jobs_completed'] <= args.drm_frame_budget)
                      and metadata['render_service']['final_status']['result'] == 0
                      and metadata['fragment_job_retired'])
        passed = passed and metadata['render_queues_passed']
    if passed:
        unload = subprocess.run(['rmmod', 'g16_lab'], capture_output=True, text=True)
        metadata['rmmod_rc'] = unload.returncode
        (result / 'rmmod.txt').write_text(unload.stdout + unload.stderr)
        passed = unload.returncode == 0
    final_log = captured_dmesg()
    (result / 'dmesg-final.log').write_text(final_log)
    new_log = final_log[len(before):] if final_log.startswith(before) else final_log
    metadata['new_kernel_fault'] = any(marker in new_log for marker in
                                        ('WARNING:', 'BUG:', 'Oops:', 'Unable to handle kernel'))
    passed = passed and not metadata['new_kernel_fault']
    if args.render_drm_target or args.render_drm_submit:
        metadata['render_node_removed'] = set(Path('/dev/dri').glob('renderD*')) == initial_render_nodes
        passed = passed and metadata['render_node_removed']
    THERMALS.check()
    metadata['thermals'] = THERMALS.summary()
    metadata['hardware_regression_passed'] = passed
    passed = passed and metadata.get('mesa_probe_returncode', 0) == 0
    metadata['passed'] = passed
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    os.sync()
    if Path('/sys/module/g16_lab').exists():
        raise RuntimeError('module still loaded after experiment; leaving watchdog armed')
    watchdog.disarm()
    THERMALS.close()
    metadata['watchdog']['armed'] = False
    metadata['watchdog']['disarmed_after_evidence_collection'] = True
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))
    print('Results:', result)
    return 0 if passed else 1

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        # A failed client must not skip diagnostics before watchdog recovery.
        # Do not unload a possibly active GPU or disarm the watchdog here.
        if ACTIVE_RESULT is not None:
            if THERMALS is not None:
                (ACTIVE_RESULT / 'thermal-summary.json').write_text(json.dumps(THERMALS.summary(), indent=2) + '\n')
            (ACTIVE_RESULT / 'exception.txt').write_text(repr(error) + '\n')
            (ACTIVE_RESULT / 'dmesg-failure.log').write_text(captured_dmesg())
            for name in ('g16_lab_crashlog', 'g16_lab_ktrace', 'g16_lab_render_prepared'):
                source = Path('/sys/kernel/debug') / name
                if source.exists():
                    (ACTIVE_RESULT / (name + '-failure.bin')).write_bytes(source.read_bytes())
            os.sync()
        raise
    finally:
        if THERMALS is not None:
            THERMALS.close()
        if DMESG_READER is not None and DMESG_READER.poll() is None:
            DMESG_READER.terminate()
            DMESG_READER.wait(timeout=2)
