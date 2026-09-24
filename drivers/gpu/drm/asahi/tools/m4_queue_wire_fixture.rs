// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Host adapter: owned byte buffers and an ordered write/publication journal.
//! Does not emulate DMA, locking, firmware execution or ASID lifetime.
#![allow(dead_code)]
extern crate self as kernel;
pub type Result<T=()> = std::result::Result<T,i32>;
pub const EINVAL:i32=22;pub const EBUSY:i32=16;pub const EIO:i32=5;pub const EOVERFLOW:i32=75;
pub mod prelude {pub use crate::{Result,EINVAL,EBUSY,EIO,EOVERFLOW};}
static ADDRESS:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
static JOURNAL:std::sync::Mutex<Vec<u8>>=std::sync::Mutex::new(Vec::new());
fn record(kind:u8,address:u64,offset:usize,bytes:&[u8]) {
    let mut log=JOURNAL.lock().unwrap();log.push(kind);log.extend(address.to_le_bytes());
    log.extend((offset as u64).to_le_bytes());log.extend((bytes.len() as u64).to_le_bytes());log.extend(bytes);
}
mod driver {pub struct AsahiDevice;}
mod mmu {
    pub struct Vm {pub slot:u32} pub struct VmBind {slot:u32}
    impl VmBind {pub fn slot(&self)->u32 {self.slot}}
    pub struct Uat {pub vm:Vm}
    impl Uat {pub fn bind(&self,vm:&Vm)->crate::Result<VmBind>{Ok(VmBind{slot:vm.slot})} pub fn kernel_vm(&self)->&Vm{&self.vm}}
}
mod g16_memory {
    use crate::*;
    pub struct Buffer {address:u64,bytes:Vec<u8>}
    impl Buffer {
        pub fn new(_: &driver::AsahiDevice,_:&mmu::Vm,size:usize)->Result<Self> {
            let address=ADDRESS.fetch_add(((size+0x3fff)&!0x3fff) as u64+0x4000,std::sync::atomic::Ordering::SeqCst);
            let bytes=vec![0;size];record(0,address,0,&bytes);Ok(Self{address,bytes})
        }
        pub fn va(&self)->u64{self.address}
        pub fn write(&mut self,offset:usize,bytes:&[u8])->Result {
            let target=self.bytes.get_mut(offset..offset.checked_add(bytes.len()).ok_or(EOVERFLOW)?).ok_or(EINVAL)?;
            target.copy_from_slice(bytes);record(1,self.address,offset,bytes);Ok(())
        }
        pub fn read(&mut self,offset:usize,bytes:&mut[u8])->Result {
            let source=self.bytes.get(offset..offset.checked_add(bytes.len()).ok_or(EOVERFLOW)?).ok_or(EINVAL)?;
            bytes.copy_from_slice(source);record(2,self.address,offset,bytes);Ok(())
        }
        pub fn u32(&mut self,offset:usize,value:u32)->Result{self.write(offset,&value.to_le_bytes())}
        pub fn u64(&mut self,offset:usize,value:u64)->Result{self.write(offset,&value.to_le_bytes())}
        pub fn read_u32(&mut self,offset:usize)->Result<u32>{let mut b=[0;4];self.read(offset,&mut b)?;Ok(u32::from_le_bytes(b))}
    }
    pub fn publish(){record(3,0,0,&[]);}
}
#[path = "../g16_compute.rs"] mod g16_compute;
#[path = "../queue-host.rs"] mod queue;
fn main(){
    let variant:u64=std::env::args().nth(1).unwrap().parse().unwrap();assert!(variant<64);
    ADDRESS.store(0xfffffc2000000000+variant*0x1000000,std::sync::atomic::Ordering::SeqCst);
    queue::exercise(variant);
    use std::io::Write;std::io::stdout().write_all(&JOURNAL.lock().unwrap()).unwrap();
}
