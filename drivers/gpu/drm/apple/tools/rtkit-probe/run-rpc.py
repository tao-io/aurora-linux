#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run bounded DCP RPC diagnostics with watchdog and thermal monitoring."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from rpc import DCP

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--guards', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--call', default='A412')
p.add_argument('--input-hex', default='')
p.add_argument('--output-size', type=int, default=4)
p.add_argument('--repeat', type=int, default=1)
p.add_argument('--expect-hex')
a = p.parse_args()
if not 1 <= a.repeat <= 100 or not 0 <= a.output_size <= 0x7000:
    p.error('invalid repeat count/output size')
payload = bytes.fromhex(a.input_hex)
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
events = []
def log(event):
    events.append(event)
    with (a.output / 'rpc.jsonl').open('a') as f:
        f.write(json.dumps(event) + '\n')
        f.flush()
        os.fsync(f.fileno())
    print(json.dumps(event), flush=True)

dcp = None
try:
    dcp = DCP(log, t.check)
    start = time.monotonic()
    for i in range(a.repeat):
        if time.monotonic() - start > 20:
            raise TimeoutError('RPC test exceeded 20 seconds')
        result = dcp.call(a.call, payload, a.output_size)
        if a.expect_hex is not None and result != bytes.fromhex(a.expect_hex):
            raise RuntimeError('RPC result differs from expected value')
    t.check()
    r.update(passed=True, calls=a.repeat, elapsed_seconds=time.monotonic()-start)
except BaseException as error:
    r.update(passed=False, failure=repr(error))
    raise
finally:
    if dcp:
        dcp.close()
    t.close()
    r['thermal'] = t.summary()
    logtext = subprocess.check_output(['journalctl', '-k', '-b', '--since', '@'+since,
                                       '--no-pager', '-o', 'cat'], text=True, timeout=5)
    (a.output / 'kernel.log').write_text(logtext)
    (a.output / 'result.json').write_text(json.dumps(r, indent=2)+'\n')
    os.sync()
w.disarm()
print(json.dumps(r, indent=2))
