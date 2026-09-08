#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bound the native KMS flip test with watchdog and thermal guards."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--guards', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--device', required=True)
p.add_argument('--stop-desktop', action='store_true')
a = p.parse_args()
sys.path.insert(0, str(a.guards))
from experiment_watchdog import ExperimentWatchdog
from experiment_thermals import ExperimentThermals

a.output.mkdir(parents=True, exist_ok=False)
w = ExperimentWatchdog()
r = {'watchdog': w.arm(), 'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
t = ExperimentThermals(a.output)
try:
    t.start()
except BaseException:
    w.disarm()
    raise
since = str(int(time.time()))
try:
    if a.stop_desktop:
        subprocess.run(['systemctl', 'stop', 'sddm'], check=True, timeout=15)
    source = Path(__file__).with_name('kms-flip-test.c')
    binary = a.output / 'kms-flip-test'
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libdrm'], text=True).split()
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', str(source), '-o', str(binary), *flags],
                   check=True, timeout=10)
    t.check()
    info = subprocess.check_output(['modetest', '-M', 'm4-dcp', '-c', '-p'], text=True, timeout=5)
    (a.output / 'resources.txt').write_text(info)
    result = subprocess.run([str(binary), a.device], text=True, capture_output=True, timeout=30)
    r.update(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
    (a.output / 'flips.log').write_text(result.stdout + result.stderr)
    t.check()
    if result.returncode or 'PASS: 180 completed native KMS flips' not in result.stdout:
        raise RuntimeError('native flip test failed')
    r['passed'] = True
except BaseException as error:
    r.update(passed=False, failure=repr(error))
    raise
finally:
    t.close()
    r['thermal'] = t.summary()
    log = subprocess.check_output(['journalctl', '-k', '-b', '--since', '@' + since, '--no-pager', '-o', 'cat'],
                                  text=True, timeout=5)
    (a.output / 'kernel.log').write_text(log)
    (a.output / 'result.json').write_text(json.dumps(r, indent=2) + '\n')
    os.sync()
w.disarm()
print(json.dumps(r, indent=2))
