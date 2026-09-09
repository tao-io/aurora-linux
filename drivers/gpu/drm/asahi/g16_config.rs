// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713/25G83 boot configuration. Only the identified wire schema and board
//! register descriptions come from the old bring-up; all VAs and GEM ownership
//! are provided by the common Asahi VM. No GPU commands are queued here.

use kernel::{c_str, device::Core, io::mem::{Mem, MemFlag}, platform, prelude::*};
use crate::{driver, g16_initdata::{self, RootPointers}, g16_memory::{self, Buffer, KERNEL_RANGE},
    g16_firmware::Firmware, mmu, pgtable::prot};

#[derive(Clone, Copy)]
#[repr(usize)]
enum Object {
    Root, Brn, Runtime, Globals, Control, FirmwareData, Power, Dynamic, Main,
    PowerPerformance, Timer, Activity254, Activity25c, Activity264, Activity26c,
    LogState, LogEntries, LogData, Fwctl, DevctrlState, DevctrlRing,
    KtraceState, KtraceData, StatisticsState, StatisticsData, Temperature,
    EventState, EventData, Retention, UmaTable,
}
const SIZES: &[usize] = &[
    0xc8, 0x4000, 0x4b8, 0xe48, 0x20, 0xeaf0, 0x670, 0x4000, 0x2658,
    0x6578, 0x88, 0xc18, 0x1248, 0xe10, 0x60,
    0x1b0, 0x28800, 0x79800, 0x4000, 0x30, 0x4000,
    0x30, 0x9000, 0x30, 0x4800, 4, 0x30, 0x4800, 0x4000, 0x4000,
];

// (slot, physical register range, mapped/requested length, relative tag, mode).
// Empty slots remain zero. Virtual addresses are deliberately absent.
const IOMAPS: &[(usize, usize, usize, usize, u64, u64)] = &[
    (0, 0x381014000, 0x4000, 0x4000, 0, 2),
    (3, 0x2201c4000, 0x30000, 0x18000, 0, 2),
    (9, 0x3803d0000, 0x1000, 0x1000, 0, 2),
    (10, 0x3803c0000, 0x2000, 0x2000, 0, 0),
    (12, 0x50165c000, 0x4000, 0x4000, 0, 2),
    (14, 0x380280000, 0x8000, 0x8000, 0, 0),
    (15, 0x38c840000, 0x24000, 0x24000, 0, 0),
    (17, 0x300000000, 0x20000, 0x20000, 0, 2),
    (22, 0x301000000, 0x8000, 0x8000, 0, 2),
    (26, 0x300d04000, 0x8000, 0x8000, 0xd04000, 2),
    (27, 0x300d0d000, 0x1000, 0x1000, 0xd0d000, 2),
    (28, 0x300d58000, 0x8000, 0x8000, 0xd58000, 2),
    (29, 0x300d10000, 0x4000, 0x4000, 0xd10000, 2),
    (31, 0x300d40000, 0x4000, 0x4000, 0xd40000, 2),
    (32, 0x300d60000, 0x8000, 0x8000, 0xd60000, 2),
    (36, 0x300e00000, 0x4000, 0x4000, 0xe00000, 0),
    (39, 0x300e08000, 0x8000, 0x8000, 0, 2),
    (40, 0x300e1c000, 0x4000, 0x4000, 0xe1c000, 2),
    (41, 0x300e1f800, 0x4000, 0x4000, 0, 2),
];

pub(crate) struct Config {
    objects: KVec<Buffer>,
    pipes: KVec<Buffer>,
    iomaps: KVec<mmu::KernelMapping>,
    loader: Mem,
    trace_seen: [u32; 2],
    iomap_bytes: KBox<[u8; 42 * 0x28]>,
}

