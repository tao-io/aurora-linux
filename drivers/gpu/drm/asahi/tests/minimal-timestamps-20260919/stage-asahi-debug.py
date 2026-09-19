#!/usr/bin/env python3
"""Guarded module/initramfs stage with saved boot-time debug flags.
Usage: stage-asahi-debug.py OUTPUT MODULE SHA LABEL FLAGS
Delegates hardware guards, backup and rollback to stage-asahi-initramfs.py.
"""
import json
from pathlib import Path
import re
import subprocess
import sys

out, module, sha, label, flags_text = sys.argv[1:]
out = Path(out)
flags = int(flags_text, 0)
assert 0 <= flags < 1 << 64 and flags & (1 << 24), 'Keep watchdog retirement progress'
start = json.loads((out/'start.json').read_text())
assert start['watchdog']['armed'] and start['watchdog']['timeout_seconds'] == 60
conf = Path('/etc/modprobe.d/99-factorio-submit-summary.conf')
previous = conf.read_text()
assert len(re.findall(r'debug_flags=\d+', previous)) == 1
updated = re.sub(r'debug_flags=\d+', f'debug_flags={flags}', previous)
(out/'debug-config-before.conf').write_text(previous)
(out/'debug-config-after.conf').write_text(updated)
try:
    conf.write_text(updated)
    subprocess.run(['python3', '/root/stage-asahi-initramfs.py', str(out), module, sha, label], check=True)
except BaseException:
    conf.write_text(previous)
    raise
print('DEBUG_FLAGS_STAGED', flags, flush=True)
