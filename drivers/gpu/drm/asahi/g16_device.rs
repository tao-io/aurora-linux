// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Owned G16G register apertures and the inner PMP power vote. The platform
//! power domain owns TVM/PMGR and stays on across probe/remove. Firmware and
//! queues must be stopped before this object is released.

use kernel::{
    bindings, c_str,
    device::Core,
    devres::Devres,
    error::to_result,
    io::{
        mem::{ExclusiveIoMem, IoMem},
        Io,
    },
    platform,
    prelude::*,
    soc::apple::rtkit,
    sync::aref::ARef,
    time::{delay::fsleep, Delta, Instant, Monotonic},
};

use crate::g16_firmware::Firmware;

const ASC_CPU_CONTROL: usize = 0x44;
const ASC_CPU_RUN: u32 = 1 << 4;

struct Management;
enum NoBuffer {}

impl rtkit::Buffer for NoBuffer {
    fn iova(&self) -> Result<usize> {
        match *self {}
    }

    fn buf(&mut self) -> Result<kernel::iosys_map::IoSysMapRef<'_, u8>> {
        match *self {}
    }
}

#[kernel::macros::vtable]
impl rtkit::Operations for Management {
    type Data = ();
    type Buffer = NoBuffer;
}

pub(crate) struct Device {
    dev: ARef<platform::Device>,
    asc: Pin<KBox<Devres<ExclusiveIoMem<0x4000>>>>,
    sgx: Pin<KBox<Devres<IoMem>>>,
    firmware: Firmware,
    power_vote: bool,
}

