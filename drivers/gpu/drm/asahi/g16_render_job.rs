// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! GEM/GPUVM ownership for a serialized M4 render pass. There are no borrowed
//! lab mappings. A hardware slot retains its last owner until replacement.
use kernel::prelude::*;
use crate::{driver, mmu, g16_memory::{self, Buffer}, g16_render::{self, Geometry, Register, Stage, Utile},
    g16_render_state::{self as state, State, Program, DepthStencil}, g16_render_command as wire,
    g17_uapi::UapiRenderCommand};

const PRIVATE_BYTES: usize = 32 * 1024 * 1024;
const PRIVATE_PAGES: usize = PRIVATE_BYTES / 4096;
// The gUPM freelist needs headroom beyond the initially populated pages.
// Equal capacity faulted on a write at freelist+0x10040 with 8192 pages.
// Derive the next ring size instead of silently overflowing a full ring.
const PRIVATE_QWORDS: usize = (PRIVATE_PAGES + 1).next_power_of_two();
const PB_BLOCK_BYTES: usize = 4 * 32768;
pub(crate) const STAMP: u32 = 0x100;
pub(crate) const SLOTS: [u8; 2] = [0x70, 0x71];

pub(crate) struct Render {
    _command_views: KVec<mmu::KernelMapping>,
    commands: [Buffer; 2],
    sequences: [Buffer; 2],
    metadata: Buffer,
    manager: Buffer,
    pages: Buffer,
    blocks: Buffer,
    block_ring: Buffer,
    pb_memory: Buffer,
    scratch: Buffer,
    tilemap: Buffer,
    tpc: Buffer,
    unknown: Buffer,
    preemption: [Buffer; 2],
    auxiliary: Buffer,
    uma: Buffer,
    private_memory: Buffer,
    pool_state: Buffer,
    page_list: Buffer,
    binding: mmu::VmBind,
    descriptor: [u8; 16],
    pub(crate) stage: Stage,
}
impl Render {
    pub(crate) fn new(dev: &driver::AsahiDevice, uat: &mmu::Uat, vm: &mmu::Vm,
        r: UapiRenderCommand, usc: u64, generation: u8,
        queues: [u64; 2], notifications: [u64; 2]) -> Result<Self> {
        let utile = |n| match n {16=>Ok(Utile::Pixels16),32=>Ok(Utile::Pixels32),_=>Err(EINVAL)};
        let geometry = Geometry::new(r.width as u32,r.height as u32,
            utile(r.utile_width)?,utile(r.utile_height)?,1).map_err(|_| EINVAL)?;
        let gpu = |size| Buffer::new_gpu(dev,uat.kernel_vm(),vm,size);
        let fw = |size| Buffer::new(dev,uat.kernel_vm(),size);
        let mut commands = [
            Buffer::new_command(dev,uat.kernel_vm(),uat.kernel_lower_vm(),Stage::Tiling.command_size())?,
            Buffer::new_command(dev,uat.kernel_vm(),uat.kernel_lower_vm(),Stage::Fragment.command_size())?];
        let mut views = KVec::new();
        for command in &mut commands { views.push(command.map_gpu_view(vm)?,GFP_KERNEL)?; }
        // Initial allocation follows pass geometry, rounded to PB blocks. The
        // firmware can partially store/reload a pass. This is not a UAPI limit.
        let pb_bytes = (4*1024*1024usize).max(usize::try_from(geometry.tilemap_bytes)? * 128)
            .checked_add(PB_BLOCK_BYTES-1).ok_or(EOVERFLOW)? & !(PB_BLOCK_BYTES-1);
        let page_count = pb_bytes / 32768;
        let block_count = pb_bytes / PB_BLOCK_BYTES;
        let block_capacity = (block_count+1).next_power_of_two();
        let mut job = Self {
            _command_views:views, commands, sequences:[fw(0x300)?,fw(0x300)?],
            metadata:fw(0x4000)?,manager:fw(0xc0)?,pages:gpu(page_count*4)?,
            blocks:fw(block_capacity*8)?,block_ring:fw(8)?,
            pb_memory:Buffer::new_gpu_aligned(dev,uat.kernel_vm(),vm,pb_bytes,PB_BLOCK_BYTES as u64)?,
            scratch:gpu(0x4000)?,tilemap:gpu(geometry.tilemap_bytes.try_into()?)?,
            tpc:gpu(geometry.tpc_bytes.try_into()?)?,unknown:gpu(0x4000)?,
            // Sizes inherited from the qualified M4 context-save experiment.
            preemption:[gpu(0x10000)?,gpu(0x10000)?],auxiliary:gpu(0x10000)?,
            uma:gpu(0x4000)?,private_memory:gpu(PRIVATE_BYTES)?,
            pool_state:gpu(PRIVATE_QWORDS*8)?,page_list:gpu(0x4000)?,
            binding:uat.bind(vm)?,descriptor:[0;16],stage:Stage::Tiling,
        };
        let base = job.private_memory.gpu_va()?;
        for i in 0..PRIVATE_PAGES { job.pool_state.u64(i*8,base+i as u64*4096)?; }
        for i in 0..PRIVATE_BYTES/0x200000 { job.page_list.u64(i*64,(1u64<<61)|(base+i as u64*0x200000))?; }
        let mut uma = [0;0xa0];
        crate::g16_compute::encode_uma(&mut uma,job.pool_state.gpu_va()?,PRIVATE_QWORDS as u32,
            PRIVATE_PAGES as u32,job.page_list.gpu_va()?,job.metadata.va()+0x500).map_err(|_| EINVAL)?;
        job.uma.write(0,&uma)?;
        let pb_base = state::compact(job.pb_memory.gpu_va()?).map_err(|_| EINVAL)?;
        for i in 0..page_count { job.pages.u32(i*4,((pb_base+i as u64*32768)>>15).try_into()?)?; }
        for i in 0..block_count { job.blocks.u32(i*8,((pb_base+i as u64*PB_BLOCK_BYTES as u64)>>15).try_into()?)?; }
        job.block_ring.u32(0,block_count.try_into()?)?;
        job.block_ring.u32(4,block_count.try_into()?)?;
        let mut manager = [0;0xc0];
        wire::parameter_manager(&mut manager,wire::ParameterManager {
            pages_fw:job.pages.va(),pages_gpu:job.pages.gpu_va()?,blocks:job.blocks.va(),ring:job.block_ring.va(),
            counter:job.metadata.va()+0x508,discard:0,list_bytes:(page_count*4).try_into()?,pages:page_count.try_into()?,
            block_capacity:block_capacity.try_into()?,write:block_count.try_into()?,read:0,
            min_pages:page_count.try_into()?,max_pages:page_count.try_into()?,id:0 }).map_err(|_| EINVAL)?;
        job.manager.write(0,&manager)?;
        let mut descriptor = g16_render::ParameterBufferDescriptor([0;4]);
        descriptor.update(job.pages.gpu_va()?,page_count.try_into()?,0,0,false,false).map_err(|_| EINVAL)?;
        job.descriptor = descriptor.to_bytes();
        let mut init = [0;0x20];
        wire::init_parameter_buffer(&mut init,job.binding.slot(),0,0,block_count.try_into()?,job.manager.va(),STAMP).map_err(|_| EINVAL)?;
        job.metadata.write(0x300,&init)?;
        // PB scene scratch has explicit GPU and firmware aliases. Statistics
        // are a distinct retained record; firmware dereferences them on retire.
        job.metadata.u64(0,job.scratch.gpu_va()?)?;
        job.metadata.u64(8,job.scratch.va())?;
        job.metadata.u64(0x40,job.metadata.va()+0x200)?;
        let mask = if r.flags & 16 != 0 {u64::MAX} else {u32::MAX as u64};
        let program = |p:crate::g17_uapi::UapiProgram| Program {
            address:usc+u64::from(p.usc & !63), resources:p.resource_spec & mask };
        let state = State { geometry,vdm:r.vdm_base,tilemap:job.tilemap.gpu_va()?,tpc:job.tpc.gpu_va()?,
            unknown:job.unknown.gpu_va()?,preemption:[job.preemption[0].gpu_va()?,job.preemption[1].gpu_va()?],
            scratch:job.scratch.gpu_va()?,auxiliary:job.auxiliary.gpu_va()?,multisample:r.multisample_control,ppp:r.ppp_control,
            background:program(r.background),eot:program(r.end_of_tile),
            partial_background:program(r.partial_background),partial_eot:program(r.partial_end_of_tile),
            depth_bias:r.depth_bias_base,scissor:r.scissor_base,query:r.occlusion_query_base,
            depth:DepthStencil{base:r.depth.base,stride:r.depth.stride as u64},
            stencil:DepthStencil{base:r.stencil.base,stride:r.stencil.stride as u64},
            depth_dimensions:r.depth_dimensions as u64,zls_control:r.zls_control,
            depth_clear:r.depth_clear,stencil_clear:r.stencil_clear,process_empty_tiles:r.flags&2!=0 };
        let mut work = KBox::new([0;0xca0],GFP_KERNEL)?;
        let mut sequence = [0;0x300];
        let mut registers = KBox::new([Register::default();128],GFP_KERNEL)?;
        for (i,stage) in [Stage::Tiling,Stage::Fragment].into_iter().enumerate() {
            let uuid = 0x16000000 | (u32::from(generation)<<8) | i as u32;
            wire::encode(&mut work[..],&mut sequence,stage,wire::Args {
                command:job.commands[i].va(),sku:job.sequences[i].va(),queue:queues[i],
                slots:job.metadata.va()+0x600+(i as u64+1)*4,
                pb_slot:job.metadata.va(),manager:job.manager.va(),pb_aux:job.metadata.va()+0x100,
                notifier:notifications[i],shared_stamp:job.metadata.va()+0x400+i as u64*16,
                stamp_address:job.metadata.va()+0x408+i as u64*16,uma:job.uma.va(),
                uma_aux:job.uma.gpu_va()?+0x100+i as u64*0x100,
                context:job.binding.slot(),generation,counter:i as u64+1,uuid,stamp:STAMP,
                event_slot:SLOTS[i] as u32,pb_id:0,pass_id:0 }).map_err(|_| EINVAL)?;
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
        dev_info!(dev.as_ref(), "G16G: render USC freelist={:#x} capacity={} populated={}\n",
            job.pool_state.gpu_va()?,PRIVATE_QWORDS,PRIVATE_PAGES);
        dev_info!(dev.as_ref(),"G16G: owned render {}x{} ASID={} PB={} pages tilemap={:#x} TPC={:#x}\n",
            r.width,r.height,job.binding.slot(),page_count,job.tilemap.gpu_va()?,job.tpc.gpu_va()?);
        Ok(job)
    }
    pub(crate) fn index(&self) -> usize { usize::from(self.stage == Stage::Fragment) }
    pub(crate) fn address(&self) -> u64 { self.commands[self.index()].va() }
    pub(crate) fn init_address(&self) -> u64 { self.metadata.va()+0x300 }
    pub(crate) fn descriptor(&self) -> &[u8;16] { &self.descriptor }
    pub(crate) fn context(&self) -> u32 { self.binding.slot() }
    pub(crate) fn status(&mut self) -> Result<[u64;4]> {
        let i = self.index();
        Ok([self.metadata.read_u32(0x400+i*16)? as u64,self.metadata.read_u32(0x408+i*16)? as u64,
            self.commands[i].read_u32(if i==0 {0x918} else {0xc60})? as u64,
            self.manager.read_u32(0x54)? as u64])
    }
}
