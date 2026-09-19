// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! GEM/GPUVM ownership for an M4 render pass in a bounded GPU batch. There are no borrowed
//! lab mappings. A hardware slot retains its last owner until replacement.
use kernel::prelude::*;
use crate::{driver, mmu, g16_memory::{self, Buffer}, g16_render::{self, Register, Stage},
    g16_render_state::{self as state, State, Program, DepthStencil}, g16_render_command as wire,
    g17_uapi::UapiRenderCommand};

const PRIVATE_BYTES: usize = 32 * 1024 * 1024;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 4096;
// The gUPM freelist needs headroom beyond the initially populated pages.
// Equal capacity faulted on a write at freelist+0x10040 with 8192 pages.
// Derive the next ring size instead of silently overflowing a full ring.
const PRIVATE_QWORDS: usize = (PRIVATE_PAGES + 1).next_power_of_two();
pub(crate) const SLOTS: [u8; 2] = [0x70, 0x71];

pub(crate) struct Render {
    _command_views: KVec<mmu::KernelMapping>,
    commands: [Buffer; 2],
    sequences: [Buffer; 2],
    metadata: Buffer,
    parameters: crate::g16_parameter::Pool,
    init_parameters: bool,
    scratch: Buffer,
    scene_list: Buffer,
    tilemap: Buffer,
    tpc: Buffer,
    unknown: Buffer,
    preemption: [Buffer; 3],
    auxiliary: Buffer,
    uma: Buffer,
    private_memory: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    binding: mmu::VmBind,
    notifier_owner: Option<crate::g16_queue::Client>,
    pub(crate) stage: Stage,
    stamp: u32,
    pool_slot: u8,
    compute_dependency: bool,
    layout: [u32; 4],
    consumed_pages: usize,
    uma_submitted: u64,
    freelist: KVec<u8>,
    page_list_image: KVec<u8>,
    pub(crate) clear_metrics: [u64; 3],
}
impl Render {
    /// Match common Asahi's last-render flush_stamps. M4 25G83 completion
    /// handlers OR TA+894 / FS+bdc into notifier+20 (TEXT 21944 / 22e78),
    /// making the ready-notifier path publish stamps without threshold delay.
    pub(crate) fn request_completion_flush(&mut self) -> Result {
        self.commands[0].u32(0x894, 1)?;
        self.commands[1].u32(0xbdc, 1)?;
        Ok(())
    }
    pub(crate) fn retain_notifier(&mut self, notifier: crate::g16_queue::Client) {
        self.notifier_owner = Some(notifier);
    }
    pub(crate) fn notifier(&self) -> Result<&crate::g16_queue::Client> {
        self.notifier_owner.as_ref().ok_or(EIO)
    }
    pub(crate) fn profiling_key(&self) -> [u32; 3] {
        [self.binding.slot(), self.layout[0], self.layout[1]]
    }

