#!/usr/bin/env python3
"""Initialize the installed J713 GPU/display stack before ordinary SDDM."""
from pathlib import Path
import subprocess

assert b'apple,j713\0' in Path('/sys/firmware/devicetree/base/compatible').read_bytes()
assert not Path('/sys/module/g16_lab').exists(), 'Lab driver owns the GPU'
subprocess.run(['modprobe', 'asahi'], check=True, timeout=25)
subprocess.run(['modprobe', 'm4_dcp_rtkit', 'kernel_client=1', 'kms=1', 'takeover=1'],
               check=True, timeout=20)
cards = [p for p in Path('/sys/class/drm').glob('card*')
         if '-' not in p.name and (p/'device/of_node/compatible').exists()
         and b'apple,t8132-display-diagnostics\0' in (p/'device/of_node/compatible').read_bytes()]
renders = [p for p in Path('/sys/class/drm').glob('renderD*')
           if (p/'device/driver').resolve().name == 'asahi']
assert len(cards) == len(renders) == 1, (cards, renders)
out = Path('/run/m4-graphics')
out.mkdir(exist_ok=True)
(out/'environment').write_text('AQ_DRM_DEVICES=/dev/dri/'+cards[0].name+'\n')
print('M4 normal desktop: display='+cards[0].name+' render='+renders[0].name, flush=True)
