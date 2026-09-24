// SPDX-License-Identifier: GPL-2.0-only OR MIT
#[path="g16_config_layout.rs"] mod layout;
mod g16_initdata;
use layout::{Object,OBJECTS};
use g16_initdata::RootPointers;
use std::io::Write;
/* IMPORTS */
#[derive(Debug,PartialEq)] enum Error {Invalid,Bounds,Busy,Io}
type Result<T=()> = std::result::Result<T,Error>;
const EINVAL:Error=Error::Invalid;
const EOVERFLOW:Error=Error::Bounds;
const EBUSY:Error=Error::Busy;
const EIO:Error=Error::Io;
struct Buffer {address:u64,gpu:u64,bytes:Vec<u8>}
impl Buffer {
    fn new(address:u64,gpu:u64,size:usize)->Self {Self{address,gpu,bytes:vec![0;size]}}
    fn va(&self)->u64 {self.address}
    fn gpu_va(&self)->Result<u64> {Ok(self.gpu)}
    fn write(&mut self,offset:usize,bytes:&[u8])->Result {let end=offset.checked_add(bytes.len()).ok_or(EOVERFLOW)?;self.bytes.get_mut(offset..end).ok_or(EOVERFLOW)?.copy_from_slice(bytes);Ok(())}
    fn u32(&mut self,offset:usize,value:u32)->Result {self.write(offset,&value.to_le_bytes())}
    fn u64(&mut self,offset:usize,value:u64)->Result {self.write(offset,&value.to_le_bytes())}
    fn read_u32(&mut self,offset:usize)->Result<u32> {Ok(u32::from_le_bytes(self.bytes.get(offset..offset+4).ok_or(EOVERFLOW)?.try_into().unwrap()))}
}
mod g16_memory {pub const TIMESTAMP_RANGE:std::ops::Range<u64>=0xfffffc3000000000..0xfffffc3004000000;pub fn publish() {}}
mod g16_power {
    /* POWER TABLES */
    pub fn requested_state()->super::Result<u32> {let v:u32=std::env::args().nth(1).unwrap().parse().unwrap();Ok(1+v%9)}
}
mod g16_device {
    pub struct Device(pub std::cell::Cell<u32>);
    impl Device {pub fn set_power_generation(&self,n:u32)->super::Result {self.0.set(n);Ok(())}}
}
struct Config {objects:Vec<Buffer>,pipes:Vec<Buffer>,iomap_bytes:[u8;layout::IO_TABLE_SIZE]}
impl Config {
/* METHODS */
}
fn snapshot(c:&Config,out:&mut impl Write) {
    for b in c.objects.iter().chain(c.pipes.iter()) {out.write_all(&b.address.to_le_bytes()).unwrap();out.write_all(&b.gpu.to_le_bytes()).unwrap();out.write_all(&b.bytes).unwrap();}
}
fn main() {
    let v:u64=std::env::args().nth(1).unwrap().parse().unwrap();let b=0xfffffc2040000000+v*0x1000000;
    let mut c=Config{objects:OBJECTS.into_iter().enumerate().map(|(i,o)|Buffer::new(b+i as u64*0x100000,0x7100000000+v*0x4000000+i as u64*0x100000,o.size())).collect(),pipes:(0..24).map(|i|Buffer::new(b+0x3000000+i*0x4000,0,if i%2==0 {0x30}else{0x1800})).collect(),iomap_bytes:core::array::from_fn(|i|(i as u8).wrapping_add(v as u8))};
    c.initialize(b+0x4000000).unwrap();let mut out=std::io::stdout();snapshot(&c,&mut out);
    let mut generation=0xfffffffe;let device=g16_device::Device(std::cell::Cell::new(0));
    for initial in [0,254,255] {
        for opcode in [0x1a,0x34,0x0a] {
            for off in [0,0x10,0x20] {c.object(Object::DevctrlState).u32(off,initial).unwrap();}
            let result=c.enqueue_control(opcode,v as u32,&mut generation,&device).unwrap();assert_eq!(result,(initial+1)%256);
            out.write_all(&result.to_le_bytes()).unwrap();out.write_all(&generation.to_le_bytes()).unwrap();assert_eq!(generation,device.0.get());snapshot(&c,&mut out);
            assert_eq!(c.enqueue_control(opcode,0,&mut generation,&device),Err(EBUSY));
        }
    }
    assert_eq!(c.enqueue_control(0xff,0,&mut generation,&device),Err(EINVAL));
    c.object(Object::DevctrlState).u32(0x20,256).unwrap();assert_eq!(c.control_indices(),Err(EIO));
    out.write_all(&c.consumed_generation().unwrap().to_le_bytes()).unwrap();snapshot(&c,&mut out);
}
