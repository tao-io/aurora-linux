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
    private_memory: g16_memory::GpuBuffer,
    pool_state: Buffer,
    page_list: Buffer,
    counter: Buffer,
    binding: mmu::VmBind,
    notifier_owner: Option<crate::g16_queue::Client>,
    pool_slot: u8,
    stamp: u32,
    consumed_pages: usize,
    uma_submitted: u64,
    /// Pool-state and page-list bytes for this owner's private pool.
    freelist: KVec<u8>,
    page_list_image: KVec<u8>,
}

const PRIVATE_BYTES: usize = 0x2000000;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 0x1000;

impl Compute {
    pub(crate) fn resident_bytes(&self) -> usize {
        let buffers = [&self.command, &self.cdm, &self.shader, &self.esl, &self.output,
            &self.preemption, &self.auxiliary, &self.context_store, &self.pool_state,
            &self.page_list, &self.counter];
        buffers.iter().map(|b| crate::util::align(b.size(), crate::mmu::UAT_PGSZ)).sum::<usize>()
            + crate::util::align(self.private_memory.size(), crate::mmu::UAT_PGSZ)
    }
    pub(crate) fn retain_notifier(&mut self, notifier: crate::g16_queue::Client) {
        self.notifier_owner = Some(notifier);
    }
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, config: &mut crate::g16_config::Config, work_queue: u64) -> Result<KBox<Self>> {
        let vm = uat.new_vm(0x81320020, 0x70_0000_0000..0x80_0000_0000)?;
        Self::in_vm(dev, uat, config, &vm, work_queue, None, None, g16_compute::STAMP, None, 0, 1)
    }
    pub(crate) fn in_vm(dev: &driver::AsahiDevice, uat: &mmu::Uat,
        config: &mut crate::g16_config::Config, vm: &mmu::Vm, work_queue: u64,
        control: Option<g16_compute::Control>, notification: Option<g16_compute::Notification>, stamp: u32,
        reusable: Option<KBox<Self>>, event_slot: u8, pool_slot: u8) -> Result<KBox<Self>> {
        if pool_slot == 0 || usize::from(pool_slot) > crate::g16_runtime::POOL_SLOTS { return Err(EINVAL); }
        // Only an owner displaced by a completely retired replacement may be
        // recycled, and only inside the same client GPUVM. Its GEM backing,
        // both command aliases, the 32 MiB private pool and the ASID lease
        // are retained; command records are rebuilt while USC state persists.
        let reusable = reusable.filter(|job| control.is_some() && job.binding.matches(vm) && job.pool_slot == pool_slot);
        let mut saved_uma = [0; 0xa0];
        let mut boxed = if let Some(mut job) = reusable {
            let owner: &mut Self = &mut *job;
            // The displaced owner is no longer cached by firmware. Preserve
            // the GPU-maintained USC allocator as one unit: descriptor
            // cursors, freelist, page list and payload. Native updateSubmitInfo
            // uses cumulative engine counts; resetting just the allocator or
            // just its payload breaks that contract.
            if owner.uma_completed()? != owner.uma_submitted { return Err(EBUSY); }
            owner.command.read(0xe00, &mut saved_uma)?;
            for buffer in [&mut owner.command, &mut owner.output, &mut owner.preemption,
                &mut owner.auxiliary] { buffer.clear()?; }
            owner.stamp = stamp;
            job
        } else {
        let uma_client = if control.is_none() { Some(config.bind_uma_table(vm)?) } else { None };
        let gpu = |size| Buffer::new_gpu(dev, uat.kernel_vm(), &vm, size);
        // The engine fetches the register list in bootstrap context zero, then
        // switches to the client ASID. Retain exactly this command GEM in both
        // contexts; no user mappings or unrelated bootstrap pages are mirrored.
        let mut command = Buffer::new_command(dev, uat.kernel_vm(), uat.kernel_lower_vm(), 0x2000)?;
        let command_client = command.map_gpu_view(&vm)?;
        let private_memory = g16_memory::GpuBuffer::new(dev, vm, PRIVATE_BYTES)?;
        let (freelist, page_list_image) = g16_memory::freelist_images(private_memory.va(), PRIVATE_PAGES)?;
        let mut job = KBox::new(Self {
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
            // Only client GPU addresses enter the private-memory page lists.
            // Firmware has no pointer to a canonical alias of this storage.
            private_memory,
            pool_state: gpu(0x20000)?,
            page_list: gpu(0x4000)?,
            counter: gpu(8)?,
            binding: uat.bind(&vm)?,
                notifier_owner: None,
            pool_slot,
            stamp,
            consumed_pages: PRIVATE_PAGES,
            uma_submitted: 0,
            freelist,
            page_list_image,
        }, GFP_KERNEL)?;
        crate::cls_dev_dbg!(Compute, dev, "G16G compute memory: command={:#x} preemption={:#x} auxiliary={:#x} context={:#x} private={:#x}+{:#x} pool={:#x} pages={:#x}\n",
            job.command.gpu_va()?, job.preemption.gpu_va()?, job.auxiliary.gpu_va()?,
            job.context_store.gpu_va()?, job.private_memory.va(), PRIVATE_BYTES,
            job.pool_state.gpu_va()?, job.page_list.gpu_va()?);
        job
        };
        let job: &mut Self = &mut *boxed;
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
            context: job.binding.slot(), generation: job.binding.generation(), stamp, event_slot,
        };
        if let Some(control) = control {
            g16_compute::encode_dispatch(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32, control, notification).map_err(|_| EINVAL)?;
        } else {
            g16_compute::encode(&mut bytes[..], addresses, 0x4000,
                PRIVATE_PAGES as u32).map_err(|_| EINVAL)?;
        }
        // The hardware indexes the global gUPM table with this field. A
        // distinct live pool must never claim another command's slot, even
        // when firmware has consumed both commands from the same work queue.
        bytes[0xe08..0xe0c].copy_from_slice(&u32::from(job.pool_slot).to_le_bytes());
        bytes[0xe00..0xe08].copy_from_slice(&job.command.va().to_le_bytes());
        if job.uma_submitted == 0 {
            job.pool_state.write(0, &job.freelist)?;
            job.page_list.write(0, &job.page_list_image)?;
        } else {
            bytes[0xe00..0xea0].copy_from_slice(&saved_uma);
        }
        job.command.write(0, &bytes[..])?;
        // Match firmware's UMA+54 completion count so TEXT 2b518 drains the
        // cached gUPM slot and saves its cursors before another owner is bound.
        job.uma_submitted = job.uma_submitted.checked_add(1).ok_or(EOVERFLOW)?;
        let submitted = job.uma_submitted;
        job.counter.u64(0, submitted)?;
        job.shader.write(0, &g16_dispatch::image(job.output.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.esl.write(0, &g16_dispatch::esl(job.shader.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.cdm.write(0, &g16_dispatch::cdm(job.esl.gpu_va()?).map_err(|_| EINVAL)?)?;
        job.output.u32(0, g16_dispatch::SENTINEL)?;
        g16_memory::publish();
        job.log_context(dev)?;
        Ok(boxed)
    }
    /// GPU-side dependency on the preceding compute in this batch. The
    /// firmware stamp advances at engine retirement, independently of the
    /// batch notifier threshold (waiting on the user stamp can deadlock).
    pub(crate) fn dependency(&mut self, firmware_stamp: u64, slot: u8, previous: u32) -> Result<u64> {
        let mut bytes = [0; 0x40];
        crate::g16_render_command::render_dependency(&mut bytes, firmware_stamp,
            slot, previous, self.stamp).map_err(|_| EINVAL)?;
        // G16 +3c selects a strict internal dependency, homologous to
        // WorkQueue Barrier::internal_barrier_type. The zero-valued render
        // dependency permits partial-render scheduling before full retirement.
        bytes[0x3c..0x40].copy_from_slice(&1u32.to_le_bytes());
        self.command.write(0xf40, &bytes)?;
        Ok(self.command.va() + 0xf40)
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
    /// Repeat the existing firmware end-timestamp record before finalize.
    /// Keep the original SKU/state at +900; only its executable sequence is
    /// copied into unused storage in the same retained command allocation.
    pub(crate) fn set_timestamp_copies(&mut self, addresses: [u64; 8]) -> Result {
        let mut pairs = [0u64; 8];
        let mut count = 0;
        for address in addresses.into_iter().filter(|a| *a != 0) {
            let offset = 0x1800 + count * 16;
            self.command.u64(offset, 0)?;
            self.command.u64(offset + 8, address)?;
            pairs[count] = self.command.va() + offset as u64;
            count += 1;
        }
        if count == 0 { return Ok(()); }
        let mut sequence = [0u8; 0x600];
        self.command.read(0x900, &mut sequence[..0x300])?;
        let size = g16_compute::timestamp_copies(&mut sequence, &pairs[..count])
            .map_err(|_| EINVAL)?;
        self.command.write(0x1000, &sequence[..size])?;
        self.command.u64(0x760, self.command.va() + 0x1000)?;
        self.command.u32(0x768, size.try_into()?)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn coalesce_stamp_flush(&mut self) -> Result {
        // JobMeta::flush_stamps. UUID at +820 and all execution stamps remain.
        self.command.u32(0x81c, 0)
    }
    pub(crate) fn uma_submitted(&self) -> u64 { self.uma_submitted }
    pub(crate) fn uma_completed(&mut self) -> Result<u64> {
        self.command.read_u64(0xe54)
    }
    pub(crate) fn set_attachments(&mut self, attachments: &crate::g16_attachments::Attachments) -> Result {
        // 25G83 StartCompute+58..15c and FinalizeCompute+68.
        self.command.write(0x958, &attachments.0)?;
        self.command.write(0xbc0, &[u8::from(attachments.count() != 0)])?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn log_context(&self, dev: &driver::AsahiDevice) -> Result {
        if crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitTiming) {
            let mut roots = [(0, 0); 2];
            self.binding.vm().context_roots(&mut roots)?;
            dev_info!(dev.as_ref(), "G16G: owned compute user context={} roots={:x?}\n", self.binding.slot(), roots);
        }
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
        dump.buffer("compute-command", &mut self.command, 0x2000)?;
        dump.buffer("compute-preemption", &mut self.preemption, 0x4000)?;
        dump.buffer("compute-auxiliary", &mut self.auxiliary, 0x4000)?;
        dump.buffer("compute-context-store", &mut self.context_store, 0x40000)?;
        dump.buffer("compute-pool-state", &mut self.pool_state, 0x20000)?;
        dump.buffer("compute-page-list", &mut self.page_list, 0x4000)?;
        dump.buffer("compute-counter", &mut self.counter, 8)
    }
    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_retired_command(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        dump.buffer("compute-retired-command", &mut self.command, 0x2000)
    }
    /// Record the retired dispatch's private-page consumption for recycling.
    pub(crate) fn record_consumption(&mut self) -> Result {
        self.consumed_pages = g16_memory::consumed_pages(&mut self.command, 0xe00, PRIVATE_PAGES)?;
        Ok(())
    }
    pub(crate) fn pool_slot(&self) -> u8 { self.pool_slot }
    pub(crate) fn same_vm(&self, vm: &mmu::Vm) -> bool { self.binding.matches(vm) }
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
