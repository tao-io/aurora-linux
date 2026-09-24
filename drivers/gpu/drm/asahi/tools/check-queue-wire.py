#!/usr/bin/env python3
"""Compare actual M4 queue code with an immutable Git baseline using host buffers.

Adapt only Arc/Mutex construction to direct host ownership; queue arithmetic,
field accesses, rejection and publication calls execute from the source files.
Ordered allocation/write/read/publication bytes must match for every variant.
Not a DMA, locking, ASID-lifetime or hardware emulator.
"""
import argparse,hashlib,json,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel',type=Path,required=True);p.add_argument('--baseline',required=True)
p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
base='drivers/gpu/drm/asahi';driver=a.kernel/base
tree=subprocess.check_output(['git','-C',str(a.kernel),'rev-parse',a.baseline+'^{tree}'],text=True).strip()
versions={}
for name in ['baseline','candidate']:
 directory=a.output/name;(directory/'tools').mkdir(parents=True);sources={}
 for filename in ['g16_queue.rs','g16_compute.rs']:
  raw=subprocess.check_output(['git','-C',str(a.kernel),'show',f'{tree}:{base}/{filename}']) if name=='baseline' else (driver/filename).read_bytes()
  (directory/filename).write_bytes(raw);sources[filename]=hashlib.sha256(raw).hexdigest()
 source=(directory/'g16_queue.rs').read_text()
 for filename in re.findall(r'#\[path = "([^"]+)"\]',source):
  raw=subprocess.check_output(['git','-C',str(a.kernel),'show',f'{tree}:{base}/{filename}']) if name=='baseline' else (driver/filename).read_bytes()
  (directory/filename).write_bytes(raw);sources[filename]=hashlib.sha256(raw).hexdigest()
 old='pub(crate) type Client = kernel::sync::Arc<kernel::sync::Mutex<ClientNotifications>>;'
 assert source.count(old)==1;source=source.replace(old,'pub(crate) type Client = ClientNotifications;')
 old='kernel::sync::Arc::pin_init(kernel::new_mutex!(Self {'
 assert source.count(old)==1;source=source.replace(old,'Ok(Self {')
 old='}), GFP_KERNEL)';assert source.count(old)==1;source=source.replace(old,'})')
 exercise=(driver/'tools/m4_queue_wire_exercise.rs').read_text();source+='\n'+exercise
 (directory/'queue-host.rs').write_text(source)
 for filename in ['m4_queue_wire_fixture.rs','m4_queue_wire_exercise.rs']:
  raw=(driver/'tools'/filename).read_bytes();(directory/'tools'/filename).write_bytes(raw);sources[filename]=hashlib.sha256(raw).hexdigest()
 subprocess.run(['rustc','--edition=2021',str(directory/'tools/m4_queue_wire_fixture.rs'),'-o',str(directory/'queue-wire')],check=True)
 versions[name]=sources
comparisons=[]
for variant in [0,1,2,15,31,63]:
 outputs=[]
 for name in versions:
  raw=subprocess.check_output([str((a.output/name/'queue-wire').resolve()),str(variant)])
  (a.output/f'{name}-{variant}.bin').write_bytes(raw);outputs.append(raw)
 assert outputs[0]==outputs[1],f'Queue wire mismatch: variant {variant}'
 comparisons.append(dict(variant=variant,bytes=len(outputs[0]),sha256=hashlib.sha256(outputs[0]).hexdigest()))
(a.output/'result.json').write_text(json.dumps(dict(baseline_tree=tree,sources=versions,comparisons=comparisons,result='pass'),indent=2)+'\n')
print(f'PASS: {len(comparisons)} variants, identical queue/client wire operations and rejection checks')
