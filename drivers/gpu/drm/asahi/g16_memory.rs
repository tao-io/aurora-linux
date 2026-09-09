// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Firmware buffers backed by common GEM objects and DRM-managed UAT ranges.

use kernel::prelude::*;
use crate::{driver, gem, mmu, pgtable::prot};

pub(crate) const KERNEL_RANGE: core::ops::Range<u64> =
    0xffff_fc20_0000_0000..0xffff_fc30_0000_0000;

pub(crate) struct Buffer {
    // Unmap before releasing our GEM/vmap references.
    mapping: mmu::KernelMapping,
    gpu_mapping: Option<mmu::KernelMapping>,
    object: gem::ObjectRef,
    size: usize,
}

impl Buffer {
    pub(crate) fn new(dev: &driver::AsahiDevice, vm: &mmu::Vm, size: usize) -> Result<Self> {
        Self::allocate(dev, vm, size, prot::PROT_FW_SHARED_RW)
    }
    fn allocate(dev: &driver::AsahiDevice, vm: &mmu::Vm, size: usize,
        protection: prot::Prot) -> Result<Self> {
        if size == 0 || size.checked_add(mmu::UAT_PGMSK).is_none() { return Err(EINVAL); }
        let mut object = gem::new_kernel_object_wc(dev, size)?;
        object.vmap()?.memset(0);
        let mapping = object.map_into_range(vm, KERNEL_RANGE, mmu::UAT_PGSZ as u64,
            protection, true)?;
        Ok(Self { mapping, gpu_mapping: None, object, size })
    }

    /// Independent lower-half mapping of the same GEM backing. Firmware CPU
    /// retains the canonical mapping; engine addresses come from this VM's
    /// allocator, never from masking a firmware address or a captured VA.
    pub(crate) fn new_gpu(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        gpu_vm: &mmu::Vm, size: usize) -> Result<Self> {
        Self::new_gpu_protected(dev, firmware_vm, gpu_vm, size, prot::PROT_GPU_SHARED_RW)
    }
    /// Instruction/control streams are immutable to the GPU (AP=2, UXN=0),
    /// while writable command state uses the distinct data permission.
    pub(crate) fn new_gpu_readonly(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        gpu_vm: &mmu::Vm, size: usize) -> Result<Self> {
        Self::new_gpu_protected(dev, firmware_vm, gpu_vm, size, prot::PROT_GPU_SHARED_RO)
    }
    fn new_gpu_protected(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        gpu_vm: &mmu::Vm, size: usize, protection: prot::Prot) -> Result<Self> {
        // Firmware-generated engine state can retain canonical aliases.
        // Match the shared firmware/GPU access of the qualified M4 objects.
        let mut buffer = Self::allocate(dev, firmware_vm, size, prot::PROT_GPU_FW_SHARED_RW)?;
        buffer.gpu_mapping = Some(buffer.object.map_into_range(gpu_vm,
            0x70_0000_0000..0x80_0000_0000, mmu::UAT_PGSZ as u64,
            protection, true)?);
        Ok(buffer)
    }
    /// Retain one explicit command-buffer view across the bootstrap and client
    /// contexts. The engine fetches its register list before switching ASID.
    pub(crate) fn map_gpu_view(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        let address = self.gpu_va()?;
        self.object.map_at(vm, address, prot::PROT_GPU_SHARED_RW, true)
    }
    pub(crate) fn gpu_va(&self) -> Result<u64> {
        self.gpu_mapping.as_ref().map(|mapping| mapping.iova()).ok_or(EINVAL)
    }
    pub(crate) fn read_u64(&mut self, offset: usize) -> Result<u64> {
        let mut bytes = [0; 8];
        self.read(offset, &mut bytes)?;
        Ok(u64::from_le_bytes(bytes))
    }
    pub(crate) fn va(&self) -> u64 { self.mapping.iova() }
    pub(crate) fn write(&mut self, offset: usize, bytes: &[u8]) -> Result {
        if offset.checked_add(bytes.len()).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        self.object.vmap()?.write(bytes, offset)
    }
    pub(crate) fn u32(&mut self, offset: usize, value: u32) -> Result {
        if offset & 3 != 0 || offset.checked_add(4).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.vmap()?;
        // SAFETY: aligned, checked u32 in retained WC normal memory. Channel
        // producer/consumer indices must be published with one word store.
        unsafe { vmap.as_mut_ptr().add(offset).cast::<u32>().write_volatile(value.to_le()) };
        Ok(())
    }
    pub(crate) fn u64(&mut self, offset: usize, value: u64) -> Result {
        self.write(offset, &value.to_le_bytes())
    }
    /// Snapshot shared bytes. Callers follow the firmware's producer/consumer
    /// protocol; no Rust slice aliases memory that firmware can mutate.
    pub(crate) fn read(&mut self, offset: usize, bytes: &mut [u8]) -> Result {
        if offset.checked_add(bytes.len()).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.vmap()?;
        // SAFETY: WC normal RAM, retained by the GEM/vmap, checked byte range.
        unsafe {
            core::arch::asm!("dmb osh", options(nostack, preserves_flags));
            let source = vmap.as_ptr().add(offset);
            for (i, out) in bytes.iter_mut().enumerate() { *out = source.add(i).read_volatile(); }
        }
        Ok(())
    }
    pub(crate) fn read_u32(&mut self, offset: usize) -> Result<u32> {
        if offset & 3 != 0 || offset.checked_add(4).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.vmap()?;
        // SAFETY: Aligned u32 in the checked GEM allocation; firmware channel
        // indices and ready flags are naturally aligned atomic-sized accesses.
        let value = unsafe { vmap.as_ptr().add(offset).cast::<u32>().read_volatile() };
        acquire();
        Ok(u32::from_le(value))
    }
}

pub(crate) fn publish() {
    // SAFETY: Orders preceding WC buffer stores before a device doorbell.
    unsafe { core::arch::asm!("dsb oshst", options(nostack, preserves_flags)) };
}
fn acquire() {
    // SAFETY: Orders firmware payload reads after its published producer.
    unsafe { core::arch::asm!("dmb oshld", options(nostack, preserves_flags)) };
}
