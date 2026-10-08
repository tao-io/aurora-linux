#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the actual layout guard with the kernel's unit-address name semantics.
Pass drivers/of/base.c from the exact kernel used to build the candidate.
No hardware access.
"""
from pathlib import Path
import subprocess, sys, tempfile
k = Path(__file__).resolve().parents[2]
s = (k/'drivers/spi/spi-apple-qspi.c').read_text()
layout = s[s.index('static bool apple_qspi_nvram_layout('):s.index('static int apple_qspi_probe(')]
of = Path(sys.argv[1]).read_text()
matcher = of[of.index('bool of_node_name_eq('):of.index('EXPORT_SYMBOL(of_node_name_eq);')]
preamble = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "spi-apple-qspi-policy.h"
typedef uint32_t u32;
struct device_node {
 const char *full_name, *compatible, *label;
 u32 reg[2]; int reg_count;
 struct device_node *child, *sibling;
};
static const char *kbasename(const char *p) {const char *q=strrchr(p,'/');return q?q+1:p;}
'''
mock = r'''
static struct device_node *of_get_child_by_name(struct device_node *p,const char *n) {
 for(p=p->child;p;p=p->sibling) if(of_node_name_eq(p,n)) return p;
 return NULL;
}
static void of_node_put(struct device_node *p) {(void)p;}
static bool of_device_is_compatible(struct device_node *p,const char *c) {return !strcmp(p->compatible,c);}
static int of_property_read_u32(struct device_node *p,const char *n,u32 *v) {
 (void)n;if(p->reg_count<1)return -1;*v=p->reg[0];return 0;
}
static int of_property_read_string(struct device_node *p,const char *n,const char **v) {
 (void)n;if(!p->label)return -1;*v=p->label;return 0;
}
static int of_property_count_u32_elems(struct device_node *p,const char *n) {(void)n;return p->reg_count;}
static int of_property_read_u32_array(struct device_node *p,const char *n,u32 *v,int count) {
 (void)n;if(p->reg_count!=count)return -1;memcpy(v,p->reg,count*sizeof(*v));return 0;
}
#define for_each_available_child_of_node(parent,node) for(node=(parent)->child;node;node=node->sibling)
'''
tests = r'''
int main(void) {
 struct device_node part={.full_name="partition@700000",.label="nvram",.reg={0x700000,0x100000},.reg_count=2};
 struct device_node parts={.full_name="partitions",.compatible="fixed-partitions",.child=&part};
 struct device_node flash={.full_name="flash@0",.compatible="jedec,spi-nor",.reg_count=1,.child=&parts};
 struct device_node root={.full_name="spi@2a1118000",.child=&flash};
 assert(of_get_child_by_name(&root,"flash@0")==NULL);
 assert(of_get_child_by_name(&root,"flash")==&flash);
 assert(apple_qspi_nvram_layout(&root));
 flash.reg[0]=1;assert(!apple_qspi_nvram_layout(&root));flash.reg[0]=0;
 flash.compatible="other";assert(!apple_qspi_nvram_layout(&root));flash.compatible="jedec,spi-nor";
 part.reg[0]=0x600000;assert(!apple_qspi_nvram_layout(&root));part.reg[0]=0x700000;
 part.reg[1]=0x200000;assert(!apple_qspi_nvram_layout(&root));part.reg[1]=0x100000;
 part.reg_count=3;assert(!apple_qspi_nvram_layout(&root));part.reg_count=2;
 part.label="other";assert(!apple_qspi_nvram_layout(&root));part.label="nvram";
 parts.compatible="other";assert(!apple_qspi_nvram_layout(&root));parts.compatible="fixed-partitions";
 assert(apple_qspi_nvram_layout(&root));
 puts("PASS: actual layout guard and kernel OF matcher; @0 regression, CS/compatible/label/range/count fences");
}
'''
with tempfile.TemporaryDirectory(prefix='j613-layout-') as d:
 p=Path(d)/'test.c'; p.write_text(preamble+matcher+mock+layout+tests)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(k/'drivers/spi'),str(p),'-o',d+'/test'],check=True)
 subprocess.run([d+'/test'],check=True)
