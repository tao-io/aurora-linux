from pathlib import Path
import hashlib,json,subprocess
B=Path(__file__).resolve().parent;m=json.loads((B/'manifest.json').read_text());R=m['release']
assert Path('/lib/modules').resolve()==Path('/usr/lib/modules').resolve()
generated={'modules.alias','modules.alias.bin','modules.builtin.alias.bin','modules.builtin.bin','modules.dep.bin','modules.symbols.bin','modules.dep'}
for n,h in m['root_payload_files'].items():
 p=Path('/')/n;assert p.is_file(),n
 if n=='usr/libexec/j613-d3h-gpu-env' and Path('/usr/libexec/j613-mesa-env').is_file() and p.read_bytes()==Path('/usr/libexec/j613-mesa-env').read_bytes():continue  # 10-08: replaced by the mesa-select wrapper
 if p.name not in generated:assert hashlib.sha256(p.read_bytes()).hexdigest()==h,n
for module in ('asahi','hid_magicmouse','hid_apple_mtp_haptic','spi_apple_qspi'):
 assert subprocess.check_output(['modinfo','-k',R,'-F','vermagic',module]).decode().strip()==R+' SMP preempt mod_unload aarch64'
assert Path('/usr/lib/modules',m['previous_release']).is_dir()
assert hashlib.sha256(Path('/opt/mesa-j613-25g83-d3/lib/dri/libgallium-26.1.4.so').read_bytes()).hexdigest()=='edb7c015e0916be6e44d6a811b4161d5cbcb94c2aba1799ada05f5f2dbf65603'
assert 'spi_apple_qspi' in (Path('/usr/lib/modules',R)/'modules.alias').read_text(),'qspi alias missing'
print('Matched D3h root modules, retained D3 and qualified Mesa verified')
