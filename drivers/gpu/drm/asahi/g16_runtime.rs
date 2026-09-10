// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned M4 runtime: stop ASC before dropping transport, mappings or power.

use core::mem::ManuallyDrop;
use kernel::{device::Core, platform, prelude::*, soc::apple::rtkit, sync::Arc,
    time::{delay::fsleep, Delta}};
use crate::{driver, g16_device::Device, g16_rtkit, mmu};

struct Inner {
    // Drop order is part of the DMA lifetime contract. RTKit drains callbacks
    // before UAT mappings are reclaimed, and Device releases power last.
    transport: rtkit::RtKit<g16_rtkit::Operations>,
    state: Arc<g16_rtkit::State>,
    queue: crate::g16_queue::Queue,
    compute_queue: crate::g16_queue::Queue,
    fragment_queue: crate::g16_queue::Queue,
    fragment_registered: bool,
    retired_render: Option<KBox<crate::g16_render_job::Render>>,
    // Older than retired_render: firmware's replacement has itself retired.
    reusable_render: Option<KBox<crate::g16_render_job::Render>>,
    retired_compute: Option<KBox<crate::g16_job::Compute>>,
    compute: crate::g16_job::Compute,
    config: crate::g16_config::Config,
    uat: mmu::Uat,
    drm: driver::AsahiDevRef,
    device: Device,
    generation: u32,
    gpu_pending: bool,
    failed: bool,
    user_generation: u8,
    render_stamp: u32,
    compute_stamp: u32,
    active: Option<Active>,
}

enum Work {
    Compute(KBox<crate::g16_job::Compute>),
    Render(KBox<crate::g16_render_job::Render>),
}
impl Work {
    fn pipe(&self) -> usize { match self { Self::Compute(_) => 2, Self::Render(r) => r.index() } }
    fn stamp(&self) -> u32 {
        match self { Self::Compute(c) => c.stamp(), Self::Render(r) => r.stamp() }
    }
    fn status(&mut self) -> Result<[u64; 4]> {
        match self { Self::Compute(c) => { let s=c.status()?; Ok([s[0],s[1],s[2],s[3]]) }, Self::Render(r)=>r.status() }
    }
}
impl Inner {
    fn publish_active(&mut self) -> Result {
        let active = self.active.as_mut().ok_or(EIO)?;
        if let Work::Render(r) = &active.work {
            // Publish both retained stages. Waiting on the CPU for final TA
            // retirement deadlocks a pass which needs a partial fragment to
            // free parameter pages. The firmware barrier encodes that DAG.
            self.queue.advance_threshold()?;
            self.fragment_queue.advance_threshold()?;
            self.queue.set_render_context(r.context())?;
            self.fragment_queue.set_render_context(r.context())?;
            self.config.parameter_descriptor(r.descriptor())?;
            let (ta,ta_next) = self.queue.append_batch_idle(&[r.init_address(),r.address()])?;
            let (frag,frag_next) = self.fragment_queue.append_batch_idle(
                &[r.fragment_dependency(),r.fragment_address()])?;
            active.queue_next[0] = ta_next;
            active.queue_next[1] = frag_next;
            active.pipe_next[0] = self.config.publish_queue(0,ta,ta_next.try_into()?,
                crate::g16_render_job::SLOTS[0],false)?;
            active.pipe_next[1] = self.config.publish_queue(1,frag,frag_next.try_into()?,
                crate::g16_render_job::SLOTS[1],!self.fragment_registered)?;
            Pin::new(&mut self.transport).send_message(0x21,0x0083000000000000)?;
            Pin::new(&mut self.transport).send_message(0x21,0x0083000000000001)?;
            self.fragment_registered = true;
            return Ok(());
        }
        let Work::Compute(c) = &active.work else { return Err(EIO); };
        self.compute_queue.advance_threshold()?;
        let (address,next) = self.compute_queue.append_idle(c.address())?;
        active.queue_next[2] = next;
        active.pipe_next[2] = self.config.publish_queue(2,address,next.try_into()?,
            crate::g16_compute::SLOT,false)?;
        Pin::new(&mut self.transport).send_message(0x21,0x0083000000000002)?;
        Ok(())
    }
}
struct Active {
    packet: Arc<crate::g16_submit::Packet>,
    work: Work,
    index: usize,
    queue_next: [u32; 3],
    pipe_next: [u32; 3],
    started: kernel::time::Instant<kernel::time::Monotonic>,
}

