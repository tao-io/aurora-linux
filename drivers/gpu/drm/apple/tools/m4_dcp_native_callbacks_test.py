#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay saved callback inputs through the kernel handler under sanitizers."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('trace',type=Path)
a=p.parse_args()
source=(Path(__file__).parent/'rtkit-probe/m4_dcp_native.c').read_text()
declarations=source[source.index('#define A(n)'):source.index('\nstatic int callback(')]
body=source[source.index('static struct m4_dcp_property *property'):source.index('\nstruct m4_dcp_native *m4_dcp_native_start')]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32, __le32;
typedef uint64_t u64;
struct device {};
struct mutex {};
struct m4_dcp_bridge {};
struct m4_dcp_buffer { u64 size,physical,dva;u32 id,flags; } allocations[64];
static u32 alloc_count;
#define M4_DCP_TAG(c,n) (((u32)(c)<<24)|((u32)('0'+(n)/100)<<16)|((u32)('0'+(n)/10%10)<<8)|(u32)('0'+(n)%10))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GFP_KERNEL 0
#define SZ_1M (1024*1024)
#define SZ_8M (8*SZ_1M)
#define U32_MAX UINT32_MAX
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define kvzalloc(n,f) calloc(1,n)
#define kvfree(p) free(p)
static void *kvmemdup(const void *p,size_t n,int f) { void *v=malloc(n);assert(v);memcpy(v,p,n);return v; }
static void strscpy(char *d,const void *s,size_t n) { snprintf(d,n,"%s",(const char *)s); }
static u32 get_unaligned_le32(const void *p) { u32 v;memcpy(&v,p,4);return v; }
static u64 get_unaligned_le64(const void *p) { u64 v;memcpy(&v,p,8);return v; }
static void put_unaligned_le32(u32 v,void *p) { memcpy(p,&v,4); }
static void put_unaligned_le64(u64 v,void *p) { memcpy(p,&v,8); }
static int m4_dcp_bridge_alloc(struct m4_dcp_bridge *b,struct m4_dcp_buffer *v) {
    assert(alloc_count<64 && v->size>0);v->id=++alloc_count;
    v->physical=0x10020000000ULL+v->id*0x100000;v->dva=0x10ff0000000ULL+v->id*0x100000;
    allocations[v->id-1]=*v;return 0;
}
static int m4_dcp_bridge_map(struct m4_dcp_bridge *b,struct m4_dcp_buffer *v) {
    assert(v->id>0 && v->id<=alloc_count);*v=allocations[v->id-1];return 0;
}
static int m4_dcp_bridge_retire(struct m4_dcp_bridge *b,u32 id) { assert(id>0 && id<=alloc_count);return 0; }
'''
harness+=declarations
harness+='\nstatic int simple_call(struct m4_dcp_native *d,u32 t,bool i,bool r,bool e) { return 0; }\n'
harness+=body
harness+=r'''
int main(int argc,char **argv) {
    struct m4_dcp_native *d=calloc(1,sizeof(*d));d->stride=10240;
    u8 invalid_output[4]={0};
    assert(callback(NULL,d,0x4431303a,NULL,0,invalid_output,4)<0);
    FILE *f=fopen(argv[1],"rb");assert(f);u32 h[3],cases=0;
    while (fread(h,sizeof(h),1,f)==1) {
        u8 *in=malloc(h[1]+1),*out=calloc(1,h[2]+1);assert(in && out);
        assert(fread(in,1,h[1],f)==h[1]);
        /* A wrong shape must fail before using or mutating callback state. */
        assert(callback(NULL,d,h[0],in,h[1]+1,out,h[2])<0);
        assert(callback(NULL,d,h[0],in,h[1],out,h[2]+1)<0);
        int ret=callback(NULL,d,h[0],in,h[1],out,h[2]);
        if (ret) { fprintf(stderr,"callback %#x failed: %d\n",h[0],ret);return 1; }
        if (h[0]==D(451)) assert(get_unaligned_le32(out+24)==alloc_count);
        if (h[0]==D(201)) assert(get_unaligned_le32(out)==get_unaligned_le64(in));
        if (h[0]==D(582)) assert(out[0]==0);
        if (h[0]==D(3)) assert(get_unaligned_le32(out+56)==1);
        free(in);free(out);cases++;
    }
    assert(d->property_count>10 && d->raw_count>=4 && alloc_count==3 && !d->chunk);
    for (u32 i=0;i<d->raw_count;i++) free(d->raw[i].data);
    free(d);fclose(f);
    printf("PASS: %u captured M4 callbacks and %u wrong-shape rejections\n",cases,cases*2);
}
'''
with tempfile.TemporaryDirectory() as temp:
    root=Path(temp);c=root/'test.c';c.write_text(harness)
    with (root/'inputs.bin').open('wb') as f:
        for line in a.trace.read_text().splitlines():
            e=json.loads(line)
            if e.get('event')!='callback':continue
            data=bytes.fromhex(e['input'])
            f.write(struct.pack('<III',int.from_bytes(e['tag'].encode(),'big'),len(data),e['output_size'])+data)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test'),str(root/'inputs.bin')],check=True)