impl Config {
    pub(crate) fn new(pdev: &platform::Device<Core>, dev: &driver::AsahiDevice,
        vm: &mmu::Vm, gpu_vm: &mmu::Vm, firmware: &Firmware) -> Result<Self> {
        let resource = pdev.as_ref().of_node().ok_or(ENODEV)?
            .reserved_mem_region_to_resource_byname(c_str!("fw-data"))?;
        let data = firmware.resources.regions[5];
        if resource.start() != data.base || resource.size() != data.size || data.size < 0x134000 {
            return Err(EINVAL);
        }
        // SAFETY: This admitted firmware data reservation is normal no-map RAM.
        // Runtime stops ASC before destroying Config. The boot-only loader
        // descriptor table at data+0x1332f0 is host-owned until initdata is sent.
        let loader = unsafe { Mem::try_new(resource, MemFlag::WC.into()) }?;
        let mut config = Self { objects: KVec::new(), pipes: KVec::new(), iomaps: KVec::new(),
            loader, trace_seen: [0; 2], iomap_bytes: KBox::new([0; 42 * 0x28], GFP_KERNEL)? };
        for (index, size) in SIZES.iter().enumerate() {
            config.objects.push(if index == Object::UmaTable as usize {
                Buffer::new_gpu(dev, vm, gpu_vm, *size)?
            } else { Buffer::new(dev, vm, *size)? }, GFP_KERNEL)?;
        }
        // Twelve firmware submission channels: TA/3D/compute at four priorities.
        // Empty independent rings avoid aliasing producer/consumer state.
        for _ in 0..12 {
            config.pipes.push(Buffer::new(dev, vm, 0x30)?, GFP_KERNEL)?;
            config.pipes.push(Buffer::new(dev, vm, 0x1800)?, GFP_KERNEL)?;
        }
        for &(slot, physical, mapped, requested, tag, mode) in IOMAPS {
            let offset = physical & mmu::UAT_PGMSK;
            let size = (mapped + offset + mmu::UAT_PGMSK) & !mmu::UAT_PGMSK;
            let protection = if slot == 36 { prot::PROT_FW_PROTECTED_MMIO }
                else if mode == 0 { prot::PROT_FW_MMIO_RO } else { prot::PROT_FW_MMIO_RW };
            // MCC slot 3 concatenates two six-page banks 32 MiB apart.
            // Their single descriptor does not imply contiguous physical MMIO.
            let mapping = vm.map_io_in_range(KERNEL_RANGE, physical - offset,
                if slot == 3 { 0x18000 } else { size }, protection)?;
            if slot == 3 {
                let second = vm.map_io(mapping.iova() + 0x18000, physical + 0x2000000,
                    0x18000, protection)?;
                config.iomaps.push(second, GFP_KERNEL)?;
            }
            let va = mapping.iova() + offset as u64;
            let entry = &mut config.iomap_bytes[slot * 0x28..(slot + 1) * 0x28];
            entry[0..8].copy_from_slice(&(physical as u64).to_le_bytes());
            entry[8..16].copy_from_slice(&va.to_le_bytes());
            entry[16..20].copy_from_slice(&(mapped as u32).to_le_bytes());
            entry[20..24].copy_from_slice(&(requested as u32).to_le_bytes());
            entry[24..32].copy_from_slice(&tag.to_le_bytes());
            entry[32..40].copy_from_slice(&mode.to_le_bytes());
            config.iomaps.push(mapping, GFP_KERNEL)?;
        }
        let sgx = vm.map_io_in_range(KERNEL_RANGE, 0x300000000, 0x4000000, prot::PROT_FW_MMIO_RW)?;
        let sgx_va = sgx.iova();
        config.iomaps.push(sgx, GFP_KERNEL)?;
        let scratch = vm.map_io_in_range(KERNEL_RANGE, 0x300d60000, 0x8000, prot::PROT_FW_SHARED_RW)?;
        let scratch_va = scratch.iova();
        config.iomaps.push(scratch, GFP_KERNEL)?;
        config.initialize(sgx_va, scratch_va)?;
        Ok(config)
    }

