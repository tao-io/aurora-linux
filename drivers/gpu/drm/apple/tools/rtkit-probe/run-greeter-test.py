#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guarded J713 SDDM cursor/partial-redraw check on native KMS."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guards', default='/root/g16-submit-test')
    parser.add_argument('--output', required=True, help='new evidence directory')
    args = parser.parse_args()
    sys.path.insert(0, args.guards)
    from experiment_watchdog import ExperimentWatchdog
    from experiment_thermals import ExperimentThermals

    out = Path(args.output)
    out.mkdir(parents=True)
    watchdog = ExperimentWatchdog()
    result = {'watchdog': watchdog.arm()}
    thermal = ExperimentThermals(out)
    try:
        thermal.start()
    except BaseException:
        watchdog.disarm()
        raise

    try:
        pid = subprocess.check_output(
            ['pgrep', '-u', 'sddm', '-x', 'Hyprland'], text=True).strip()
        env = dict(os.environ)
        for entry in Path('/proc', pid, 'environ').read_bytes().split(b'\0'):
            if entry.startswith(b'XDG_RUNTIME_DIR='):
                env['XDG_RUNTIME_DIR'] = entry.split(b'=', 1)[1].decode()
        socket = next(Path(env['XDG_RUNTIME_DIR']).glob('hypr/*/.socket.sock'))
        env['HYPRLAND_INSTANCE_SIGNATURE'] = socket.parent.name

        def ctl(*arguments):
            run = subprocess.run(
                ['runuser', '-u', 'sddm', '--', 'hyprctl', *arguments],
                env=env, text=True, capture_output=True, timeout=2)
            if run.returncode:
                raise RuntimeError(run.stdout + run.stderr)
            return run.stdout

        def move_cursor(x, y):
            ctl('eval', 'local c=hl.cursor or hl.dsp.cursor; '
                f'hl.dispatch(c.move({{x={x},y={y}}}))')

        original = json.loads(ctl('-j', 'cursorpos'))
        result['original_cursor'] = original
        paths = {p.resolve() for p in Path('/sys/kernel/debug/dri').glob('*/m4_status')}
        if len(paths) != 1:
            raise RuntimeError('Expected one native KMS device')
        status = paths.pop()
        result['frames'] = []

        for i in range(18):
            thermal.check()
            x = 280 + i * 36
            before = dict(line.split() for line in status.read_text().splitlines())
            move_cursor(x, 120)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                now = dict(line.split() for line in status.read_text().splitlines())
                if int(now['completed']) > int(before['completed']):
                    break
                time.sleep(0.01)
            else:
                raise RuntimeError('No cursor flip')
            if now['failed'] != '0':
                raise RuntimeError('KMS failed')
            time.sleep(0.06)
            with status.with_name('m4_scanout').open('rb') as snapshot:
                fields = struct.unpack('<8I', snapshot.read(32))
                magic, width, height, stride, fmt, fb, imported, size = fields
                if (magic, width, height, stride) != (0x4246344d, 2560, 1664, 10240):
                    raise RuntimeError('Unexpected snapshot')
                snapshot.seek(32 + 220 * stride)
                crop = snapshot.read(128 * stride)
            if len(crop) != 128 * stride:
                raise RuntimeError('Short scanout snapshot')
            (out / f'cursor-{i:02}.bgra').write_bytes(crop)
            # The qualified greeter's upper strip has only one bright object:
            # the white cursor. Old positions must be repainted on buffer reuse.
            white = [(n // 4) % 2560 for n in range(0, len(crop), 4)
                     if min(crop[n:n + 3]) > 220]
            result['frames'].append({
                'i': i, 'x': x, 'y': 120, 'framebuffer': fb, 'status': now,
                'cursor': json.loads(ctl('-j', 'cursorpos')),
                'white_pixels': len(white),
                'white_min_x': min(white) if white else None,
                'white_max_x': max(white) if white else None,
            })
            if (len(white) < 20 or abs(min(white) - 2 * x) > 2 or
                    max(white) > 2 * x + 64):
                raise RuntimeError('Cursor pixels missing, stale, or outside expected position')

        move_cursor(original['x'], original['y'])
        thermal.check()
        result['passed'] = True
    except BaseException as error:
        result['error'] = str(error)
        raise
    finally:
        thermal.close()
        result['thermal'] = thermal.summary()
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        os.sync()

    watchdog.disarm()
    print(json.dumps({'passed': True, 'frames': len(result['frames']),
                      'evidence': str(out), 'thermal': result['thermal']}, indent=2))


if __name__ == '__main__':
    main()
