#!/usr/bin/env python3
"""Run a bounded gpu-experiment command with performance CPU governors.

Preserves frequency limits and thermal policy; restores governors afterwards.
Usage on target: with-cpu-performance.py GUARD_OUTPUT COMMAND [ARG ...]
This is a temporary latency diagnostic, not persistent power configuration.
"""
import json, signal, subprocess, sys
from pathlib import Path
out=Path(sys.argv[1]);g=json.loads((out/'start.json').read_text())
assert g['watchdog']['armed'] and g['watchdog']['timeout_seconds']==60
policies=list(Path('/sys/devices/system/cpu/cpufreq').glob('policy*'))
assert policies
before={str(p/'scaling_governor'):(p/'scaling_governor').read_text().strip() for p in policies}
for p in policies:
    assert 'performance' in (p/'scaling_available_governors').read_text().split()
(out/'cpu-governors-before.json').write_text(json.dumps(before,indent=2))
def interrupted(sig,frame):raise InterruptedError(f'guard signal {sig}')
signal.signal(signal.SIGTERM,interrupted)
signal.signal(signal.SIGINT,interrupted)
child=None
try:
    for p in before:Path(p).write_text('performance')
    child=subprocess.Popen(sys.argv[2:])
    result=child.wait(timeout=max(1,g['seconds']-10))
    if result:raise subprocess.CalledProcessError(result,sys.argv[2:])
finally:
    if child is not None and child.poll() is None:
        child.terminate()
        try:child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill();child.wait()
    for p,value in before.items():Path(p).write_text(value)
    (out/'cpu-governors-restored.json').write_text(json.dumps({p:Path(p).read_text().strip() for p in before},indent=2))
