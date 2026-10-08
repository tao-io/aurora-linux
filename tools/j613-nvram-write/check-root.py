from pathlib import Path
import hashlib,json,subprocess,runpy,tempfile,sys
B=Path(__file__).resolve().parent;M=json.loads((B/'manifest.json').read_text())
assert subprocess.check_output(['uname','-r'],text=True).strip()==M['release']
assert Path('/lib/modules').resolve()==Path('/usr/lib/modules').resolve()
p=Path('/lib/modules')/M['release']/'kernel/drivers/spi/spi-apple-qspi.ko'
expected=M['rollback_module_sha256'] if '--baseline' in sys.argv else M['qspi_module_sha256']
assert hashlib.sha256(p.read_bytes()).hexdigest()==expected
# Run the full D3h checker with only the deliberate QSPI replacement expected.
base=Path('/root/j613-d3-haptics-transport')
m=json.loads((base/'manifest.json').read_text())
m['root_payload_files'][str(p).replace('/lib/','usr/lib/',1).lstrip('/')]=expected
with tempfile.TemporaryDirectory() as td:
 t=Path(td);(t/'manifest.json').write_text(json.dumps(m))
 (t/'check-root.py').write_bytes((B/'check-d3h-root.py').read_bytes())
 runpy.run_path(str(t/'check-root.py'),run_name='__main__')
print('Candidate root QSPI hash and full D3h integration verified')
