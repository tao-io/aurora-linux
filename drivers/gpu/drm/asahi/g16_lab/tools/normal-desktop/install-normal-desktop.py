#!/usr/bin/env python3
"""Install staged normal-desktop files on J713; does not start GPU work.

Copy tools/normal-desktop/* to /root/m4-normal-desktop-stage first, then run
this file on the target with --backup NEW_DIRECTORY. Start/validate SDDM through
normal-login-check.py afterward. Every overwritten file is backed up.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--backup',type=Path,required=True)
a=p.parse_args()
assert os.geteuid() == 0
assert b'apple,j713\0' in Path('/sys/firmware/devicetree/base/compatible').read_bytes()
a.backup.mkdir(parents=True,exist_ok=False)
stage=Path('/root/m4-normal-desktop-stage')
mapping={
    'graphics.env':'/etc/environment.d/90-omarchy-j713-rendering.conf',
    'm4-graphics-init.py':'/usr/local/libexec/m4-graphics-init.py',
    'm4-drm-devices.py':'/usr/local/libexec/m4-drm-devices.py',
    'm4-graphics.service':'/etc/systemd/system/m4-graphics.service',
    'sddm-service.conf':'/etc/systemd/system/sddm.service.d/90-omarchy-j713-rendering.conf',
    'm4-hyprland-session':'/usr/local/bin/m4-hyprland-session',
    'm4-hyprland-trace':'/usr/local/bin/m4-hyprland-trace',
    'm4-greeter':'/usr/local/bin/m4-greeter',
    'session.lua':'/usr/local/share/m4-desktop/session.lua',
    'greeter.lua':'/usr/local/share/m4-desktop/greeter.lua',
    'hyprland.desktop':'/usr/local/share/wayland-sessions/hyprland.desktop',
    'sddm.conf':'/etc/sddm.conf.d/95-m4-accelerated.conf',
}
assert all((stage/name).is_file() for name in mapping)
files={}
def install(source, destination, mode=0o644):
    target=Path(destination)
    saved=a.backup/target.relative_to('/')
    if target.exists() or target.is_symlink():
        saved.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(target,saved,follow_symlinks=False)
    target.parent.mkdir(parents=True,exist_ok=True)
    temporary=target.with_name(target.name+'.m4-new')
    shutil.copyfile(source,temporary)
    temporary.chmod(mode)
    os.replace(temporary,target)
    files[str(target)]=dict(existed=saved.exists(),sha256=hashlib.sha256(target.read_bytes()).hexdigest())
for name,target in mapping.items():
    install(stage/name,target,0o755 if name in ('m4-graphics-init.py','m4-drm-devices.py','m4-hyprland-session','m4-hyprland-trace','m4-greeter') else 0o644)
install(Path('/root/m4_dcp_rtkit-asahi.ko'),
        '/lib/modules/'+os.uname().release+'/updates/m4_dcp_rtkit.ko')
# User managers read the actual card selected during boot, rather than assuming
# that probe ordering always assigns a particular card number.
link=Path('/etc/environment.d/91-m4-drm.conf')
saved=a.backup/link.relative_to('/')
existed=link.exists() or link.is_symlink()
if existed:
    saved.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(link,saved,follow_symlinks=False)
    link.unlink()
link.symlink_to('/run/m4-graphics/environment')
files[str(link)]={'existed':existed,'symlink':str(link.readlink())}
(a.backup/'manifest.json').write_text(json.dumps(files,indent=2)+'\n')
subprocess.run(['depmod','-a'],check=True,timeout=20)
subprocess.run(['systemd-analyze','verify','/etc/systemd/system/m4-graphics.service'],check=True,timeout=10)
subprocess.run(['systemctl','daemon-reload'],check=True,timeout=10)
subprocess.run(['systemctl','enable','sddm'],check=True,timeout=5)
os.sync()
print('Installed normal M4 desktop; backups:',a.backup)
