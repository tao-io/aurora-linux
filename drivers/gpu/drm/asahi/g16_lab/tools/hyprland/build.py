#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build native-architecture Mesa for Hyprland/Zink and the compositing probe."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import shlex
import subprocess

OPTIONS = [
    '-Dbuildtype=debugoptimized', '-Dvulkan-drivers=asahi', '-Dgallium-drivers=zink',
    '-Dtools=asahi', '-Dbuild-tests=false', '-Dplatforms=x11,wayland',
    '-Dglx=disabled', '-Degl=enabled', '-Dgbm=enabled', '-Dgles1=disabled',
    '-Dgles2=enabled', '-Dllvm=enabled', '-Dvideo-codecs=[]',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    mesa, build = args.mesa.resolve(), args.output.resolve()
    revision = subprocess.check_output(['git', '-C', str(mesa), 'rev-parse', 'HEAD'], text=True).strip()
    source_status = subprocess.check_output(['git', '-C', str(mesa), 'status', '--porcelain'], text=True)
    info = build / 'meson-info/meson-info.json'
    setup = ['meson', 'setup']
    if info.exists():
        if Path(json.loads(info.read_text())['directories']['source']).resolve() != mesa:
            parser.error('build directory belongs to another source checkout')
        setup += ['--reconfigure']
    build.mkdir(parents=True, exist_ok=True)
    with (build / 'hyprland-setup.log').open('w') as log:
        subprocess.run([*setup, str(build), str(mesa), *OPTIONS], stdout=log, stderr=subprocess.STDOUT, check=True)
    with (build / 'hyprland-build.log').open('w') as log:
        subprocess.run(['ninja', '-C', str(build), f'-j{args.jobs}'], stdout=log, stderr=subprocess.STDOUT, check=True)
    source = Path(__file__).with_name('composite.c')
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'egl', 'glesv2'], text=True))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', str(source),
                    '-o', str(build / 'hyprland-composite-probe'), '-lm', *flags], check=True)
    manifest = {'mesa_revision': revision, 'source_status': source_status,
                'architecture': platform.machine(), 'options': OPTIONS,
                'probe_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                'installed_system_files': False, 'scope': 'build only; no M4 execution claim'}
    (build / 'hyprland-build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
