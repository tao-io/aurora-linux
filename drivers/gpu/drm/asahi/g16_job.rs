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
    auxiliary: Buffer,
    context_store: Buffer,
    private_memory: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    counter: Buffer,
    binding: mmu::VmBind,
    stamp: u32,
}

const PRIVATE_BYTES: usize = 0x2000000;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 0x1000;

impl Compute {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, config: &mut crate::g16_config::Config, work_queue: u64) -> Result<Self> {
        let vm = uat.new_vm(0x81320020, 0x70_0000_0000..0x80_0000_0000)?;
        Self::in_vm(dev, uat, config, &vm, work_queue, None, None, 1, g16_compute::STAMP)
    }
    pub(crate) fn in_vm(dev: &driver::AsahiDevice, uat: &mmu::Uat,
        config: &mut crate::g16_config::Config, vm: &mmu::Vm, work_queue: u64,
        control: Option<g16_compute::Control>, notification: Option<g16_compute::Notification>, generation: u8, stamp: u32) -> Result<Self> {
        let uma_client = if control.is_none() { Some(config.bind_uma_table(vm)?) } else { None };
        let gpu = |size| Buffer::new_gpu(dev, uat.kernel_vm(), &vm, size);
        // The engine fetches the register list in bootstrap context zero, then
        // switches to the client ASID. Retain exactly this command GEM in both
        // contexts; no user mappings or unrelated bootstrap pages are mirrored.
        let mut command = Buffer::new_command(dev, uat.kernel_vm(), uat.kernel_lower_vm(), g16_compute::SIZE)?;
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
            auxiliary: gpu(0x4000)?,
            context_store: gpu(0x40000)?,
            // Initial 32 MiB pool for qualification across the resident cores.
            // Metadata capacity is an initial allocation policy, not a driver
            // submission or lifetime limit. No captured block is retained.
            private_memory: gpu(PRIVATE_BYTES)?,
            pool_state: gpu(0x20000)?,
            page_list: gpu(0x4000)?,
            counter: gpu(8)?,
            binding: uat.bind(&vm)?,
            stamp,
        };
        let mut bytes = KBox::new([0; g16_compute::SIZE], GFP_KERNEL)?;
        let addresses = Addresses {
            command: job.command.va(), command_gpu: job.command.gpu_va()?,
            cdm_gpu: job.cdm.gpu_va()?,
            // RunCompute+883 and StartCompute+178 are the firmware and
            // GPU aliases of one metrics record, not independent storage.
            // Matched 25G83 native UAT walks resolve both to the same page.
            preemption_gpu: job.preemption.gpu_va()?, metrics: job.auxiliary.va(),
            auxiliary_gpu: job.auxiliary.gpu_va()?,
            work_queue, stats: config.compute_stats(), context_store_gpu: job.context_store.gpu_va()?,
            pool_state_gpu: job.pool_state.gpu_va()?, page_list_gpu: job.page_list.gpu_va()?,
            counter: job.counter.va(),
            context: job.binding.slot(), generation, stamp,
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
        dev_info!(dev.as_ref(), "G16G compute memory: command={:#x} preemption={:#x} auxiliary={:#x} context={:#x} private={:#x}+{:#x} pool={:#x} pages={:#x}\n",
            job.command.gpu_va()?, job.preemption.gpu_va()?, job.auxiliary.gpu_va()?,
            job.context_store.gpu_va()?, private_base, PRIVATE_BYTES,
            job.pool_state.gpu_va()?, job.page_list.gpu_va()?);
        Ok(job)
    }
    pub(crate) fn set_user_timestamps(&mut self, addresses: [u64; 2]) -> Result {
        if addresses == [0, 0] { return Ok(()); }
        // Timestamp reads the retained pointer pair and publishes nanoseconds.
        self.command.u64(0x844, addresses[0])?;
        self.command.u64(0x84c, addresses[1])?;
        self.command.u64(0xabc + 36, self.command.va() + 0x844)?;
        self.command.u64(0xb0c + 36, self.command.va() + 0x844)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn set_attachments(&mut self, attachments: &crate::g16_attachments::Attachments) -> Result {
        // 25G83 StartCompute+58..15c and FinalizeCompute+68.
        self.command.write(0x958, &attachments.0)?;
        self.command.write(0xbc0, &[u8::from(attachments.count() != 0)])?;
        g16_memory::publish();
        Ok(())
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
    /// Fault-only owned records: never walk application addresses or copy the
    /// private pool. These sequential observations preserve the command's UMA
    /// descriptor beside the hardware slot after retirement has failed.
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump,
        bootstrap: &mmu::Vm) -> Result {
        let gpu = self.command.gpu_va()?;
        let mut mappings = [0u8; 24];
        mappings[..8].copy_from_slice(&gpu.to_le_bytes());
        mappings[8..16].copy_from_slice(&(bootstrap.translate_iova(gpu)? as u64).to_le_bytes());
        mappings[16..24].copy_from_slice(&(self.binding.vm().translate_iova(gpu)? as u64).to_le_bytes());
        dump.record("compute-command-mappings", gpu, mappings.len(),
            |out| { out.copy_from_slice(&mappings); Ok(()) })?;
        dump.buffer("compute-command", &mut self.command, g16_compute::SIZE)?;
        dump.buffer("compute-preemption", &mut self.preemption, 0x4000)?;
        dump.buffer("compute-auxiliary", &mut self.auxiliary, 0x4000)?;
        dump.buffer("compute-context-store", &mut self.context_store, 0x40000)?;
        dump.buffer("compute-pool-state", &mut self.pool_state, 0x20000)?;
        dump.buffer("compute-page-list", &mut self.page_list, 0x4000)?;
        dump.buffer("compute-counter", &mut self.counter, 8)
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_retired_command(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("compute-retired-command", &mut self.command, g16_compute::SIZE)
    }
    pub(crate) fn stamp(&self) -> u32 { self.stamp }
    pub(crate) fn address(&self) -> u64 { self.command.va() }
    pub(crate) fn status(&mut self) -> Result<[u64; 10]> {
        Ok([self.command.read_u32(0xee0)? as u64, self.command.read_u32(0xee4)? as u64,
            self.command.read_u64(0x8c0)?, self.command.read_u64(0x8c8)?,
            self.command.read_u32(0x8ac)? as u64, self.command.read_u64(0xf00)?,
            self.command.read_u32(0xe44)? as u64, self.command.read_u64(0x89c)?,
            self.command.read_u32(0x76c)? as u64, self.output.read_u32(0)? as u64])
    }
}
