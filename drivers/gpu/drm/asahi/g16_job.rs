// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned resources for the first compute-engine command. The runtime retains
//! this graph on any unproven retirement; a queue pointer is never ownership.

use kernel::prelude::*;
use crate::{driver, g16_dispatch, g16_compute::{self, Addresses}, g16_memory::{self, Buffer}, mmu};

pub(crate) struct Compute {
    _command_client: mmu::KernelMapping,
    _uma_client: mmu::KernelMapping,
    command: Buffer,
    cdm: Buffer,
    shader: Buffer,
    esl: Buffer,
    output: Buffer,
    preemption: Buffer,
    metrics: Buffer,
    auxiliary: Buffer,
    slot_state: Buffer,
    context_store: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    counter: Buffer,
    binding: mmu::VmBind,
}

impl Compute {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, config: &mut crate::g16_config::Config) -> Result<Self> {
        let vm = uat.new_vm(0x81320020, 0x70_0000_0000..0x80_0000_0000)?;
        let uma_client = config.bind_uma_table(&vm)?;
        let gpu = |size| Buffer::new_gpu(dev, uat.kernel_vm(), &vm, size);
        // The engine fetches the register list in bootstrap context zero, then
        // switches to the client ASID. Retain exactly this command GEM in both
        // contexts; no user mappings or unrelated bootstrap pages are mirrored.
        let mut command = Buffer::new_gpu(dev, uat.kernel_vm(), uat.kernel_lower_vm(), g16_compute::SIZE)?;
        let command_client = command.map_gpu_view(&vm)?;
        let mut job = Self {
            _command_client: command_client,
            _uma_client: uma_client,
            command,
            cdm: Buffer::new_gpu_readonly(dev, uat.kernel_vm(), &vm, 0x4000)?,
            shader: Buffer::new_gpu_readonly(dev, uat.kernel_vm(), &vm, 0x4000)?,
            esl: Buffer::new_gpu_readonly(dev, uat.kernel_vm(), &vm, 0x4000)?,
            output: gpu(0x4000)?,
            preemption: gpu(0x4000)?,
            metrics: gpu(0x4000)?,
            auxiliary: gpu(0x4000)?,
            slot_state: gpu(0x4000)?,
            context_store: gpu(0x40000)?,
            // Empty pool for this no-spill diagnostic. Size the metadata for a 64 MiB
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
            preemption_gpu: job.preemption.gpu_va()?, metrics: job.metrics.va(),
            auxiliary_gpu: job.auxiliary.gpu_va()?,
            slot_state: job.slot_state.va(), context_store_gpu: job.context_store.gpu_va()?,
            pool_state_gpu: job.pool_state.gpu_va()?, page_list_gpu: job.page_list.gpu_va()?,
            counter: job.counter.va(),
            context: job.binding.slot(), generation: 1,
        }, 0x4000).map_err(|_| EINVAL)?;
        job.command.write(0, &bytes[..])?;
        job.shader.write(0, &g16_dispatch::shader(job.output.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.esl.write(0, &g16_dispatch::esl(job.shader.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.cdm.write(0, &g16_dispatch::cdm(job.esl.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.output.u32(0, g16_dispatch::SENTINEL)?;
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
    pub(crate) fn status(&mut self) -> Result<[u64; 10]> {
        Ok([self.command.read_u32(0xee0)? as u64, self.command.read_u32(0xee4)? as u64,
            self.command.read_u64(0x8c0)?, self.command.read_u64(0x8c8)?,
            self.command.read_u32(0x8ac)? as u64, self.command.read_u64(0xf00)?,
            self.command.read_u32(0xe44)? as u64, self.command.read_u64(0x89c)?,
            self.command.read_u32(0x76c)? as u64, self.output.read_u32(0)? as u64])
    }
}
