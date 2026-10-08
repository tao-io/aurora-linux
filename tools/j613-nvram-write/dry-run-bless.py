#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Owner-led Linux userspace rehearsal; only a private snapshot copy is writable.
The asahi-bless disk inventory is READ-ONLY. No Rubra command is sent here.
Logs contain syscall metadata, never variable contents or UUID values.
"""
from pathlib import Path
import hashlib,json,os,pwd,re,shutil,stat,subprocess,sys
assert len(sys.argv)==3,'usage: dry-run-bless.py NVRAM-SNAPSHOT NEW-OUTPUT-DIR'
src=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve()
assert src.is_file() and src.stat().st_size==0x100000
assert stat.S_ISREG(src.stat().st_mode)
private=Path(pwd.getpwuid(src.stat().st_uid).pw_dir)/'Projects/mac-dualboot/private'
assert not src.stat().st_mode & 0o077,'Source must be private'
assert src.is_relative_to(private.resolve()) and out.is_relative_to(private.resolve()),'Private paths required'
assert not out.exists()
out.mkdir(mode=0o700);dst=out/'nvram-private-copy.bin'
with src.open('rb') as r,dst.open('xb') as w:
 os.chmod(dst,0o600);shutil.copyfileobj(r,w)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
before=sha(src);assert sha(dst)==before and dst.stat().st_ino!=src.stat().st_ino
so=out/'mtd-file-tracer.so'
subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror','-Wno-sign-compare',str(Path(__file__).with_name('mtd-file-tracer.c')),'-ldl','-o',str(so)],check=True)
# Private mount/PID/network namespaces: no real MTD, mem, or host process FDs.
# Only the inventory nodes used by upstream get_boot_candidates are exposed.
# Upstream opens them read-only; the shim rejects writable device opens/writes.
# ro-bind sets nodev, so dev-bind is required for the read-only inventory path.
cmd=['bwrap','--ro-bind','/','/','--dev','/dev','--proc','/proc',
     '--unshare-pid','--unshare-net','--die-with-parent','--cap-drop','ALL',
     '--tmpfs','/run','--bind',str(out),'/run/j613-nvram-dry-run']
for node in sorted(Path('/dev').glob('nvme0n1*')):
 if stat.S_ISBLK(node.stat().st_mode):cmd+=['--dev-bind',str(node),str(node)]
cmd+=['--setenv','LD_PRELOAD','/run/j613-nvram-dry-run/mtd-file-tracer.so','--setenv','J613_NVRAM_TRACE_FILE','/run/j613-nvram-dry-run/nvram-private-copy.bin',
      'sh','-c','test ! -e /dev/mtd0 && test ! -e /dev/mtd0ro && test ! -e /dev/mtd/by-name/nvram && exec asahi-bless --device "$1" --next --set-boot TahoeResearch --yes','dry-run','/run/j613-nvram-dry-run/nvram-private-copy.bin']
env=dict(os.environ)
with (out/'trace-metadata.txt').open('w') as log:
 r=subprocess.run(cmd,env=env,stdout=log,stderr=log,timeout=60)
assert sha(src)==before,'Source changed!'
summary=dict(returncode=r.returncode,source_sha256=before,copy_sha256=sha(dst),source_unchanged=True,regular_file_only=True,hardware_write=False,mtd_devices_hidden=True,capabilities_dropped=True,bless_sha256=sha(Path(shutil.which('asahi-bless'))))
# Retain any tool output privately; publish only tracer-generated metadata.
(out/'trace-sanitized.txt').write_text(''.join(line for line in (out/'trace-metadata.txt').read_text().splitlines(True) if re.fullmatch(r'TRACE (write|erase) offset=0x[0-9a-f]+ length=0x[0-9a-f]+\n?',line)))
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
raise SystemExit(r.returncode)