    /// The global hardware UMA descriptor table is consumed after the engine
    /// switches to a client ASID. Reserve its one owned GEM view in that VM
    /// before client resources are allocated; preserve the bootstrap mapping.
    pub(crate) fn bind_uma_table(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        self.object(Object::UmaTable).map_gpu_view(vm)
    }
    fn va(&self, object: Object) -> u64 { self.objects[object as usize].va() }
    fn object(&mut self, object: Object) -> &mut Buffer { &mut self.objects[object as usize] }
    fn ptr(&mut self, owner: Object, offset: usize, target: Object, delta: u64) -> Result {
        let va = self.va(target).checked_add(delta).ok_or(EOVERFLOW)?;
        self.object(owner).u64(offset, va)
    }
    fn initialize(&mut self, sgx: u64, scratch: u64) -> Result {
        use Object::*;
        let mut root = [0; g16_initdata::ROOT_SIZE];
        g16_initdata::encode_root(&mut root, RootPointers {
            brn: self.va(Brn), runtime: self.va(Runtime), globals: self.va(Globals),
            control: self.va(Control), firmware_data: self.va(FirmwareData),
            power: self.va(Power), dynamic: self.va(Dynamic),
        }).map_err(|_| EINVAL)?;
        self.object(Root).write(0, &root)?;
        for (offset, target) in [(0, Main), (0x10, Timer), (0x1c0, EventState), (0x1c8, EventData),
            (0x1d0, LogState), (0x1d8, LogEntries), (0x1e0, KtraceState), (0x1e8, KtraceData),
            (0x1f0, StatisticsState), (0x1f8, StatisticsData), (0x200, LogData),
            (0x254, Activity254), (0x25c, Activity25c), (0x264, Activity264),
            (0x26c, Activity26c), (0x469, PowerPerformance)] {
            self.ptr(Runtime, offset, target, 0)?;
        }
        self.object(Runtime).u64(0x18, scratch)?;
        self.object(Runtime).u32(0x250, 1)?;
        self.object(Runtime).u32(0x2f8, 4)?;
        let uma_gpu = self.object(UmaTable).gpu_va()?;
        self.object(Runtime).u64(0x2e0, uma_gpu)?;
        self.ptr(Runtime, 0x2e8, UmaTable, 0)?;
        self.object(Activity25c).u32(0xc18, u32::MAX)?;
        self.object(Activity25c).u32(0xc30, u32::MAX)?;
        for pipe in 0..12 {
            let state = self.pipes[pipe * 2].va();
            let ring = self.pipes[pipe * 2 + 1].va();
            let offset = 0x20 + pipe * 0x20;
            for (off, va) in [(0, state), (8, state + 0x10), (16, state + 0x20), (24, ring)] {
                self.object(Runtime).u64(offset + off, va)?;
            }
        }
        for (offset, target, delta) in [(0x1a0, DevctrlState, 0), (0x1a8, DevctrlState, 0x10),
            (0x1b0, DevctrlState, 0x20), (0x1b8, DevctrlRing, 0)] {
            self.ptr(Runtime, offset, target, delta)?;
        }
        self.ptr(FirmwareData, 0x4f88, Fwctl, 0)?;
        self.ptr(FirmwareData, 0x4f90, Fwctl, 0x40)?;
        self.object(FirmwareData).u32(0x4fd4, 1)?;
        self.object(Globals).u32(0x78, 1)?;
        self.object(Globals).u32(0xe28, 3)?;
        // 25G83 host Globals: progress intervals and idle/context timers in
        // milliseconds. These match the common Asahi constructor roles and
        // the J713 producer. A zero-filled timer is not a disabled timer.
        for (offset, value) in [(0x24, 3000), (0x998, 40), (0x99c, 10),
            (0x9a0, 250), (0x9b8, 2), (0x9bc, 40), (0x9c0, 5),
            (0x9c8, 40), (0x9cc, 50)] {
            self.object(Globals).u32(offset, value)?;
        }
        self.ptr(PowerPerformance, 0x3b50, Temperature, 0)?;
        self.object(PowerPerformance).u32(4, 200000)?;
        self.object(PowerPerformance).u32(8, 200000)?;
        self.object(Main).u64(0x28, sgx)?;
        let (objects, iomaps) = (&mut self.objects, &self.iomap_bytes);
        objects[Main as usize].write(0x640, &iomaps[..])?;
        self.object(Main).u32(0xea0, 1)?;
        self.object(Main).u32(0xeb8, 1)?;
        self.object(Main).u64(0xf24, 0x0000000607002004)?;
        self.object(Main).u32(0xfc0, 10)?;
        self.object(Main).u32(0xfc4, 9)?;
        self.object(Main).u32(0x259c, 1)?;
        // Fender initialization also constructs scheduler accounting buckets:
        // 25G83 imports ea0/ee0 to globals 193b48/193b7c, and its 3b614
        // path initializes the buckets only with both enabled. Disabling ee0
        // while ea0 is enabled makes the first queue submission assert.
        // Retain its save area; device-control separately disables idle power
        // off, and the runtime verifies that the low boot pstate is retained.
        self.ptr(Main, 0xe88, Retention, 0)?;
        self.object(Main).u32(0xee0, 1)?;
        for (offset, value) in [(4, 4), (8, 6), (12, 3), (16, 7), (20, 7), (40, 8), (48, 1)] {
            self.object(Main).u32(0x2540 + offset, value)?;
        }
        self.object(Control).u32(0, 0x11)?;
        Ok(())
    }

