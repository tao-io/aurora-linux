from pathlib import Path
import subprocess,json,os
B=Path(__file__).resolve().parent
for k in ('J613_RIG_LOCK_HELD','J613_OWNER_RELEASED'):assert os.environ.get(k)=='1',k
A=B/'installed';A.mkdir(exist_ok=False)
subprocess.run(['bash',str(B/'check-nvram.sh'),str(A)],check=True)
subprocess.run(['bash',str(B/'check-haptics-nofinger.sh'),str(A)],check=True)
m=json.loads((B/'manifest.json').read_text())
subprocess.run(['ssh','rubra','sudo python3 /root/j613-d3-nvram-write/check-root.py'],check=True)
r=subprocess.check_output(['ssh','rubra','sudo sha256sum /boot/efi/aurora/j613-25g83-g54/stage2.bin'])
(A/'esp-readback.txt').write_bytes(r);assert r.decode().split()[0]==m['stage_sha256']
(A/'result.json').write_text(json.dumps(dict(passed=True,read_only=True,nvram_write_tested=False,stage_sha256=m['stage_sha256']),indent=2)+'\n')
print('Installed read-only checks passed; NVRAM write ladder still separate')
