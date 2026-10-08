# SPDX-License-Identifier: GPL-2.0
from pathlib import Path
import gzip,struct,json,hashlib,subprocess,sys
sys.path.insert(0,'/Users/satchlj/Projects/mac-aurora-omarchy/machines/m3-air/research/2026-09-30-gpu-clock/tools')
from package_staged_module import entries,MAGIC
B=Path(__file__).resolve().parent;P=B.parent/'j613-d3-haptics-transport-20261007';m=json.loads((B/'manifest.json').read_text());old=json.loads((P/'manifest.json').read_text())
sha=lambda d:hashlib.sha256(d).hexdigest();s=(B/'stage2-d3h-nvram-write.bin').read_bytes();baseline=(P/'stage2-d3h.bin').read_bytes()
assert sha(s)==m['stage_sha256'];assert sha(baseline)==m['previous_stage_sha256'];assert s[:m['initrd_offset']]==baseline[:old['initrd_offset']]
assert s[m['initrd_offset']+m['initrd_gzip_bytes']:]==baseline[old['initrd_offset']+old['initrd_gzip_bytes']:]
k=gzip.decompress(s[m['kernel_offset']:m['kernel_offset']+m['kernel_gzip_bytes']]);assert sha(k)==m['kernel_sha256']==old['kernel_sha256']
e=gzip.decompress(s[m['initrd_offset']:m['initrd_offset']+m['initrd_gzip_bytes']]);assert e.startswith(MAGIC)
raw=e[len(MAGIC)+4:];assert struct.unpack_from('<I',e,len(MAGIC))[0]==len(raw)==m['actual_cpio_length'];assert sha(raw)==m['cpio_sha256'];assert raw==(B/'initramfs.cpio').read_bytes()
base_e=gzip.decompress(baseline[old['initrd_offset']:old['initrd_offset']+old['initrd_gzip_bytes']]);base_raw=base_e[len(MAGIC)+4:]
f={n:d for n,h,d in entries(raw)};bf={n:d for n,h,d in entries(base_raw)};assert f.keys()==bf.keys()
qp='usr/lib/modules/'+m['release']+'/kernel/drivers/spi/spi-apple-qspi.ko';assert [n for n in f if f[n]!=bf[n]]==[qp]
assert sha(f[qp])==m['qspi_module_sha256'];assert b'read_only=0' not in f['usr/lib/systemd/system/initrd-qspi-readonly.service']
assert bytes.fromhex(m['qspi_build_id_note_hex']) in f[qp]
for p in B.glob('*.py'):compile(p.read_text(),str(p),'exec')
for p in B.glob('*.sh'):subprocess.run(['bash','-n',str(p)],check=True)
out=dict(passed=True,stage_sha256=m['stage_sha256'],loader_kernel_dt_cmdline_identical=True,only_initramfs_delta=qp,actual_cpio_length=len(raw),m1n1_length_verified=True,default_read_only=True,hardware_tested=False)
(B/'offline-checks.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
