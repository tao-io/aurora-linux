// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Common DRM scheduling and syncobj ownership for M4 submissions.
use kernel::{c_str, dma_fence::*, drm::sched, new_mutex, prelude::*,
    sync::{Arc, Mutex}, xarray};
use crate::{driver, file, g16_drm::Shared, g17_uapi, mmu, queue};

#[derive(Default)]
pub(crate) struct Completion;
#[vtable]
impl FenceOps for Completion {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("asahi") }
    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr { c_str!("m4-gpu") }
}

pub(crate) struct Destination { mapping: Arc<mmu::KernelMapping>, offset: usize }
impl Destination {
    fn resolve(objects: Pin<&xarray::XArray<KBox<file::Object>>>,
        timestamp: g17_uapi::UapiTimestamp) -> Result<Option<Self>> {
        if timestamp.handle == 0 { return Ok(None); }
        let guard = objects.lock();
        let object = guard.get(timestamp.handle.try_into()?).ok_or(ENOENT)?;
        match &*object {
            file::Object::TimestampBuffer(mapping) => {
                let offset = timestamp.offset as usize;
                if offset & 7 != 0 || offset.checked_add(8).ok_or(EOVERFLOW)? > mapping.size() {
                    return Err(EINVAL);
                }
                Ok(Some(Self { mapping: mapping.clone(), offset }))
            }
        }
    }
    fn write(&self, ticks: u64) -> Result {
        // GET_PARAMS exposes nanoseconds; the hardware clock is 24 MHz.
        let ns = (ticks / 3).checked_mul(125).and_then(|whole|
            whole.checked_add((ticks % 3) * 125 / 3)).ok_or(EOVERFLOW)?;
        self.mapping.with_cpu_bytes(|bytes| {
            bytes[self.offset..self.offset + 8].copy_from_slice(&ns.to_le_bytes());
            Ok(())
        })
    }
}
#[derive(Clone, Copy)]
pub(crate) enum Payload {
    Compute(crate::g16_compute::Control),
    Render { command: g17_uapi::UapiRenderCommand, usc: u64 },
}
pub(crate) struct Command {
    pub(crate) payload: Payload,
    pub(crate) attachments: [crate::g16_attachments::Attachments; 2],
    timestamps: [Option<Destination>; 2],
}
impl Command {
    pub(crate) fn completed(&self, timestamps: [u64; 2]) -> Result {
        for (dst, ticks) in self.timestamps.iter().zip(timestamps) {
            if let Some(dst) = dst { dst.write(ticks)?; }
        }
        Ok(())
    }
}

// Queue-owned, fully displaced render storage. The runtime retains the most
// recent firmware owner separately; only its retired predecessor returns here.
// This avoids losing every game allocation when the compositor submits next.
// Queue/packet ownership bounds lifetime without a global VM-pinning cache.
pub(crate) type RenderCache = Arc<Mutex<Option<KBox<crate::g16_render_job::Render>>>>;

pub(crate) struct Packet {
    pub(crate) id: u64,
    pub(crate) vm: mmu::Vm,
    pub(crate) commands: KVec<Command>,
    pub(crate) render_cache: RenderCache,
    pub(crate) completion: UserFence<Completion>,
    _vm_job: mmu::T8140VmJobGuard,
}
impl Packet {
    pub(crate) fn finish(&self, result: Result) {
        if let Err(error) = result {
            self.vm.status().record(error.to_errno());
            self.completion.set_error(error);
        }
        crate::g16_memory::publish();
        self.completion.signal();
    }
}

pub(crate) struct Job { shared: Shared, packet: Arc<Packet>, dev: driver::AsahiDevRef }
impl sched::JobImpl for Job {
    fn run(job: &mut sched::Job<Self>) -> Result<Option<Fence>> {
        let mut guard = job.shared.lock();
        let result = Option::as_mut(&mut *guard).ok_or(ENODEV)
            .and_then(|runtime| runtime.publish(job.packet.clone()));
        if let Err(error) = result {
            job.packet.finish(Err(error));
            return Err(error);
        }
        drop(guard);
        driver::queue_g16_completion_worker(job.dev.clone());
        Ok(Some(Fence::from_fence(&job.packet.completion)))
    }
    fn timed_out(job: &mut sched::Job<Self>) -> sched::Status {
        if let Some(runtime) = Option::as_mut(&mut *job.shared.lock()) {
            runtime.fail_job(job.packet.id, ETIMEDOUT);
        }
        sched::Status::NoDevice
    }
    fn cancel(job: &mut sched::Job<Self>) {
        let mut guard = job.shared.lock();
        if let Some(runtime) = Option::as_mut(&mut *guard) {
            runtime.fail_job(job.packet.id, ECANCELED);
        }
        // Any published packet remains retained by runtime even if DRM
        // cancels its scheduling reference. Unpublished jobs have no DMA.
        job.packet.finish(Err(ECANCELED));
    }
}

