#!/usr/bin/env python3
"""Exercise the real M4 global-failure handler against retained packet owners."""
from pathlib import Path
import subprocess
import tempfile
source = Path('linux/linux-asahi/drivers/gpu/drm/asahi/g16_runtime.rs').read_text()
start = source.index('    fn fail_all(')
end = source.index('\n    /// Poll one completion', start)
callback = source[start:end]
harness = r'''
use std::{mem::ManuallyDrop,sync::{Arc,atomic::{AtomicBool,AtomicI32,AtomicUsize,Ordering}}};
type Error=i32;
struct Health(AtomicBool);
impl Health {fn mark_failed(&self){self.0.store(true,Ordering::SeqCst);}}
struct State {health:Health}
struct Packet {error:AtomicI32,signals:AtomicUsize}
impl Packet {
 fn finish(&self,e:Result<(),Error>){
  let e=e.unwrap_err();
  if self.error.compare_exchange(0,e,Ordering::SeqCst,Ordering::SeqCst).is_ok(){self.signals.fetch_add(1,Ordering::SeqCst);}
 }
 fn new()->Arc<Self>{Arc::new(Self{error:AtomicI32::new(0),signals:AtomicUsize::new(0)})}
}
struct Active {packet:Arc<Packet>,drops:Arc<AtomicUsize>}
impl Drop for Active {fn drop(&mut self){self.drops.fetch_add(1,Ordering::SeqCst);}}
struct Inner {state:State,inflight:Vec<Active>,pending:Vec<Vec<Active>>}
struct Runtime {inner:ManuallyDrop<Inner>}
impl Runtime {
/*CALLBACK*/
}
fn main(){
 let a=Packet::new();let b=Packet::new();let c=Packet::new();
 let live_drops=Arc::new(AtomicUsize::new(0));let pending_drops=Arc::new(AtomicUsize::new(0));
 let mut r=Runtime{inner:ManuallyDrop::new(Inner{state:State{health:Health(AtomicBool::new(false))},
  inflight:vec![Active{packet:a.clone(),drops:live_drops.clone()},Active{packet:a.clone(),drops:live_drops.clone()},Active{packet:b.clone(),drops:live_drops.clone()}],
  pending:vec![vec![Active{packet:c.clone(),drops:pending_drops.clone()}]]})};
 r.fail_all(-5);
 assert!(r.inner.state.health.0.load(Ordering::SeqCst));
 for p in [&a,&b,&c] {assert_eq!(p.error.load(Ordering::SeqCst),-5);assert_eq!(p.signals.load(Ordering::SeqCst),1);}
 assert_eq!(r.inner.inflight.len(),3);assert_eq!(live_drops.load(Ordering::SeqCst),0,"active DMA owners released");
 assert!(r.inner.pending.is_empty());assert_eq!(pending_drops.load(Ordering::SeqCst),1);
 println!("PASS global failure wakes all packets and retains published DMA owners");
 r.fail_all(-110);
 for p in [&a,&b,&c] {assert_eq!(p.error.load(Ordering::SeqCst),-5);assert_eq!(p.signals.load(Ordering::SeqCst),1);}
 assert_eq!(live_drops.load(Ordering::SeqCst),0);println!("PASS repeated cancellation preserves primary packet errors");
 // Simulate a separate proven hardware-stop event before destroying the graph.
 unsafe{ManuallyDrop::drop(&mut r.inner)};
 assert_eq!(live_drops.load(Ordering::SeqCst),3);
}
'''.replace('/*CALLBACK*/',callback)
with tempfile.TemporaryDirectory(prefix='g16-failure-') as directory:
    source = Path(directory)/'test.rs'; source.write_text(harness)
    binary = Path(directory)/'test'
    subprocess.run(['rustc','--edition=2021',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
