# SPDX-License-Identifier: GPL-2.0
"""Offline module-only repack of EXACT D3h: no kernel, DT, loader or root writes."""
from pathlib import Path
import gzip,hashlib,json,struct,sys,tarfile,io
sys.path.insert(0,'/Users/satchlj/Projects/mac-aurora-omarchy/machines/m3-air/research/2026-09-30-gpu-clock/tools')
from package_staged_module import entries,MAGIC
B=Path(__file__).resolve().parent;P=B.parent/'j613-d3-haptics-transport-20261007'
sha=lambda b:hashlib.sha256(b).hexdigest()
m=json.loads((P/'manifest.json').read_text());stage=(P/'stage2-d3h.bin').read_bytes()
assert sha(stage)==m['stage_sha256']=='ac340ae2bbe205370fc1480ccf634c9a22a25089b38426ab3ce5a4c09b030143'
R=m['release'];qp='usr/lib/modules/'+R+'/kernel/drivers/spi/spi-apple-qspi.ko';mod=(B/'spi-apple-qspi.ko').read_bytes()
assert b'vermagic='+R.encode()+b' SMP preempt mod_unload aarch64\0' in mod
shoff=struct.unpack_from('<Q',mod,40)[0];shentsize,shnum,shstr=struct.unpack_from('<HHH',mod,58)
sections=[struct.unpack_from('<IIQQQQIIQQ',mod,shoff+i*shentsize) for i in range(shnum)]
ss=sections[shstr];names=mod[ss[4]:ss[4]+ss[5]];notes=[]
for sec in sections:
 name=names[sec[0]:].split(b'\0',1)[0]
 if name==b'.note.gnu.build-id':notes.append(mod[sec[4]:sec[4]+sec[5]])
assert len(notes)==1;note=notes[0]

expanded=gzip.decompress(stage[m['initrd_offset']:m['initrd_offset']+m['initrd_gzip_bytes']]);raw=expanded[len(MAGIC)+4:]
assert expanded.startswith(MAGIC) and struct.unpack_from('<I',expanded,len(MAGIC))[0]==len(raw)==m['actual_cpio_length']
assert sha(raw)==m['cpio_sha256'];old=list(entries(raw));archive=bytearray();changed=[]
for n,h,d in old:
 if n==qp:d=mod;changed.append(n)
 if n=='usr/lib/systemd/system/initrd-qspi-readonly.service':
  assert b'read_only=0' not in d
 h=list(h);h[6]=len(d);h[11]=len(n.encode())+1
 archive.extend(b'070701'+''.join(f'{v:08x}' for v in h).encode()+n.encode()+b'\0');archive.extend(bytes((-len(archive))%4));archive.extend(d);archive.extend(bytes((-len(archive))%4))
assert changed==[qp];archive.extend(bytes((-len(archive))%512));cpio=bytes(archive)
files={n:d for n,h,d in entries(cpio)}
for n,h,d in old:assert files[n]==(mod if n==qp else d)
igz=gzip.compress(MAGIC+struct.pack('<I',len(cpio))+cpio,9,mtime=0)
ioff=m['initrd_offset'];end=ioff+m['initrd_gzip_bytes'];suffix=stage[end:]
result=stage[:ioff]+igz+suffix
assert result[:ioff]==stage[:ioff];assert result[ioff+len(igz):]==suffix
r=gzip.decompress(result[ioff:ioff+len(igz)]);assert struct.unpack_from('<I',r,len(MAGIC))[0]==len(cpio) and r[len(MAGIC)+4:]==cpio
(B/'stage2-d3h-nvram-write.bin').write_bytes(result);(B/'initramfs.cpio').write_bytes(cpio)
# Owner-only root replacement: the same D3h release, QSPI module changed only.
with tarfile.open(B/'root-payload.tgz','w:gz') as t:
 x=tarfile.TarInfo(qp);x.size=len(mod);x.mode=0o644;t.addfile(x,io.BytesIO(mod))
# Keep manifest small: no inherited 1600-entry root payload table.
out={k:v for k,v in m.items() if k not in ('root_payload_files','qspi_force_read_only','missing_patches','preserved_patches')}
out.update(previous_stage_sha256=sha(stage),previous_stage_bytes=len(stage),stage_sha256=sha(result),stage_bytes=len(result),initrd_gzip_bytes=len(igz),actual_cpio_length=len(cpio),cpio_sha256=sha(cpio),qspi_module_sha256=sha(mod),root_payload_sha256=sha((B/'root-payload.tgz').read_bytes()),root_payload_files={qp:sha(mod)},read_only_default=True,runtime_opt_in_only=True,allowed_write_range=[0x700000,0x800000],program_page_bytes=256,erase_sector_bytes=4096,hardware_tested=False,ram_qualified=False,installed=False,nvram_write_qualified=False,loader_kernel_dt_cmdline_identical=True,same_release_module_only=True,qspi_build_id_note_hex=note.hex(),qspi_build_id=note[16:].hex(),rollback_stage_sha256=sha(stage),baseline_source_commit='be0f3ccd5938a40d769535464b18c378a7296b96')
(B/'manifest.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
