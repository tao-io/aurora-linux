#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded real-Hyprland probe; invoke only inside the armed render runner."""
import json
import array
import hashlib
import mmap
import struct
import os
from pathlib import Path
import signal
import subprocess
import time


def verify_scanout(result, width, height):
    frames = list(Path('/sys/firmware/devicetree/base/chosen').glob('framebuffer@*'))
    if len(frames) != 1:
        raise RuntimeError('Expected one retained panel framebuffer')
    fb = frames[0]
    integer = lambda name: int.from_bytes((fb / name).read_bytes(), 'big')
    address, size = struct.unpack('>QQ', (fb / 'reg').read_bytes())
    stride = integer('stride')
    if ((fb / 'format').read_bytes() != b'x2r10g10b10\0' or
        integer('width') != width or integer('height') != height or
        stride != width * 4 or size != stride * height or size > 64 * 1024 * 1024):
        raise RuntimeError('Unexpected retained framebuffer layout')
    # Read only the DT-declared framebuffer, after the compositor presents.
    fd = os.open('/dev/mem', os.O_RDONLY | os.O_CLOEXEC)
    try:
        with mmap.mmap(fd, size, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ, offset=address) as mapping:
            raw = mapping[:]
    finally:
        os.close(fd)
    words = array.array('I', raw)
    red = sum(((pixel >> 20) & 1023) > 960 and
              ((pixel >> 10) & 1023) < 64 and (pixel & 1023) < 64 for pixel in words)
    evidence = {'address': hex(address), 'bytes': size, 'format': 'x2r10g10b10',
                'red_pixels': red, 'sha256': hashlib.sha256(raw).hexdigest()}
    (result / 'scanout.json').write_text(json.dumps(evidence, indent=2) + '\n')
    (result / 'scanout.raw').write_bytes(raw)
    if red < width * height // 2:
        raise RuntimeError(f'Panel scanout did not contain the red terminal: {red} pixels')
    return red


def main():
    result = Path(os.environ['M4_RENDER_OUTPUT'])
    width = int(os.environ.get('M4_HYPR_WIDTH', '256'))
    height = int(os.environ.get('M4_HYPR_HEIGHT', '256'))
    if not (1 <= width <= 2560 and 1 <= height <= 1600):
        raise ValueError('Probe size exceeds the J713 panel bounds')
    runtime = Path('/run/m4h')
    runtime.mkdir(mode=0o700, exist_ok=True)
    env = {**os.environ, 'XDG_RUNTIME_DIR': str(runtime),
           'AQ_HEADLESS_RENDER_NODE': '/dev/dri/renderD128',
           'HYPRLAND_NO_SD_NOTIFY': '1', 'AQ_TRACE': '1',
           'GBM_BACKENDS_PATH': '/root/m4mesa/lib/gbm'}
    physical = os.environ.get('M4_HYPR_PHYSICAL') == '1'
    output_name = 'Unknown-1' if physical else 'HEADLESS-1'
    env['M4_HYPR_OUTPUT'] = output_name
    if physical:
        env.pop('AQ_HEADLESS_RENDER_NODE', None)
        env['AQ_DRM_DEVICES'] = '/dev/dri/card0'
        env['AQ_USE_RENDER_NODE'] = '1'
    env.pop('WAYLAND_DISPLAY', None)
    env.pop('DISPLAY', None)
    env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
    config = Path(__file__).with_name('minimal.lua')
    command = ['Hyprland', '--i-am-really-stupid', '--config', str(config)]
    if physical:
        command = ['seatd-launch', '--', *command]
    child = None
    terminal = None
    with (result / 'hyprland.log').open('w') as log:
        try:
            if physical:
                subprocess.run(['systemctl', 'stop', 'sddm'], check=True, timeout=5)
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
            if not any(m.get('name') == output_name and m.get('width') == width
                       and m.get('height') == height for m in outputs):
                raise RuntimeError(f'Expected active {width}x{height} {output_name} output')
            terminal = subprocess.Popen(['foot', '--app-id=m4-gpu-check',
                '-o', 'colors.background=ff0000', '-o', 'font=monospace:size=10',
                '/bin/sh', '-c', 'printf "M4 GPU compositing\n"; sleep 8'],
                env=env, stdout=log, stderr=log, start_new_session=True)
            time.sleep(0.8)
            pixels = result / 'hyprland.ppm'
            subprocess.run(['grim', '-t', 'ppm', '-o', output_name, str(pixels)],
                           env=env, check=True, stdout=log, stderr=log, timeout=6)
            data = pixels.read_bytes()
            header, dimensions, maximum, rgb = data.split(b'\n', 3)
            image_width, image_height = map(int, dimensions.split())
            if (header, image_width, image_height, maximum) != (b'P6', width, height, b'255') or len(rgb) != width * height * 3:
                raise RuntimeError('Unexpected screenshot format')
            red = sum(rgb[i] > 240 and rgb[i+1] < 16 and rgb[i+2] < 16
                      for i in range(0, len(rgb), 3))
            if red < width * height // 2:
                raise RuntimeError(f'Expected a composed red terminal, found {red} red pixels')
            log.flush()
            text = (result / 'hyprland.log').read_text()
            if not any('Renderer:' in line and 'zink' in line.lower() and 'M4' in line
                       for line in text.splitlines()):
                raise RuntimeError('Hyprland did not identify the M4 Zink renderer')
            scanout_red = verify_scanout(result, width, height) if physical else None
            print(json.dumps({'passed': True, 'hyprland': True,
                              'scope': f'{width}x{height} {output_name} compositor',
                              'physical_display': physical,
                              'scanout_red_pixels_verified': scanout_red,
                              'red_pixels_verified': red, 'screenshot': pixels.name}))
        finally:
            for process in (terminal, child):
                if process is None:
                    continue
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                # seatd-launch can exit before Hyprland and Xwayland. Wait
                # for the group, not just the launcher, then reap descendants.
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    process.poll()
                    try:
                        os.killpg(process.pid, 0)
                    except ProcessLookupError:
                        break
                    time.sleep(0.05)
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=2)
            if physical:
                subprocess.run(['systemctl', 'start', 'sddm'], check=True, timeout=5)


if __name__ == '__main__':
    main()
