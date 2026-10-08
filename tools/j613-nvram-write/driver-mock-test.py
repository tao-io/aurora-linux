#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the actual driver admission/modify functions with a fake NOR transport.
No hardware access. The copied block is extracted afresh, never a hand-written model.
"""
from pathlib import Path
import subprocess,tempfile
K=Path(__file__).resolve().parents[2]
s=(K/'drivers/spi/spi-apple-qspi.c').read_text()
setter=s[s.index('static int apple_qspi_set_read_only('):s.index('static int apple_qspi_get_read_only(')]
block=s[s.index('static int apple_qspi_read_register('):s.index('static const struct spi_controller_mem_ops apple_qspi_mem_ops')]
preamble=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "spi-apple-qspi-policy.h"
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define GFP_KERNEL 0
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
#define msecs_to_jiffies(x) (x)
#define time_after_eq(a,b) ((a)>=(b))
#define usleep_range(a,b) (jiffies++)
static char warning[512];
static void mock_dev_err(void *d,const char *f,...) {
 (void)d;va_list ap;va_start(ap,f);vsnprintf(warning,sizeof(warning),f,ap);va_end(ap);
}
#define dev_warn_ratelimited(d,f,...) mock_dev_err((void*)(d),f,__VA_ARGS__)
#define dev_err(d,f,...) mock_dev_err((void*)(d),f,__VA_ARGS__)
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
#define SPI_MEM_DATA_IN 1
#define SPI_MEM_DATA_OUT 2
#define SPI_MEM_OP_CMD(o,w) .cmd={.opcode=(o),.buswidth=(w),.nbytes=1}
#define SPI_MEM_OP_NO_ADDR .addr={0}
#define SPI_MEM_OP_ADDR(n,a,w) .addr={.nbytes=(n),.val=(a),.buswidth=(w)}
#define SPI_MEM_OP_NO_DUMMY .dummy={0}
#define SPI_MEM_OP_NO_DATA .data={0}
#define SPI_MEM_OP_DATA_OUT(n,p,w) .data={.nbytes=(n),.buf.out=(p),.buswidth=(w),.dir=SPI_MEM_DATA_OUT}
#define SPI_MEM_OP_DATA_IN(n,p,w) .data={.nbytes=(n),.buf.in=(p),.buswidth=(w),.dir=SPI_MEM_DATA_IN}
#define SPI_MEM_OP(c,a,d,x) {c,a,d,x}
struct spi_mem_op {
 struct {u8 opcode,buswidth,nbytes;} cmd;
 struct {u64 val;u8 buswidth,nbytes;} addr;
 struct {u8 buswidth,nbytes;} dummy;
 struct {u32 nbytes;u8 buswidth,dir; union {void *in;const void *out;} buf;} data;
};
struct apple_qspi {bool nvram_write_candidate,write_fault;};
struct spi_device {void *controller;int dev;};
struct spi_mem {struct spi_device *spi;};
struct apple_qspi_op_shape {u8 width;};
static struct apple_qspi qspi;
static struct spi_device spi={.controller=&qspi};
static struct spi_mem mem={.spi=&spi};
static bool read_only=true,write_fault_latched,runtime_ready;
struct kernel_param {void *arg;};
static int kstrtobool(const char *v,bool *b) {
 if(!strcmp(v,"0")){*b=false;return 0;}
 if(!strcmp(v,"1")){*b=true;return 0;}
 return -EINVAL;
}
static unsigned long jiffies;
static u8 flash[0x800000],status[3],id[]={0xef,0x65,0x17};
static int wrens,programs,erases,fail_opcode,bad_wel,busy_forever,bad_status,bad_read;
static void *spi_controller_get_devdata(void *c) {return c;}
static bool apple_qspi_opcode_is_read(u8 o) {
 return o==3||o==5||o==0x35||o==0x15||o==0x9f;
}
static bool apple_qspi_supports_op(struct spi_mem *m,const struct spi_mem_op *o) {
 (void)m; return o->cmd.nbytes==1 && o->cmd.buswidth==1 && o->addr.nbytes<=4 && o->data.nbytes<=4096;
}
static int apple_qspi_shape_op(const struct spi_mem_op *o,struct apple_qspi_op_shape *s) {
 s->width=o->data.nbytes?o->data.buswidth:1;return 0;
}
static int apple_qspi_transfer_op(struct spi_mem *m,const struct spi_mem_op *o) {
 (void)m;
 if(o->cmd.opcode==fail_opcode){fail_opcode=-1;return -EIO;}
 switch(o->cmd.opcode) {
 case 0x9f:memcpy(o->data.buf.in,id,3);break;
 case 5:status[0] = busy_forever && (programs||erases)?status[0]|1:status[0]&~1;
  *(u8*)o->data.buf.in=status[0];break;
 case 0x35:*(u8*)o->data.buf.in=status[1]^(bad_status && (programs||erases)?2:0);break;
 case 0x15:*(u8*)o->data.buf.in=status[2];break;
 case 3:assert(o->addr.val+o->data.nbytes<=sizeof(flash));
  memcpy(o->data.buf.in,flash+o->addr.val,o->data.nbytes);
  if(bad_read && (programs||erases))((u8*)o->data.buf.in)[0]^=1;
  break;
 case 6:wrens++;if(!bad_wel)status[0]|=2;break;
 case 4:status[0]&=~2;break;
 case 2:assert(status[0]&2);assert(apple_qspi_mutation_allowed(2,3,o->addr.val,o->data.nbytes,1));
  programs++;for(u32 i=0;i<o->data.nbytes;i++)flash[o->addr.val+i]&=((u8*)o->data.buf.out)[i];
  status[0]&=~2;break;
 case 0x20:assert(status[0]&2);assert(apple_qspi_mutation_allowed(0x20,3,o->addr.val,0,0));
  erases++;memset(flash+o->addr.val,0xff,4096);status[0]&=~2;break;
 default:assert(0);
 }
 return 0;
}
'''
tests=r'''
static void reset(void) {
 memset(flash,0xff,sizeof(flash));memset(status,0,3);
 qspi=(struct apple_qspi){.nvram_write_candidate=true};
 read_only=false;write_fault_latched=false;jiffies=0;
 wrens=programs=erases=bad_wel=busy_forever=bad_status=bad_read=0;fail_opcode=-1;
 id[0]=0xef;
}
int main(void) {
 struct kernel_param kp={.arg=&read_only};
 read_only=true;runtime_ready=false;
 assert(apple_qspi_set_read_only("0",&kp)==-EPERM && read_only);
 assert(!apple_qspi_set_read_only("1",&kp));
 runtime_ready=true;assert(!apple_qspi_set_read_only("0",&kp) && !read_only);
 read_only=true;write_fault_latched=true;
 assert(apple_qspi_set_read_only("0",&kp)==-EPERM && read_only);
 assert(apple_qspi_set_read_only("bad",&kp)==-EINVAL);
 u8 data[256];memset(data,0x55,sizeof(data));
 struct spi_mem_op pp=SPI_MEM_OP(SPI_MEM_OP_CMD(2,1),SPI_MEM_OP_ADDR(3,0x700000,1),SPI_MEM_OP_NO_DUMMY,
  SPI_MEM_OP_DATA_OUT(256,data,1));
 struct spi_mem_op se=SPI_MEM_OP(SPI_MEM_OP_CMD(0x20,1),SPI_MEM_OP_ADDR(3,0x700000,1),SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);
 struct spi_mem_op wren=SPI_MEM_OP(SPI_MEM_OP_CMD(6,1),SPI_MEM_OP_NO_ADDR,SPI_MEM_OP_NO_DUMMY,SPI_MEM_OP_NO_DATA);
 reset();read_only=true;assert(apple_qspi_exec_op(&mem,&pp)==-EACCES);assert(!wrens);
 reset();qspi.nvram_write_candidate=false;assert(apple_qspi_exec_op(&mem,&pp)==-EACCES);assert(!wrens);
 assert(strstr(warning,"reason=layout opcode=02"));
 assert(apple_qspi_exec_op(&mem,&wren)==-EACCES && !wrens);
 assert(strstr(warning,"reason=layout opcode=06 addr=0 addr_bytes=0 len=0 width=1"));
 reset();assert(!apple_qspi_exec_op(&mem,&wren));assert(!wrens);
 for(int op=0;op<256;op++) if(op!=4&&op!=6) {
  reset();wren.cmd.opcode=op;assert(apple_qspi_exec_op(&mem,&wren));assert(!wrens);
 }
 wren.cmd.opcode=6;
 reset();pp.addr.val=0x6fffff;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);
 pp.addr.val=0x700000;
 reset();pp.addr.val=0x7fffff;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);pp.addr.val=0x700000;
 reset();pp.addr.val=0x100700000ULL;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);pp.addr.val=0x700000;
 reset();pp.addr.val=0x700001;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);pp.addr.val=0x700000;
 reset();pp.data.buswidth=4;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);pp.data.buswidth=1;
 reset();status[0]=4;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);
 reset();status[1]=0x40;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);
 reset();status[2]=4;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);
 reset();id[0]=0xc2;assert(apple_qspi_exec_op(&mem,&pp));assert(!wrens);
 reset();flash[0x700000]=0;assert(apple_qspi_exec_op(&mem,&pp)==-EINVAL);assert(!wrens);
 reset();assert(!apple_qspi_exec_op(&mem,&pp));assert(programs==1&&wrens==1&&!status[0]);
 assert(!apple_qspi_exec_op(&mem,&pp));assert(programs==1&&wrens==1);
 assert(!apple_qspi_exec_op(&mem,&se));assert(erases==1&&flash[0x700000]==0xff);
 int faults[]={6,2,4};for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);i++){
  reset();fail_opcode=faults[i];assert(apple_qspi_exec_op(&mem,&pp));
  assert(qspi.write_fault&&write_fault_latched&&read_only);
  read_only=false;int old=wrens;assert(apple_qspi_exec_op(&mem,&pp)==-EACCES);assert(wrens==old);
 }
 reset();bad_wel=1;assert(apple_qspi_exec_op(&mem,&pp));assert(qspi.write_fault);
 reset();busy_forever=1;assert(apple_qspi_exec_op(&mem,&pp)==-ETIMEDOUT);assert(qspi.write_fault);
 reset();bad_status=1;assert(apple_qspi_exec_op(&mem,&pp));assert(qspi.write_fault);
 reset();bad_read=1;assert(apple_qspi_exec_op(&mem,&pp));assert(qspi.write_fault);
 puts("PASS: actual exec/modify functions, virtual WREN, reject-before-WREN, readback, injected faults, no rearm");
}
'''
with tempfile.TemporaryDirectory(prefix='j613-qspi-mock-') as d:
 p=Path(d)/'test.c';p.write_text(preamble+setter+block+tests)
 subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined','-I'+str(K/'drivers/spi'),str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
