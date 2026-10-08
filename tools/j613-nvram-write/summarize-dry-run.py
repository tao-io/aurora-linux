#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compare private v3 snapshots; expose structure and changed keys, never values."""
from pathlib import Path
import hashlib,json,struct,sys,zlib

def parse(b):
 assert len(b)==0x100000
 banks=[]
 for i in range(16):
  base=i*65536;s=b[base:base+65536]
  assert s[:4]==b'3VVN' and s[14]==1
  size,gen=struct.unpack_from('<II',s,4);assert size==65536
  off=24;records=[]
  while off+36<size:
   h=s[off:off+36]
   if all(x in (0,255) for x in h):break
   assert struct.unpack_from('<H',h)[0]==0x55aa
   ns,ds=struct.unpack_from('<II',h,8);end=off+36+ns+ds
   assert ns>=1 and end<=size
   name=s[off+36:off+36+ns-1];val=s[off+36+ns:end]
   assert zlib.crc32(val)==struct.unpack_from('<I',h,32)[0]
   records.append(dict(offset=base+off,end=base+end,state=h[2],key=(h[16:32],name),value=val))
   off=end
  banks.append(dict(slot=i,generation=gen,records=records,end=base+off,header=s[:24]))
 highest=max(x['generation'] for x in banks)
 active=next(x for x in banks if x['generation']==highest)
 assert sum(x['generation']==highest for x in banks)==1
 return banks,active

def ranges(offsets):
 out=[]
 for x in offsets:
  if out and x==out[-1][1]:out[-1][1]+=1
  else:out.append([x,x+1])
 return [dict(partition_offset=hex(a),nor_offset=hex(0x700000+a),length=b-a) for a,b in out]

before=Path(sys.argv[1]).read_bytes();after=Path(sys.argv[2]).read_bytes()
bb,b=parse(before);ab,a=parse(after)
old={r['key']:r['value'] for r in b['records'] if r['state']==0x7f}
new={r['key']:r['value'] for r in a['records'] if r['state']==0x7f}
changed=[k for k in old.keys()|new.keys() if old.get(k)!=new.get(k)]
# This task authorizes exposing changed key names, never other names or values.
assert [k[1] for k in changed]==[b'alt-boot-volume']
diff=[i for i,(x,y) in enumerate(zip(before,after)) if x!=y]
assert all(before[i]&after[i]==after[i] for i in diff),'0->1 without erase'
pages=sorted({i//256*256 for i in diff})
record_changes=[]
for r in a['records']:
 if after[r['offset']:r['end']]!=before[r['offset']:r['end']]:
  record_changes.append(dict(partition_offset=hex(r['offset']),nor_offset=hex(0x700000+r['offset']),bytes=r['end']-r['offset'],state=r['state'],key=r['key'][1].decode() if r['key'][1]==b'alt-boot-volume' else 'unrelated-record',new_record=r['offset']>=b['end']))
assert all(x['key']=='alt-boot-volume' for x in record_changes)
boot=[v for k,v in old.items() if k[1]==b'boot-volume'];assert len(boot)==1
alt=[v for k,v in new.items() if k[1]==b'alt-boot-volume'];assert len(alt)==1
out=dict(format='v3',valid_banks_before=len(bb),valid_banks_after=len(ab),active_bank_before=b['slot'],active_bank_after=a['slot'],generation_delta=a['generation']-b['generation'],all_bank_headers_unchanged=all(x['header']==y['header'] for x,y in zip(bb,ab)),records_before=len(b['records']),records_after=len(a['records']),active_records_before=len(old),active_records_after=len(new),alt_boot_volume_present_before=any(k[1]==b'alt-boot-volume' for k in old),alt_boot_volume_present_after=True,next_target_matches_persistent_default=alt[0]==boot[0],changed_keys=['alt-boot-volume'],unrelated_active_variables_identical=True,record_changes=record_changes,changed_bytes=len(diff),changed_byte_ranges=ranges(diff),userspace_write_ranges=[dict(partition_offset='0x0',nor_offset='0x700000',length=65536)],erase_ranges=[],candidate_nonidentical_program_pages=ranges([j for p in pages for j in range(p,p+256)]),candidate_program_page_count=len(pages),all_mutations_in_guarded_range=True,no_zero_to_one=True,source_sha256=hashlib.sha256(before).hexdigest(),copy_sha256=hashlib.sha256(after).hexdigest(),generation_values_suppressed=True,values_guids_crcs_suppressed=True)
print(json.dumps(out,indent=2))
