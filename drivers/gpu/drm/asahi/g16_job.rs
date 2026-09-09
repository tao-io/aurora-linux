// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned resources for the first compute-engine command. The runtime retains
//! this graph on any unproven retirement; a queue pointer is never ownership.

use kernel::prelude::*;
use crate::{driver, g16_compute::{self, Addresses}, g16_memory::{self, Buffer}, mmu};

pub(crate) struct Compute {
    command: Buffer,
    slot_state: Buffer,
    context_store: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    counter: Buffer,
}

impl Compute {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat) -> Result<Self> {
        let gpu = |size| Buffer::new_gpu(dev, uat.kernel_vm(), uat.kernel_lower_vm(), size);
        let mut job = Self {
            command: gpu(g16_compute::SIZE)?,
            slot_state: gpu(0x4000)?,
            context_store: gpu(0x40000)?,
            // Empty pool for this terminate. Size the metadata for a 64 MiB
            // pool rather than populating a captured private-memory graph.
            pool_state: gpu(0x20000)?,
            page_list: gpu(0x4000)?,
            counter: gpu(8)?,
        };
        let mut bytes = KBox::new([0; g16_compute::SIZE], GFP_KERNEL)?;
        g16_compute::encode(&mut bytes[..], Addresses {
            command: job.command.va(), command_gpu: job.command.gpu_va()?,
            slot_state: job.slot_state.va(), context_store_gpu: job.context_store.gpu_va()?,
            pool_state_gpu: job.pool_state.gpu_va()?, page_list_gpu: job.page_list.gpu_va()?,
            counter: job.counter.va(),
        }, 0x4000).map_err(|_| EINVAL)?;
        job.command.write(0, &bytes[..])?;
        g16_memory::publish();
        Ok(job)
    }
    pub(crate) fn address(&self) -> u64 { self.command.va() }
    pub(crate) fn status(&mut self) -> Result<[u64; 9]> {
        Ok([self.command.read_u32(0xee0)? as u64, self.command.read_u32(0xee4)? as u64,
            self.command.read_u64(0x8c0)?, self.command.read_u64(0x8c8)?,
            self.command.read_u32(0x8ac)? as u64, self.command.read_u64(0xf00)?,
            self.command.read_u32(0xe44)? as u64, self.command.read_u64(0x89c)?,
            self.command.read_u32(0x76c)? as u64])
    }
}
