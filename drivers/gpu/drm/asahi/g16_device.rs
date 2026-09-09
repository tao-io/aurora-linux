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
