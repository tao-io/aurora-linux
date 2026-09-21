#!/usr/bin/env python3
"""Compile production scattered-PTE mapping against a sparse leaf-walk model.

Checks rejection before any leaf store and exact addresses across table boundaries.
No hardware execution. Run from the workspace or copy alongside pgtable.rs/tools.
"""
from pathlib import Path
import subprocess
import argparse
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,required=True,help='pgtable.rs from the candidate checkout (mmu.rs must be alongside)')
source=parser.parse_args().source
s=source.read_text()
if '    pub(crate) fn map_scattered_pages(' not in s:
    raise SystemExit('The selected source does not contain the batched-mapping candidate')
start=s.index('    pub(crate) fn map_scattered_pages(')
brace=s.index('{',start);depth=1;end=brace+1
while depth:
    depth += (s[end]=='{')-(s[end]=='}');end+=1
function=s[start:end]
mmu=source.with_name('mmu.rs').read_text()
start=mmu.index('        for range in sgt.iter() {', mmu.index('    fn map_node('))
end=mmu.index('        // Retain authoritative overlap',start)
collect=mmu[start:end]
code=r'''
use std::{ops::Range,sync::atomic::{AtomicU64,Ordering}};
type PhysicalAddr=u64;
type Result<T=()>=std::result::Result<T,i32>;
const EINVAL:i32=22;const EBUSY:i32=16;const EFAULT:i32=14;const EOVERFLOW:i32=75;const GFP_KERNEL:()=();
const UAT_PGBIT:usize=14;const UAT_PGSZ:usize=1<<UAT_PGBIT;const UAT_PGMSK:usize=UAT_PGSZ-1;
struct Prot(u64);impl Prot {fn as_pte(&self)->u64 {self.0}}
struct UatPageTable { entries:Vec<AtomicU64>, missing:Option<usize>,walks:usize,published:usize }
impl UatPageTable {
 fn new(n:usize)->Self {Self {entries:(0..n).map(|_|AtomicU64::new(0)).collect(),missing:None,walks:0,published:0}}
 fn pte_bits(&self)->u64 {3}
 fn with_pages(&mut self,r:Range<u64>,_:bool,_:bool,write:bool,mut cb:impl FnMut(u64,&[AtomicU64])->Result)->Result {
  self.walks+=1;let mut a=(r.start>>UAT_PGBIT) as usize;let end=(r.end>>UAT_PGBIT) as usize;
  while a<end {let b=end.min((a/2048+1)*2048);
   if self.missing!=Some(a/2048) {cb((a*UAT_PGSZ) as u64,&self.entries[a..b])?;if write{self.published+=1;}}
   a=b;
  }Ok(())
 }
 fn values(&self)->Vec<u64> {self.entries.iter().map(|e|e.load(Ordering::Relaxed)).collect()}
 FUNCTION
}
struct Sg {addr:usize,len:usize}
impl Sg {fn dma_address(&self)->usize {self.addr} fn dma_len(&self)->usize {self.len}}
struct Pages(Vec<u64>);impl Pages {fn push(&mut self,p:u64,_:())->Result {self.0.push(p);Ok(())}}
fn collect(sgt:&[Sg],mut offset:usize,mut left:usize)->Result<Vec<u64>> {
 let mut pages=Pages(Vec::new());
 COLLECT
 Ok(pages.0)
}
fn main(){
 for step in [1,2,3,9] {let mut sg=Vec::new();let mut flat=Vec::new();
  for i in 0..17 {let base=(i*23+1)*UAT_PGSZ;sg.push(Sg{addr:base,len:step*UAT_PGSZ});
   flat.extend((0..step).map(|j|(base+j*UAT_PGSZ)as u64));}
  for skip in 0..flat.len() {for count in [1,flat.len()-skip] {
   assert_eq!(collect(&sg,skip*UAT_PGSZ,count*UAT_PGSZ).unwrap(),flat[skip..skip+count]);
  }}
  assert_eq!(collect(&sg,flat.len()*UAT_PGSZ,UAT_PGSZ),Err(EFAULT));
  assert_eq!(collect(&sg,0,(flat.len()+1)*UAT_PGSZ),Err(EFAULT));
 }
 assert_eq!(collect(&[Sg{addr:0x4000,len:0x4000},Sg{addr:0x8001,len:0x4000}],0,0x8000),Err(EINVAL));
 assert_eq!(collect(&[Sg{addr:0x4000,len:0x4001}],0,0x4000),Err(EINVAL));
 assert_eq!(collect(&[Sg{addr:usize::MAX&!UAT_PGMSK,len:UAT_PGSZ}],0,UAT_PGSZ),Err(EOVERFLOW));
 for n in [1,2,2047,2048,2049,4096] {
  let begin=7;let mut t=UatPageTable::new(n+begin+4);
  let pages:Vec<u64>=(0..n).map(|i|(((i*1531)%8191+1)*UAT_PGSZ)as u64).collect();
  let range=((begin*UAT_PGSZ)as u64)..(((begin+n)*UAT_PGSZ)as u64);
  t.map_scattered_pages(range.clone(),&pages,Prot(0x400)).unwrap();
  assert_eq!(&t.values()[begin..begin+n],pages.iter().map(|p|p|0x403).collect::<Vec<_>>());
  assert_eq!(&t.values()[..begin],vec![0;begin]);assert_eq!(&t.values()[begin+n..],vec![0;4]);
  assert_eq!(t.walks,2);assert_eq!(t.published,(begin+n-1)/2048-begin/2048+1);
  // A collision in the last leaf must not install earlier leaves.
  let mut t=UatPageTable::new(n+begin+4);t.entries[begin+n-1].store(0xdead,Ordering::Relaxed);
  let before=t.values();assert_eq!(t.map_scattered_pages(range.clone(),&pages,Prot(0)),Err(EBUSY));
  assert_eq!(before,t.values());assert_eq!(t.published,0);
  // Bad source alignment, count, or range cannot publish even one PTE.
  for kind in 0..5 {let mut t=UatPageTable::new(n+begin+4);let mut p=pages.clone();let mut r=range.clone();
   match kind {0=>p[n-1]+=1,1=>{p.pop();},2=>p.push(0x4000),3=>r.start+=1,4=>r.end+=1,_=>unreachable!()}
   assert_eq!(t.map_scattered_pages(r,&p,Prot(0)),Err(EINVAL));assert!(t.values().iter().all(|x|*x==0));assert_eq!(t.published,0);
  }
 }
 let mut t=UatPageTable::new(4096);t.missing=Some(1);let p=vec![0x4000;4096];
 assert_eq!(t.map_scattered_pages(0..(4096*UAT_PGSZ)as u64,&p,Prot(0)),Err(EFAULT));
 assert!(t.values().iter().all(|x|*x==0));assert_eq!(t.published,0);
 for r in [0..0,0x8000..0x4000] {assert_eq!(t.map_scattered_pages(r,&[],Prot(0)),Err(EINVAL));}
 println!("PASS: production SG subrange collection and scattered mapping; exact PTEs, table boundaries, late collision, missing tables, source alignment/count, empty/reversed ranges, preflight before publication");
}
'''.replace('FUNCTION',function).replace('COLLECT',collect)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.rs').write_text(code)
 subprocess.run(['rustc','--edition=2021',str(p/'test.rs'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
