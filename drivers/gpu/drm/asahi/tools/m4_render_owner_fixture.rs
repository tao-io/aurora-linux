// SPDX-License-Identifier: GPL-2.0-only OR MIT
mod g16_render;
mod g16_compute;
#[path="g16_render_command.rs"] mod wire;
mod g16_attachments;
use g16_render::Stage;
use std::io::Write;
/* IMPORTS */
#[derive(Debug)] enum Error {Invalid,Bounds}
impl From<std::num::TryFromIntError> for Error {fn from(_:std::num::TryFromIntError)->Self {Self::Bounds}}
type Result<T=()> = std::result::Result<T,Error>;
const EINVAL:Error=Error::Invalid;
mod g16_memory {pub fn publish() {}}
struct Buffer {address:u64,bytes:Vec<u8>}
impl Buffer {
    fn new(address:u64)->Self {Self{address,bytes:(0..0x4000).map(|i|(i^(address as usize>>14)) as u8).collect()}}
    fn va(&self)->u64 {self.address}
    fn write(&mut self,offset:usize,bytes:&[u8])->Result {let end=offset.checked_add(bytes.len()).ok_or(Error::Bounds)?;self.bytes.get_mut(offset..end).ok_or(Error::Bounds)?.copy_from_slice(bytes);Ok(())}
    fn read(&mut self,offset:usize,bytes:&mut[u8])->Result {let end=offset.checked_add(bytes.len()).ok_or(Error::Bounds)?;bytes.copy_from_slice(self.bytes.get(offset..end).ok_or(Error::Bounds)?);Ok(())}
    fn u32(&mut self,offset:usize,value:u32)->Result {self.write(offset,&value.to_le_bytes())}
    fn u64(&mut self,offset:usize,value:u64)->Result {self.write(offset,&value.to_le_bytes())}
    fn read_u32(&mut self,offset:usize)->Result<u32> {let mut b=[0;4];self.read(offset,&mut b)?;Ok(u32::from_le_bytes(b))}
    fn read_u64(&mut self,offset:usize)->Result<u64> {let mut b=[0;8];self.read(offset,&mut b)?;Ok(u64::from_le_bytes(b))}
}
struct Parameters {manager:Buffer}
impl Parameters {fn lock(&mut self)->&mut Self {self}}
struct Render {commands:[Buffer;2],sequences:[Buffer;2],metadata:Buffer,uma:Buffer,
    stage:Stage,parameters:Parameters,compute_dependency:bool,init_parameters:bool,stamp:u32}
impl Render {
/* METHODS */
    fn snapshot(&self,out:&mut impl Write) {
        for b in self.commands.iter().chain(self.sequences.iter()).chain([&self.metadata,&self.uma,&self.parameters.manager]) {
            out.write_all(&b.address.to_le_bytes()).unwrap();out.write_all(&b.bytes).unwrap();
        }
    }
}
fn main() {
    let variant:u64=std::env::args().nth(1).unwrap().parse().unwrap();let b=0xfffffc2040000000+variant*0x100000;
    let mut r=Render{commands:[Buffer::new(b),Buffer::new(b+0x4000)],sequences:[Buffer::new(b+0x8000),Buffer::new(b+0xc000)],metadata:Buffer::new(b+0x10000),uma:Buffer::new(b+0x14000),stage:Stage::Tiling,parameters:Parameters{manager:Buffer::new(b+0x18000)},compute_dependency:false,init_parameters:variant%2==0,stamp:0x100+variant as u32*0x100};
    for (i,stage) in [Stage::Tiling,Stage::Fragment].into_iter().enumerate() {
        let a=wire::Args{command:r.commands[i].va(),sku:r.sequences[i].va(),queue:b+0x20000,stats:b+0x24000,pb_slot:r.metadata.va(),manager:b+0x18000,pb_aux:b+0x10100,notifier:b+0x28000,shared_stamp:b+0x2c000,stamp_address:b+0x30000,uma:r.uma.va(),uma_aux:0x7100000000,timestamp_storage:b+0x10600+i as u64*0x20,context:1,generation:1,counter:variant+1,uuid:variant as u32,stamp:r.stamp,event_slot:0x70+i as u32,fragment_slot:0x71,fragment_stamp:r.stamp,pb_id:2,pass_id:0};
        wire::encode(&mut r.commands[i].bytes,&mut r.sequences[i].bytes,stage,a).unwrap();
    }
    let mut out=std::io::stdout();r.snapshot(&mut out);
    r.request_completion_flush().unwrap();r.snapshot(&mut out);
    let attachment=g16_attachments::Attachments::new(&[(0x710001007f+variant*0x4000,0x8100)]).unwrap();
    r.set_attachments(&[attachment,g16_attachments::Attachments::EMPTY]).unwrap();r.snapshot(&mut out);
    assert!(r.compute_dependency().is_none());r.wait_for_prior(b+0x34000,15,0x100).unwrap();
    out.write_all(&r.compute_dependency().unwrap().to_le_bytes()).unwrap();
    let ptr=r.wait_for_prior_fragment(b+0x38000,0x71,0x200).unwrap();out.write_all(&ptr.to_le_bytes()).unwrap();
    out.write_all(&r.init_address().unwrap_or(0).to_le_bytes()).unwrap();out.write_all(&r.fragment_dependency().to_le_bytes()).unwrap();r.snapshot(&mut out);
    for stage in [Stage::Tiling,Stage::Fragment] {r.stage=stage;for value in r.status().unwrap() {out.write_all(&value.to_le_bytes()).unwrap();}}
    out.write_all(&r.uma_completed().unwrap().to_le_bytes()).unwrap();
    for pairs in [[[0,0],[0,0]],[[b+0x3c000,b+0x3c008],[b+0x40000,b+0x40008]]] {r.set_user_timestamps(pairs).unwrap();r.snapshot(&mut out);}
    for pair in r.timestamps().unwrap() {for value in pair {out.write_all(&value.to_le_bytes()).unwrap();}}
    let original=r.sequences[1].bytes.clone();
    for count in [0,1,8] {r.sequences[1].bytes.copy_from_slice(&original);let pointers=core::array::from_fn(|i|if i<count {b+0x44000+i as u64*16}else{0});r.set_fragment_timestamp_copies(pointers).unwrap();r.snapshot(&mut out);}
    r.sequences[1].bytes.copy_from_slice(&original);r.omit_internal_timestamps([true,true]).unwrap();r.snapshot(&mut out);
}
