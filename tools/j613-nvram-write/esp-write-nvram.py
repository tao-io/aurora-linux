"""Dry-run default; atomic replacement only after this candidate's RAM PASS.
Caller must hold rig lock + watcher and serialize Snapper/ESP writers.
No additional D3 ESP backup: retain original artifact off ESP and existing G54.
"""
from pathlib import Path
import os,sys,json,hashlib,math,subprocess,tarfile
B=Path(__file__).resolve().parent;m=json.loads((B/'manifest.json').read_text());src=B/'stage2-d3h-nvram-write.bin';esp=Path('/boot/efi');slot=esp/'aurora/j613-25g83-g54';target=slot/'stage2.bin';tmp=slot/'.stage2-d3h-nvram-write.new';backup=slot/'stage2-g54.bin'
def digest(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert digest(src)==m['stage_sha256'] and src.stat().st_size==m['stage_bytes']
assert m['root_initramfs_module_identical'] and not m['btf_present']
assert digest(B/'root-payload.tgz')==m['root_payload_sha256']
qp='usr/lib/modules/'+m['release']+'/kernel/drivers/spi/spi-apple-qspi.ko'
with tarfile.open(B/'root-payload.tgz') as t:
 assert hashlib.sha256(t.extractfile(qp).read()).hexdigest()==m['qspi_module_sha256']
old_sha=digest(target);assert old_sha in ('f3f9c97a5c79437c7e5d87d2b6cc0a45737eb692b9bf450bbbe64bcef4bbe9fc',m['previous_stage_sha256'])
assert digest(backup)=='45ed9c480c30483a8cf400416a3eb18c13009cd18c92a1dcc3540d2abddd16c7'
assert not tmp.exists()
v=os.statvfs(esp);total=v.f_blocks*v.f_frsize;used=(v.f_blocks-v.f_bfree)*v.f_frsize;alloc=math.ceil(src.stat().st_size/v.f_frsize)*v.f_frsize;reserve=1024**2
out=dict(total=total,used=used,temporary_alloc=alloc,peak=used+alloc,peak_percent=(used+alloc)*100/total,extra_d3_backup_peak_percent=(used+2*alloc)*100/total,reserve=reserve)
print(json.dumps(out,indent=2),flush=True);assert (used+alloc+reserve)*100<total*80,'Insufficient headroom; no automatic pruning'
if '--apply' not in sys.argv:sys.exit(0)
assert os.geteuid()==0
assert os.environ.get('J613_D3H_HAPTICS_APPROVED')=='1'
assert Path('/sys/module/spi_apple_qspi/parameters/read_only').read_text().strip()=='Y'
for k in ('J613_RIG_LOCK_HELD','J613_OWNER_RELEASED','J613_ESP_WRITERS_SERIALIZED'):assert os.environ.get(k)=='1',k
ram=json.loads((B/'ram/result.json').read_text());assert ram['passed'] and ram['stage_sha256']==m['stage_sha256']
assert subprocess.check_output(['uname','-r']).decode().strip()==m['release']
assert int(Path('/sys/class/power_supply/macsmc-battery/capacity').read_text())>30 or Path('/sys/class/power_supply/macsmc-ac/online').read_text().strip()=='1'
subprocess.run(['python3',str(B/'check-root.py')],check=True)
# Caller pins every other ESP file and records the live watcher + serialization.
with src.open('rb') as a,tmp.open('xb') as z:
 while chunk:=a.read(1024**2):z.write(chunk)
 z.flush();os.fsync(z.fileno())
os.sync();assert digest(tmp)==m['stage_sha256'] and digest(target)==old_sha
v=os.statvfs(esp);observed=(v.f_blocks-v.f_bfree)*v.f_frsize;assert (observed+reserve)*100<total*80
print(json.dumps(dict(observed_peak=observed,observed_peak_percent=observed*100/total)),flush=True)
os.replace(tmp,target);os.sync();os.sync();assert digest(target)==m['stage_sha256'];assert digest(backup)=='45ed9c480c30483a8cf400416a3eb18c13009cd18c92a1dcc3540d2abddd16c7'
print('READ-ONLY CANDIDATE ESP READBACK PASS; default startup and all other ESP files untouched')
