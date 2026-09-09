// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned resources for the first compute-engine command. The runtime retains
//! this graph on any unproven retirement; a queue pointer is never ownership.

use kernel::prelude::*;
use crate::{driver, g16_compute::{self, Addresses}, g16_memory::{self, Buffer}, mmu};

pub(crate) struct Compute {
    _command_client: mmu::KernelMapping,
    command: Buffer,
    cdm: Buffer,
    slot_state: Buffer,
    context_store: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    counter: Buffer,
    binding: mmu::VmBind,
}

impl Compute {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat) -> Result<Self> {
        let vm = uat.new_vm(0x81320020, 0x70_0000_0000..0x80_0000_0000)?;
        let gpu = |size| Buffer::new_gpu(dev, uat.kernel_vm(), &vm, size);
        // The engine fetches the register list in bootstrap context zero, then
        // switches to the client ASID. Retain exactly this command GEM in both
        // contexts; no user mappings or unrelated bootstrap pages are mirrored.
        let mut command = Buffer::new_gpu(dev, uat.kernel_vm(), uat.kernel_lower_vm(), g16_compute::SIZE)?;
        let command_client = command.map_gpu_view(&vm)?;
        let mut job = Self {
            _command_client: command_client,
            command,
            cdm: Buffer::new_gpu_readonly(dev, uat.kernel_vm(), &vm, 0x4000)?,
            slot_state: gpu(0x4000)?,
            context_store: gpu(0x40000)?,
            // Empty pool for this terminate. Size the metadata for a 64 MiB
            // pool rather than populating a captured private-memory graph.
            pool_state: gpu(0x20000)?,
            page_list: gpu(0x4000)?,
            counter: gpu(8)?,
            binding: uat.bind(&vm)?,
        };
        let mut bytes = KBox::new([0; g16_compute::SIZE], GFP_KERNEL)?;
        g16_compute::encode(&mut bytes[..], Addresses {
            command: job.command.va(), command_gpu: job.command.gpu_va()?,
            cdm_gpu: job.cdm.gpu_va()?,
            slot_state: job.slot_state.va(), context_store_gpu: job.context_store.gpu_va()?,
            pool_state_gpu: job.pool_state.gpu_va()?, page_list_gpu: job.page_list.gpu_va()?,
            counter: job.counter.va(),
            context: job.binding.slot(), generation: 1,
        }, 0x4000).map_err(|_| EINVAL)?;
        job.command.write(0, &bytes[..])?;
        job.cdm.u32(0, 0x40000000)?; // immutable CDM terminate
        g16_memory::publish();
        Ok(job)
    }
    pub(crate) fn log_context(&self, dev: &driver::AsahiDevice) -> Result {
        let mut roots = [(0, 0); 2];
        self.binding.vm().context_roots(&mut roots)?;
        dev_info!(dev.as_ref(), "G16G: owned compute user context={} generation=1 roots={:x?}\n", self.binding.slot(), roots);
        Ok(())
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
