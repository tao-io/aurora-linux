#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the Zink compositing gate, with a mandatory watchdog on J713."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from experiment_watchdog import ExperimentWatchdog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa-build', required=True, type=Path)
    parser.add_argument('--results', required=True, type=Path)
    parser.add_argument('--icd', type=Path, help='explicit Vulkan ICD manifest')
    parser.add_argument('--development-gpu', action='store_true')
    args = parser.parse_args()
    compatible = Path('/sys/firmware/devicetree/base/compatible')
    m4 = compatible.exists() and b'apple,j713' in compatible.read_bytes().split(b'\0')
    if not m4 and not args.development_gpu:
        parser.error('non-J713 hosts require --development-gpu; results are not M4 validation')
    if m4 and os.geteuid():
        parser.error('J713 tests require root to arm the hardware watchdog')
    build = args.mesa_build.resolve()
    exe = build / 'hyprland-composite-probe'
    if not exe.is_file() or not (build / 'hyprland-build.json').is_file():
        parser.error('run build.py first')
    command = ['meson', 'devenv', '-C', str(build), 'env',
               'MESA_LOADER_DRIVER_OVERRIDE=zink', 'GALLIUM_DRIVER=zink']
    if args.icd:
        if not args.icd.is_file():
            parser.error('ICD manifest does not exist')
        command.append('VK_DRIVER_FILES=' + str(args.icd.resolve()))
    command.append(str(exe))
    if args.development_gpu:
        command.append('--development-gpu')
    result = args.results / datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ-zink-composite')
    result.mkdir(parents=True, exist_ok=False)
    metadata = {'platform': 'J713' if m4 else 'development host', 'command': command,
                'build': json.loads((build / 'hyprland-build.json').read_text()),
                'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
    before = subprocess.check_output(['dmesg'], text=True) if m4 else ''
    watchdog = ExperimentWatchdog() if m4 else None
    if watchdog:
        metadata['watchdog'] = watchdog.arm()
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    os.sync()
    try:
        run = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired as error:
        for name, data in [('stdout.log', error.stdout), ('stderr.log', error.stderr)]:
            (result / name).write_bytes(data.encode() if isinstance(data, str) else data or b'')
        metadata['timed_out'] = True
        (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
        os.sync()
        raise RuntimeError('compositing test timed out; any armed watchdog remains armed') from error
    (result / 'stdout.log').write_text(run.stdout)
    (result / 'stderr.log').write_text(run.stderr)
    metadata['returncode'] = run.returncode
    metadata['passed'] = run.returncode == 0
    if not run.returncode:
        metadata['test'] = json.loads(run.stdout)
        metadata['passed'] = bool(metadata['test'].get('passed'))
    if m4:
        after = subprocess.check_output(['dmesg'], text=True)
        (result / 'dmesg.log').write_text(after)
        new = after[len(before):] if after.startswith(before) else after
        metadata['new_kernel_fault'] = any(s in new for s in ('WARNING:', 'BUG:', 'Oops:', 'Unable to handle kernel'))
        metadata['passed'] &= not metadata['new_kernel_fault']
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    os.sync()
    if watchdog:
        if not metadata['passed']:
            raise RuntimeError('compositing test failed; leaving watchdog armed for recovery')
        watchdog.disarm()
        metadata['watchdog']['armed'] = False
        metadata['watchdog']['disarmed_after_evidence_collection'] = True
        (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))
    print('Results:', result)
    return 0 if metadata['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