pub(crate) struct Runtime {
    inner: ManuallyDrop<Inner>,
}

impl Runtime {
    pub(crate) fn drm(&self) -> driver::AsahiDevRef { self.inner.drm.clone() }
    pub(crate) fn core_mask(&self) -> u32 { self.inner.device.core_mask() }
    pub(crate) fn healthy(&self) -> bool { !self.inner.failed && self.inner.state.healthy() }
    pub(crate) fn new_user_vm(&mut self, id: u64, range: core::ops::Range<u64>) -> Result<mmu::Vm> {
        use crate::util::RangeExt;
        let driver_range = 0x70_0000_0000..0x80_0000_0000;
        if range.overlaps(driver_range.clone()) { return Err(EINVAL); }
        let vm = self.inner.uat.new_vm(id, range)?;
        let mut reserved = KVec::new();
        reserved.push(driver_range, GFP_KERNEL)?;
        let mut mappings = KVec::new();
        mappings.push(self.inner.config.bind_uma_table(&vm)?, GFP_KERNEL)?;
        mappings.push(self.inner.config.bind_parameter_table(&vm)?, GFP_KERNEL)?;
        // M4 TA writes context save state at the architectural compact base
        // (normal VA 0x1000000000), including for a zero active selector.
        // Own and reserve this per-VM backing in the driver, not in Mesa.
        let scratch_range = 0x10_0000_0000..0x10_0001_0000;
        if vm.driver_range_overlaps(scratch_range.clone()) { return Err(EINVAL); }
        let mut scratch = crate::gem::new_kernel_object_wc(&self.inner.drm, 0x10000)?;
        scratch.vmap()?.memset(0);
        mappings.push(scratch.map_at(&vm, scratch_range.start, mmu::PROT_GPU_SHARED_RW, true)?, GFP_KERNEL)?;
        reserved.push(scratch_range, GFP_KERNEL)?;
        vm.install_driver_mappings(mappings, reserved)?;
        Ok(vm)
    }

    pub(crate) fn map_timestamp(&self, mut bo: crate::gem::ObjectRef,
        range: core::ops::Range<usize>) -> Result<mmu::KernelMapping> {
        bo.map_range_into_range(self.inner.uat.kernel_vm(), range,
            crate::g16_memory::KERNEL_RANGE, mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW, false)
    }

    pub(crate) fn publish(&mut self, packet: Arc<crate::g16_submit::Packet>) -> Result {
        if !self.healthy() { return Err(EIO); }
        if self.inner.active.is_some() { return Err(EBUSY); }
        self.start_command(packet, 0)
    }

    fn start_command(&mut self, packet: Arc<crate::g16_submit::Packet>, index: usize) -> Result {
        let inner = &mut *self.inner;
        inner.config.reset_trace_budget();
        let payload = packet.commands.get(index).ok_or(EINVAL)?.payload;
        let generation = inner.user_generation.wrapping_add(1).max(1);
        // Firmware indexes its per-event command records with stamp[14:8].
        // Reusing 0x100 aliases every render to the same record. Advance the
        // event value exactly as the common Asahi queues do, across clients.
        if matches!(payload, crate::g16_submit::Payload::Render { .. }) {
            inner.render_stamp = inner.render_stamp.wrapping_add(0x100).max(0x100);
        } else {
            inner.compute_stamp = inner.compute_stamp.wrapping_add(0x100).max(0x100);
        }
        let mut work = match payload {
            crate::g16_submit::Payload::Compute(control) => Work::Compute(KBox::new(crate::g16_job::Compute::in_vm(
                &inner.drm,&inner.uat,&mut inner.config,&packet.vm,inner.compute_queue.address(),Some(control),
                Some(inner.compute_queue.notification()),generation,inner.compute_stamp)?, GFP_KERNEL)?),
            crate::g16_submit::Payload::Render { command,usc } => Work::Render(crate::g16_render_job::Render::new(
                &inner.drm,&inner.uat,&packet.vm,command,usc,generation,inner.render_stamp,
                [inner.queue.address(),inner.fragment_queue.address()],
                [inner.queue.notification().address,inner.fragment_queue.notification().address],
                inner.config.render_stats(), inner.reusable_render.take())?),
        };
        let attachments = &packet.commands[index].attachments;
        match &mut work {
            Work::Compute(compute) => compute.set_attachments(&attachments[0])?,
            Work::Render(render) => render.set_attachments(attachments)?,
        }
        inner.user_generation = generation;
        inner.device.check_boot_pstate()?;
        // Retain all DMA backing before the first shared publication, including
        // failure paths that cannot prove whether a mailbox doorbell arrived.
        inner.active = Some(Active { packet, work, index, queue_next:[0;3],pipe_next:[0;3],
            started:kernel::time::Instant::<kernel::time::Monotonic>::now() });
        inner.gpu_pending = true;
        let publish = inner.publish_active();
        if let Err(error) = publish {
            inner.failed = true;
            dev_err!(inner.drm.as_ref(), "G16G: GPU retirement unproven after userspace publication error {:?}\n", error);
        }
        publish
    }

