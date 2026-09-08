#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production reserved-region iterator with stubbed OF translation."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[5]
source = (root / 'drivers/iommu/of_iommu.c').read_text()
start = source.index('void of_iommu_get_resv_regions(')
function = source[start:source.index('\n}', start)+2]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t __be32, u32;
typedef uint64_t phys_addr_t;
struct device_node { int id; } nodes[]={{0},{1},{2},{3}};
struct device { struct device_node *of_node; } dev={&nodes[2]};
struct resource { uint64_t start,end; };
struct list_head { int count; };
struct iommu_resv_region { struct list_head list; } region;
struct of_phandle_iterator { struct device_node *node; };
enum iommu_resv_type { IOMMU_RESV_TRANSLATED };
static __be32 data[20];
static int cells, errors, refs, calls, bad_translate;
#define CONFIG_OF_ADDRESS 1
#define IS_ENABLED(x) (x)
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IOMMU_CACHE 4
#define GFP_KERNEL 0
static void put_node(struct device_node **p) { if (*p) refs--; }
#define __free(x) __attribute__((cleanup(put_node)))
#define of_for_each_phandle(it,err,owner,name,args,n) for ((it)->node=&nodes[0]; (it)->node; (it)->node=NULL)
#define dev_err(...) (errors++)
#define dev_warn(...) (errors++)
static int of_property_present(struct device_node *n,const char *s) { return 1; }
static int of_address_to_resource(struct device_node *n,int i,struct resource *p) { p->start=0x1000;p->end=0x1fff;return 0; }
static const __be32 *of_get_property(struct device_node *n,const char *s,int *size) { *size=cells*4;return data; }
static u32 be32_to_cpup(const __be32 *p) { return *p; }
static struct device_node *of_find_node_by_phandle(u32 id) { if (!id || id>3) return NULL;refs++;return &nodes[id]; }
static int of_dma_is_coherent(struct device_node *n) { return 0; }
static const __be32 *of_translate_dma_region(struct device_node *n,const __be32 *p,phys_addr_t *a,size_t *s) {
    calls++; assert(p>=data && p+4<=data+cells);
    if (bad_translate == n->id) return NULL;
    *a=((uint64_t)p[0]<<32)|p[1];*s=((uint64_t)p[2]<<32)|p[3];return p+4;
}
static enum iommu_resv_type iommu_resv_region_get_type(struct device *d,struct resource *p,phys_addr_t a,size_t s) { return IOMMU_RESV_TRANSLATED; }
static struct iommu_resv_region *iommu_alloc_resv_region_tr(uint64_t p,phys_addr_t a,size_t s,int prot,int type,int flags) { assert(a==0x10001b5c000 && s==0xc0000);return &region; }
static struct iommu_resv_region *iommu_alloc_resv_region(phys_addr_t a,size_t s,int p,int t,int f) { abort(); }
static void list_add_tail(struct list_head *r,struct list_head *l) { l->count++; }
'''
harness += function
harness += r'''
int main(void) {
    /* DCP precedes PIODMA, and a third owner follows it. */
    const __be32 shared[]={1,0x100,0x1b5c000,0,0xc0000, 2,0x100,0x1b5c000,0,0xc0000, 3,0x100,0x1b5c000,0,0xc0000};
    memcpy(data,shared,sizeof(shared));cells=15;
    struct list_head list={0};of_iommu_get_resv_regions(&dev,&list);
    assert(list.count==1 && !errors && calls==3 && refs==0);
    /* Failed translation must stop without dereferencing a NULL cursor. */
    list.count=calls=0;bad_translate=2;of_iommu_get_resv_regions(&dev,&list);
    assert(!list.count && errors==1 && calls==2 && refs==0);
    /* Unknown owners cannot be skipped safely without tuple dimensions. */
    bad_translate=0;data[0]=0x123;errors=calls=0;of_iommu_get_resv_regions(&dev,&list);
    assert(errors==1 && calls==0 && refs==0);
    puts("PASS: shared owners, failed translation, invalid phandle, node references");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp)/'test.c'
    c.write_text(harness)
    subprocess.run(['cc','-std=gnu11','-O1','-fsanitize=address,undefined',str(c),'-o',str(c.with_suffix(''))],check=True)
    subprocess.run([str(c.with_suffix(''))],check=True)
