// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Run only inside the J713 watchdog/thermal experiment wrapper. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "asahi_drm.h"

static void checked(int fd, unsigned long op, void *data)
{
    if (ioctl(fd, op, data)) {
        perror("Asahi ioctl");
        assert(0);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setbuf(stdout, NULL);
    int fd[2];
    uint32_t *cpu[2];
    struct drm_asahi_params_global params = {0};
    struct drm_asahi_get_params query = {
        .pointer = (uintptr_t)&params, .size = sizeof(params),
    };
    for (unsigned i = 0; i < 2; i++) {
        fd[i] = open(argv[1], O_RDWR | O_CLOEXEC);
        assert(fd[i] >= 0);
        checked(fd[i], DRM_IOCTL_ASAHI_GET_PARAMS, &query);
        assert(params.chip_id == 0x8132 && params.gpu_generation == 16);
        assert(params.usc_generation == 3 && params.gpu_hal_generation == 200);
        struct drm_asahi_vm_create vm = {
            .kernel_end = params.vm_end,
            .kernel_start = params.vm_end - params.vm_kernel_min_size,
        };
        checked(fd[i], DRM_IOCTL_ASAHI_VM_CREATE, &vm);
        struct drm_asahi_gem_create bo = {
            .size = 0x4000, .vm_id = vm.vm_id,
            .flags = DRM_ASAHI_GEM_VM_PRIVATE,
        };
        checked(fd[i], DRM_IOCTL_ASAHI_GEM_CREATE, &bo);
        struct drm_asahi_gem_mmap_offset offset = {.handle = bo.handle};
        checked(fd[i], DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &offset);
        cpu[i] = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd[i], offset.offset);
        assert(cpu[i] != MAP_FAILED);
        cpu[i][0] = 0xabc00000 + i;
        struct drm_asahi_gem_bind_op bind = {
            .flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE,
            .handle = bo.handle, .range = 0x4000, .addr = 0x10000000000,
        };
        struct drm_asahi_vm_bind binds = {
            .vm_id = vm.vm_id, .num_binds = 1, .stride = sizeof(bind),
            .userptr = (uintptr_t)&bind,
        };
        checked(fd[i], DRM_IOCTL_ASAHI_VM_BIND, &binds);
        bind.addr = 0x7000000000;
        assert(ioctl(fd[i], DRM_IOCTL_ASAHI_VM_BIND, &binds) == -1);
        assert(errno == EINVAL);
        printf("client %u: chip=%x mask=%llx VM=%u GEM=%u; driver range rejected\n",
               i, params.chip_id, (unsigned long long)params.core_masks[0], vm.vm_id, bo.handle);
    }
    assert(cpu[0][0] == 0xabc00000 && cpu[1][0] == 0xabc00001);
    /* Hold both files, GPUVMs and CPU mappings across platform teardown. */
    int unbind = open("/sys/bus/platform/drivers/asahi/unbind", O_WRONLY);
    assert(unbind >= 0);
    const char name[] = "302600000.gpu";
    assert(write(unbind, name, strlen(name)) == (ssize_t)strlen(name));
    close(unbind);
    for (unsigned i = 0; i < 2; i++) {
        assert(ioctl(fd[i], DRM_IOCTL_ASAHI_GET_PARAMS, &query) == -1);
        assert(errno == ENODEV);
        assert(cpu[i][0] == 0xabc00000 + i);
        munmap(cpu[i], 0x4000);
        close(fd[i]);
    }
    puts("PASS: common DRM params/GEM/GPUVM, reserved range and open-file teardown");
    return 0;
}
