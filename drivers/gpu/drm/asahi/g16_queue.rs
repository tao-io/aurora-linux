// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned M4 firmware work queue. Command backing lives with its queue until
//! firmware retirement; no captured addresses or globally shared user VM.

use kernel::prelude::*;
use crate::{driver, g16_memory::{self, Buffer}, mmu};

pub(crate) struct Queue {
    info: Buffer,
    ring_state: Buffer,
    ring: Buffer,
    notifier: Buffer,
    context: Buffer,
    command: Buffer,
    stamp: Buffer,
}

impl Queue {
    pub(crate) fn new(dev: &driver::AsahiDevice, vm: &mmu::Vm, capacity: u32) -> Result<Self> {
        // Pipe messages carry a u16 ring index. Ring wrap uses a power-of-two
        // mask; capacity is supplied by the queue policy rather than a lab cap.
        if !capacity.is_power_of_two() || !(2..=65536).contains(&capacity) { return Err(EINVAL); }
        let mut queue = Self {
            info: Buffer::new(dev, vm, 0x24c0)?,
            ring_state: Buffer::new(dev, vm, 0x60)?,
            ring: Buffer::new(dev, vm, capacity as usize * 8)?,
            notifier: Buffer::new(dev, vm, 0x18)?,
            context: Buffer::new(dev, vm, 0x38)?,
            command: Buffer::new(dev, vm, 0x40)?,
            stamp: Buffer::new(dev, vm, 8)?,
        };
        queue.info.u64(0, queue.ring_state.va())?;
        queue.info.u64(8, queue.ring.va())?;
        queue.info.u64(0x10, queue.notifier.va())?;
        queue.info.u64(0x18, queue.info.va() + 0xb0)?;
        queue.info.u32(0x2c, u32::MAX)?;
        queue.info.u32(0x30, 2)?;
        queue.info.u32(0x34, 2)?;
        queue.info.u64(0x38, 0xffff000000000000)?;
        queue.info.u32(0x48, 2)?;
        queue.info.u32(0x4c, u32::MAX)?;
        queue.info.u64(0xa4, queue.context.va())?;
        queue.notifier.u64(8, queue.notifier.va())?;
        queue.context.write(0, &[255, 255])?;
        queue.context.write(5, &[1])?;
        queue.context.write(0x33, &[255])?;
        queue.ring_state.u32(0x50, capacity)?;
        // Barrier opcode 4, already-satisfied wait at the owned zero stamp.
        queue.command.u32(0, 4)?;
        queue.command.u64(4, queue.stamp.va())?;
        queue.command.u64(0xc, queue.stamp.va())?;
        queue.command.u32(0x20, 0x80)?;
        Ok(queue)
    }

    pub(crate) fn prepare_barrier(&mut self) -> Result<u64> {
        if self.ring_state.read_u32(0x40)? != 0 { return Err(EBUSY); }
        self.ring.u64(0, self.command.va())?;
        g16_memory::publish();
        self.ring_state.u32(0x40, 1)?;
        g16_memory::publish();
        Ok(self.info.va())
    }

    /// Publish into a fresh queue. The caller must retain command backing
    /// across every failure after this point, until engine retirement/reset.
    pub(crate) fn prepare_command(&mut self, command: u64) -> Result<u64> {
        if command & 7 != 0 || self.ring_state.read_u32(0x40)? != 0 { return Err(EINVAL); }
        self.ring.u64(0, command)?;
        g16_memory::publish();
        self.ring_state.u32(0x40, 1)?;
        g16_memory::publish();
        Ok(self.info.va())
    }

    pub(crate) fn status(&mut self) -> Result<[u32; 6]> {
        Ok([self.info.read_u32(0x20)?, self.info.read_u32(0x24)?, self.info.read_u32(0x28)?,
            self.ring_state.read_u32(0x30)?, self.ring_state.read_u32(0x40)?,
            self.info.read_u32(0x80)?])
    }
    pub(crate) fn context_registered(&mut self) -> Result<bool> {
        let mut state = [0; 3];
        self.context.read(0, &mut state)?;
        Ok(state[0] != 255 && state[1] != 255 && state[2] == 4)
    }
}
