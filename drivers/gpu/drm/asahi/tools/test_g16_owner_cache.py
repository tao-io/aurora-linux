#!/usr/bin/env python3
"""Exercise the production retired-owner cache, including Drop and allocation failure."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parent.parent
source=root/'g16_owner_cache.rs'
if not source.is_file():
    source=root/'linux/linux-asahi/drivers/gpu/drm/asahi/g16_owner_cache.rs'
code=r'''
extern crate self as kernel;
pub mod prelude { pub use crate::{KVec,GFP_KERNEL}; }
pub const GFP_KERNEL: () = ();
static FAIL: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    pub fn new()->Self {Self(Vec::new())}
    pub fn iter(&self)->std::slice::Iter<'_,T>{self.0.iter()}
    pub fn remove(&mut self,n:usize)->Result<T,()>{if n<self.0.len(){Ok(self.0.remove(n))}else{Err(())}}
    pub fn push(&mut self,v:T,_:())->Result<(),()>{
        if FAIL.swap(false,std::sync::atomic::Ordering::Relaxed){Err(())}else{self.0.push(v);Ok(())}
    }
}
#[path="SOURCE"] mod production;
use production::{OwnerCache,CACHE_BYTES};
use std::sync::{Arc,atomic::{AtomicUsize,Ordering}};
struct Owner { id:usize, bytes:usize, live:Arc<AtomicUsize> }
impl Owner {
    fn new(id:usize,bytes:usize,live:&Arc<AtomicUsize>)->Self {
        live.fetch_add(bytes,Ordering::Relaxed); Self{id,bytes,live:live.clone()}
    }
}
impl Drop for Owner {fn drop(&mut self){self.live.fetch_sub(self.bytes,Ordering::Relaxed);}}
fn main(){
    let live=Arc::new(AtomicUsize::new(0));
    let mut cache=OwnerCache::new();
    for id in 0..10000 {
        let bytes=(1+id%20)*1024*1024;
        cache.put(Owner::new(id,bytes,&live),bytes);
        assert!(live.load(Ordering::Relaxed)<=CACHE_BYTES);
        if id%7==0 {let x=cache.take(|x:&Owner|x.id==id).unwrap();assert_eq!(x.id,id);drop(x);}
    }
    drop(cache);assert_eq!(live.load(Ordering::Relaxed),0);
    let mut cache=OwnerCache::new();
    let half=CACHE_BYTES/2;
    for id in 0..3 {cache.put(Owner::new(id,half,&live),half);}
    assert!(cache.take(|x:&Owner|x.id==0).is_none());
    drop(cache.take(|x:&Owner|x.id==1).unwrap());
    assert_eq!(live.load(Ordering::Relaxed),half);
    FAIL.store(true,Ordering::Relaxed);
    cache.put(Owner::new(3,half,&live),half);
    assert_eq!(live.load(Ordering::Relaxed),half);
    // An oversized owner bypasses the cache without evicting useful entries.
    cache.put(Owner::new(4,CACHE_BYTES+1,&live),CACHE_BYTES+1);
    assert_eq!(live.load(Ordering::Relaxed),half);
    drop(cache.take(|x:&Owner|x.id==2).unwrap());
    cache.put(Owner::new(5,1,&live),usize::MAX);
    assert_eq!(live.load(Ordering::Relaxed),0);
    cache.put(Owner::new(6,CACHE_BYTES,&live),CACHE_BYTES);
    drop(cache);
    assert_eq!(live.load(Ordering::Relaxed),0);
    println!("PASS: production cache bounds, FIFO eviction, take accounting, oversized owners, allocation failure and Drop");
}
'''.replace('SOURCE',str(source))
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/'cache.rs';exe=Path(d)/'cache'
    src.write_text(code)
    subprocess.run(['rustc','--edition=2021',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
