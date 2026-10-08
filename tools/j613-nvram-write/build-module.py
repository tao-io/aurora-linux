# SPDX-License-Identifier: GPL-2.0
from pathlib import Path
import subprocess,json,hashlib,os
B=Path(__file__).resolve().parent;K=Path('/home/satchlj.guest/j613-d3-20261007/kernel')
assert B.name=='j613-d3-nvram-write-20261007'
ps=subprocess.check_output(['ps','-eo','args'],text=True)
assert not any(x.startswith(('make ', '/usr/bin/make ', 'ninja ')) for x in ps.splitlines()),'Shared build active; coordinate first'
assert (K/'include/config/kernel.release').read_text().strip()=='7.1.9-eryk-m4j613-j613-d3h'
cmd=['make','-C',str(K),'M='+str(B),'ARCH=arm64','LLVM=1','LOCALVERSION=-j613-d3h','modules']
r=subprocess.run(cmd,capture_output=True,text=True);(B/'module-build.log').write_text(r.stdout+r.stderr);r.check_returncode()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
out=dict(argv=cmd,kernel_release='7.1.9-eryk-m4j613-j613-d3h',source_sha256=sha(B/'spi-apple-qspi.c'),policy_sha256=sha(B/'spi-apple-qspi-policy.h'),module_sha256=sha(B/'spi-apple-qspi.ko'),symvers_sha256=sha(K/'Module.symvers'),config_sha256=sha(K/'.config'),hardware_tested=False)
(B/'module-build-manifest.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
