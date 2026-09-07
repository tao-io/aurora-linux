#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Watchdog-guarded J713 DRM/GEM smoke test; no GPU firmware is started."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from experiment_watchdog import ExperimentWatchdog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', required=True, type=Path)
    parser.add_argument('--test', required=True, type=Path)
    parser.add_argument('--results', required=True, type=Path)
    args = parser.parse_args()
    if os.geteuid() or Path('/sys/module/g16_lab').exists():
        parser.error('requires root and an unloaded g16_lab')
    if subprocess.check_output(['modinfo', '-F', 'vermagic', str(args.module)], text=True).split()[0] != os.uname().release:
        parser.error('module does not match running kernel')
    result = args.results / datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ-drm-memory')
    result.mkdir(parents=True, exist_ok=False)
    before = set(Path('/dev/dri').glob('renderD*'))
    metadata = {'kernel': os.uname().release, 'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest()}
    watchdog = ExperimentWatchdog()
    metadata['watchdog'] = watchdog.arm()
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    subprocess.run(['insmod', str(args.module), 'stage=drm-frontend'], check=True)
    for _ in range(100):
        nodes = set(Path('/dev/dri').glob('renderD*')) - before
        if len(nodes) == 1:
            break
        time.sleep(0.02)
    if len(nodes) != 1:
        raise RuntimeError('expected exactly one new render node')
    run = subprocess.run([str(args.test), str(nodes.pop())], capture_output=True, text=True)
    (result / 'test.stdout').write_text(run.stdout)
    (result / 'test.stderr').write_text(run.stderr)
    metadata['test_returncode'] = run.returncode
    if not run.returncode:
        metadata['test'] = json.loads(run.stdout)
    unload = subprocess.run(['rmmod', 'g16_lab'], capture_output=True, text=True)
    metadata['rmmod_returncode'] = unload.returncode
    (result / 'rmmod.txt').write_text(unload.stdout + unload.stderr)
    (result / 'dmesg.log').write_text(subprocess.check_output(['dmesg'], text=True))
    metadata['passed'] = run.returncode == 0 and unload.returncode == 0
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    os.sync()
    if unload.returncode:
        raise RuntimeError('module teardown failed; leaving watchdog armed')
    watchdog.disarm()
    metadata['watchdog']['armed'] = False
    metadata['watchdog']['disarmed_after_evidence_collection'] = True
    (result / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))
    print('Results:', result)
    return 0 if metadata['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