impl Device {
    pub(crate) fn new(pdev: &platform::Device<Core>, firmware: Firmware) -> Result<Self> {
        // Claim ASC before taking a vote. The larger SGX aperture contains
        // this control window and the separate mailbox provider, so it is
        // mapped without a conflicting claim over those child resources.
        let asc = KBox::pin_init(
            pdev.io_request_by_name(c_str!("asc"))
                .ok_or(EINVAL)?
                .iomap_exclusive_sized::<0x4000>(),
            GFP_KERNEL,
        )?;
        let sgx = KBox::pin_init(
            pdev.io_request_by_name(c_str!("sgx"))
                .ok_or(EINVAL)?
                .iomap(),
            GFP_KERNEL,
        )?;
        // SAFETY: the platform device is live for the entire call. The C API
        // creates a managed supplier link to the declared, ready PMP driver.
        to_result(unsafe { bindings::apple_pmp_link_device(pdev.as_ref().as_raw()) })?;
        if asc.access(pdev.as_ref())?.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            dev_err!(
                pdev.as_ref(),
                "G16G: ASC already running; refusing to take ownership\n"
            );
            return Err(EBUSY);
        }
        let mut device = Self {
            dev: pdev.into(),
            asc,
            sgx,
            firmware,
            power_vote: false,
        };
        // SAFETY: PMP lifetime is linked above. Command 0xf, logical device 5
        // is the executed J713 AGX power contract; the bridge waits for ACK.
        to_result(unsafe { bindings::apple_pmp_set_device_power(0x0f, 5, 1) })?;
        device.power_vote = true;
        let registers = device.sgx.access(pdev.as_ref())?;
        let version = registers.try_read32(0xd04000)?;
        let counts = registers.try_read32(0xd04010)?;
        let core_mask = registers.try_read32(0xe01500)?;
        dev_info!(pdev.as_ref(), "G16G: power acknowledged, version={:#010x} counts={:#010x} core-mask={:#x}, firmware={}\n",
            version, counts, core_mask, device.firmware.version());
        // These words identify the qualified G16G configuration. The fused
        // core mask varies with SKU; absent cores must never be enabled.
        if version != 0x0a021100
            || counts != 0x0011010a
            || core_mask == 0
            || core_mask & !0x3ff != 0
        {
            return Err(ENODEV);
        }
        Ok(device)
    }

    pub(crate) fn check_drm(&self, pdev: &platform::Device<Core>) -> Result {
        use kernel::dma::{Device as _, DmaMask};
        // SAFETY: This is the admitted T8132's 42-bit physical DMA capability.
        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(42)?)? };
        for _ in 0..2 {
            let drm: ARef<crate::driver::AsahiDevice> = kernel::drm::Device::new(
                pdev.as_ref(), crate::driver::AsahiData::new(pdev, None))?;
            if drm.gpu().is_ok() { return Err(EIO); }
            let mut object = crate::gem::new_kernel_object(&drm, 0x4000)?;
            object.vmap()?.memset(0);
            drop(object);
            drop(drm);
        }
        dev_info!(pdev.as_ref(), "G16G: initialized DRM data and GEM allocation/cleanup passed twice; backend absent, device unregistered\n");
        Ok(())
    }

    /// Temporary bring-up check of the common walker while ASC is stopped.
    /// No context root is installed and no firmware-visible payload is sent.
    pub(crate) fn check_tables(&self, pdev: &platform::Device<Core>) -> Result {
        use crate::{pgtable::{UatPageTable, prot}, pgtable_memory::ReservedTables, uat::UatGeometry};
        use kernel::{io::mem::{Mem, MemFlag}, page::Page};
        use core::sync::atomic::{AtomicU64, Ordering};
        if self.asc.access(pdev.as_ref())?.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            return Err(EBUSY);
        }
        let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
        // SAFETY: Validated reserved RAM; ASC is stopped. This is a read-only
        // check that no context has been installed before touching tables.
        let ttbs = unsafe { Mem::try_new(node.reserved_mem_region_to_resource_byname(c_str!("ttbs"))?, MemFlag::WB.into()) }?;
        let contexts = unsafe { core::slice::from_raw_parts(ttbs.ptr().cast::<AtomicU64>(), 128) };
        if contexts.iter().any(|entry| entry.load(Ordering::Acquire) != 0) {
            return Err(EBUSY);
        }
        let make_tables = || -> Result<ReservedTables> {
            let mut resources = KVec::new();
            for name in [c_str!("pagetables"), c_str!("shared-l2")] {
                resources.push(node.reserved_mem_region_to_resource_byname(name)?, GFP_KERNEL)?;
            }
            // SAFETY: Device::new admitted the board's reserved no-map regions
            // and exclusive ASC control. No context or running firmware can
            // modify the tables throughout this synchronous check.
            unsafe { ReservedTables::new(resources) }
        };
        let snapshot = |tables: &ReservedTables| -> Result<KVec<u64>> {
            let mut words = KVec::new();
            for index in [1, 3] {
                let region = self.firmware.resources.regions[index];
                for address in (region.base..region.base + region.size).step_by(0x4000) {
                    tables.with_page(address, |entries| {
                        for entry in entries {
                            words.push(entry.load(Ordering::Acquire), GFP_KERNEL)?;
                        }
                        Ok(())
                    })?;
                }
            }
            Ok(words)
        };
        let geometry = UatGeometry::new(42).ok_or(EINVAL)?;
        let tables = make_tables()?;
        let before = snapshot(&tables)?;
        drop(tables);
        let payload = Page::alloc_page(GFP_KERNEL | __GFP_ZERO)?;
        for _ in 0..2 {
            // SAFETY: All TTBAT entries are zero and ASC remains stopped.
            // The table owner is dropped here before any firmware startup.
            let mut table = unsafe { UatPageTable::new_with_reserved_ttb(
                self.firmware.resources.regions[1].base,
                geometry.kernel_va_base()..geometry.kernel_va_top(), 42, 42, make_tables()?) }?;
            let boundary = geometry.kernel_va_base() + (1 << 25);
            let spans = [boundary - 0x8000..boundary + 0x18000,
                geometry.kernel_va_top() - 0x4000..geometry.kernel_va_top()];
            for span in &spans {
                table.prepare_map(span.clone())?;
                table.map_pages(span.clone(), payload.phys(), prot::PROT_GPU_SHARED_RW, true)?;
                if table.prepare_map(span.clone()) != Err(EBUSY)
                    || !table.covers_range(span.clone(), true, true)? {
                    return Err(EIO);
                }
                for va in (span.start..span.end).step_by(0x4000) {
                    if table.translate_iova(va + 13)? != payload.phys() + 13 { return Err(EIO); }
                }
                table.reprot_pages(span.clone(), prot::PROT_GPU_SHARED_RO)?;
                if !table.covers_range(span.clone(), true, false)?
                    || table.covers_range(span.clone(), false, true)? { return Err(EIO); }
                table.unmap_pages(span.clone())?;
                if table.covers_range(span.clone(), false, false)? { return Err(EIO); }
            }
            drop(table);
            if snapshot(&make_tables()?)? != before { return Err(EIO); }
        }
        dev_info!(pdev.as_ref(), "G16G: reserved UAT walker passed two map/protect/unmap/drop cycles across leaf boundaries; inherited tables unchanged, contexts disabled\n");
        Ok(())
    }

    /// Boot management and application endpoints without submitting initdata.
    /// A fresh system boot is required for each attempt until reset is ported.
    pub(crate) fn boot_transport(&self, pdev: &platform::Device<Core>) -> Result {
        let state = crate::g16_rtkit::State::new(pdev, self.firmware.resources.regions[5])?;
        let mut rtkit = core::pin::pin!(rtkit::RtKit::<crate::g16_rtkit::Operations>::new(
            pdev.as_ref(), None, 0, state.clone())?);
        let asc = self.asc.access(pdev.as_ref())?;
        let control = asc.read32(ASC_CPU_CONTROL);
        if control & ASC_CPU_RUN != 0 { return Err(EBUSY); }
        asc.write32(control | ASC_CPU_RUN, ASC_CPU_CONTROL);
        let result = (|| {
            rtkit.as_mut().wake()?;
            if !rtkit.as_ref().is_running() || !state.healthy() { return Err(EIO); }
            for endpoint in [0x20, 0x21] {
                if !rtkit.as_mut().has_endpoint(endpoint) { return Err(ENODEV); }
                rtkit.as_mut().start_endpoint(endpoint)?;
            }
            fsleep(Delta::from_millis(100));
            if !state.healthy() { return Err(EIO); }
            dev_info!(pdev.as_ref(), "G16G: RTKit IOP/AP running, crash storage retained, application endpoints started; no initdata sent\n");
            Ok(())
        })();
        asc.write32(asc.read32(ASC_CPU_CONTROL) & !ASC_CPU_RUN, ASC_CPU_CONTROL);
        let start = Instant::<Monotonic>::now();
        while asc.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            if start.elapsed() >= Delta::from_millis(100) { return Err(ETIMEDOUT); }
            fsleep(Delta::from_micros(10));
        }
        dev_info!(pdev.as_ref(), "G16G: ASC stopped before RTKit transport cleanup\n");
        result
    }

    /// Exercise the normal interrupt-driven RTKit management transport before
    /// giving firmware any host allocations or starting application endpoints.
    /// The caller must reboot before attempting another firmware session.
    pub(crate) fn discover(&self, pdev: &platform::Device<Core>) -> Result {
        let mut rtkit = core::pin::pin!(rtkit::RtKit::<Management>::new_epmap_only(
            pdev.as_ref(),
            None,
            0,
            ()
        )?);
        let asc = self.asc.access(pdev.as_ref())?;
        let control = asc.read32(ASC_CPU_CONTROL);
        if control & ASC_CPU_RUN != 0 {
            return Err(EBUSY);
        }
        asc.write32(control | ASC_CPU_RUN, ASC_CPU_CONTROL);
        let result = rtkit.as_mut().discover_epmap();

        // Stop first, while the transport and all firmware reservations remain
        // alive. Only then may RTKit drain callbacks and release its mailbox.
        asc.write32(asc.read32(ASC_CPU_CONTROL) & !ASC_CPU_RUN, ASC_CPU_CONTROL);
        let start = Instant::<Monotonic>::now();
        while asc.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN != 0 {
            if start.elapsed() >= Delta::from_millis(100) {
                dev_err!(pdev.as_ref(), "G16G: ASC failed to stop after discovery\n");
                return Err(ETIMEDOUT);
            }
            fsleep(Delta::from_micros(10));
        }
        let snapshot = result?;
        dev_info!(pdev.as_ref(), "G16G: RTKit management discovery HELLO={:#018x} versions={}/{} endpoints={:016x?}; ASC stopped\n",
            snapshot.raw_hello, snapshot.hello_min_version, snapshot.hello_max_version,
            snapshot.endpoints);
        if snapshot.hello_min_version != 12
            || snapshot.hello_max_version != 12
            || snapshot.endpoints != [0x0000_0003_0000_0003, 0, 0, 0]
        {
            return Err(EIO);
        }
        Ok(())
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        if !self.power_vote {
            return;
        }
        // Discovery stops ASC before its RTKit client is released. Require
        // that proof before relinquishing the inner power vote.
        match self.asc.try_access() {
            Some(asc) if asc.read32(ASC_CPU_CONTROL) & ASC_CPU_RUN == 0 => {}
            _ => {
                dev_err!(
                    self.dev.as_ref(),
                    "G16G: cannot prove ASC stopped; retaining PMP vote\n"
                );
                return;
            }
        }
        // SAFETY: the managed device link keeps PMP bound through remove.
        if let Err(error) = to_result(unsafe { bindings::apple_pmp_set_device_power(0x0f, 5, 0) }) {
            dev_err!(
                self.dev.as_ref(),
                "G16G: PMP power release failed: {:?}\n",
                error
            );
        }
    }
}
