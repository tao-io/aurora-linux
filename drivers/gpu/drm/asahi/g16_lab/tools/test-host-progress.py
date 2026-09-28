#!/usr/bin/env python3
"""Run actual shared retirement counters against a deterministic host clock."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source',type=Path,default=Path('linux/linux-asahi/drivers/gpu/drm/asahi/agx_host_progress.rs'))
s=parser.parse_args().source.read_text()
body=s[s.index('pub(crate) struct Progress'):]
harness=r'''
use std::sync::{Arc,atomic::{AtomicU64,AtomicBool,Ordering}};
struct Monotonic;
impl Monotonic {fn ktime_get()->u64{static T:AtomicU64=AtomicU64::new(1);T.fetch_add(1,Ordering::SeqCst)}}
/*BODY*/
fn main(){
 let p=Arc::new(Progress::new());let generation=p.snapshot().0;
 assert!(generation>0);assert_eq!(p.snapshot(),(generation,0,0));
 let q=Progress::new();assert_ne!(q.snapshot().0,generation);
 // Boot is conservatively busy. Repeated markers cannot invent transitions.
 assert_eq!(p.activity_snapshot(),(generation,1));
 p.set_pending(true);assert_eq!(p.activity_snapshot().1,1);
 p.set_pending(false);assert_eq!(p.activity_snapshot().1,2);
 p.set_pending(false);assert_eq!(p.activity_snapshot().1,2);
 p.set_pending(true);assert_eq!(p.activity_snapshot().1,3);
 // Verified completion alone must not clear other outstanding work.
 p.record_completion();assert_eq!(p.activity_snapshot().1,3);
 p.set_pending(false);assert_eq!(p.activity_snapshot().1,4);
 let exhausted=Progress::new();
 exhausted.activity_epoch.store(u64::MAX,Ordering::Relaxed);
 exhausted.set_pending(false);assert_eq!(exhausted.activity_snapshot().1,u64::MAX);
 let barrier=Arc::new(std::sync::Barrier::new(2));
 let mark=p.clone();let sync=barrier.clone();
 let t=std::thread::spawn(move||{mark.set_pending(true);sync.wait();sync.wait();mark.set_pending(false);});
 barrier.wait();assert_eq!(p.activity_snapshot().1,5);barrier.wait();t.join().unwrap();
 assert_eq!(p.activity_snapshot().1,6);
 let done=Arc::new(AtomicBool::new(false));let writer=p.clone();let finished=done.clone();
 let thread=std::thread::spawn(move||{for _ in 0..100000 {writer.set_pending(true);writer.record_completion();writer.set_pending(false);}finished.store(true,Ordering::Release);});
 let(mut count,mut timestamp,mut epoch)=(0,0,6);
 while !done.load(Ordering::Acquire){
  let(g,c,t)=p.snapshot();assert_eq!(g,generation);assert!(c>=count);assert!(t>=timestamp);
  if c>0{assert!(t>=generation+c,"new count exposed before its timestamp");}
  let (ag,e)=p.activity_snapshot();assert_eq!(ag,generation);assert!(e>=epoch);epoch=e;
  count=c;timestamp=t;
 }
 thread.join().unwrap();assert_eq!(p.snapshot().1,100001);assert_eq!(p.activity_snapshot().1,200006);
 println!("PASS generation, retirement, busy/idle transitions, saturation and concurrent snapshots");
}
'''.replace('/*BODY*/',body)
with tempfile.TemporaryDirectory(prefix='host-progress-') as tmp:
 p=Path(tmp)/'test.rs';p.write_text(harness);exe=p.with_suffix('')
 subprocess.run(['rustc','--edition=2021',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