    pub(crate) fn fail_job(&mut self, id: u64, error: Error) {
        let inner = &mut *self.inner;
        if let Some(active) = inner.active.as_ref() {
            if active.packet.id == id {
                inner.failed = true;
                active.packet.finish(Err(error));
                dev_err!(inner.drm.as_ref(), "G16G: GPU retirement unproven, job {} error {:?}; retaining DMA owners\n", id, error);
            }
        }
    }

    /// Poll one completion snapshot. The worker drops the runtime mutex between
    /// snapshots, so queued ioctls and device removal can make progress.
    pub(crate) fn service_job(&mut self) -> bool {
        if self.inner.failed { return false; }
        if self.inner.active.is_none() {
            let inner = &mut *self.inner;
            if let Err(error) = inner.config.drain(&inner.drm) {
                inner.failed = true;
                inner.gpu_pending = true;
                let _ = inner.device.log_engine_state();
                dev_err!(inner.drm.as_ref(), "G16G: GPU retirement unproven after idle firmware error {:?}; retaining DMA owners\n", error);
            }
            return false;
        }
        let id = self.inner.active.as_ref().unwrap().packet.id;
        match self.poll_job() {
            Ok(pending) => pending,
            Err(error) => {
                let _ = self.inner.device.log_engine_state();
                self.fail_job(id, error); false
            }
        }
    }
    fn poll_job(&mut self) -> Result<bool> {
        let inner = &mut *self.inner;
        inner.config.drain(&inner.drm)?;
        if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
        inner.device.check_boot_pstate()?;
        let active = inner.active.as_mut().ok_or(EIO)?;
        let status = active.work.status()?;
        let pipe_index = active.work.pipe();
        let queue = match pipe_index { 0=>inner.queue.status()?,1=>inner.fragment_queue.status()?,_=>inner.compute_queue.status()? };
        let pipe = inner.config.pipe_indices(pipe_index)?;
        let next = active.queue_next[pipe_index];
        if status[0] == active.work.stamp() as u64
            && status[1] == active.work.stamp() as u64
            && (pipe_index != 2 || (status[2] != 0 && status[3] > status[2]))
            && queue == [next, next, next, next, next, 0]
            && pipe == [active.pipe_next[pipe_index]; 3] {
            if let Work::Render(render) = &mut active.work {
                dev_info!(inner.drm.as_ref(),"G16G: userspace render stage {:?} retired stamps={:x?} queue={:?}\n",render.stage,status,queue);
                if render.stage == crate::g16_render::Stage::Tiling {
                    render.stage = crate::g16_render::Stage::Fragment;
                    active.started = kernel::time::Instant::<kernel::time::Monotonic>::now();
                    return Ok(true);
                }
            } else {
                if let Work::Compute(compute) = &mut active.work {
                    dev_info!(inner.drm.as_ref(), "G16G: compute completion status={:x?}\n", compute.status()?);
                }
                active.packet.commands[active.index].completed([status[2], status[3]])?;
            }
            let active = inner.active.take().ok_or(EIO)?;
            inner.gpu_pending = false;
            let packet = active.packet.clone();
            let index = active.index + 1;
            // Exact firmware/engine retirement permits releasing command,
            // private-memory pool and ASID lease before the next command.
            match active.work {
                Work::Render(render) => {
                    // Firmware caches the parameter manager slot. Only after
                    // its replacement retires may the predecessor be freed or
                    // reused by a matching VM/pass geometry. A cache miss
                    // allocates normally and drops the spare.
                    inner.reusable_render = inner.retired_render.replace(render);
                }
                Work::Compute(compute) => {
                    // Firmware caches the UMA descriptor by its canonical VA.
                    // Reusing a freed command at that VA can retain the prior
                    // client's pool address after the ASID changes. Keep its
                    // descriptor and backing until a distinct replacement has
                    // retired, just as for the render parameter manager.
                    inner.retired_compute = Some(compute);
                }
            }
            if index == packet.commands.len() {
                dev_info!(inner.drm.as_ref(), "G16G: userspace GPU job {} retired ({} commands) monotonic_ns={}\n", packet.id, index, <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get());
                packet.finish(Ok(()));
                return Ok(false);
            }
            if let Err(error) = self.start_command(packet.clone(), index) {
                packet.finish(Err(error));
                return Err(error);
            }
            return Ok(true);
        }
        if active.started.elapsed() >= Delta::from_secs(2) {
            dev_err!(inner.drm.as_ref(), "G16G: userspace GPU timeout job={} status={:x?} queue={:?} pipe={:?}\n",
                active.packet.id, status, queue, pipe);
            inner.device.log_engine_state()?;
            match &mut active.work {
                Work::Compute(c) => c.log_progress(&inner.drm)?,
                Work::Render(r) => r.log_progress(&inner.drm)?,
            }
            inner.config.log_firmware_state(&inner.drm)?;
            return Err(ETIMEDOUT);
        }
        Ok(true)
    }

