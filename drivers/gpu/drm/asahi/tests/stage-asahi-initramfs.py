#!/usr/bin/env python3
"""Target-side module + initramfs staging inside gpu-experiment.py.

Usage: stage-asahi-initramfs.py GUARD_OUTPUT MODULE SHA256 LABEL
Stops the desktop, preserves a rollback, and leaves reboot to guarded-reboot.py.
Does not reload the live driver or change the normal boot/session policy.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

out, source, digest, label = sys.argv[1:]
out, source = Path(out), Path(source)
assert re.fullmatch('[a-z0-9-]+', label)
assert re.fullmatch('[0-9a-f]{64}', digest)
start = json.loads((out / 'start.json').read_text())
assert start['watchdog']['armed'] and start['watchdog']['timeout_seconds'] == 60
assert b'apple,j713\0' in Path('/sys/firmware/devicetree/base/compatible').read_bytes()
assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
release = os.uname().release
assert subprocess.check_output(['modinfo', '-F', 'vermagic', source], text=True).split()[0] == release
assert subprocess.check_output(['modinfo', '-F', 'name', source], text=True).strip() == 'asahi'
module = Path('/lib/modules') / release / 'kernel/drivers/gpu/drm/asahi/asahi.ko'
initrd = Path('/boot/asahi/initramfs.asahi2.cpio')
temporary = initrd.with_name('initramfs.' + label + '.cpio')
backup = Path('/root') / (label + '-before')
backup.mkdir(exist_ok=False)
for path in (module, initrd):
    shutil.copy2(path, backup / path.name)
subprocess.run(['systemctl', 'stop', 'display-manager'], check=True, timeout=15)
try:
    deadline = time.monotonic() + 10
    while True:
        clients = subprocess.run(['fuser', '/dev/dri/renderD128', '/dev/dri/card1'], capture_output=True)
        if clients.returncode == 1 or time.monotonic() >= deadline:
            break
        time.sleep(0.25)
    assert clients.returncode == 1, (clients.stdout, clients.stderr)
    shutil.copy2(source, module.with_suffix('.stage'))
    os.replace(module.with_suffix('.stage'), module)
    subprocess.run(['depmod', '-a', release], check=True, timeout=10)
    subprocess.run(['mkinitcpio', '-k', release, '-c', '/root/mkinitcpio-asahi.conf',
                    '-g', temporary], check=True, timeout=30)
    assert temporary.stat().st_size > 10000000
    # Inspect the actual new archive; do not trust mkinitcpio's exit alone.
    packed = temporary.read_bytes()
    assert source.read_bytes() in packed, 'new module absent from uncompressed initramfs'
    os.replace(temporary, initrd)
    result = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (module, initrd)}
    result.update(backup=str(backup), boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip())
    (out / 'deployment.json').write_text(json.dumps(result, indent=2) + '\n')
    os.sync()
except BaseException:
    shutil.copy2(backup / module.name, module)
    shutil.copy2(backup / initrd.name, initrd)
    subprocess.run(['depmod', '-a', release], timeout=10)
    subprocess.run(['systemctl', 'start', 'display-manager'], timeout=20)
    raise
print('PASS: module and initramfs staged; guarded reboot required', flush=True)
