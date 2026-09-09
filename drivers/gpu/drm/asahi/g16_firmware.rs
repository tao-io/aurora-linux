// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G16G firmware identity for the J713 execution evidence. A matching hash
//! selects a firmware ABI; it is not a firmware signature check or a claim
//! that Linux has acquired power, UAT or queue ownership.

#[cfg(test)]
#[path = "g16_resources.rs"]
mod g16_resources;
#[cfg(not(test))]
use crate::g16_resources;

const TEXT_SIZE: u64 = 0x60000;
const DATA_SIZE: u64 = 0x13c000;
pub(crate) const TEXT_SHA256: [u8; 32] = [
    0x70, 0x4f, 0x1b, 0x4c, 0xd7, 0x4c, 0x03, 0x6e, 0xa8, 0x5b, 0xdc, 0x19, 0x1b, 0xdb, 0xe7, 0x66,
    0x18, 0x5b, 0x5b, 0x1c, 0xfa, 0x8d, 0x4a, 0xf5, 0x91, 0xb0, 0x65, 0x89, 0x0d, 0x1e, 0xc4, 0x2c,
];

#[derive(Debug)]
pub(crate) struct Firmware {
    pub(crate) resources: g16_resources::Resources,
}

impl Firmware {
    pub(crate) const fn version(&self) -> &'static str {
        "RTKit-3255.160.4.release"
    }

    fn matching_layout(resources: &g16_resources::Resources) -> bool {
        resources.regions[4].size == TEXT_SIZE
            && resources.regions[5].size == DATA_SIZE
            && resources.firmware_vas == [0xffff_fc00_0000_0000, 0xffff_fc00_0006_0000]
    }

    fn from_digest(resources: g16_resources::Resources, digest: [u8; 32]) -> Option<Self> {
        if !Self::matching_layout(&resources) || digest != TEXT_SHA256 {
            return None;
        }
        Some(Self { resources })
    }
}

/// Hash the actual reserved __TEXT mapping. Do not substitute a file requested
/// from userspace; that file would not identify the code loaded by iBoot.
#[cfg(not(test))]
pub(crate) fn identify_loaded(
    pdev: &kernel::platform::Device<kernel::device::Core>,
    resources: g16_resources::Resources,
) -> kernel::error::Result<Firmware> {
    use kernel::{
        bindings, c_str,
        io::mem::{Mem, MemFlag},
        prelude::*,
    };

    if !Firmware::matching_layout(&resources) {
        dev_err!(pdev.as_ref(), "G16G: unsupported firmware segment layout\n");
        return Err(ENODEV);
    }
    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    let text = node.reserved_mem_region_to_resource_byname(c_str!("fw-text"))?;
    if text.start() != resources.regions[4].base || text.size() != TEXT_SIZE {
        return Err(EINVAL);
    }
    // SAFETY: from_device checked the bootloader's no-map, nonoverlapping
    // firmware __TEXT region and its read-only segment flag. The mapping is
    // used only for hashing here and is dropped before returning. No mutable
    // reference, firmware patch, MMIO write or DMA is exposed by this function.
    let mapping = unsafe { Mem::try_new(text, MemFlag::WB.into()) }?;
    let mut digest = [0u8; 32];
    // SAFETY: the live mapping covers mapping.size() readable bytes and
    // digest is a distinct SHA256_DIGEST_SIZE-byte writable array. The kernel
    // SHA-256 implementation consumes the input synchronously.
    unsafe { bindings::sha256(mapping.ptr(), mapping.size(), digest.as_mut_ptr()) };
    if digest != TEXT_SHA256 {
        dev_err!(
            pdev.as_ref(),
            "G16G: unsupported loaded firmware SHA-256 {:02x?}\n",
            digest
        );
    }
    Firmware::from_digest(resources, digest).ok_or(ENODEV)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn resources(delta: u64) -> g16_resources::Resources {
        let regions = [
            (0x103fffb8000, 0x4000),
            (0x103fff78000, 0x40000),
            (0x103fff70000, 0x4000),
            (0x103fff74000, 0x4000),
            (0x10000cc0000, TEXT_SIZE),
            (0x10001df4000, DATA_SIZE),
        ]
        .map(|(base, size)| g16_resources::Region {
            base: base + delta,
            size,
        });
        g16_resources::Resources::validate(
            1,
            regions,
            [0xffff_fc00_0000_0000, 0xffff_fc00_0006_0000],
            [1, 0],
        )
        .unwrap()
    }

    #[test]
    fn firmware_identity_survives_physical_relocation() {
        for delta in [0, 0x400000000] {
            let fw = Firmware::from_digest(resources(delta), TEXT_SHA256).unwrap();
            assert_eq!(fw.version(), "RTKit-3255.160.4.release");
            assert_eq!(fw.resources.regions[4].base, 0x10000cc0000 + delta);
        }
    }

    #[test]
    fn unknown_code_and_different_layout_cannot_select_this_abi() {
        let mut changed = TEXT_SHA256;
        changed[0] ^= 1;
        assert!(Firmware::from_digest(resources(0), changed).is_none());
        let mut resized = resources(0);
        resized.regions[5].size += 0x4000;
        assert!(Firmware::from_digest(resized, TEXT_SHA256).is_none());
    }
}
