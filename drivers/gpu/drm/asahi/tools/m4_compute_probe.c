// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Run only inside the watchdog/thermal wrapper; argv: render node, fixture dir. */
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "asahi_drm.h"

static void checked(int fd, unsigned long op, void *data)
{
    if (ioctl(fd, op, data)) { perror("Asahi ioctl"); abort(); }
}
struct client { int fd; uint32_t vm, queue, sync; volatile uint32_t *output; void *maps[4]; };
static void create(struct client *c, const char *node, const char *dir)
{
    c->fd = open(node, O_RDWR | O_CLOEXEC); assert(c->fd >= 0);
    struct drm_asahi_params_global params = {0};
    struct drm_asahi_get_params query = {.pointer=(uintptr_t)&params, .size=sizeof(params)};
    checked(c->fd, DRM_IOCTL_ASAHI_GET_PARAMS, &query);
    assert(params.chip_id == 0x8132);
    struct drm_asahi_vm_create vm = {.kernel_end=params.vm_end,
        .kernel_start=params.vm_end-params.vm_kernel_min_size};
    checked(c->fd, DRM_IOCTL_ASAHI_VM_CREATE, &vm); c->vm=vm.vm_id;
    struct drm_asahi_queue_create queue = {.vm_id=vm.vm_id,
        .priority=DRM_ASAHI_PRIORITY_MEDIUM, .usc_exec_base=0x10000000000};
    checked(c->fd, DRM_IOCTL_ASAHI_QUEUE_CREATE, &queue); c->queue=queue.queue_id;
    const char *names[] = {"m4-shader.bin", "m4-esl.bin", "m4-cdm.bin"};
    for (unsigned i=0; i<4; i++) {
        struct drm_asahi_gem_create bo = {.size=0x4000, .vm_id=vm.vm_id,
            .flags=DRM_ASAHI_GEM_VM_PRIVATE};
        checked(c->fd, DRM_IOCTL_ASAHI_GEM_CREATE, &bo);
        struct drm_asahi_gem_mmap_offset offset = {.handle=bo.handle};
        checked(c->fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &offset);
        c->maps[i]=mmap(NULL,0x4000,PROT_READ|PROT_WRITE,MAP_SHARED,c->fd,offset.offset);
        assert(c->maps[i]!=MAP_FAILED);
        memset(c->maps[i],0,0x4000);
        if (i<3) {
            char path[1024]; assert(snprintf(path,sizeof(path),"%s/%s",dir,names[i])<(int)sizeof(path));
            FILE *input=fopen(path,"rb"); assert(input);
            assert(fread(c->maps[i],1,0x4000,input)>0); fclose(input);
        } else { c->output=c->maps[i]; *c->output=0xdeadbeef; }
        struct drm_asahi_gem_bind_op op = {.handle=bo.handle, .range=0x4000,
            .addr=0x10000010000+(uint64_t)i*0x10000,
            .flags=DRM_ASAHI_BIND_READ | (i==3?DRM_ASAHI_BIND_WRITE:0)};
        struct drm_asahi_vm_bind bind = {.vm_id=vm.vm_id,.num_binds=1,
            .stride=sizeof(op),.userptr=(uintptr_t)&op};
        checked(c->fd, DRM_IOCTL_ASAHI_VM_BIND, &bind);
    }
    struct drm_syncobj_create sync={0}; checked(c->fd,DRM_IOCTL_SYNCOBJ_CREATE,&sync);c->sync=sync.handle;
    __asm__ volatile("dsb oshst" ::: "memory");
}
static void submit(struct client *c)
{
    struct { struct drm_asahi_cmd_header header; struct drm_asahi_cmd_compute compute; } cmd={
        .header={.cmd_type=DRM_ASAHI_CMD_COMPUTE,.size=sizeof(cmd.compute),
            .vdm_barrier=DRM_ASAHI_BARRIER_NONE,.cdm_barrier=DRM_ASAHI_BARRIER_NONE},
        .compute={.cdm_ctrl_stream_base=0x10000030000,.cdm_ctrl_stream_end=0x10000030030}};
    struct drm_asahi_sync sync={.sync_type=DRM_ASAHI_SYNC_SYNCOBJ,.handle=c->sync};
    struct drm_asahi_submit submit={.syncs=(uintptr_t)&sync,.out_sync_count=1,
        .queue_id=c->queue,.cmdbuf=(uintptr_t)&cmd,.cmdbuf_size=sizeof(cmd)};
    checked(c->fd,DRM_IOCTL_ASAHI_SUBMIT,&submit);
}
static void wait_result(struct client *c)
{
    struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC,&now)==0);
    struct drm_syncobj_wait wait={.handles=(uintptr_t)&c->sync,.count_handles=1,
        .timeout_nsec=(int64_t)now.tv_sec*1000000000+now.tv_nsec+5000000000,
        .flags=DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL};
    checked(c->fd,DRM_IOCTL_SYNCOBJ_WAIT,&wait);
    __asm__ volatile("dmb osh" ::: "memory");
    printf("fd %d VM %u queue %u output=%08x\n",c->fd,c->vm,c->queue,*c->output);
    assert(*c->output==0x44a72000);
}
int main(int argc, char **argv)
{
    assert(argc==3);setbuf(stdout,NULL);
    struct client clients[2]={0};
    for(unsigned i=0;i<2;i++)create(&clients[i],argv[1],argv[2]);
    for(unsigned round=0;round<4;round++) {
        for(unsigned i=0;i<2;i++) {
            *clients[i].output=0xdeadbeef;
            __asm__ volatile("dsb oshst" ::: "memory");
            submit(&clients[i]);
        }
        for(unsigned i=0;i<2;i++) wait_result(&clients[i]);
    }
    for(unsigned i=0;i<2;i++) {
        for(unsigned j=0;j<4;j++)munmap(clients[i].maps[j],0x4000);
        close(clients[i].fd);
    }
    puts("PASS: eight GPU compute submissions, two clients with identical GPU VAs, syncobj waits");
}
