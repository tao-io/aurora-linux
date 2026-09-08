#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded real-Hyprland probe; invoke only inside the armed render runner."""
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    result = Path(os.environ['M4_RENDER_OUTPUT'])
    runtime = Path('/run/m4h')
    runtime.mkdir(mode=0o700, exist_ok=True)
    env = {**os.environ, 'XDG_RUNTIME_DIR': str(runtime),
           'AQ_HEADLESS_RENDER_NODE': '/dev/dri/renderD128',
           'HYPRLAND_NO_SD_NOTIFY': '1', 'AQ_TRACE': '1',
           'GBM_BACKENDS_PATH': '/root/m4mesa/lib/gbm'}
    env.pop('WAYLAND_DISPLAY', None)
    env.pop('DISPLAY', None)
    env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
    config = Path(__file__).with_name('minimal.lua')
    command = ['Hyprland', '--i-am-really-stupid', '--config', str(config)]
    child = None
    terminal = None
    with (result / 'hyprland.log').open('w') as log:
        try:
            child = subprocess.Popen(command, env=env, stdout=log, stderr=log,
                                     start_new_session=True)
            deadline = time.monotonic() + 7
            instances = []
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    raise RuntimeError(f'Hyprland exited during startup: {child.returncode}')
                instances = list((runtime / 'hypr').glob('*/.socket.sock'))
                sockets = [p for p in runtime.glob('wayland-*') if not p.name.endswith('.lock')]
                if instances and sockets:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError('Hyprland did not create its sockets within seven seconds')
            env['HYPRLAND_INSTANCE_SIGNATURE'] = instances[-1].parent.name
            env['WAYLAND_DISPLAY'] = sockets[0].name
            monitors = subprocess.check_output(['hyprctl', '-j', 'monitors'], env=env,
                                               text=True, timeout=2)
            (result / 'hyprland-monitors.json').write_text(monitors)
            outputs = json.loads(monitors)
            if not any(m.get('name') == 'HEADLESS-1' and m.get('width') == 256
                       and m.get('height') == 256 for m in outputs):
                raise RuntimeError('Expected active 256x256 headless output')
            terminal = subprocess.Popen(['foot', '--app-id=m4-gpu-check',
                '-o', 'colors.background=ff0000', '-o', 'font=monospace:size=10',
                '/bin/sh', '-c', 'printf "M4 GPU compositing\n"; sleep 8'],
                env=env, stdout=log, stderr=log, start_new_session=True)
            time.sleep(0.8)
            pixels = result / 'hyprland.ppm'
            subprocess.run(['grim', '-t', 'ppm', '-o', 'HEADLESS-1', str(pixels)],
                           env=env, check=True, stdout=log, stderr=log, timeout=3)
            data = pixels.read_bytes()
            header, dimensions, maximum, rgb = data.split(b'\n', 3)
            width, height = dimensions.split()
            if (header, width, height, maximum) != (b'P6', b'256', b'256', b'255') or len(rgb) != 256 * 256 * 3:
                raise RuntimeError('Unexpected screenshot format')
            red = sum(rgb[i] > 240 and rgb[i+1] < 16 and rgb[i+2] < 16
                      for i in range(0, len(rgb), 3))
            if red < 256 * 256 // 2:
                raise RuntimeError(f'Expected a composed red terminal, found {red} red pixels')
            log.flush()
            text = (result / 'hyprland.log').read_text()
            if not any('Renderer:' in line and 'zink' in line.lower() and 'M4' in line
                       for line in text.splitlines()):
                raise RuntimeError('Hyprland did not identify the M4 Zink renderer')
            print(json.dumps({'passed': True, 'hyprland': True,
                              'scope': '256x256 headless compositor',
                              'red_pixels_verified': red, 'screenshot': pixels.name}))
        finally:
            for process in (terminal, child):
                if process is None or process.poll() is not None:
                    continue
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=1)


if __name__ == '__main__':
    main()
