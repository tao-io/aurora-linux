#!/usr/bin/env python3
"""Compile and exercise the production M4 packet split loop without hardware.

The shell around that loop substitutes Vec for allocation/fence ownership.
All packet boundaries and per-engine limits come from g16_submit.rs itself.
"""
from pathlib import Path
import re
import subprocess
import tempfile
base = Path(__file__).resolve().parent.parent
if not (base/'g16_submit.rs').is_file():
    base /= 'linux/linux-asahi/drivers/gpu/drm/asahi'
s = (base/'g16_submit.rs').read_text()
loop = s[s.index('        let mut commands = commands.into_iter().peekable();'):s.index('            let packet = Arc::new(Packet')]
runtime = (base/'g16_runtime.rs').read_text()
constants = '\n'.join(re.findall(r'pub\(crate\) const (?:COMPUTE_DEPTH|MAX_PACKET_COMMANDS):[^\n]+', runtime))
source = r'''
#![allow(dead_code)]
const GFP_KERNEL: () = ();
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn with_capacity(n: usize, _: ()) -> Result<Self, ()> { Ok(Self(Vec::with_capacity(n))) }
    fn push(&mut self, x: T, _: ()) -> Result<(), ()> { self.0.push(x); Ok(()) }
    fn len(&self) -> usize { self.0.len() }
}
#[derive(Clone, Copy)] enum Payload { Compute(u8), Render }
#[derive(Clone, Copy)] struct Command { payload: Payload, id: usize }
mod g16_runtime {
''' + constants + r'''
    pub fn packet_limit() -> usize { super::LIMIT.load(std::sync::atomic::Ordering::Relaxed) }
}
static LIMIT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(8);
fn split(commands: Vec<Command>) -> Result<Vec<Vec<Command>>, ()> {
    let mut packets = Vec::new();
''' + loop + r'''
            packets.push(single.0);
        }
        Ok(packets)
}
fn main() {
    let mut checked = 0;
    for limit in 1..=32 {
        LIMIT.store(limit, std::sync::atomic::Ordering::Relaxed);
        for renders in 0..=40 {
            for computes in 0..=40 {
                for mixed in [false, true] {
                    let mut commands = Vec::new();
                    let mut remain = [renders, computes];
                    while remain != [0, 0] {
                        let engine = if mixed {
                            if remain[0] > 0 && (commands.len() % 2 == 0 || remain[1] == 0) { 0 } else { 1 }
                        } else { usize::from(remain[0] == 0) };
                        remain[engine] -= 1;
                        commands.push(Command { id:commands.len(), payload: if engine == 0 {Payload::Render} else {Payload::Compute(0)} });
                    }
                    let packets = split(commands.clone()).unwrap();
                    let mut id = 0;
                    for packet in &packets {
                        assert!(!packet.is_empty() && packet.len() <= limit);
                        let c = packet.iter().filter(|c| matches!(c.payload, Payload::Compute(_))).count();
                        assert!(c <= 16 && packet.len() - c <= 16);
                        for cmd in packet { assert_eq!(cmd.id, id); id += 1; }
                    }
                    assert_eq!(id, commands.len());
                    if limit == 32 && renders <= 16 && computes <= 16 && id > 0 { assert_eq!(packets.len(), 1); }
                    checked += 1;
                }
            }
        }
    }
    println!("PASS: {checked} production packet splits preserve order and per-engine bounds");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp)/'packet.rs'; exe = Path(tmp)/'packet'
    src.write_text(source)
    subprocess.run(['/usr/bin/rustc', '--edition=2021', '-O', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
