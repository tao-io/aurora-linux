#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Owner-led Linux userspace rehearsal; only a private snapshot copy is writable.
The asahi-bless disk inventory is READ-ONLY. No Rubra command is sent here.
Logs contain syscall metadata, never variable contents or UUID values.
"""
from pathlib import Path
import hashlib,json,os,shutil,stat,subprocess,sys
assert len(sys.argv)==3,'usage: dry-run-bless.py NVRAM-SNAPSHOT NEW-OUTPUT-DIR'
src=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
assert src.is_file() and src.stat().st_size==0x100000
assert stat.S_ISREG(src.stat().st_mode)
assert not out.exists()
out.mkdir(mode=0o700);dst=out/'nvram-private-copy.bin'
with src.open('rb') as r,dst.open('xb') as w:
 os.chmod(dst,0o600);shutil.copyfileobj(r,w)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
before=sha(src);assert sha(dst)==before and dst.stat().st_ino!=src.stat().st_ino
so=out/'mtd-file-tracer.so'
subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror','-Wno-sign-compare',str(Path(__file__).with_name('mtd-file-tracer.c')),'-ldl','-o',str(so)],check=True)
env=dict(os.environ,LD_PRELOAD=str(so),J613_NVRAM_TRACE_FILE=str(dst))
with (out/'trace-metadata.txt').open('w') as log:
 r=subprocess.run(['asahi-bless','--device',str(dst),'--next','--set-boot','TahoeResearch','--yes'],env=env,stdout=log,stderr=log,timeout=60)
assert sha(src)==before,'Source changed!'
summary=dict(returncode=r.returncode,source_sha256=before,copy_sha256=sha(dst),source_unchanged=True,regular_file_only=True,hardware_write=False)
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
raise SystemExit(r.returncode)
