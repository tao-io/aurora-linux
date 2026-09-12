#!/usr/bin/env python3
"""Send one request to the guarded J713 desktop UI bridge and fetch its snapshot.

Usage: desktop-ui-remote.py REMOTE_OUTPUT JSON LOCAL_IMAGE_STEM
The remote server must already be running inside gpu-experiment.py. This client
owns no watchdog and launches no workload on its own. Inspect the PNG before
sending the next menu input. Uses the existing native scanout decoder.
"""
import argparse, json, shlex, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output');p.add_argument('request');p.add_argument('stem',type=Path)
a=p.parse_args()
request=json.loads(a.request)
reply=subprocess.check_output(['ssh','-o','ConnectTimeout=10','root@omarchy-m4',
    shlex.join(['python3','/root/desktop-ui.py','send',a.output,json.dumps(request)])],text=True,timeout=20)
r=json.loads(reply);image=r['image']
assert image.startswith(a.output+'/') and image.endswith('.m4fb')
a.stem.parent.mkdir(parents=True,exist_ok=True)
raw=Path(str(a.stem)+'.m4fb');png=Path(str(a.stem)+'.png')
subprocess.run(['scp','-o','ConnectTimeout=10','root@omarchy-m4:'+image,str(raw)],check=True,timeout=30)
subprocess.run([sys.executable,str(Path(__file__).with_name('scanout-image.py')),str(raw),str(png)],check=True,timeout=20)
print(json.dumps({'snapshot':str(png),'remote':r}))
