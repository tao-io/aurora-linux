#!/usr/bin/env python3
"""Exercise the actual G16 DRM timeout callback against controlled runtime states.

No GPU access. Checks completion racing a queued timeout, a newly started
successor, admission delay, a real stalled command, and removed hardware.
The mock models hardware observations, not the timeout callback being tested.
"""
from pathlib import Path
import subprocess
import tempfile
source=Path('linux/linux-asahi/drivers/gpu/drm/asahi/g16_submit.rs').read_text()
start=source.index('    fn timed_out(')
end=source.index('    fn cancel(',start)
callback=source[start:end]
harness=r'''
use std::{ops::{Deref,DerefMut},sync::{Arc,Mutex,MutexGuard,atomic::{AtomicUsize,Ordering}}};
const ETIMEDOUT:i32=-110;
const ENODEV:i32=-19;
macro_rules! dev_info {($($t:tt)*)=>{()};}
#[derive(Clone,Copy,Debug)] enum Observation {Retired, FreshSuccessor, PendingAdmission, Hung}
struct Runtime {observation:Observation,healthy:bool, polls:usize, faults:usize, failed:Vec<u64>}
impl Runtime {
 fn service_job(&mut self) {self.polls+=1;if matches!(self.observation,Observation::Hung){self.healthy=false;self.faults+=1;}}
 fn healthy(&self)->bool {self.healthy}
 fn fail_job(&mut self,id:u64,_error:i32){self.healthy=false;self.failed.push(id);}
}
struct Shared(Arc<Mutex<Option<Runtime>>>);
impl Shared {fn lock(&self)->MutexGuard<'_,Option<Runtime>> {self.0.lock().unwrap()}}
struct Packet{id:u64, finished:std::cell::Cell<Option<i32>>}
impl Packet {fn finish(&self,result:Result<(),i32>){self.finished.set(result.err());}}
struct TestJob{shared:Shared,packet:Packet,dev:Arc<AtomicUsize>}
mod sched {
 use super::*;
 #[derive(Debug,PartialEq)] pub enum Status {NoDevice,NoHang}
 pub struct Job<T>(pub T);
 impl<T>Deref for Job<T>{type Target=T;fn deref(&self)->&T{&self.0}}
 impl<T>DerefMut for Job<T>{fn deref_mut(&mut self)->&mut T{&mut self.0}}
}
mod driver {use super::*;pub fn queue_g16_completion_worker(dev:Arc<AtomicUsize>){dev.fetch_add(1,Ordering::SeqCst);}}
impl TestJob {
/*CALLBACK*/
}
fn main(){
 for observation in [Observation::Retired,Observation::FreshSuccessor,Observation::PendingAdmission,Observation::Hung] {
  let mut job=sched::Job(TestJob{shared:Shared(Arc::new(Mutex::new(Some(Runtime{observation,healthy:true,polls:0,faults:0,failed:vec![]})))),packet:Packet{id:50077,finished:std::cell::Cell::new(None)},dev:Arc::new(AtomicUsize::new(0))});
  let status=TestJob::timed_out(&mut job);
  let locked=job.shared.lock();let runtime=locked.as_ref().unwrap();
  assert_eq!(runtime.polls,1,"must observe hardware after acquiring runtime lock");
  if matches!(observation,Observation::Hung){
   assert_eq!(status,sched::Status::NoDevice);assert_eq!(runtime.faults,1);assert_eq!(runtime.failed,vec![50077]);assert_eq!(job.dev.load(Ordering::SeqCst),0);
  }else{
   assert_eq!(status,sched::Status::NoHang);assert!(runtime.healthy && runtime.failed.is_empty());assert_eq!(runtime.faults,0);assert_eq!(job.dev.load(Ordering::SeqCst),1);
  }
  println!("PASS {observation:?}");
 }
 let mut job=sched::Job(TestJob{shared:Shared(Arc::new(Mutex::new(None))),packet:Packet{id:7,finished:std::cell::Cell::new(None)},dev:Arc::new(AtomicUsize::new(0))});
 assert_eq!(TestJob::timed_out(&mut job),sched::Status::NoDevice);
 assert_eq!(job.dev.load(Ordering::SeqCst),0);assert_eq!(job.packet.finished.get(),Some(ENODEV));println!("PASS removed hardware finishes packet");
}
'''.replace('/*CALLBACK*/',callback)
with tempfile.TemporaryDirectory(prefix='g16-timeout-') as directory:
 path=Path(directory)/'test.rs';path.write_text(harness)
 binary=Path(directory)/'test'
 subprocess.run(['rustc','--edition=2021',str(path),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
