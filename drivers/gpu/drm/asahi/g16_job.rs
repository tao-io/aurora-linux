// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned resources for the first compute-engine command. The runtime retains
//! this graph on any unproven retirement; a queue pointer is never ownership.

use kernel::prelude::*;
use crate::{driver, g16_dispatch, g16_compute::{self, Addresses}, g16_memory::{self, Buffer}, mmu};

pub(crate) struct Compute {
    _command_client: mmu::KernelMapping,
    _uma_client: Option<mmu::KernelMapping>,
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
    private_memory: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    counter: Buffer,
    binding: mmu::VmBind,
}

const PRIVATE_BYTES: usize = 0x2000000;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 0x1000;

impl Compute {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, config: &mut crate::g16_config::Config) -> Result<Self> {
        let vm = uat.new_vm(0x81320020, 0x70_0000_0000..0x80_0000_0000)?;
        Self::in_vm(dev, uat, config, &vm, None, None, 1)
    }
    pub(crate) fn in_vm(dev: &driver::AsahiDevice, uat: &mmu::Uat,
        config: &mut crate::g16_config::Config, vm: &mmu::Vm,
        control: Option<g16_compute::Control>, notification: Option<g16_compute::Notification>, generation: u8) -> Result<Self> {
        let uma_client = if control.is_none() { Some(config.bind_uma_table(vm)?) } else { None };
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
            // Initial 32 MiB pool for qualification across the resident cores.
            // Metadata capacity is an initial allocation policy, not a driver
            // submission or lifetime limit. No captured block is retained.
            private_memory: gpu(PRIVATE_BYTES)?,
            pool_state: gpu(0x20000)?,
            page_list: gpu(0x4000)?,
            counter: gpu(8)?,
            binding: uat.bind(&vm)?,
        };
        let mut bytes = KBox::new([0; g16_compute::SIZE], GFP_KERNEL)?;
        let addresses = Addresses {
            command: job.command.va(), command_gpu: job.command.gpu_va()?,
            cdm_gpu: job.cdm.gpu_va()?,
            preemption_gpu: job.preemption.gpu_va()?, metrics: job.metrics.va(),
            auxiliary_gpu: job.auxiliary.gpu_va()?,
            slot_state: job.slot_state.va(), context_store_gpu: job.context_store.gpu_va()?,
            pool_state_gpu: job.pool_state.gpu_va()?, page_list_gpu: job.page_list.gpu_va()?,
            counter: job.counter.va(),
            context: job.binding.slot(), generation,
        };
        if let Some(control) = control {
            g16_compute::encode_dispatch(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32, control, notification).map_err(|_| EINVAL)?;
        } else {
            g16_compute::encode(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32).map_err(|_| EINVAL)?;
        }
        let private_base = job.private_memory.gpu_va()?;
        for page in 0..PRIVATE_PAGES {
            job.pool_state.u64(page * 8, private_base + page as u64 * 0x1000)?;
        }
        for block in 0..PRIVATE_BYTES / 0x200000 {
            job.page_list.u64(block * 64, (1u64 << 61) | (private_base + block as u64 * 0x200000))?;
        }
        job.command.write(0, &bytes[..])?;
        job.shader.write(0, &g16_dispatch::image(job.output.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.esl.write(0, &g16_dispatch::esl(job.shader.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.cdm.write(0, &g16_dispatch::cdm(job.esl.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.output.u32(0, g16_dispatch::SENTINEL)?;
        g16_memory::publish();
        job.log_context(dev)?;
        Ok(job)
    }
    pub(crate) fn log_context(&self, dev: &driver::AsahiDevice) -> Result {
        let mut roots = [(0, 0); 2];
        self.binding.vm().context_roots(&mut roots)?;
        dev_info!(dev.as_ref(), "G16G: owned compute user context={} roots={:x?}\n", self.binding.slot(), roots);
        Ok(())
    }
    pub(crate) fn log_progress(&mut self, dev: &driver::AsahiDevice) -> Result {
        let mut cells = [0u64; 4];
        for (index, cell) in cells.iter_mut().enumerate() {
            *cell = self.preemption.read_u64(0x1480 + index * 8)?;
        }
        dev_info!(dev.as_ref(), "G16G: compute preemption progress={:x?} private-pages={}\n", cells, PRIVATE_PAGES);
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