    /// Publish the loader's host-supplied MMIO table after RTKit has reached
    /// its application wait state, before sending initdata. FW never consumes
    /// this table concurrently with publication under that boot protocol.
    pub(crate) fn publish_loader(&mut self) -> Result {
        let mut map = self.loader.iosys_map(0x1332f0, 42 * 0x28)?;
        for index in 0..42 * 0x28 {
            // SAFETY: bounded byte in retained WC firmware data reservation.
            if unsafe { map.as_ptr().add(index).read_volatile() } != 0 { return Err(EBUSY); }
        }
        map.write(&self.iomap_bytes[..], 0)?;
        g16_memory::publish();
        Ok(())
    }

    /// Snapshot only admitted firmware-data RAM, never a GPU register bank.
    /// These 25G83 globals describe engine issue and power accounting. Values
    /// are diagnostics; neither a cached snapshot nor a stamp proves DMA done.
    pub(crate) fn log_firmware_state(&mut self, dev: &driver::AsahiDevice) -> Result {
        for (address, count) in [(0x68158usize, 1usize), (0x128d60, 8),
            (0x129b20, 4), (0x129bc8, 8), (0x129d00, 8), (0x147368, 6), (0x192900, 5), (0x1935a0, 1)] {
            let map = self.loader.iosys_map(address.checked_sub(0x60000).ok_or(EINVAL)?, count * 8)?;
            let mut words = [0u64; 8];
            for (index, word) in words[..count].iter_mut().enumerate() {
                let mut bytes = [0u8; 8];
                for (j, byte) in bytes.iter_mut().enumerate() {
                    // SAFETY: checked retained firmware data mapping, byte access
                    // avoids assuming atomicity of packed firmware fields.
                    *byte = unsafe { map.as_ptr().add(index * 8 + j).read_volatile() };
                }
                *word = u64::from_le_bytes(bytes);
            }
            dev_info!(dev.as_ref(), "G16G: firmware RAM +{:#x}={:x?}\n", address, &words[..count]);
        }
        let mut uma = [0u64; 4];
        for (index, word) in uma.iter_mut().enumerate() {
            *word = self.object(Object::UmaTable).read_u64(32 + index * 8)?;
        }
        dev_info!(dev.as_ref(), "G16G: hardware UMA slot1={:x?}\n", uma);
        let power = self.object(Object::PowerPerformance).read_u32(0x38)?;
        dev_info!(dev.as_ref(), "G16G: firmware GPU power state={:#x}\n", power);
        Ok(())
    }