    pub(crate) fn new(pdev: &platform::Device<Core>, device: Device) -> Result<Self> {
        device.require_stopped(pdev)?;
        let drm: driver::AsahiDevRef = kernel::drm::Device::new(
            pdev.as_ref(), driver::AsahiData::new(pdev, None))?;
        // SAFETY: Device exclusively owns ASC control and power. Construction
        // is stopped, and Runtime::drop stops ASC before dropping this UAT.
        let uat = unsafe { mmu::Uat::new_t8132(&drm, device.firmware()) }?;
        let mut config = crate::g16_config::Config::new(pdev, &drm, uat.kernel_vm(), uat.kernel_lower_vm(), device.firmware())?;
        let queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), 256)?;
        let compute_queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), 256)?;
        let fragment_queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), 256)?;
        let compute = crate::g16_job::Compute::new(&drm, &uat, &mut config, compute_queue.address())?;
        let state = g16_rtkit::State::new(pdev, drm.clone(), device.firmware().resources.regions[5])?;
        let transport = rtkit::RtKit::new(pdev.as_ref(), None, 0, state.clone())?;
        Ok(Self { inner: ManuallyDrop::new(Inner { transport, state, queue, compute_queue, fragment_queue, fragment_registered:false, retired_render:None,reusable_render:None,retired_compute:None, compute, config, uat, drm, device, generation: 0, gpu_pending: false, failed: false, user_generation: 1, render_stamp: 0, compute_stamp: crate::g16_compute::STAMP, active: None }) })
    }

    pub(crate) fn boot(&mut self, pdev: &platform::Device<Core>) -> Result {
        self.inner.device.start_asc(pdev)?;
        Pin::new(&mut self.inner.transport).wake()?;
        if !Pin::new(&self.inner.transport).is_running() || !self.inner.state.healthy() {
            return Err(EIO);
        }
        for endpoint in [0x20, 0x21] {
            if !Pin::new(&mut self.inner.transport).has_endpoint(endpoint) { return Err(ENODEV); }
            Pin::new(&mut self.inner.transport).start_endpoint(endpoint)?;
        }
        fsleep(Delta::from_millis(100));
        if !self.inner.state.healthy() { return Err(EIO); }
        let mut roots = [(0, 0); 1];
        self.inner.uat.kernel_vm().context_roots(&mut roots)?;
        dev_info!(pdev.as_ref(), "G16G: RTKit IOP/AP running with owned common UAT, context0=[{:#x},{:#x}]; application endpoints started, no initdata sent\n", roots[0].0, roots[0].1);
        self.inner.device.check_boot_pstate()?;
        self.inner.device.set_power_generation(0)?;
        {
            let inner = &mut *self.inner;
            inner.config.enqueue_control(0x1a, 0, &mut inner.generation, &inner.device)?;
        }
        self.inner.config.publish_loader()?;
        let root = self.inner.config.root();
        dev_info!(pdev.as_ref(), "G16G: sending relocated GEM initdata root={:#x}\n", root);
        Pin::new(&mut self.inner.transport).send_message(0x20, 0x0081000000000000 | (root & ((1u64 << 44) - 1)))?;
        Pin::new(&mut self.inner.transport).send_message(0x21, 0x0083000000000011)?;
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        loop {
            let inner = &mut *self.inner;
            inner.config.drain(&inner.drm)?;
            if !inner.state.healthy() { return Err(EIO); }
            if inner.config.ready()? {
                dev_info!(pdev.as_ref(), "G16G: firmware accepted relocated GEM initdata, control-ready=1; no GPU work queued\n");
                break;
            }
            if start.elapsed() >= Delta::from_secs(5) { return Err(ETIMEDOUT); }
            fsleep(Delta::from_millis(10));
        }
        self.wait_control(1)?;
        self.send_control(0x34)?;
        self.inner.config.rearm_scheduler()?;
        self.send_control(0x0a)?;
        Pin::new(&mut self.inner.transport).send_message(0x21, 0x0083000000000010)?;
        fsleep(Delta::from_millis(100));
        let inner = &mut *self.inner;
        inner.config.drain(&inner.drm)?;
        inner.device.check_boot_pstate()?;
        if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
        dev_info!(pdev.as_ref(), "G16G: device-control NOP, RIARTT and idle-power policy consumed; firmware ready, boot pstate retained\n");
        self.check_barrier()?;
        self.check_compute()
    }

    fn check_barrier(&mut self) -> Result {
        let inner = &mut *self.inner;
        let queue = inner.queue.prepare_barrier()?;
        let next = inner.config.publish_queue(0, queue, 1, 0, true)?;
        Pin::new(&mut inner.transport).send_message(0x21, 0x0083000000000000)?;
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        let mut completed = None;
        loop {
            let inner = &mut *self.inner;
            inner.config.drain(&inner.drm)?;
            if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
            inner.device.check_boot_pstate()?;
            let pipe = inner.config.pipe_indices(0)?;
            let queue = inner.queue.status()?;
            if pipe == [next; 3] && queue == [1, 1, 1, 1, 1, 0] && inner.queue.context_registered()? {
                let completion = completed.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                if completion.elapsed() >= Delta::from_millis(50) {
                    dev_info!(inner.drm.as_ref(), "G16G: owned GEM queue barrier retired, pipe={:?} queue={:?}, context registered, firmware ready\n", pipe, queue);
                    return Ok(());
                }
            } else if completed.is_some() { return Err(EIO); }
            if start.elapsed() >= Delta::from_secs(2) {
                dev_err!(inner.drm.as_ref(), "G16G: queue barrier timeout pipe={:?} queue={:?}\n", pipe, queue);
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_millis(10));
        }
    }

    fn check_compute(&mut self) -> Result {
        use crate::g16_compute::{SLOT, STAMP};
        let inner = &mut *self.inner;
        inner.device.check_boot_pstate()?;
        inner.device.log_engine_state()?;
        dev_info!(inner.drm.as_ref(), "G16G: compute publication power-generation={}/{}\n", inner.config.consumed_generation()?, inner.generation);
        let mut roots = [(0, 0); 1];
        inner.uat.kernel_vm().context_roots(&mut roots)?;
        dev_info!(inner.drm.as_ref(), "G16G: pre-compute context0 roots={:x?}\n", roots);
        inner.config.log_firmware_state(&inner.drm)?;
        inner.compute.log_context(&inner.drm)?;
        // Mark before the first queue publication. Even an unsuccessful
        // mailbox send can have delivered its doorbell to firmware.
        inner.gpu_pending = true;
        let queue = inner.compute_queue.prepare_command(inner.compute.address())?;
        let next = inner.config.publish_queue(2, queue, 1, SLOT, true)?;
        Pin::new(&mut inner.transport).send_message(0x21, 0x0083000000000002)?;
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        let mut completed = None;
        loop {
            let inner = &mut *self.inner;
            let status = inner.compute.status()?;
            let pipe = inner.config.pipe_indices(2)?;
            let queue = inner.compute_queue.status()?;
            if let Err(error) = inner.config.drain(&inner.drm) {
                dev_err!(inner.drm.as_ref(), "G16G: compute firmware error pipe={:?} queue={:?} status={:x?}\n", pipe, queue, status);
                return Err(error);
            }
            if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
            inner.device.check_boot_pstate()?;
            // The two firmware stores have different producers: notifier
            // completion and in-order engine-node retirement. Require both,
            // valid timestamp order, queue idle, and a stability interval.
            if pipe == [next; 3] && queue == [1, 1, 1, 1, 1, 0]
                && status[0] == STAMP as u64 && status[1] == STAMP as u64
                && status[2] != 0 && status[3] > status[2]
                && status[9] == crate::g16_dispatch::VALUE as u64 {
                let completion = completed.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                if completion.elapsed() >= Delta::from_millis(50) {
                    inner.gpu_pending = false;
                    dev_info!(inner.drm.as_ref(), "G16G: owned compute store retired, stamps/timestamps={:x?} queue={:?}, firmware ready\n", status, queue);
                    return Ok(());
                }
            } else if completed.is_some() { return Err(EIO); }
            if start.elapsed() >= Delta::from_secs(2) {
                dev_err!(inner.drm.as_ref(), "G16G: compute timeout pipe={:?} queue={:?} status={:x?}\n", pipe, queue, status);
                inner.config.log_firmware_state(&inner.drm)?;
                inner.compute.log_progress(&inner.drm)?;
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_micros(100));
        }
    }

    fn send_control(&mut self, opcode: u32) -> Result {
        let inner = &mut *self.inner;
        let next = inner.config.enqueue_control(opcode, 0, &mut inner.generation, &inner.device)?;
        Pin::new(&mut inner.transport).send_message(0x21, 0x0084000000000011)?;
        self.wait_control(next)
    }

    fn wait_control(&mut self, next: u32) -> Result {
        let start = kernel::time::Instant::<kernel::time::Monotonic>::now();
        let mut completed = None;
        loop {
            let inner = &mut *self.inner;
            inner.config.drain(&inner.drm)?;
            inner.device.check_boot_pstate()?;
            if !inner.state.healthy() || !inner.config.ready()? { return Err(EIO); }
            let indices = inner.config.control_indices()?;
            // The startup NOP is retired without incrementing fw-data+0xeae8;
            // established M4 runs show 1/0, 2/1, 3/2 host/consumed. That
            // counter is power accounting, not a command-completion fence.
            if indices == [next; 3] {
                let completion = completed.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                if completion.elapsed() >= Delta::from_millis(50) {
                    dev_info!(inner.drm.as_ref(), "G16G: device-control retired index={} power-generation={}/{}\n",
                        next, inner.config.consumed_generation()?, inner.generation);
                    return Ok(());
                }
            } else if completed.is_some() { return Err(EIO); }
            if start.elapsed() >= Delta::from_secs(2) {
                dev_err!(inner.drm.as_ref(), "G16G: device-control timeout indices={:?} expected={} power-generation={}/{}\n",
                    indices, next, inner.config.consumed_generation()?, inner.generation);
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_millis(10));
        }
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        if self.inner.gpu_pending {
            // Stopping ASC alone does not stop GPU DMA. Keep the complete
            // transport, mappings, GEM objects and PMP vote until reboot.
            // The leaked callback graph must pin the module's code too.
            // SAFETY: We are executing within this live module; the reference
            // is intentionally retained with Inner until system reset.
            unsafe { kernel::bindings::__module_get(crate::THIS_MODULE.as_ptr()) };
            let _ = self.inner.device.log_engine_state();
            let _ = self.inner.device.stop_asc();
            dev_err!(self.inner.drm.as_ref(), "G16G: GPU retirement unproven; retaining runtime until reboot\n");
            return;
        }
        if let Err(error) = self.inner.device.stop_asc() {
            // SAFETY: same retained callback/code lifetime as the DMA case.
            unsafe { kernel::bindings::__module_get(crate::THIS_MODULE.as_ptr()) };
            dev_err!(self.inner.drm.as_ref(), "G16G: ASC failed to stop; retaining runtime backing ({:?})\n", error);
            return;
        }
        dev_info!(self.inner.drm.as_ref(), "G16G: ASC stopped before RTKit transport cleanup\n");
        // SAFETY: ASC is stopped and any submitted engine work retired.
        // Incomplete or failed jobs retain Inner above. Callbacks drain before UAT and power are released.
        unsafe { ManuallyDrop::drop(&mut self.inner) };
    }
}
