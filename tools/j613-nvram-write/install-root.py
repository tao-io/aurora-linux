"""Owner-only root module update. Default dry-run; no unload/reload or live opt-in."""
from pathlib import Path
import hashlib,json,os,subprocess,sys
B=Path(__file__).resolve().parent;M=json.loads((B/'manifest.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
R=M['release'];target=Path('/usr/lib/modules')/R/'kernel/drivers/spi/spi-apple-qspi.ko'
assert sha(B/'spi-apple-qspi.ko')==M['qspi_module_sha256']
assert subprocess.check_output(['uname','-r'],text=True).strip()==R
old='168f1a90bb68a2c2031dc48ea9e9a8e247a08695dc5d2cdc7f6d768ed04cc10c'
assert Path('/lib/modules').resolve()==Path('/usr/lib/modules').resolve()
assert sha(target) in (old,M['qspi_module_sha256']),'Unexpected root module'
if sha(target)==M['qspi_module_sha256']:
 assert sha(B/'spi-apple-qspi-d3h-rollback.ko')==old
 subprocess.run(['python3',str(B/'check-root.py')],check=True)
 print('Candidate already installed; rollback verified');raise SystemExit(0)
print('PLAN: back up original D3h QSPI module, atomically replace, depmod; no driver reload')
if '--apply' not in sys.argv:raise SystemExit(0)
assert os.geteuid()==0
for k in ('J613_RIG_LOCK_HELD','J613_OWNER_RELEASED','J613_D3H_HAPTICS_APPROVED'):assert os.environ.get(k)=='1',k
# Root replacement precedes RAM qualification: switch-root loads this copy.
assert os.environ.get('J613_CANDIDATE_ROOT_INSTALL_APPROVED')=='1'
assert Path('/sys/module/spi_apple_qspi/parameters/read_only').read_text().strip()=='Y'
# Validate existing D3h integration BEFORE replacing its module.
subprocess.run(['python3',str(B/'check-root.py'),'--baseline'],check=True)
backup=B/'spi-apple-qspi-d3h-rollback.ko';temp=target.with_name('.spi-apple-qspi-nvram-write.new')
assert not temp.exists()
if not backup.exists():
 with backup.open('xb') as f:f.write(target.read_bytes());f.flush();os.fsync(f.fileno())
assert sha(backup)==old
with temp.open('xb') as f:f.write((B/'spi-apple-qspi.ko').read_bytes());f.flush();os.fsync(f.fileno())
os.chmod(temp,0o644);assert sha(temp)==M['qspi_module_sha256'];assert sha(target)==old
os.replace(temp,target);os.sync();subprocess.run(['depmod','-a',R],check=True)
assert sha(target)==M['qspi_module_sha256']
subprocess.run(['python3',str(B/'check-root.py')],check=True)
print('Root module replaced; running module untouched. Rollback module retained beside this script.')
