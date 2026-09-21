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
impl<T> std::ops::Deref for KVec<T> { type Target=[T]; fn deref(&self)->&[T]{&self.0} }
impl<'a,T> IntoIterator for &'a KVec<T> {type Item=&'a T;type IntoIter=std::slice::Iter<'a,T>;fn into_iter(self)->Self::IntoIter{self.0.iter()} }
impl<T> KVec<T> {
    pub fn new()->Self {Self(Vec::new())}
    pub fn iter(&self)->std::slice::Iter<'_,T>{self.0.iter()}
    pub fn remove(&mut self,n:usize)->Result<T,()>{if n<self.0.len(){Ok(self.0.remove(n))}else{Err(())}}
    pub fn push(&mut self,v:T,_:())->Result<(),()>{
        if FAIL.swap(false,std::sync::atomic::Ordering::Relaxed){Err(())}else{self.0.push(v);Ok(())}
    }
}
#[path="SOURCE"] mod production;
use production::{OwnerCache,CACHE_BYTES,GLOBAL_CACHE_BYTES};
use std::sync::{Arc,atomic::{AtomicUsize,Ordering}};
struct Owner { id:usize, bytes:usize, live:Arc<AtomicUsize> }
impl Owner {
    fn new(id:usize,bytes:usize,live:&Arc<AtomicUsize>)->Self {
        live.fetch_add(bytes,Ordering::Relaxed); Self{id,bytes,live:live.clone()}
    }
}
impl Drop for Owner {fn drop(&mut self){self.live.fetch_sub(self.bytes,Ordering::Relaxed);}}
#[derive(Clone)]
struct Handle(std::rc::Rc<std::cell::RefCell<Option<OwnerCache<Owner>>>>);
impl Handle {
    fn new()->Self{Self(std::rc::Rc::new(std::cell::RefCell::new(Some(OwnerCache::new()))))}
    fn put(&self, owner:Owner){let bytes=owner.bytes;self.0.borrow_mut().as_mut().unwrap().put(owner,bytes);}
}
impl production::CacheControl for Handle {
    fn same(&self,other:&Self)->bool{std::rc::Rc::ptr_eq(&self.0,&other.0)}
    fn closed(&self)->bool{self.0.borrow().is_none()}
    fn trim_for(&self,bytes:usize){if let Some(c)=self.0.borrow_mut().as_mut(){c.trim_for(bytes);}}
    fn evict_one(&self)->bool{self.0.borrow_mut().as_mut().is_some_and(|c|c.evict_one())}
}
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
    // Independent queues share one idle budget. Taking or destroying an
    // owner must return its charge even if another queue uses the space.
    assert_eq!(GLOBAL_CACHE_BYTES, 2*CACHE_BYTES);
    let mut a=OwnerCache::new();let mut b=OwnerCache::new();let mut c=OwnerCache::new();
    a.put(Owner::new(10,CACHE_BYTES,&live),CACHE_BYTES);
    b.put(Owner::new(11,CACHE_BYTES,&live),CACHE_BYTES);
    c.put(Owner::new(12,CACHE_BYTES,&live),CACHE_BYTES);
    assert!(c.take(|_:&Owner|true).is_none());
    assert_eq!(live.load(Ordering::Relaxed),GLOBAL_CACHE_BYTES);
    drop(a.take(|_:&Owner|true).unwrap());
    FAIL.store(true,Ordering::Relaxed);
    c.put(Owner::new(13,CACHE_BYTES,&live),CACHE_BYTES);
    assert_eq!(live.load(Ordering::Relaxed),CACHE_BYTES);
    c.put(Owner::new(14,CACHE_BYTES,&live),CACHE_BYTES);
    assert_eq!(live.load(Ordering::Relaxed),GLOBAL_CACHE_BYTES);
    drop(b);drop(c);drop(a);
    assert_eq!(live.load(Ordering::Relaxed),0);
    let mut d=OwnerCache::new();d.put(Owner::new(15,CACHE_BYTES,&live),CACHE_BYTES);
    assert!(d.take(|_:&Owner|true).is_some());drop(d);
    assert_eq!(live.load(Ordering::Relaxed),0);
    assert_eq!(production::statistics()[0],0);
    let mut registry=production::Registry::new();
    let a=Handle::new();let b=Handle::new();let c=Handle::new();
    for (i,cache) in [&a,&b,&c].into_iter().enumerate(){
        assert!(registry.prepare(cache.clone(),CACHE_BYTES));
        cache.put(Owner::new(20+i,CACHE_BYTES,&live));
        assert!(live.load(Ordering::Relaxed)<=GLOBAL_CACHE_BYTES);
    }
    assert!(a.0.borrow_mut().as_mut().unwrap().take(|_|true).is_none());
    assert_eq!(live.load(Ordering::Relaxed),GLOBAL_CACHE_BYTES);
    // The active cache can replace itself without evicting another queue.
    let evictions=production::statistics()[4];
    assert!(registry.prepare(c.clone(),CACHE_BYTES));
    c.put(Owner::new(23,CACHE_BYTES,&live));
    assert_eq!(production::statistics()[4],evictions+1);
    assert!(b.0.borrow_mut().as_mut().unwrap().take(|_|true).is_some());
    // Closing a queue releases backing even while the registry owns a handle.
    c.0.borrow_mut().take();
    assert_eq!(live.load(Ordering::Relaxed),0);
    assert!(!registry.prepare(c.clone(),CACHE_BYTES));
    assert!(!registry.prepare(a.clone(),usize::MAX));
    FAIL.store(true,Ordering::Relaxed);
    assert!(!registry.prepare(a.clone(),CACHE_BYTES));
    drop(registry);drop(a);drop(b);drop(c);
    assert_eq!(production::statistics()[0],0);
    println!("PASS: cache LRU admits new queues, self eviction, close releases registered backing, allocation failure");
    println!("PASS: device-wide budget, independent queues, take/Drop budget return, allocation-failure budget return;");
    println!("PASS: production cache bounds, FIFO eviction, take accounting, oversized owners, allocation failure and Drop");
}
'''.replace('SOURCE',str(source))
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/'cache.rs';exe=Path(d)/'cache'
    src.write_text(code)
    subprocess.run(['rustc','--edition=2021',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