    #[cfg(CONFIG_DEV_COREDUMP)]
    pub(crate) fn capture_fault(&mut self, dump: &mut crate::g16_fault::Dump) -> Result {
        // Retained pass-owned state only. In particular, partial tiling can
        // exhaust parameter pages while fragment work waits for a dependency.
        // Keep allocator and command evidence together without reading user BOs.
        self.notifier()?.lock().capture_render_fault(dump)?;
        let mut parameters = self.parameters.lock();
        dump.buffer("render-parameter-manager", &mut parameters.manager, 0xc0)?;
        dump.buffer("render-parameter-ring", &mut parameters.ring, 8)?;
        drop(parameters);
        dump.buffer("render-scene-metadata", &mut self.metadata, 0x4000)?;
        dump.buffer("render-uma-descriptor", &mut self.uma, 0x300)?;
        dump.buffer("render-pool-state", &mut self.pool_state, PRIVATE_QWORDS * 8)?;
        for (i, stage) in [Stage::Tiling, Stage::Fragment].into_iter().enumerate() {
            dump.buffer(["render-ta-command", "render-fragment-command"][i],
                &mut self.commands[i], stage.command_size())?;
            dump.buffer(["render-ta-sequence", "render-fragment-sequence"][i],
                &mut self.sequences[i], if i == 0 { 0x400 } else { 0x800 })?;
        }
        Ok(())
    }

    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, vm: &mmu::Vm,
        r: UapiRenderCommand, usc: u64, label: u8, stamp: u32, counter: u64,
        queues: [u64; 2], notifications: [crate::g16_compute::Notification; 2], stats: [u64; 2],
        pool_slot: u8, reusable: Option<KBox<Self>>,
        shared: Option<crate::g16_parameter::Pool>) -> Result<KBox<Self>> {
        if usize::from(pool_slot) >= crate::g16_runtime::POOL_SLOTS { return Err(EINVAL); }
        let geometry = crate::g16_parameter::geometry(r)?;
        let pb_bytes = crate::g16_parameter::bytes(&geometry)?;
        let layout = [r.width as u32, r.height as u32,
            r.utile_width as u32, r.utile_height as u32];
        // Only an owner displaced by a completely retired replacement may
        // be recycled. Never borrow the firmware's most recently used owner,
        // and never reuse private mappings across distinct client GPUVMs.
        let reusable = reusable.filter(|job| job.binding.matches(vm) && job.pool_slot == pool_slot);
        let reused = reusable.is_some();
        let mut job = if let Some(mut job) = reusable {
            // A render size change does not invalidate the command/context
            // storage or the USC pool. Grow only geometry-dependent backing;
            // each new pass still rebuilds its logical sizes and descriptors.
            // This owner is already displaced and retired, so replacing a
            // too-small buffer obeys the same lifetime rule as dropping it.
            let gpu = |size| Buffer::new_gpu(dev,uat.kernel_vm(),vm,size);
            if job.tilemap.size() < geometry.tilemap_bytes.try_into()? {
                job.tilemap = gpu(geometry.tilemap_bytes.try_into()?)?;
            }
            if job.tpc.size() < geometry.tpc_bytes.try_into()? {
                job.tpc = gpu(geometry.tpc_bytes.try_into()?)?;
            }
            job.clear()?;
            if let Some(shared) = shared {
                job.parameters = shared;
            } else if job.parameters.lock().shared {
                job.parameters = crate::g16_parameter::Parameters::new(
                    dev, uat, vm, pb_bytes, u32::from(pool_slot), false)?;
            } else {
                job.parameters.lock().rebuild(dev, uat, vm, pb_bytes)?;
            }
            job.layout = layout;
            job.stage = Stage::Tiling;
            job.stamp = stamp;
            job
        } else {
            let gpu = |size| Buffer::new_gpu(dev,uat.kernel_vm(),vm,size);
            let fw = |size| Buffer::new(dev,uat.kernel_vm(),size);
            let mut commands = [
                Buffer::new_command(dev,uat.kernel_vm(),uat.kernel_lower_vm(),Stage::Tiling.command_size())?,
                Buffer::new_command(dev,uat.kernel_vm(),uat.kernel_lower_vm(),Stage::Fragment.command_size())?];
            let mut views = KVec::new();
            for command in &mut commands { views.push(command.map_gpu_view(vm)?,GFP_KERNEL)?; }
            let private_memory = gpu(PRIVATE_BYTES)?;
            let (freelist, page_list_image) = g16_memory::freelist_images(
                private_memory.gpu_va()?, PRIVATE_PAGES)?;
            KBox::new(Self {
                _command_views:views, commands, sequences:[fw(0x400)?,fw(0x800)?],
                metadata:fw(0x4000)?,
                parameters: match shared {
                    Some(shared) => shared,
                    None => crate::g16_parameter::Parameters::new(
                        dev, uat, vm, pb_bytes, u32::from(pool_slot), false)?,
                }, init_parameters:false,
                // Zero-backed per-scene list; native PB slot+28 binds its
                // compact address in both TA and fragment register lists.
                scratch:gpu(0x4000)?,scene_list:gpu(0x4000)?,tilemap:gpu(geometry.tilemap_bytes.try_into()?)?,
                tpc:gpu(geometry.tpc_bytes.try_into()?)?,unknown:gpu(0x4000)?,
                // Sizes inherited from the qualified M4 context-save experiment.
                preemption:[gpu(0x10000)?,gpu(0x10000)?,gpu(0x4000)?],auxiliary:gpu(0x10000)?,
                uma:gpu(0x4000)?,private_memory,
                pool_state:gpu(PRIVATE_QWORDS*8)?,page_list:gpu(0x4000)?,
                binding:uat.bind(vm)?,
                notifier_owner: None,stage:Stage::Tiling,stamp,pool_slot,compute_dependency:false,layout,
                consumed_pages:PRIVATE_PAGES, uma_submitted:0, freelist, page_list_image, clear_metrics:[0; 3],
            }, GFP_KERNEL)?
        };
        if job.uma_submitted == 0 {
            let owner: &mut Self = &mut *job;
            // Initialize once. Firmware owns the freelist and its cursors
            // thereafter; both backing addresses remain fixed within this VM.
            owner.pool_state.write(0, &owner.freelist)?;
            owner.page_list.write(0, &owner.page_list_image)?;
        }
        if job.uma_submitted == 0 {
            let mut uma = [0;0xa0];
            crate::g16_compute::encode_uma(&mut uma,job.pool_state.gpu_va()?,PRIVATE_QWORDS as u32,
                PRIVATE_PAGES as u32,job.page_list.gpu_va()?,job.metadata.va()+0x500,
                crate::g16_compute::UmaEngine::Render).map_err(|_| EINVAL)?;
            // Compute and render owners have disjoint private-slot ranges.
            // Firmware may prepare all owners while their pools remain retained.
            uma[8..12].copy_from_slice(&(1 + crate::g16_runtime::POOL_SLOTS as u32 + u32::from(pool_slot)).to_le_bytes());
            uma[..8].copy_from_slice(&job.uma.va().to_le_bytes());
            job.uma.write(0,&uma)?;
        }
        job.compute_dependency = false;
        // Native updateSubmitInfo increments once per submitted engine. The
        // firmware drains the pool when completed == submitted, and saves its
        // read/write/free cursors at UMA+20..2c (25G83 TEXT 2b618..670). Keep
        // that descriptor and the GPU-maintained freelist/payload together.
        // A reused owner has been displaced by a fully retired replacement;
        // no firmware/GPU consumer can still reference its counter here.
        job.uma_submitted = job.uma_submitted.checked_add(2).ok_or(EOVERFLOW)?;
        let submitted = job.uma_submitted;
        job.metadata.u64(0x500, submitted)?;
        let (parameter_manager, parameter_id, parameter_pages, shared_pool, init_parameters) = {
            let mut pool = job.parameters.lock();
            (pool.manager.va(), pool.id, pool.pages(), pool.shared, pool.claim_init())
        };
        job.init_parameters = init_parameters;
        let mut init = [0;0x20];
        // This owner was rebuilt, not incrementally grown. In 25G83, a nonzero
        // InitBuffer+0x10 selects the append path (TEXT 2159c -> 2f100), which
        // returns without refreshing the hardware descriptor when the staged
        // block cursor already equals manager+0x3c. A prior owner's hardware
        // writeback can then replace a host-written descriptor with stale VAs.
        // Request full reinitialization: firmware drains the old slot, rebuilds
        // its page list from the block ring and publishes the new descriptor
        // in order (TEXT 215b4 -> 2eb4c). The table is firmware-owned.
        wire::init_parameter_buffer(&mut init,job.binding.slot(),parameter_id,0,0,parameter_manager,stamp).map_err(|_| EINVAL)?;
        job.metadata.write(0x300,&init)?;
        let mut dependency = [0;0x40];
        wire::render_dependency(&mut dependency,notifications[0].fw_stamp,
            SLOTS[0],stamp,stamp).map_err(|_| EINVAL)?;
        job.metadata.write(0x340,&dependency)?;
        // PB scene scratch has explicit GPU and firmware aliases. Statistics
        // are a distinct retained record; firmware dereferences them on retire.
        let scratch_gpu = job.scratch.gpu_va()?;
        let scratch_fw = job.scratch.va();
        let statistics = job.metadata.va()+0x200;
        job.metadata.u64(0,scratch_gpu)?;
        job.metadata.u64(8,scratch_fw)?;
        let scene_list = state::compact(job.scene_list.gpu_va()?).map_err(|_| EINVAL)?;
        job.metadata.u64(0x28,scene_list)?;
        job.metadata.u64(0x40,statistics)?;
        let mask = if r.flags & 16 != 0 {u64::MAX} else {u32::MAX as u64};
        let program = |p:crate::g17_uapi::UapiProgram| Program {
            address:usc+u64::from(p.usc & !63), resources:p.resource_spec & mask };
        let state = State { geometry,parameter_buffer_id:parameter_id,vdm:r.vdm_base,tilemap:job.tilemap.gpu_va()?,tpc:job.tpc.gpu_va()?,
            unknown:job.unknown.gpu_va()?,preemption:[job.preemption[0].gpu_va()?,job.preemption[1].gpu_va()?,job.preemption[2].gpu_va()?],
            scratch:job.scratch.gpu_va()?,scene_list:job.scene_list.gpu_va()?,auxiliary:job.auxiliary.gpu_va()?,multisample:r.multisample_control,ppp:r.ppp_control,
            // UAPI values are already validated. Merge limits are float bits,
            // not geometry counts; color storage is measured in 2 KiB blocks.
            merge_upper:[r.merge_upper_x,r.merge_upper_y],
            tilebuffer_blocks:(u32::from(r.sample_size)*u32::from(r.utile_width)*u32::from(r.utile_height)*u32::from(r.samples)).div_ceil(2048),
            background:program(r.background),eot:program(r.end_of_tile),
            partial_background:program(r.partial_background),partial_eot:program(r.partial_end_of_tile),
            depth_bias:r.depth_bias_base,scissor:r.scissor_base,query:r.occlusion_query_base,
            depth:DepthStencil{base:r.depth.base,stride:r.depth.stride as u64},
            stencil:DepthStencil{base:r.stencil.base,stride:r.stencil.stride as u64},
            depth_dimensions:r.depth_dimensions as u64,zls_control:r.zls_control,
            depth_clear:r.depth_clear,stencil_clear:r.stencil_clear,process_empty_tiles:r.flags&2!=0,
            integer_depth_bias:r.flags&(1<<18)!=0 };
        let mut work = KBox::new([0;0xca0],GFP_KERNEL)?;
        let mut sequence = [0;0x400];
        let mut registers = KBox::new([Register::default();128],GFP_KERNEL)?;
        for (i,stage) in [Stage::Tiling,Stage::Fragment].into_iter().enumerate() {
            // A diagnostic command label, not the ASID binding generation.
            let uuid = 0x16000000 | (u32::from(label)<<8) | i as u32;
            wire::encode(&mut work[..],&mut sequence,stage,wire::Args {
                command:job.commands[i].va(),sku:job.sequences[i].va(),queue:queues[i],
                stats:stats[i],
                pb_slot:job.metadata.va(),manager:parameter_manager,pb_aux:job.metadata.va()+0x100,
                notifier:notifications[i].address,shared_stamp:notifications[i].fw_stamp,
                stamp_address:notifications[i].stamp,uma:job.uma.va(),
                uma_aux:job.uma.gpu_va()?+0x100+i as u64*0x100,
                timestamp_storage:job.metadata.va()+0x600+i as u64*0x20,
                context:job.binding.slot(),generation:job.binding.generation(),counter:counter + u64::from(i == 0),uuid,stamp:stamp,
                event_slot:SLOTS[i] as u32,fragment_slot:SLOTS[1] as u32,
                fragment_stamp:stamp,pb_id:parameter_id,pass_id:0 }).map_err(|_| EINVAL)?;
            let (mut count,mode) = state::registers(&mut registers[..],stage,&state,uuid).map_err(|_| EINVAL)?;
            // Common Asahi USC execution window selectors (TA/ISP), retaining
            // the real queue base instead of relying on the boot register.
            registers[count] = Register { offset:if i==0 {0x10060} else {0x10068},flagged:true,value:usc };
            count+=1;
            g16_render::write_registers(&mut work[..],stage,job.commands[i].gpu_va()?,&registers[..count]).map_err(|_| EINVAL)?;
            state::mirrors(&mut work[..],stage,&state,job.tpc.va(),job.uma.va(),job.uma.va()+0x100+i as u64*0x100,mode).map_err(|_| EINVAL)?;
            job.commands[i].write(0,&work[..stage.command_size()])?;
            job.sequences[i].write(0,&sequence[..wire::sequence_size(stage)])?;
        }
        g16_memory::publish();
        crate::cls_dev_dbg!(Render, dev, "G16G: render USC freelist={:#x} capacity={} populated={}\n",
            job.pool_state.gpu_va()?,PRIVATE_QWORDS,PRIVATE_PAGES);
        crate::cls_dev_dbg!(SubmitTiming, dev,"G16G: owned render {}x{} ASID={} PB={} pages parameter_id={} shared_pool={} init_pool={} tilemap={:#x} TPC={:#x} VDM={:#x} reused={}\n",
            r.width,r.height,job.binding.slot(),parameter_pages,parameter_id,shared_pool,init_parameters,job.tilemap.gpu_va()?,job.tpc.gpu_va()?,r.vdm_base,reused);
        Ok(job)
    }
    pub(crate) fn set_attachments(&mut self, attachments: &[crate::g16_attachments::Attachments; 2]) -> Result {
        // StartVertex+88/count+188; StartFragment+a8/count+1a8.
        // The following UMA bindings and WFI records stay intact.
        for (i, (start, finish_flag)) in [(0x88, 0x2ec), (0xa8, 0x32c)].into_iter().enumerate() {
            self.sequences[i].write(start, &attachments[i].0)?;
            self.sequences[i].write(finish_flag, &[u8::from(attachments[i].count() != 0)])?;
        }
        g16_memory::publish();
        Ok(())
    }
    /// Reset command state while retaining GEM backing, both GPU aliases and
    /// the ASID lease. In particular, clear old completion
    /// stamps, preemption records and parameter allocator state before
    /// their new event values are encoded. The USC allocator remains intact.
    /// Parameter pages are GPU-written payload. Reset their allocator and
    /// scene metadata, retaining payload contents after retirement in this VM.
    fn clear(&mut self) -> Result {
        if self.uma_completed()? != self.uma_submitted { return Err(EBUSY); }
        let profiling = crate::debug::debug_enabled(crate::debug::DebugFlags::SubmitSummary);
        let now = || <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get();
        let start = if profiling { now() } else { 0 };
        for buffer in &mut self.commands { buffer.clear()?; }
        for buffer in &mut self.sequences { buffer.clear()?; }
        for buffer in &mut self.preemption { buffer.clear()?; }
        // The USC allocator is persistent. Do not clear its payload, freelist,
        // page list or descriptor while retaining firmware's saved cursors.
        for buffer in [
            &mut self.metadata,
            &mut self.scratch, &mut self.scene_list, &mut self.tilemap, &mut self.tpc,
            &mut self.unknown, &mut self.auxiliary,
        ] { buffer.clear()?; }
        self.clear_metrics = if profiling {
            [0, 0, (now() - start).max(0) as u64]
        } else { [0; 3] };
        Ok(())
    }
    /// Record the retired pass's private-page consumption for recycling.
    pub(crate) fn record_consumption(&mut self) -> Result {
        self.consumed_pages = crate::g16_memory::consumed_pages(&mut self.uma, 0, PRIVATE_PAGES)?;
        Ok(())
    }
    pub(crate) fn stamp(&self) -> u32 { self.stamp }
    pub(crate) fn index(&self) -> usize { usize::from(self.stage == Stage::Fragment) }
    pub(crate) fn address(&self) -> u64 { self.commands[self.index()].va() }
    pub(crate) fn wait_for_prior(&mut self, stamp_address: u64, slot: u8, value: u32) -> Result {
        let mut barrier = [0; 0x40];
        wire::render_dependency(&mut barrier, stamp_address, slot,
            value, self.stamp).map_err(|_| EINVAL)?;
        barrier[0x3c..0x40].copy_from_slice(&1u32.to_le_bytes());
        self.metadata.write(0x380, &barrier)?;
        self.compute_dependency = true;
        Ok(())
    }
    pub(crate) fn compute_dependency(&self) -> Option<u64> {
        self.compute_dependency.then_some(self.metadata.va() + 0x380)
    }
    pub(crate) fn wait_for_prior_fragment(&mut self, stamp_address: u64,
        slot: u8, value: u32) -> Result<u64> {
        let mut barrier = [0; 0x40];
        wire::render_dependency(&mut barrier, stamp_address, slot,
            value, self.stamp).map_err(|_| EINVAL)?;
        barrier[0x3c..0x40].copy_from_slice(&1u32.to_le_bytes());
        // 0x400..0x41f contains stamps; 0x500 begins the UMA counters.
        self.metadata.write(0x440, &barrier)?;
        Ok(self.metadata.va() + 0x440)
    }
    pub(crate) fn init_address(&self) -> Option<u64> {
        self.init_parameters.then_some(self.metadata.va()+0x300)
    }
    pub(crate) fn fragment_dependency(&self) -> u64 { self.metadata.va()+0x340 }
    pub(crate) fn fragment_address(&self) -> u64 { self.commands[1].va() }
    pub(crate) fn pool_slot(&self) -> u8 { self.pool_slot }
    pub(crate) fn same_vm(&self, vm: &mmu::Vm) -> bool { self.binding.matches(vm) }
    pub(crate) fn context(&self) -> u32 { self.binding.slot() }
    pub(crate) fn uma_submitted(&self) -> u64 { self.uma_submitted }
    pub(crate) fn uma_completed(&mut self) -> Result<u64> {
        self.uma.read_u64(0x54)
    }
    /// Snapshot retained firmware/GPU state without acknowledging faults or
    /// changing the in-flight pass. Keep diagnostics off the submission path.
    pub(crate) fn log_progress(&mut self, dev: &driver::AsahiDevice) -> Result {
        dev_info!(dev.as_ref(), "G16G: stalled render stage={:?} context={}\n",
            self.stage, self.context());
        dev_info!(dev.as_ref(), "G16G: render GPU timestamps={:x?}\n", self.timestamps()?);
        for (stage, offset) in [("ta", 0x100), ("fragment", 0x200)] {
            let mut words = [0u64; 4];
            for (index, word) in words.iter_mut().enumerate() {
                *word = self.uma.read_u64(offset+index*8)?;
            }
            dev_info!(dev.as_ref(), "G16G: render UMA metrics {}={:x?}\n", stage, words);
        }
        let mut parameters = self.parameters.lock();
        let crate::g16_parameter::Parameters { manager, ring, .. } = &mut *parameters;
        let [ta_command, fragment_command] = &mut self.commands;
        let [ta_sequence, fragment_sequence] = &mut self.sequences;
        for (name, buffer, start, length) in [
            ("command-ta", ta_command, 0, Stage::Tiling.command_size()),
            ("command-fragment", fragment_command, 0, Stage::Fragment.command_size()),
            ("sequence-ta", ta_sequence, 0, wire::sequence_size(Stage::Tiling)),
            ("sequence-fragment", fragment_sequence, 0, wire::sequence_size(Stage::Fragment)),
            ("parameter-manager", manager, 0, 0xc0),
            ("parameter-ring", ring, 0, 8),
            ("scene", &mut self.metadata, 0, 0x80),
            ("scene-statistics", &mut self.scratch, 0, 0x80),
            ("uma", &mut self.uma, 0, 0xa0),
        ] {
            for offset in (start..start+length).step_by(32) {
                let count = (start+length-offset).min(32)/8;
                let mut words = [0u64; 4];
                for (index, word) in words[..count].iter_mut().enumerate() {
                    *word = buffer.read_u64(offset+index*8)?;
                }
                dev_info!(dev.as_ref(), "G16G: render {} +{:#x}={:x?}\n",
                    name, offset, &words[..count]);
            }
        }
        Ok(())
    }
    pub(crate) fn status(&mut self) -> Result<[u64;4]> {
        let i = self.index();
        Ok([self.metadata.read_u32(0x400+i*16)? as u64,self.metadata.read_u32(0x408+i*16)? as u64,
            self.commands[i].read_u32(if i==0 {0x918} else {0xc60})? as u64,
            self.parameters.lock().manager.read_u32(0x54)? as u64])
    }
    pub(crate) fn set_user_timestamps(&mut self, addresses: [[u64; 2]; 2]) -> Result {
        for (i, pair) in addresses.into_iter().enumerate() {
            // Timestamp+24 is the optional user pointer pair, as in compute.
            // Keep the internal timestamps separate for retirement diagnostics.
            // The packet retains each validated TimestampBuffer mapping until
            // both engines retire, including failed jobs.
            let offset = 0x610 + i * 0x20;
            self.metadata.u64(offset, pair[0])?;
            self.metadata.u64(offset + 8, pair[1])?;
            let pointer = if pair == [0, 0] { 0 } else { self.metadata.va() + offset as u64 };
            let start = if i == 0 { 0x1cc } else { 0x1ec };
            self.sequences[i].u64(start + 0x24, pointer)?;
            self.sequences[i].u64(start + 0x50 + 0x24, pointer)?;
        }
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn set_fragment_timestamp_copies(&mut self, addresses: [u64; 8]) -> Result {
        let mut pairs = [0u64; 8];
        let mut count = 0;
        for address in addresses.into_iter().filter(|address| *address != 0) {
            let offset = 0x700 + count * 16;
            self.metadata.u64(offset, 0)?;
            self.metadata.u64(offset + 8, address)?;
            pairs[count] = self.metadata.va() + offset as u64;
            count += 1;
        }
        if count == 0 { return Ok(()); }
        let mut sequence = [0u8; 0x800];
        self.sequences[1].read(0, &mut sequence[..wire::sequence_size(Stage::Fragment)])?;
        let size = wire::fragment_timestamp_copies(&mut sequence, &pairs[..count]).map_err(|_| EINVAL)?;
        self.sequences[1].write(0, &sequence[..size])?;
        self.commands[1].u32(0x1c, size.try_into()?)?;
        g16_memory::publish();
        Ok(())
    }
    pub(crate) fn omit_internal_timestamps(&mut self, omit: [bool; 2]) -> Result {
        for (i, stage) in [Stage::Tiling, Stage::Fragment].into_iter().enumerate() {
            if !omit[i] { continue; }
            let mut sequence = [0u8; 0x380];
            let total = wire::sequence_size(stage);
            self.sequences[i].read(0, &mut sequence[..total])?;
            let size = crate::g16_compute::omit_timestamps(&mut sequence,
                if i == 0 { 0x1cc } else { 0x1ec }, total,
                if i == 0 { 0x80 } else { 0xa0 }).map_err(|_| EINVAL)?;
            self.sequences[i].write(0, &sequence[..total])?;
            self.commands[i].u32(if i == 0 { 0x778 } else { 0x1c }, size.try_into()?)?;
        }
        Ok(())
    }
    pub(crate) fn timestamps(&mut self) -> Result<[[u64; 2]; 2]> {
        Ok([[self.metadata.read_u64(0x600)?, self.metadata.read_u64(0x608)?],
            [self.metadata.read_u64(0x620)?, self.metadata.read_u64(0x628)?]])
    }
}
