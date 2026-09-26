#!/usr/bin/env python3
"""Run production Packet::finish with modeled fence and VM guard actuators."""
from pathlib import Path
import subprocess
import tempfile

text = (Path(__file__).resolve().parents[1] / 'g16_submit.rs').read_text()
start = text.index('    pub(crate) fn finish(&self, result: Result) {')
end = text.index('{', start) + 1
depth = 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
finish = text[start:end]
source = r'''
#![allow(dead_code,unused_variables)]
use std::sync::{Arc,atomic::{AtomicBool,AtomicI32,AtomicUsize,Ordering}};
#[derive(Clone,Copy)]struct Error(i32);impl Error{fn to_errno(self)->i32{self.0}}
type Result=std::result::Result<(),Error>;
macro_rules! pr_info {($($t:tt)*)=>{()}}
mod debug {pub enum DebugFlags{SubmitTiming}pub fn debug_enabled(_:DebugFlags)->bool{false}}
mod g16_memory {pub fn publish(){}}
mod kernel {pub mod time {pub struct Monotonic;pub trait ClockSource{fn ktime_get()->i64;}
 impl ClockSource for Monotonic{fn ktime_get()->i64{0}}}}
struct Mutex<T>(std::sync::Mutex<T>);impl<T>Mutex<T>{fn lock(&self)->std::sync::MutexGuard<'_,T>{self.0.lock().unwrap()}}
struct Guard(Arc<AtomicUsize>);impl Drop for Guard{fn drop(&mut self){assert_eq!(self.0.fetch_sub(1,Ordering::SeqCst),1);}}
struct Vm;impl Vm{fn status(&self)->&Self{self}fn record(&self,_:i32){}}
struct Completion{signals:AtomicUsize,error:AtomicI32,active:Arc<AtomicUsize>}
impl Completion {
 fn set_error(&self,e:Error){self.error.store(e.0,Ordering::SeqCst);}
 fn signal(&self){
  // Check ownership at the precise point userspace can observe completion.
  let expected=usize::from(self.error.load(Ordering::SeqCst)!=0);
  assert_eq!(self.active.load(Ordering::SeqCst),expected);
  self.signals.fetch_add(1,Ordering::SeqCst);
 }
}
struct Packet {id:u64,vm:Vm,completion:Completion,finish_claimed:AtomicBool,
 failure:AtomicI32,vm_job:Mutex<Option<Guard>>}
impl Packet {
 fn new()->Self{let active=Arc::new(AtomicUsize::new(1));Self{id:0,vm:Vm,
  completion:Completion{signals:AtomicUsize::new(0),error:AtomicI32::new(0),active:active.clone()},
  finish_claimed:AtomicBool::new(false),failure:AtomicI32::new(0),
  vm_job:Mutex(std::sync::Mutex::new(Some(Guard(active))))}}
''' + finish + r'''
}
#[test]fn success_retires_before_signal_while_packet_stays_alive(){
 let p=Packet::new();p.finish(Ok(()));assert!(p.vm_job.lock().is_none());
 assert_eq!(p.completion.signals.load(Ordering::SeqCst),1);
 p.finish(Err(Error(-5)));assert_eq!(p.completion.error.load(Ordering::SeqCst),0);
 assert_eq!(p.completion.signals.load(Ordering::SeqCst),1);
}
#[test]fn failure_keeps_guard_and_late_success_cannot_release_quarantine(){
 let p=Packet::new();let active=p.completion.active.clone();p.finish(Err(Error(-5)));
 assert!(p.vm_job.lock().is_some());p.finish(Ok(()));
 assert_eq!(active.load(Ordering::SeqCst),1);
 assert_eq!(p.completion.signals.load(Ordering::SeqCst),1);
 assert_eq!(p.failure.load(Ordering::SeqCst),-5);
 drop(p);assert_eq!(active.load(Ordering::SeqCst),0);
}
#[test]fn concurrent_success_has_one_retirement_and_signal(){
 let p=Arc::new(Packet::new());let threads:Vec<_>=(0..8).map(|_|{
  let p=p.clone();std::thread::spawn(move||p.finish(Ok(())))
 }).collect();for t in threads{t.join().unwrap();}
 assert_eq!(p.completion.signals.load(Ordering::SeqCst),1);
 assert_eq!(p.completion.active.load(Ordering::SeqCst),0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'test.rs'
    path.write_text(source)
    binary = Path(directory) / 'test'
    subprocess.run(['rustc', '--edition=2021', '--test', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
