#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Offline ASan/UBSan checks of the actual bounded AFKEP V2 query code.

The transport (m4_dcpext_afk.c) and the DPTX lifecycle/callback/link state
machine (m4_dcpext_dptx.c) come from the kernel worktree; the PoC's
query-number logic for the mode-count / AV power / route validate / route
cycle queries (dcpext_afk_query.h) is layered on top through a shim that
maps its globals onto the per-device structs. Every assertion is kept.
"""
from pathlib import Path
import json
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
kernel = root / 'linux/linux-asahi/drivers/gpu/drm/apple/m4-dcpext'
source = (root / 'tools/m4-dcpext-probe/dcpext_afk_query.h').read_text()
# Per-endpoint state, the transport functions and the DPTX trial come from the kernel code.
for line in ('static unsigned int afk_query;\n', 'static u16 afk_query_interface;\nstatic u8 afk_query_sequence;\n',
             'static int afk_query_result = -ENODATA;\n', '#include "dcpext_dptx_trial.h"\n'):
    assert line in source, line
    source = source.replace(line, '')
source = source.replace('static bool afk_query_open, afk_query_sent, afk_query_replied;\n', 'static bool afk_query_open;\n')
source = source.replace('static bool afk_route_disconnect, afk_route_dirty;\n', 'static bool afk_route_disconnect;\n')
named = source.index('/* Flat OSSerialize dictionaries emitted by the observed V2 publications.')
wrapper = source.index('static int afk_query_publication_for(')
source = source[:named] + '#define afk_query_publication_named m4_dcpext_afk_publication_named\n' + source[wrapper:]
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "m4_dcpext_afk.h"
#include "m4_dcpext_dptx.h"
#define module_param(...)
#define module_param_named(...)
#define module_param_string(...)
#define MODULE_PARM_DESC(...)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dma_rmb() do {} while (0)
#define dma_wmb() do {} while (0)
#define ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
#define pr_info(...) do {} while (0)
#define pr_err(...) do {} while (0)
static bool quiesce_before_afk_stop, dptx_quiesce_handoff;
static struct { bool ap_quiesced; } session;
static unsigned int port;
static u32 session_atc_target(void) {return 0x8010;}
static unsigned int afk_query;
static bool session_atc_active, session_atc_hpd;
static int aux_status, aux_calls;
static int session_atc_aux(bool enable) {aux_calls++;if(!aux_status)session_atc_active=enable;return aux_status;}
static int session_atc_check_partner(void) {return aux_status;}
static u16 get_unaligned_le16(const void *p) {u16 v;memcpy(&v,p,2);return v;}
static u32 get_unaligned_le32(const void *p) {u32 v;memcpy(&v,p,4);return v;}
static u64 get_unaligned_le64(const void *p) {u64 v;memcpy(&v,p,8);return v;}
static void put_unaligned_le16(u16 v,void *p) {memcpy(p,&v,2);}
static void put_unaligned_le32(u32 v,void *p) {memcpy(p,&v,4);}
static struct m4_dcpext_afk q; /* One endpoint channel; the PoC globals alias its fields. */
static struct m4_dcpext_dptx d; /* The DPTX lifecycle on that endpoint. */
#define afk_probe_rings q.rings
#define afk_probe_running q.running
#define afk_probe_stopping q.stopping
#define afk_probe_error q.error
#define afk_query_interface q.interface
#define afk_query_sequence q.sequence
static u64 afk_probe_deadline,now;
static u64 session_now(void *p) {(void)p;return now;}
static int sends,send_result;
static u32 sent_type,sent_ptr;
static int afk_probe_send(u32 type,u64 ptr) {sends++;sent_type=type;sent_ptr=ptr;return send_result;}
static int q_send(void *cookie,u8 ep,u64 message) {(void)cookie;assert(ep==0x23);return afk_probe_send(message>>48,message&0xffffffffffffULL);}
static int q_allocate(void *cookie,u32 size,u8 **buffer,u64 *dva) {(void)cookie;(void)size;(void)buffer;(void)dva;return -ENOSYS;}
static const struct m4_dcpext_afk_ops q_ops = {.send=q_send,.allocate=q_allocate};
static u8 ring[128*11];
static int main_rate_calls, main_drive_calls, main_error;
static int phy_set_rate(void *c,struct m4_dcpext_dptx_link *s,u32 rate) {
 (void)c;main_rate_calls++;if(main_error)return main_error;s->rate=rate;return 0;
}
static int phy_begin_config(void *c,struct m4_dcpext_dptx_link *s) {(void)c;(void)s;return 0;}
static int phy_set_drive(void *c,struct m4_dcpext_dptx_link *s,const u32 drive[6]) {
 (void)c;(void)drive;assert(s->rate==10 && s->lanes==2);main_drive_calls++;return main_error;
}
static int phy_validate_config(void *c,struct m4_dcpext_dptx_link *s) {(void)c;(void)s;return main_error;}
static const struct m4_dcpext_dptx_phy_ops phy = {.set_rate=phy_set_rate,.begin_config=phy_begin_config,
 .set_drive=phy_set_drive,.validate_config=phy_validate_config};
static int afk_query_write(const u8 *payload,u32 bytes);
static int d_write(void *c,const u8 *p,u32 n) {(void)c;return afk_query_write(p,n);}
static u64 d_now(void *c) {(void)c;return now;}
static int d_aux(void *c,bool enable) {(void)c;return session_atc_aux(enable);}
static int d_partner(void *c) {(void)c;return session_atc_check_partner();}
static bool d_hpd(void *c) {(void)c;return session_atc_hpd;}
static const struct m4_dcpext_dptx_ops d_ops = {.write=d_write,.now_ms=d_now,.aux=d_aux,
 .check_partner=d_partner,.hpd=d_hpd,.phy=&phy};
/* The PoC selected behaviour by query number; the struct carries explicit policy. */
static void d_sync(void) {d.interface=q.interface;d.aux_callbacks=afk_query>=6;d.hpd_routing=afk_query>=7;}
static int dptx_trial_send(void) {d_sync();return m4_dcpext_dptx_send(&d,&d_ops,NULL);}
static int dptx_trial_callback(const u8 *p,u32 n) {d_sync();return m4_dcpext_dptx_callback(&d,&d_ops,NULL,p,n);}
static int dptx_trial_receive(const u8 *p,u32 n) {d_sync();return m4_dcpext_dptx_receive(&d,&d_ops,NULL,p,n);}
static int dptx_trial_progress(void) {d_sync();return m4_dcpext_dptx_progress(&d,&d_ops,NULL);}
static int dptx_record_display_hints(const u8 *p,u32 n) {return m4_dcpext_dptx_display_hints(&d.monitor,p,n);}
#define dptx_trial_phase d.phase
#define dptx_trial_callbacks d.callbacks
#define dptx_trial_observe_until d.observe_until
#define afk_query_sent d.sent
#define afk_query_replied d.replied
#define afk_query_result d.result
#define afk_route_dirty d.route_dirty
#define afk_display_dirty d.display_dirty
#define afk_hpd_dirty d.hpd_dirty
#define session_atc_fault d.phy_fault
#define link_training d.training
#define link_capabilities d.capabilities
#define iboot_probe d.iboot_gate
#define iboot_ready_for_hpd d.iboot_ready
#define iboot_finished d.iboot_finished
#define session_link d.link
#define dptx_link_state m4_dcpext_dptx_link
#define dptx_monitor_seen d.monitor.seen
#define dptx_monitor_name d.monitor.name
#define dptx_monitor_width d.monitor.width
#define dptx_monitor_height d.monitor.height
#define DPTX_TRIAL_CONNECT M4_DCPEXT_DPTX_CONNECT
#define DPTX_TRIAL_REQUEST M4_DCPEXT_DPTX_REQUEST
#define DPTX_TRIAL_RELEASE M4_DCPEXT_DPTX_RELEASE
#define DPTX_TRIAL_DISCONNECT M4_DCPEXT_DPTX_DISCONNECT
#define DPTX_TRIAL_DONE M4_DCPEXT_DPTX_DONE
#define DPTX_TRIAL_HPD_ON M4_DCPEXT_DPTX_HPD_ON
#define DPTX_TRIAL_HPD_WAIT M4_DCPEXT_DPTX_HPD_WAIT
#define DPTX_TRIAL_HPD_OFF M4_DCPEXT_DPTX_HPD_OFF
'''
captures = json.loads((root / '_provenance/displayport-poc-20260914/afk-ep24-20260914-2/decoded.json').read_text())['records']
for i, record in enumerate(captures):
    raw = bytes.fromhex(record['raw_hex'])
    program += f'static const u8 publication_{i}[] = {{' + ','.join(str(v) for v in raw) + '};\n'
captures = json.loads((root / '_provenance/displayport-poc-20260914/afk-ep2a-20260914-1/decoded.json').read_text())['records']
for i, record in enumerate(captures):
    raw = bytes.fromhex(record['raw_hex'])
    program += f'static const u8 dptx_publication_{i}[] = {{' + ','.join(str(v) for v in raw) + '};\n'
hint_records = json.loads((root / '_provenance/displayport-poc-20260914/afk-hpd-dpxbar-trial-20260914-1/decoded.json').read_text())['records']
hints = [bytes.fromhex(r['raw_hex']) for r in hint_records if r['bytes'] == 256]
assert len(hints) == 2 and b'PHL 237E7' in hints[0]
for i, raw in enumerate(hints):
    program += f'static const u8 hint_{i}[] = {{' + ','.join(str(v) for v in raw) + '};\n'
caps = json.loads((root / '_provenance/displayport-poc-20260914/afk-monitor-cleanup-trial-20260914-2/monitor-decoded.json').read_text())['records']
for command in (7, 13, 20):
    matching = [bytes.fromhex(r['raw_hex']) for r in caps if r.get('category') == 1
                and r['bytes'] >= 96 and int.from_bytes(bytes.fromhex(r['raw_hex'])[36:40], 'little') == command]
    assert len(matching) == 1
    program += f'static const u8 capability_{command}[] = {{' + ','.join(str(v) for v in matching[0]) + '};\n'
training_records = json.loads((root / '_provenance/displayport-poc-20260914/afk-link-training-20260914-1/decoded.json').read_text())['records']
training_drive, = {bytes.fromhex(r['raw_hex']) for r in training_records
                  if r.get('bytes') == 152 and bytes.fromhex(r['raw_hex'])[36:40] == b'\x03\x00\x00\x00'
                  and bytes.fromhex(r['raw_hex'])[25] == 15}
program += 'static const u8 captured_drive[]={' + ','.join(str(v) for v in training_drive) + '};\n'
image_records = json.loads((root / '_provenance/displayport-poc-20260914/iboot-image-20260914-1/afk-decoded.json').read_text())['records']
get_drive, = [bytes.fromhex(r['raw_hex']) for r in image_records if r.get('bytes') == 152 and r.get('category') == 1 and bytes.fromhex(r['raw_hex'])[36:40] == b'\x04\x00\x00\x00']
program += 'static const u8 captured_get_drive[]={' + ','.join(str(v) for v in get_drive) + '};\n'
edid_records = json.loads((root / '_provenance/displayport-poc-20260914/edid-discovery-20260914-1/edid-decoded.json').read_text())['records']
for n, record in enumerate(edid_records):
    raw = bytes.fromhex(record['raw_hex'])
    program += f'static const u8 captured_av_{n}[]={{' + ','.join(str(v) for v in raw) + '};\n'
program += source
program += r'''
static void reset(void) {
 memset(ring,0,sizeof(ring));memset(&q,0,sizeof(q));memset(&d,0,sizeof(d));q.ops=&q_ops;q.endpoint=0x23;
 afk_probe_rings[0].header=ring;afk_probe_rings[0].data=ring+384;
 afk_probe_rings[0].block=128;afk_probe_rings[0].size=1024;afk_probe_running=true;
 afk_query=true;afk_query_interface=0;afk_query_sequence=0;
 afk_query_open=afk_query_sent=afk_query_replied=afk_query_closing=afk_query_closed=false;
 afk_route_disconnect=afk_route_dirty=false;
 afk_display_dirty=false;dptx_trial_phase=DPTX_TRIAL_CONNECT;dptx_trial_callbacks=0;
 link_capabilities=false;
 iboot_probe=iboot_ready_for_hpd=iboot_finished=false;
 link_training=false;session_link=(struct dptx_link_state){0};
 main_rate_calls=main_drive_calls=main_error=0;
 session_atc_active=session_atc_fault=false;aux_status=aux_calls=0;
 session_atc_hpd=true;afk_hpd_dirty=false;dptx_trial_observe_until=0;
 dptx_monitor_seen=false;memset(dptx_monitor_name,0,sizeof(dptx_monitor_name));
 afk_query_result=-ENODATA;sends=send_result=0;now=0;afk_probe_deadline=2000;
}
static void reply(u8 p[52]) {
 memset(p,0,52);put_unaligned_le16(1,p+2);put_unaligned_le32(44,p+4);
 p[16]=0xc0;p[17]=2;p[25]=1;put_unaligned_le32(3,p+32);put_unaligned_le32(20,p+36);
 p[40]=1;put_unaligned_le32(6,p+44);put_unaligned_le32(4,p+48);
}
int main(void) {
 u8 second[sizeof(dptx_publication_0)];
 memcpy(second,dptx_publication_0,sizeof(second));
 bool replaced=false;
 for(unsigned i=0;i+8<sizeof(second);i++)if(!memcmp(second+i,"dispext0",8)){second[i+7]='1';replaced=true;}
 assert(replaced);
 assert(afk_query_publication_for(dptx_publication_0,sizeof(dptx_publication_0),1,7)==1);
 assert(afk_query_publication_for(second,sizeof(second),1,7)==0);
 port=1;
 assert(afk_query_publication_for(dptx_publication_0,sizeof(dptx_publication_0),1,7)==0);
 assert(afk_query_publication_for(second,sizeof(second),1,7)==1);
 port=0;
 puts("PASS: each controller admits only its own DPTX service");
 assert(afk_query_publication_named(captured_av_0,sizeof(captured_av_0),1,"dispext0:dcpav-service-epic:0")==1);
 assert(afk_query_publication_named(captured_av_1,sizeof(captured_av_1),3,"dispext0:dcpav-service-epic:0")==0);
 assert(afk_query_publication_named(captured_av_0,sizeof(captured_av_0),3,"dispext0:dcpav-service-epic:0")==-EPROTO);
 assert(afk_query_publication_named(captured_av_0,sizeof(captured_av_0)-1,1,"dispext0:dcpav-service-epic:0")==-EPROTO);
 u8 publish[116]={0},p[52],saved[sizeof(ring)];
 reset();put_unaligned_le16(1,publish+2);put_unaligned_le32(108,publish+4);
 publish[16]=0x11;publish[18]=1;memcpy(publish+24,"disp0-service",14);put_unaligned_le32(0xd3,publish+56);
 put_unaligned_le32(0x81000002,publish+60);put_unaligned_le32(0x0900000c,publish+64);
 memcpy(publish+68,"interface-id",12);put_unaligned_le32(0x04000040,publish+80);put_unaligned_le32(1,publish+84);
 put_unaligned_le32(0x0900000f,publish+92);memcpy(publish+96,"ep-has-desc-mgr",15);put_unaligned_le32(0x8b000001,publish+112);
 assert(!afk_query_receive(publish,sizeof(publish)) && afk_query_interface==1);
 assert(!afk_query_progress() && afk_query_open && sends==1 && sent_type==0xa2 && sent_ptr==128);
 const u8 expected_open[24]={0,0,1,0,16,0,0,0,0,0,0,0,0,0,0,0,0x12,0,1,0,0,0,0,0};
 assert(!memcmp(ring+400,expected_open,24));
 assert(!afk_query_progress() && afk_query_sent && sends==2 && sent_ptr==256);
 const u8 expected_command[48]={1,0,1,0,40,0,0,0,0,0,0,0,0,0,0,0,0xc0,1,0,0,0,0,0,0,
  0,1,0,0,20,0,0,0,3,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0};
 assert(!memcmp(ring+528,expected_command,48));
 reply(p);assert(!afk_query_receive(p,52) && afk_query_replied && !afk_query_result);
 assert(!afk_query_progress() && afk_query_closing && sends==3);
 assert(ring[656+16]==0x13 && ring[656+18]==1);
 memset(p,0,24);put_unaligned_le16(1,p+2);put_unaligned_le32(16,p+4);p[16]=0x13;p[18]=1;
 assert(!afk_query_receive(p,24) && afk_query_closed);
 puts("PASS: qualified publication, native open/command/close bytes, matched mode-count reply and close acknowledgement");
 for(int bad=0;bad<8;bad++) {
  reset();afk_query_interface=1;afk_query_sent=true;reply(p);
  switch(bad) {case 0:p[2]=2;break;case 1:p[4]++;break;case 2:p[25]=2;break;
   case 3:p[24]=1;break;case 4:p[16]=0xc1;break;
   case 5:p[36]=21;break;case 6:p[40]=2;break;case 7:p[32]=4;break;}
  assert(afk_query_receive(p,52)==-EPROTO);
 }
 reset();afk_query_interface=1;afk_query_sent=true;reply(p);p[28]=1;
 assert(!afk_query_receive(p,52) && afk_query_result==-EREMOTEIO);
 reset();afk_query_interface=1;afk_query_sent=true;reply(p);p[8]=0x7e;p[9]=0x80;p[10]=0xf4;p[11]=0xe5;p[12]=2;
 assert(!afk_query_receive(p,52) && !afk_query_result);
 puts("PASS: wrong interface, length, tag, OOB flag, type, reply extent, HPD and operation rejected; firmware common field accepted; remote error retained");
 reset();afk_query_interface=1;put_unaligned_le32(128,ring+128);memcpy(saved,ring,sizeof(ring));
 assert(afk_query_message(0x12,false)==-ENOSPC && !sends && !memcmp(saved,ring,sizeof(ring)));
 reset();put_unaligned_le32(1024,ring+256);memcpy(saved,ring,sizeof(ring));
 assert(afk_query_message(0x12,false)==-EPROTO && !sends && !memcmp(saved,ring,sizeof(ring)));
 reset();put_unaligned_le32(896,ring+256);put_unaligned_le32(256,ring+128);
 assert(!afk_query_message(0x12,false) && sent_ptr==0);
 assert(!afk_query_tx_empty());put_unaligned_le32(0,ring+128);assert(afk_query_tx_empty());
 put_unaligned_le32(1,ring+128);put_unaligned_le32(1,ring+256);assert(!afk_query_tx_empty());
 reset();send_result=-ETIMEDOUT;assert(afk_query_message(0x12,false)==-ETIMEDOUT);
 assert(get_unaligned_le32(ring+256)==128 && ring[416]==0x12);
 reset();afk_query_interface=1;afk_query_open=afk_query_sent=true;now=1600;
 assert(!afk_query_progress() && afk_query_closing && !afk_query_replied);
 puts("PASS: full/invalid ring no writes, exact-end wrap, notification failure preserves publication, missing reply initiates close");
 reset();afk_query=2;afk_query_interface=1;
 assert(!afk_query_progress() && sent_ptr==128);
 assert(!afk_query_progress() && sent_ptr==384);
 u8 *cmd=ring+528;
 assert(get_unaligned_le32(cmd+4)==120 && cmd[16]==0xc0 && cmd[17]==1 && cmd[25]==1);
 assert(get_unaligned_le32(cmd+28)==96 && !get_unaligned_le32(cmd+32));
 assert(get_unaligned_le32(cmd+36)==7 && get_unaligned_le32(cmd+40)==32 && get_unaligned_le32(cmd+44)==0x69706378);
 u8 power[128];memcpy(power,cmd,128);power[17]=2;put_unaligned_le32(0,power+28);power[112]=1;
 assert(get_unaligned_le64(power+112)==1);
 assert(!afk_query_receive(power,128) && afk_query_result==0);
 reset();afk_query=2;afk_query_interface=1;afk_query_sent=true;power[44]^=1;
 assert(afk_query_receive(power,128)==-EPROTO);
 reset();afk_query=2;afk_query_interface=1;put_unaligned_le32(128,ring+128);memcpy(saved,ring,sizeof(ring));
 assert(afk_query_message(0xc0,true)==-ENOSPC && !memcmp(saved,ring,sizeof(ring)));
 reset();afk_query=2;afk_query_interface=1;put_unaligned_le32(896,ring+256);put_unaligned_le32(256,ring+128);
 memcpy(saved,ring,sizeof(ring));assert(afk_query_message(0xc0,true)==-ENOSPC && !memcmp(saved,ring,sizeof(ring)));
 /* Tail too short, but room at zero: duplicate header, whole payload, preserve unread bytes. */
 put_unaligned_le32(512,ring+128);memcpy(saved,ring,sizeof(ring));
 assert(!afk_query_message(0xc0,true) && sent_ptr==256);
 assert(!memcmp(ring+384+896,ring+384,16) && get_unaligned_le32(ring+384)==0x20504f49);
 assert(get_unaligned_le32(ring+384+4)==128 && !memcmp(saved+384+512,ring+384+512,384));
 reset();afk_query=2;
 assert(!afk_query_receive(publication_1,sizeof(publication_1)) && !afk_query_interface);
 assert(!afk_query_receive(publication_2,sizeof(publication_2)) && !afk_query_interface);
 assert(!afk_query_receive(publication_3,sizeof(publication_3)) && !afk_query_interface);
 assert(!afk_query_receive(publication_0,sizeof(publication_0)) && afk_query_interface==1);
 u8 corrupt[sizeof(publication_0)];memcpy(corrupt,publication_0,sizeof(corrupt));
 corrupt[308]=2;assert(afk_query_publication_for(corrupt,sizeof(corrupt),1,afk_query)==-EPROTO);
 memcpy(corrupt,publication_0,sizeof(corrupt));corrupt[304]=63;
 assert(afk_query_publication_for(corrupt,sizeof(corrupt),1,afk_query)==-EPROTO);
 memcpy(corrupt,publication_0,sizeof(corrupt));corrupt[67]=0x89;
 assert(afk_query_publication_for(corrupt,sizeof(corrupt),1,afk_query)==-EPROTO);
 for(u32 len=0;len<sizeof(publication_0);len++)assert(afk_query_publication_for(publication_0,len,1,afk_query)<0);
 puts("PASS: native AV getPower header and response, multi-block admission, no split-wrap publication, exact service selection");
 reset();afk_query=3;
 assert(!afk_query_receive(dptx_publication_1,sizeof(dptx_publication_1)) && !afk_query_interface);
 assert(!afk_query_receive(dptx_publication_0,sizeof(dptx_publication_0)) && afk_query_interface==1);
 assert(!afk_query_progress() && sent_ptr==128);
 assert(!afk_query_progress() && sent_ptr==384);
 cmd=ring+528;
 assert(get_unaligned_le32(cmd+4)==136 && get_unaligned_le32(cmd+28)==112);
 assert(!get_unaligned_le32(cmd+32) && get_unaligned_le32(cmd+36)==12);
 assert(get_unaligned_le32(cmd+40)==48 && get_unaligned_le32(cmd+44)==0x69706378);
 assert(get_unaligned_le32(cmd+96)==0x100 && get_unaligned_le32(cmd+100)==0x8010);
 u8 route[144];memcpy(route,cmd,144);route[17]=2;put_unaligned_le32(0,route+28);
 put_unaligned_le32(0x1234,route+128);
 assert(!afk_query_receive(route,144) && !afk_query_result);
 for(int bad=0;bad<8;bad++) {
  u8 copy[144];memcpy(copy,route,144);
  reset();afk_query=3;afk_query_interface=1;afk_query_sent=true;
  const u32 off[]={32,36,40,44,96,100,25,24};copy[off[bad]]^=1;
  assert(afk_query_receive(copy,144)==-EPROTO);
 }
 for(u32 len=0;len<144;len++) {
  reset();afk_query=3;afk_query_interface=1;afk_query_sent=true;
  assert(afk_query_receive(route,len)<0);
 }
 reset();afk_query=3;afk_query_interface=1;afk_query_sent=true;
 put_unaligned_le32(0xe00002c7,route+112);
 assert(!afk_query_receive(route,144) && afk_query_result==-EREMOTEIO);
 puts("PASS: DPTX unit selection, native validate header, route encoding, reply bounds/status and mismatched echoes");
 reset();afk_query=4;afk_query_interface=1;
 assert(!afk_query_progress() && !afk_route_dirty);
 assert(!afk_query_progress() && afk_route_dirty && sent_ptr==384);
 cmd=ring+528;
 assert(get_unaligned_le32(cmd+4)==120 && get_unaligned_le32(cmd+28)==96);
 assert(get_unaligned_le32(cmd+36)==11 && get_unaligned_le32(cmd+40)==32);
 assert(get_unaligned_le32(cmd+44)==0x69706378 && !get_unaligned_le32(cmd+96));
 assert(get_unaligned_le32(cmd+100)==0x8010);
 u8 connect[128];memcpy(connect,cmd,128);connect[17]=2;put_unaligned_le32(0,connect+28);
 assert(!afk_query_receive(connect,128) && !afk_query_result && afk_route_dirty);
 assert(!afk_query_progress() && afk_route_disconnect && afk_query_sent && !afk_query_replied);
 assert(sent_ptr==640 && afk_route_dirty);
 cmd=ring+384+384+16;
 assert(cmd[25]==2 && !get_unaligned_le32(cmd+100));
 u8 disconnect[128];memcpy(disconnect,cmd,128);disconnect[17]=2;put_unaligned_le32(0,disconnect+28);
 assert(!afk_query_receive(disconnect,128) && !afk_route_dirty && !afk_query_result);
 assert(!afk_query_progress() && afk_query_closing);
 for(int bad=0;bad<4;bad++) {
  reset();afk_query=4;afk_query_interface=1;afk_query_sent=true;afk_route_disconnect=afk_route_dirty=true;
  u8 copy[128];memcpy(copy,disconnect,128);
  const u32 off[]={25,100,112,28};copy[off[bad]]=1;
  int ret=afk_query_receive(copy,128);
  assert(afk_route_dirty && (ret || afk_query_result));
 }
 reset();afk_query=4;afk_query_interface=1;afk_query_sent=afk_query_open=true;afk_route_dirty=true;
 now=1600;assert(!afk_query_progress() && afk_query_closing && afk_route_dirty);
 reset();afk_query=4;afk_query_interface=1;afk_query_sent=true;afk_route_dirty=true;
 connect[28]=1;assert(!afk_query_receive(connect,128) && afk_query_result==-EREMOTEIO);
 assert(!afk_query_progress()); /* open */
 assert(!afk_query_progress() && afk_route_disconnect);
 assert(!afk_query_receive(disconnect,128) && !afk_route_dirty && afk_query_result==-EREMOTEIO);
 puts("PASS: connect/disconnect tags and native header; lost, failed or mismatched disconnect retains recovery requirement");
 /* Four distinct commands: connect, request, release (native group 8), disconnect. */
 for(int failure=-1;failure<4;failure++) {
  reset();afk_query=5;afk_query_interface=1;
  assert(!afk_query_progress());
  for(int step=0;step<4;step++) {
   if(failure==0 && (step==1 || step==2))continue;
   u32 before=get_unaligned_le32(ring+256);
   assert(!afk_query_progress());
   const u8 *wire=ring+384+before+16;
   const u32 commands[]={11,6,7,11}, groups[]={0,0,8,0}, sizes[]={32,16,16,32};
   assert(wire[25]==step+1 && get_unaligned_le32(wire+36)==commands[step]);
   assert(get_unaligned_le32(wire+32)==groups[step]<<16 && get_unaligned_le32(wire+40)==sizes[step]);
   u8 rsp[128];u32 len=96+sizes[step];memcpy(rsp,wire,len);
   rsp[17]=2;put_unaligned_le32(0,rsp+28);
   if(step==failure)put_unaligned_le32(0xe00002c7,rsp+(sizes[step]==32?112:96));
   /* Model firmware consuming each packet before the next bounded send. */
   put_unaligned_le32(get_unaligned_le32(ring+256),ring+128);
   assert(!afk_query_receive(rsp,len));
  }
  assert(!afk_query_progress() && dptx_trial_phase==DPTX_TRIAL_DONE && afk_query_closing);
  assert(afk_display_dirty==(failure==2));assert(afk_route_dirty==(failure==3));
  assert(failure==-1 ? !afk_query_result : afk_query_result==-EREMOTEIO);
 }
 puts("PASS: native request/release command groups, failed-connect skips activation, cleanup acknowledgements clear independent obligations");
 reset();afk_query=5;afk_query_interface=1;
 u8 callback[112]={0};put_unaligned_le16(1,callback+2);put_unaligned_le32(104,callback+4);
 callback[8]=0x81;callback[16]=0xc0;callback[17]=1;callback[25]=0xa5;
 put_unaligned_le32(80,callback+28);put_unaligned_le32(16,callback+40);put_unaligned_le32(0x69706378,callback+44);
 assert(!afk_query_receive(callback,sizeof(callback)) && dptx_trial_callbacks==1 && afk_query_result==-EOPNOTSUPP);
 const u8 *unsupported=ring+400;
 assert(get_unaligned_le32(unsupported+4)==24 && unsupported[8]==0x81 && unsupported[17]==2 && unsupported[25]==0xa5);
 assert(get_unaligned_le32(unsupported+28)==0xe00002c7);
 assert(!afk_route_dirty && !afk_display_dirty);
 for(u32 len=0;len<32;len++)assert(dptx_trial_callback(callback,len)<0);
 callback[24]=1;assert(dptx_trial_callback(callback,sizeof(callback))==-EOPNOTSUPP);
 callback[24]=0;dptx_trial_callbacks=32;assert(dptx_trial_callback(callback,sizeof(callback))==-EOPNOTSUPP);
 puts("PASS: inline callback receives explicit unsupported status; OOB, truncation and excessive callbacks rejected without success claims");
 for(int op=0;op<5;op++) {
  const u32 commands[]={26,18,0,1,10},sizes[]={32,32,16,16,32};u8 cb[128]={0};
  u32 len=96+sizes[op];reset();afk_query=6;afk_query_interface=1;
  put_unaligned_le16(1,cb+2);put_unaligned_le32(len-8,cb+4);cb[16]=0xc0;cb[17]=1;cb[25]=7;
  put_unaligned_le32(len-32,cb+28);put_unaligned_le32(commands[op],cb+36);
  put_unaligned_le32(sizes[op],cb+40);put_unaligned_le32(0x69706378,cb+44);
  assert(!afk_query_receive(cb,len));
  const u8 *rsp=ring+400;
  assert(get_unaligned_le32(rsp+4)==len-8 && rsp[25]==7 && !get_unaligned_le32(rsp+28));
  assert(!get_unaligned_le32(rsp+96));
  if(op<2) assert(get_unaligned_le64(rsp+112)==(op==1) && !aux_calls);
  else if(op==4)assert(get_unaligned_le64(rsp+112)==2 && !aux_calls);
  else assert(aux_calls==1 && session_atc_active==(op==2));
  if(op==2) {
   reset();afk_query=6;afk_query_interface=1;aux_status=-EIO;
   assert(afk_query_receive(cb,len)==-EIO && session_atc_fault && !sends);
  }
 }
 puts("PASS: native boolean/status reply layouts; AUX callbacks invoke hardware abstraction and latch failed operations without success replies");
 reset();afk_query=7;afk_query_interface=1;link_capabilities=true;
 assert(!dptx_trial_callback(capability_7,sizeof(capability_7)));
 assert(get_unaligned_le64(ring+400+112)==0x1e && !get_unaligned_le32(ring+400+96));
 assert(!aux_calls && !session_atc_active);
 reset();afk_query=7;afk_query_interface=1;link_capabilities=true;aux_status=-ENOLINK;
 assert(dptx_trial_callback(capability_7,sizeof(capability_7))==-ENOLINK && session_atc_fault && !sends);
 reset();afk_query=7;afk_query_interface=1;link_capabilities=true;
 u8 spread[144];memcpy(spread,capability_13,144);
 memset(spread+96,0xa5,48);spread[96]=0x1e;
 assert(!dptx_trial_callback(spread,144));
 const u8 *spread_rsp=ring+400;
 assert(!memcmp(spread_rsp+96,spread+96,16));
 assert(!get_unaligned_le32(spread_rsp+112) && !spread_rsp[128]);
 assert(!memcmp(spread_rsp+116,spread+116,12) && !memcmp(spread_rsp+129,spread+129,15));
 assert(!aux_calls);
 reset();afk_query=7;afk_query_interface=1;link_capabilities=true;
 assert(!dptx_trial_callback(capability_20,sizeof(capability_20)));
 assert(!get_unaligned_le32(ring+400+96) && !aux_calls);
 for(unsigned cmd=5;cmd<=12;cmd++) {
  if(cmd==7 || cmd==10)continue;
  reset();afk_query=7;afk_query_interface=1;link_capabilities=true;
  u8 physical[128];memcpy(physical,capability_7,128);put_unaligned_le32(cmd,physical+36);
  assert(!dptx_trial_callback(physical,128));
  assert(get_unaligned_le32(ring+400+28)==0xe00002c7 && !aux_calls);
 }
 puts("PASS: captured capability replies use native M4 offsets and preserve input; disconnected partner fails; physical link operations remain Unsupported");

 for(unsigned fault=0;fault<2;fault++) {
  reset();afk_query=7;afk_query_interface=1;link_training=link_capabilities=true;
  u8 physical[128];memcpy(physical,capability_7,128);
  put_unaligned_le32(9,physical+36);memset(physical+96,0,32);physical[112]=10;
  if(fault)main_error=-ETIMEDOUT;
  int status=dptx_trial_callback(physical,128);
  assert(main_rate_calls==1);
  if(fault)assert(status==-ETIMEDOUT && session_atc_fault && session_link.fault && !sends);
  else {
   assert(!status && session_link.rate==10 && !get_unaligned_le32(ring+400+28));
   assert(get_unaligned_le32(ring+400+112)==10);
  }
 }
 reset();afk_query=7;afk_query_interface=1;link_training=link_capabilities=true;
 assert(!dptx_trial_callback(capability_7,sizeof(capability_7)));
 assert(get_unaligned_le64(ring+400+112)==10 && !main_rate_calls);
 reset();afk_query=7;afk_query_interface=1;link_training=link_capabilities=true;
 session_link.rate=10;session_link.lanes=2;
 assert(!dptx_trial_callback(captured_drive,sizeof(captured_drive)) && main_drive_calls==1);
 assert(!get_unaligned_le32(ring+400+28) && !get_unaligned_le32(ring+400+96));
 assert(get_unaligned_le32(ring+400+32)==0x10000 && get_unaligned_le32(ring+400+40)==56);
 assert(!memcmp(ring+400+100,captured_drive+100,sizeof(captured_drive)-100));
 reset();afk_query=7;afk_query_interface=1;link_training=link_capabilities=true;
 session_link.rate=10;session_link.lanes=2;session_link.drive[1]=2;session_link.drive[5]=1;
 assert(!dptx_trial_callback(captured_get_drive,sizeof(captured_get_drive)) && !main_drive_calls);
 assert(get_unaligned_le32(ring+400+32)==0x10000 && !get_unaligned_le32(ring+400+28));
 assert(!get_unaligned_le32(ring+400+112) && get_unaligned_le32(ring+400+132)==2 && get_unaligned_le32(ring+400+148)==1);
 assert(!memcmp(ring+400+96,captured_get_drive+96,16));
 puts("PASS: opt-in HBR callback dispatch, level-zero drive writes, native response offsets and failed-PHY retention latch");

 for(unsigned order=0;order<2;order++) {
 reset();d.release_before_hpd_off=order;afk_query=7;afk_query_interface=1;assert(!afk_query_progress());
 const u32 orders[][6]={{0,1,5,7,2,3},{0,1,5,2,7,3}};
 const u32 *phases=orders[order];
 for(unsigned i=0;i<6;i++) {
  u32 before=get_unaligned_le32(ring+256);
  if(i==3) {
   assert(!afk_query_progress() && dptx_trial_phase==DPTX_TRIAL_HPD_WAIT);
   assert(afk_hpd_dirty && dptx_trial_observe_until==600);
   now=599;assert(!afk_query_progress() && dptx_trial_phase==DPTX_TRIAL_HPD_WAIT);
   now=600;
  }
  assert(!afk_query_progress() && dptx_trial_phase==phases[i]);
  if(q.rings[0].size-before < 16+(phases[i]==1 || phases[i]==2 ? 112 : 128)) before=0;
  const u8 *wire=ring+384+before+16;u32 len=get_unaligned_le32(wire+4)+8;
  if(phases[i]==5 || phases[i]==7) {
   assert(get_unaligned_le32(wire+36)==8 && get_unaligned_le32(wire+32)==8<<16);
   assert(get_unaligned_le64(wire+112)==(phases[i]==5));
  }
  u8 rsp[128];memcpy(rsp,wire,len);rsp[17]=2;put_unaligned_le32(0,rsp+28);
  put_unaligned_le32(get_unaligned_le32(ring+256),ring+128);
  assert(!afk_query_receive(rsp,len));
 }
 assert(!afk_query_progress() && afk_query_closing && !afk_hpd_dirty && !afk_display_dirty && !afk_route_dirty);
 }
 reset();afk_query=7;afk_query_interface=1;dptx_trial_phase=DPTX_TRIAL_HPD_ON;session_atc_hpd=false;
 assert(dptx_trial_send()==-ENOLINK && !sends && !afk_hpd_dirty);
 reset();afk_query=7;afk_query_interface=1;dptx_trial_phase=DPTX_TRIAL_HPD_OFF;afk_hpd_dirty=true;afk_query_sent=true;
 u8 bad_hpd[128]={0};bad_hpd[16]=0xc0;bad_hpd[17]=2;bad_hpd[25]=8;
 put_unaligned_le32(0xe00002c7,bad_hpd+28);
 assert(!dptx_trial_receive(bad_hpd,128) && afk_hpd_dirty);
 puts("PASS: native HPD payload, bounded observation, both release/HPD-off orderings; absent HPD refuses notification and failed off retains obligation");
 reset();afk_query=7;afk_query_interface=9;
 assert(!dptx_trial_callback(hint_0,sizeof(hint_0)) && dptx_monitor_seen);
 assert(!strcmp(dptx_monitor_name,"PHL 237E7") && dptx_monitor_width==1920 && dptx_monitor_height==1080);
 const u8 *hint_rsp=ring+384+16;
 assert(sent_ptr==384 && get_unaligned_le32(hint_rsp+4)==248);
 assert(!get_unaligned_le32(hint_rsp+28) && !get_unaligned_le32(hint_rsp+240));
 assert(!memcmp(hint_rsp+96,hint_0+96,144));
 put_unaligned_le32(sent_ptr,ring+128);
 assert(!dptx_trial_callback(hint_1,sizeof(hint_1)) && dptx_monitor_seen);
 assert(!strcmp(dptx_monitor_name,"PHL 237E7"));
 for(unsigned size=0;size<160;size++)assert(dptx_record_display_hints(hint_0+96,size)==-EPROTO);
 u8 malformed_hint[160];memcpy(malformed_hint,hint_0+96,160);
 malformed_hint[92]=0x1b;assert(dptx_record_display_hints(malformed_hint,160)==-EPROTO);
 memcpy(malformed_hint,hint_0+96,160);memset(malformed_hint+92,'A',16);
 assert(dptx_record_display_hints(malformed_hint,160)==-EPROTO);
 memcpy(malformed_hint,hint_0+96,160);put_unaligned_le32(16385,malformed_hint+56);
 assert(dptx_record_display_hints(malformed_hint,160)==-EPROTO);
 reset();afk_query=7;afk_query_interface=1;iboot_probe=true;
 dptx_trial_phase=DPTX_TRIAL_HPD_ON;
 assert(!dptx_trial_progress() && !afk_query_sent && !sends);
 iboot_ready_for_hpd=true;
 assert(!dptx_trial_progress() && afk_query_sent && sends==1);
 dptx_trial_phase=DPTX_TRIAL_HPD_WAIT;dptx_trial_observe_until=0;
 assert(!dptx_trial_progress() && dptx_trial_phase==DPTX_TRIAL_HPD_WAIT);
 iboot_finished=true;
 assert(!dptx_trial_progress() && dptx_trial_phase==DPTX_TRIAL_HPD_OFF);
 reset();u8 oversized[257]={0};assert(afk_query_write(oversized,sizeof(oversized))==-EINVAL && !sends);
 puts("PASS: captured PHL 237E7 hint identifies 1920x1080; native status208 and 3-block reply; unplug preserves evidence; malformed names/sizes rejected");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='displayport-afk-query-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(program)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-I', str(kernel),
                    str(kernel / 'm4_dcpext_afk.c'), str(kernel / 'm4_dcpext_dptx.c'), str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