struct AddressSpace<'a>(&'a mmu::Vm);
impl g17_uapi::GpuAddressSpace for AddressSpace<'_> {
    fn covers(&self, address: u64, size: u64, access: g17_uapi::GpuAccess) -> bool {
        let Some(end) = address.checked_add(size) else { return false; };
        let (read, write) = match access {
            g17_uapi::GpuAccess::Read => (true, false),
            g17_uapi::GpuAccess::Write => (false, true),
            g17_uapi::GpuAccess::ReadWrite => (true, true),
        };
        address >= 0x4000 && end <= crate::g16_drm::USER_TOP
            && !self.0.driver_range_overlaps(address..end)
            && self.0.covers_range(address, size, read, write)
    }
}

pub(crate) struct Queue {
    entity: sched::Entity<Job>,
    _scheduler: Arc<sched::Scheduler<Job>>,
    shared: Shared,
    dev: driver::AsahiDevRef,
    vm: mmu::Vm,
    usc: u64,
    fences: FenceContexts,
    render_cache: RenderCache,
}

impl Queue {
    pub(crate) fn new(shared: Shared, scheduler: Arc<sched::Scheduler<Job>>,
        dev: driver::AsahiDevRef, vm: mmu::Vm, priority: u32, usc: u64) -> Result<Self> {
        g17_uapi::QueueUscWindow { base: usc, user_start: 0x4000,
            user_end: crate::g16_drm::USER_TOP }.validate().map_err(|_| EINVAL)?;
        let priority = match priority {
            0 => sched::Priority::Low, 1 => sched::Priority::Normal,
            2 => sched::Priority::High, 3 => sched::Priority::Kernel,
            _ => return Err(EINVAL),
        };
        Ok(Self { entity: sched::Entity::new(&scheduler, priority)?, _scheduler: scheduler,
            shared, dev, vm, usc,
            render_cache: Arc::pin_init(new_mutex!(None), GFP_KERNEL)?,
            fences: FenceContexts::new(1, c_str!("asahi_m4_queue"), kernel::static_lock_class!())? })
    }
}
impl queue::Queue for Queue {
    fn submit(&mut self, id: u64, mut syncs: KVec<file::SyncItem>, in_sync_count: usize,
        raw: &[u8], objects: Pin<&xarray::XArray<KBox<file::Object>>>) -> Result {
        let vm_job = self.vm.retain_t8140_job()?;
        let mut parser = g17_uapi::UapiCommandParser::new(raw);
        let mut commands = KVec::new();
        while let Some(command) = parser.next_hardware().map_err(|_| EINVAL)? {
            if let g17_uapi::ParsedHardwareCommand::Render { payload, vertex_attachments, fragment_attachments, .. } = command {
                validate_render(payload, self.usc, &AddressSpace(&self.vm))?;
                g17_uapi::validate_attachments(&AddressSpace(&self.vm), &vertex_attachments).map_err(|_| EINVAL)?;
                g17_uapi::validate_attachments(&AddressSpace(&self.vm), &fragment_attachments).map_err(|_| EINVAL)?;
                commands.push(Command { payload: Payload::Render { command:payload, usc:self.usc },
                    attachments: [encode_attachments(&self.vm, &vertex_attachments)?,
                                  encode_attachments(&self.vm, &fragment_attachments)?],
                    timestamps:[None,None] }, GFP_KERNEL)?;
                continue;
            }
            let g17_uapi::ParsedHardwareCommand::Compute { payload, attachments, .. } = command
                else { return Err(EINVAL); };
            // Native sampler/helper programming is a separate M4 wire path.
            // Reject it explicitly until translated; never silently discard it.
            if payload.flags != 0 || payload.sampler_count != 0 || payload.sampler_heap != 0 {
                return Err(ENOTSUPP);
            }
            let cmd = g17_uapi::translate_compute_command(payload, attachments,
                g17_uapi::QueueUscWindow { base: self.usc, user_start: 0x4000,
                    user_end: crate::g16_drm::USER_TOP }, &AddressSpace(&self.vm))
                .map_err(|_| EINVAL)?;
            commands.push(Command {
                payload: Payload::Compute(crate::g16_compute::Control { base: cmd.control_stream_base,
                    end: cmd.control_stream_end, usc_base: self.usc }),
                attachments: [encode_attachments(&self.vm, &cmd.attachments)?,
                              crate::g16_attachments::Attachments::EMPTY],
                timestamps: [Destination::resolve(objects, cmd.timestamps.start)?,
                    Destination::resolve(objects, cmd.timestamps.end)?],
            }, GFP_KERNEL)?;
        }
        parser.finish().map_err(|_| EINVAL)?;
        let packet = Arc::new(Packet { id, vm: self.vm.clone(), commands,
            render_cache: self.render_cache.clone(),
            completion: self.fences.new_fence(0, Completion)?.into(), _vm_job: vm_job }, GFP_KERNEL)?;
        let mut job = self.entity.new_job(1, Job { shared: self.shared.clone(), packet, dev: self.dev.clone() })?;
        for sync in syncs.drain(0..in_sync_count) {
            if let Some(fence) = sync.fence { job.add_dependency(fence)?; }
        }
        let mut job = job.arm();
        let finished = job.fences().finished();
        job.push();
        for mut sync in syncs {
            if let Some(chain) = sync.chain_fence.take() {
                sync.syncobj.add_point(chain, &finished, sync.timeline_value);
            } else { sync.syncobj.replace_fence(Some(&finished)); }
        }
        Ok(())
    }
}