    /// Serialized device-control producer. Ring geometry is a firmware ABI
    /// constraint; no queue or allocation count is limited by a lab pool.
    pub(crate) fn enqueue_control(&mut self, opcode: u32, payload: u32,
        generation: &mut u32, device: &crate::g16_device::Device) -> Result<u32> {
        if !matches!(opcode, 0x1a | 0x34 | 0x0a) { return Err(EINVAL); }
        let [read, cfi, write] = self.control_indices()?;
        if read != write || cfi != write { return Err(EBUSY); }
        let mut entry = [0; 0x40];
        entry[..4].copy_from_slice(&opcode.to_le_bytes());
        entry[4..8].copy_from_slice(&payload.to_le_bytes());
        self.object(Object::DevctrlRing).write(write as usize * 0x40, &entry)?;
        // All three admitted opcodes require a power assertion in the M4 ABI.
        *generation = generation.wrapping_add(1);
        device.set_power_generation(*generation)?;
        g16_memory::publish();
        let next = (write + 1) % 256;
        self.object(Object::DevctrlState).u32(0x20, next)?;
        g16_memory::publish();
        Ok(next)
    }
    pub(crate) fn control_indices(&mut self) -> Result<[u32; 3]> {
        let state = self.object(Object::DevctrlState);
        let indices = [state.read_u32(0)?, state.read_u32(0x10)?, state.read_u32(0x20)?];
        if indices.iter().any(|index| *index >= 256) { return Err(EIO); }
        Ok(indices)
    }
    pub(crate) fn consumed_generation(&mut self) -> Result<u32> {
        self.object(Object::FirmwareData).read_u32(0xeae8)
    }
    pub(crate) fn rearm_scheduler(&mut self) -> Result {
        if self.object(Object::FirmwareData).read_u32(0x4fd4)? != 0 { return Err(EBUSY); }
        self.object(Object::FirmwareData).u32(0x4fd4, 1)?;
        g16_memory::publish();
        Ok(())
    }

    pub(crate) fn publish_queue(&mut self, pipe: usize, queue: u64, wptr: u16, slot: u8, new: bool) -> Result<u32> {
        if pipe >= 12 || slot >= 128 || queue & 7 != 0 { return Err(EINVAL); }
        let state = &mut self.pipes[pipe * 2];
        let consumer = state.read_u32(0)?;
        let cfi = state.read_u32(0x10)?;
        let producer = state.read_u32(0x20)?;
        if consumer >= 256 || cfi >= 256 || producer >= 256 { return Err(EIO); }
        let next = (producer + 1) % 256;
        if next == consumer || next == cfi { return Err(EBUSY); }
        let timestamp: u64;
        // SAFETY: architectural counter is readable at EL1.
        unsafe { core::arch::asm!("mrs {t}, cntpct_el0", t = out(reg) timestamp, options(nomem, nostack, preserves_flags)) };
        let mut entry = [0; 0x18];
        entry[..8].copy_from_slice(&timestamp.to_le_bytes());
        entry[8..16].copy_from_slice(&queue.to_le_bytes());
        entry[16..20].copy_from_slice(&((pipe % 3) as u32).to_le_bytes());
        entry[20..22].copy_from_slice(&wptr.to_le_bytes());
        entry[22] = slot; // must match command metadata scheduler slot
        entry[23] = u8::from(new);
        self.pipes[pipe * 2 + 1].write(producer as usize * 0x18, &entry)?;
        g16_memory::publish();
        self.pipes[pipe * 2].u32(0x20, next)?;
        g16_memory::publish();
        Ok(next)
    }
    pub(crate) fn pipe_indices(&mut self, pipe: usize) -> Result<[u32; 3]> {
        if pipe >= 12 { return Err(EINVAL); }
        let state = &mut self.pipes[pipe * 2];
        Ok([state.read_u32(0)?, state.read_u32(0x10)?, state.read_u32(0x20)?])
    }

