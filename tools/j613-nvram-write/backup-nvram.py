#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run on Alba in an owner-led window. Two independent READS; no live writes.
Raw NVRAM is private: never print it, attach it, or commit it.
"""
from pathlib import Path
import hashlib,json,os,socket,subprocess,sys
assert len(sys.argv)==2,'usage: backup-nvram.py NEW-PRIVATE-ALBA-DIR'
assert socket.gethostname().lower().split('.')[0]=='alba','Backup destination must be Alba'
for k in ('J613_OWNER_RELEASED','J613_RIG_LOCK_HELD'):assert os.environ.get(k)=='1',k
os.umask(0o077);out=Path(sys.argv[1]).resolve();out.mkdir(mode=0o700,exist_ok=False)
ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=5','-o','ServerAliveInterval=2','-o','ServerAliveCountMax=2','rubra']
preflight=r'''set -eu
 test "$(uname -r)" = 7.1.9-eryk-m4j613-j613-d3h
 test "$(cat /sys/module/spi_apple_qspi/parameters/read_only)" = Y
 test "$(cat /sys/class/mtd/mtd0/name)" = nvram
 test "$(cat /sys/class/mtd/mtd0/size)" = 1048576
 test "$(cat /sys/class/mtd/mtd0/erasesize)" = 4096
 test "$(cat /sys/bus/spi/devices/spi0.0/spi-nor/jedec_id)" = ef6517000000
 test -c /dev/mtd0ro
 test "$(readlink -f /dev/mtd/by-name/nvram)" = /dev/mtd0
'''
subprocess.run(ssh+[preflight],check=True,timeout=20)
hashes=[]
for i in (1,2):
 p=out/f'nvram-read-{i}.bin'
 with p.open('xb') as f:
  subprocess.run(ssh+['sudo -n dd if=/dev/mtd0ro bs=1048576 count=1 iflag=fullblock status=none'],stdout=f,check=True,timeout=90)
  f.flush();os.fsync(f.fileno())
 assert p.stat().st_size==1048576
 hashes.append(hashlib.sha256(p.read_bytes()).hexdigest())
assert hashes[0]==hashes[1],'NVRAM changed between reads: STOP; do not qualify writes'
summary=dict(host='alba',bytes=1048576,sha256=hashes[0],two_independent_reads=True,hashes_match=True,hardware_write=False)
(out/'backup-receipt.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
