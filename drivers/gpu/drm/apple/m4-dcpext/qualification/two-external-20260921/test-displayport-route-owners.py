#!/usr/bin/env python3
"""Exercise the actual shared PHY arbiter, including concurrent claims."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'linux/linux-asahi/drivers/gpu/drm/apple/m4-dcpext/m4_dcpext_routes.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#define DEFINE_MUTEX(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
'''
checks = r'''
static atomic_int using_phy;
static void *race(void *arg) {
 unsigned owner=(uintptr_t)arg;
 for(unsigned i=0;i<100000;i++) {
  if(!m4_dcpext_route_claim(owner,0))continue;
  assert(atomic_fetch_add(&using_phy,1)==0);
  assert(m4_dcpext_route_claim(owner,0));
  assert(atomic_fetch_sub(&using_phy,1)==1);
  m4_dcpext_route_release(owner,0);
 }
 return 0;
}
int main(void) {
 assert(!m4_dcpext_route_claim(2,0) && !m4_dcpext_route_claim(0,2));
 assert(m4_dcpext_route_claim(0,1));
 assert(!m4_dcpext_route_claim(1,1));
 m4_dcpext_route_release(1,1); /* Foreign release cannot steal a port. */
 assert(!m4_dcpext_route_claim(1,1));
 assert(m4_dcpext_route_claim(0,0)); /* Claim destination before retirement. */
 assert(!m4_dcpext_route_claim(1,0));
 m4_dcpext_route_release(0,1);
 assert(m4_dcpext_route_claim(1,1));
 assert(m4_dcpext_route_claim(0,0));
 m4_dcpext_route_release(1,1);m4_dcpext_route_release(0,0);
 pthread_t a,b;
 assert(!pthread_create(&a,0,race,(void *)0));
 assert(!pthread_create(&b,0,race,(void *)1));
 assert(!pthread_join(a,0) && !pthread_join(b,0));
 assert(route_owners[0]==-1 && route_owners[1]==-1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)/'test.c';p.write_text(shim+source+checks)
    subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-Wno-sign-compare','-fsanitize=address,undefined','-pthread',
                    str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
print('PASS: two controllers cannot own one PHY; migration, foreign release and 200000 concurrent claims')