    pub(crate) fn root(&self) -> u64 { self.va(Object::Root) }
    pub(crate) fn ready(&mut self) -> Result<bool> { Ok(self.object(Object::Control).read_u32(0x14)? == 1) }

    /// Bounded drains of firmware-owned rings. Logs are diagnostics, never
    /// interpreted as proof of command completion.
    pub(crate) fn drain(&mut self, dev: &driver::AsahiDevice) -> Result {
        use Object::*;
        for thread in 0..9 {
            let offset = thread * 0x30;
            let mut consumer = self.object(LogState).read_u32(offset)?;
            let producer = self.object(LogState).read_u32(offset + 0x20)?;
            if consumer >= 256 || producer >= 256 { return Err(EIO); }
            while consumer != producer {
                let entry_offset = (thread * 256 + consumer as usize) * 0x48;
                let payload = self.object(LogEntries).read_u32(entry_offset + 8)? as usize & 255;
                let mut text = [0u8; 200];
                self.object(LogData).read((thread * 256 + payload) * 0xd8 + 0x10, &mut text)?;
                let length = text.iter().position(|b| *b == 0).unwrap_or(text.len());
                dev_info!(dev.as_ref(), "G16G FW[{}]: {}\n", thread,
                    core::str::from_utf8(&text[..length]).unwrap_or("<non-UTF8 firmware log>"));
                consumer = (consumer + 1) & 255;
            }
            self.object(LogState).u32(offset, consumer)?;
        }
        for (state, data, count, trace) in [(KtraceState, KtraceData, 512, true),
            (StatisticsState, StatisticsData, 256, false), (EventState, EventData, 256, false)] {
            let mut consumer = self.object(state).read_u32(0)?;
            let producer = self.object(state).read_u32(0x20)?;
            if consumer >= count || producer >= count { return Err(EIO); }
            while consumer != producer {
                let mut record = [0; 0x48];
                self.object(data).read(consumer as usize * 0x48, &mut record)?;
                if trace {
                    if u32::from_le_bytes(record[..4].try_into().unwrap()) != 5 { return Err(EIO); }
                    let tag = u32::from_le_bytes(record[44..48].try_into().unwrap());
                    let phase = u32::from_le_bytes(record[48..52].try_into().unwrap());
                    let mut args = [0u64; 4];
                    for (i, arg) in args.iter_mut().enumerate() {
                        *arg = u64::from_le_bytes(record[12 + i * 8..20 + i * 8].try_into().unwrap());
                    }
                    // Keep both power and engine evidence without flooding
                    // printk during retries. Drain and validate every record.
                    let class = usize::from((tag & 0xffffff) >= 0x400);
                    let seen = &mut self.trace_seen[class];
                    *seen = seen.saturating_add(1);
                    if *seen <= 192 {
                        dev_info!(dev.as_ref(), "G16G trace thread={} id={:#x} phase={} args={:x?}\n", tag >> 24, tag & 0xffffff, phase, args);
                    } else if *seen == 193 {
                        dev_info!(dev.as_ref(), "G16G trace class={} subsequent records counted without printk\n", class);
                    }
                    if tag & 0xffffff == 0x11c { return Err(EOVERFLOW); }
                } else if matches!(state, EventState) {
                    dev_info!(dev.as_ref(), "G16G event: {:02x?}\n", record);
                    if u32::from_le_bytes(record[..4].try_into().unwrap()) != 1 {
                        return Err(EIO);
                    }
                } else if u32::from_le_bytes(record[..4].try_into().unwrap()) == 0x16 {
                    dev_err!(dev.as_ref(), "G16G recovery statistics: {:02x?}\n", record);
                    return Err(EIO);
                }
                consumer = (consumer + 1) % count;
            }
            self.object(state).u32(0, consumer)?;
        }
        g16_memory::publish();
        Ok(())
    }
}
