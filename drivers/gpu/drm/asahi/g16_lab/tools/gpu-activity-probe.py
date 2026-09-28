#!/usr/bin/env python3
"""Observe pending/idle transitions around an existing guarded GPU probe.

Run inside the normal watchdog/thermal guard. Never renews the watchdog itself.
"""
import argparse,json,subprocess,threading,time
from pathlib import Path
from idle_work_progress import parse_activity
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--activity',required=True,type=Path)
p.add_argument('--output',required=True,type=Path)
p.add_argument('command',nargs=argparse.REMAINDER)
a=p.parse_args();command=a.command[1:] if a.command[:1]==['--'] else a.command
assert command
samples=[];errors=[];stop=threading.Event()
def collect():
    try:
        while not stop.is_set():
            row=parse_activity(a.activity.read_text())
            if not row['healthy']:raise RuntimeError('unhealthy runtime')
            if samples and (row['generation_ns']!=samples[0]['generation_ns']
                            or row['epoch']<samples[-1]['epoch']):
                raise RuntimeError('runtime generation/epoch changed backwards')
            samples.append(dict(monotonic=time.monotonic(),**row))
            stop.wait(.002)
    except Exception as e:errors.append(repr(e))
t=threading.Thread(target=collect);t.start()
try:
    time.sleep(.2)
    for _ in range(8):subprocess.run(command,check=True,timeout=3)
    time.sleep(.5)
finally:
    stop.set();t.join(timeout=2)
    a.output.write_text(''.join(json.dumps(row)+'\n' for row in samples))
assert not errors,errors
assert samples and {r['pending'] for r in samples}=={0,1},'did not observe both pending and idle'
assert samples[-1]['epoch']>samples[0]['epoch'],'no activity transition'
print('PASS activity telemetry: idle and pending observed, monotonic epoch, healthy runtime')