fn encode_attachments(vm: &mmu::Vm, list: &g17_uapi::UapiAttachmentList)
    -> Result<crate::g16_attachments::Attachments> {
    use crate::g16_attachments::{Attachments, CAPACITY};
    use g17_uapi::{GpuAddressSpace, GpuAccess};
    let mut ranges = [(0, 0); CAPACITY];
    for (range, entry) in ranges.iter_mut().zip(list.as_slice()) {
        *range = Attachments::range(entry.address, entry.size).map_err(|_| EINVAL)?;
        if !AddressSpace(vm).covers(range.0, range.1, GpuAccess::Write) {
            return Err(EINVAL);
        }
    }
    Attachments::new(&ranges[..list.as_slice().len()]).map_err(|_| EINVAL)
}

fn validate_render(r: g17_uapi::UapiRenderCommand, usc: u64, space: &AddressSpace<'_>) -> Result {
    use g17_uapi::{GpuAddressSpace, GpuAccess};
    let empty = g17_uapi::UapiHelperProgram { binary:0, config:0, data:0 };
    if r.flags & !(2|16) != 0 || r.layers != 1 || r.samples != 1
        || r.sampler_count != 0 || r.sampler_heap != 0
        || r.vertex_helper != empty || r.fragment_helper != empty
        || r.depth.compression_base != 0 || r.stencil.compression_base != 0
        || r.depth.compression_stride != 0 || r.stencil.compression_stride != 0
        || r.ppp_control & !0x203 != 0 { return Err(ENOTSUPP); }
    // The initial render SKU has no timestamp opcodes. Reject requests until
    // the genuine GPU timestamp sequence is ported; never substitute CPU time.
    for t in [r.vertex_timestamps.start,r.vertex_timestamps.end,
        r.fragment_timestamps.start,r.fragment_timestamps.end] {
        if t.handle != 0 { return Err(ENOTSUPP); }
    }
    // sample_size describes color tilebuffer storage. Depth/stencil-only
    // passes legitimately require zero bytes of color storage.
    if r.width == 0 || r.height == 0 || r.width > 16384 || r.height > 16384
        || !matches!(r.utile_width,16|32) || !matches!(r.utile_height,16|32)
        || u32::from(r.sample_size)*u32::from(r.utile_width)*u32::from(r.utile_height)>32768 {
        return Err(EINVAL);
    }
    crate::g16_render_state::compact(r.vdm_base).map_err(|_| EINVAL)?;
    if r.vdm_base & 3 != 0 || !space.covers(r.vdm_base,4,GpuAccess::Read)
        || r.scissor_base == 0 || r.scissor_base & 7 != 0 || !space.covers(r.scissor_base,8,GpuAccess::Read) {
        return Err(EINVAL);
    }
    for (p,access) in [(r.depth_bias_base,GpuAccess::Read),(r.occlusion_query_base,GpuAccess::Write)] {
        if p != 0 && (p & 7 != 0 || !space.covers(p,8,access)) { return Err(EINVAL); }
    }
    for zls in [r.depth,r.stencil] { g17_uapi::validate_zls(space,zls,1).map_err(|_| EINVAL)?; }
    let window = g17_uapi::QueueUscWindow { base:usc,user_start:0x4000,user_end:crate::g16_drm::USER_TOP };
    for p in [r.background,r.end_of_tile,r.partial_background,r.partial_end_of_tile] {
        window.full_program(space,p.usc).map_err(|_| EINVAL)?;
        if !space.covers(usc+u64::from(p.usc & !63),64,GpuAccess::Read) { return Err(EINVAL); }
    }
    Ok(())
}
