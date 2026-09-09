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
    compute: crate::g16_job::Compute,
    config: crate::g16_config::Config,
    uat: mmu::Uat,
    drm: driver::AsahiDevRef,
    device: Device,
    generation: u32,
    gpu_pending: bool,
}

pub(crate) struct Runtime {
    inner: ManuallyDrop<Inner>,
}

impl Runtime {
    pub(crate) fn new(pdev: &platform::Device<Core>, device: Device) -> Result<Self> {
        device.require_stopped(pdev)?;
        let drm: driver::AsahiDevRef = kernel::drm::Device::new(
            pdev.as_ref(), driver::AsahiData::new(pdev, None))?;
        // SAFETY: Device exclusively owns ASC control and power. Construction
        // is stopped, and Runtime::drop stops ASC before dropping this UAT.
        let uat = unsafe { mmu::Uat::new_t8132(&drm, device.firmware()) }?;
        let config = crate::g16_config::Config::new(pdev, &drm, uat.kernel_vm(), uat.kernel_lower_vm(), device.firmware())?;
        let queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), 256)?;
        let compute_queue = crate::g16_queue::Queue::new(&drm, uat.kernel_vm(), 256)?;
        let compute = crate::g16_job::Compute::new(&drm, &uat)?;
        let state = g16_rtkit::State::new(pdev, device.firmware().resources.regions[5])?;
        let transport = rtkit::RtKit::new(pdev.as_ref(), None, 0, state.clone())?;
        Ok(Self { inner: ManuallyDrop::new(Inner { transport, state, queue, compute_queue, compute, config, uat, drm, device, generation: 0, gpu_pending: false }) })
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
                && status[2] != 0 && status[3] > status[2] {
                let completion = completed.get_or_insert(kernel::time::Instant::<kernel::time::Monotonic>::now());
                if completion.elapsed() >= Delta::from_millis(50) {
                    inner.gpu_pending = false;
                    dev_info!(inner.drm.as_ref(), "G16G: owned compute terminate retired, stamps/timestamps={:x?} queue={:?}, firmware ready\n", status, queue);
                    return Ok(());
                }
            } else if completed.is_some() { return Err(EIO); }
            if start.elapsed() >= Delta::from_secs(2) {
                dev_err!(inner.drm.as_ref(), "G16G: compute timeout pipe={:?} queue={:?} status={:x?}\n", pipe, queue, status);
                inner.device.log_engine_state()?;
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
