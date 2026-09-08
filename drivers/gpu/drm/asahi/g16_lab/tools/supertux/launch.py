#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the installed SuperTux inside the existing watchdog-supervised desktop."""
import argparse
import json
import os
from pathlib import Path
import pwd
import subprocess
import sys
import time

UNIT = 'm4-accelerated-hyprland.service'
GAME_UNIT = 'm4-supertux.service'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=3600)
    parser.add_argument('--stop', action='store_true')
    parser.add_argument('--level', help='optional level file for a gameplay check')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error('--seconds must be between 1 and 3600')
    if os.geteuid():
        os.execvp('sudo', ['sudo', '/usr/local/bin/m4-supertux', *sys.argv[1:]])
    if args.stop:
        subprocess.run(['systemctl', 'stop', GAME_UNIT], check=True)
        return

    for dependency in ('/usr/bin/supertux2',
                       '/opt/m4mesa/supertux/lib/libSDL2-2.0.so.0',
                       '/opt/m4mesa/lib/m4-supertux-glcheck.so'):
        if not Path(dependency).is_file():
            raise RuntimeError('Missing SuperTux dependency: ' + dependency)

    # The desktop runner owns the sole watchdog FD and renews it only after
    # real GPU pixel checks. Never create an independent heartbeat here.
    result = Path(Path('/run/g16-lab-firmware-used').read_text().strip())
    manifest = json.loads((result / 'manifest.json').read_text())
    guard = manifest['watchdog']
    if not guard['armed'] or guard['timeout_seconds'] != 60:
        raise RuntimeError('The 60-second GPU watchdog guard is not armed')
    health = result / 'interactive-health.jsonl'
    checks = health.read_text().splitlines()
    if not checks:
        raise RuntimeError('Wait for the accelerated desktop to finish its first GPU health check')
    last = json.loads(checks[-1])
    if time.time() - health.stat().st_mtime > 12 or not last.get('check', {}).get('passed'):
        raise RuntimeError('The accelerated desktop has no fresh successful GPU health check')
    supervisor = subprocess.check_output(
        ['systemctl', 'show', UNIT, '-p', 'MainPID', '--value'], text=True).strip()
    if not any(p.resolve() == Path('/dev/watchdog0')
               for p in Path('/proc', supervisor, 'fd').iterdir()):
        raise RuntimeError('The desktop supervisor does not own the watchdog')
    uid = int(Path('/sys/module/g16_lab/parameters/render_trusted_uid').read_text())
    if not uid:
        raise RuntimeError('The lab GPU has no trusted desktop user')
    account = pwd.getpwuid(uid)
    if subprocess.run(['systemctl', 'is-active', '--quiet', GAME_UNIT]).returncode == 0:
        raise RuntimeError('SuperTux is already running')
    # Keep saves and the restricted graphics settings separate from an ordinary
    # SuperTux installation. Do all writes to the user's files as that user.
    game_dir = Path(account.pw_dir, '.local/share/supertux2-m4')
    subprocess.run(['runuser', '-u', account.pw_name, '--', 'python3', '-c',
                    'from pathlib import Path; import re,sys\n'
                    'p=Path(sys.argv[1]); p.mkdir(parents=True,exist_ok=True)\n'
                    'p=p/"config"\n'
                    's=p.read_text() if p.exists() else "(supertux-config (video (fancy_gfx #f)))\\n"\n'
                    's=re.sub(r"\\(fancy_gfx\\s+#[tf]\\)", "(fancy_gfx #f)", s)\n'
                    'p.write_text(s)\n', str(game_dir)], check=True)
    runtime = Path('/run/user', str(uid))
    compositor = subprocess.check_output(['pgrep', '-u', str(uid), '-x', 'Hyprland'], text=True).split()
    if len(compositor) != 1:
        raise RuntimeError('Expected one accelerated Hyprland session')
    keys = {'LD_LIBRARY_PATH', '__EGL_VENDOR_LIBRARY_FILENAMES',
            'LIBGL_DRIVERS_PATH', 'GBM_BACKENDS_PATH', 'GALLIUM_DRIVER',
            'ASAHI_M4_LAB', 'ASAHI_M4_WC_ONLY', 'ASAHI_MESA_DEBUG', 'VK_DRIVER_FILES',
            'MESA_GLES_VERSION_OVERRIDE', 'MESA_LOADER_DRIVER_OVERRIDE',
            'LIBGL_ALWAYS_SOFTWARE'}
    raw = Path('/proc', compositor[0], 'environ').read_bytes().split(b'\0')
    env = {k.decode(): v.decode() for entry in raw if b'=' in entry
           for k, v in [entry.split(b'=', 1)] if k.decode() in keys}
    env.update(XDG_RUNTIME_DIR=str(runtime),
               WAYLAND_DISPLAY=next(p.name for p in runtime.glob('wayland-*') if p.is_socket()),
               SDL_VIDEODRIVER='wayland', SDL_APP_ID='m4-supertux',
               SDL_VIDEO_WAYLAND_WMCLASS='m4-supertux',
               LD_PRELOAD='/opt/m4mesa/lib/m4-supertux-glcheck.so')
    env['LD_LIBRARY_PATH'] = '/opt/m4mesa/supertux/lib:' + env.get('LD_LIBRARY_PATH', '')
    socket = next(runtime.glob('hypr/*/.socket.sock'))
    ctl_env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime),
                   HYPRLAND_INSTANCE_SIGNATURE=socket.parent.name)
    subprocess.run(['hyprctl', 'eval',
                    'hl.window_rule({match={class="m4-supertux"},float=true,'
                    'size={800,450},center=true,opacity="1 1"})'], env=ctl_env, check=True)
    command = ['systemd-run', '--unit=' + GAME_UNIT, '--collect', '--uid=' + account.pw_name,
               '--property=RuntimeMaxSec=' + str(args.seconds), '--property=TimeoutStopSec=3',
               '--property=BindsTo=' + UNIT, '--property=After=' + UNIT]
    for key, value in env.items():
        command += ['-E', key + '=' + value]
    command += ['/usr/bin/supertux2', '--renderer', 'opengl20', '--window',
                '--geometry', '800x450', '--userdir', str(game_dir),
                '--disable-sound', '--disable-music', '--show-fps']
    if args.level:
        command.append(str(Path(args.level).resolve(strict=True)))
    subprocess.run(command, check=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, StopIteration, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'SuperTux M4: {error}\nStart M4 Accelerated Hyprland before launching this game.', file=sys.stderr)
        sys.exit(1)
