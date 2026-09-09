// SPDX-License-Identifier: GPL-2.0-only
/* Host-only differential fixture using the original executed M4 C encoder. */
#include <stdio.h>
#include <assert.h>
#include "../g16_lab/render/g16_render_pack.h"
int main(void) {
 const u64 b=0xfffffc2001000000ULL;
 for(unsigned stage=0;stage<2;stage++) {
  struct g16_render_job_args a={
   .sku={.command_va=b,.sku_va=b+0x4000,.queue_va=b+0x8000,.slot_array_va=b+0xc004,
    .pb_slot_va=b+0x10000,.buffer_info_va=b+0x14000,.context_id=3,.pb_slot=2,.pass_id=4,
    .uuid=0x1234,.stamp=0x300,.stamp_va=b+0x24000,.stamp_slot=0x71,.fragment_increment=1},
   .command_counter=0x100000001,.notifier_va=b+0x1c000,.shared_stamp_va=b+0x20000,
   .pb_aux_va=b+0x18000,.context_generation=2};
  g16_render_put(a.sku.uma,b+0x28000,8);g16_render_put(a.sku.uma+0x18,0x7003000000,8);
  u8 command[0xca0],sku[0x300];memset(command,0xa5,sizeof(command));memset(sku,0xa5,sizeof(sku));
  int bytes=g16_render_pack_sku(sku,sizeof(sku),stage,&a.sku);assert(bytes>0);
  assert(!g16_render_pack_job(command,sizeof(command),stage,&a,bytes));
  assert(fwrite(command,1,sizeof(command),stdout)==sizeof(command));
  assert(fwrite(sku,1,sizeof(sku),stdout)==sizeof(sku));
 }
 struct g16_render_pb_manager_args a={.page_list_fw_va=b+0x10000,.page_list_gpu_va=0x7002000000,
  .block_list_va=b+0x14000,.block_ring_va=b+0x18000,.counter_va=b+0x1c000,
  .page_list_bytes=0x4000,.page_count=128,.block_capacity=64,.block_wptr=32,
  .max_pages=256,.min_pages=128,.id=2};
 u8 manager[0xc0],init[0x20];
 assert(!g16_render_pack_pb_manager(manager,sizeof(manager),&a));
 assert(!g16_render_pack_init_pb(init,sizeof(init),3,2,0,32,b+0x14000,0x300));
 assert(fwrite(manager,1,sizeof(manager),stdout)==sizeof(manager));
 assert(fwrite(init,1,sizeof(init),stdout)==sizeof(init));
}
