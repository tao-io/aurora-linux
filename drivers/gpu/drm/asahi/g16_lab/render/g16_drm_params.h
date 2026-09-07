/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef G16_DRM_PARAMS_H
#define G16_DRM_PARAMS_H

/* Aurora appended these two identity fields after the in-tree Asahi prefix. */
struct g16_drm_params_global {
	struct drm_asahi_params_global base;
	u32 usc_generation;
	u32 gpu_hal_generation;
};

static int g16_drm_get_params_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_asahi_get_params *args = data;
	struct g16_drm_params_global params = {
		.base = {
			.gpu_generation = 16, .gpu_variant = 'G', .chip_id = 0x8132,
			.max_frequency_khz = J713_GPU_PSTATE_MAX_FREQUENCY_MHZ * 1000,
			.vm_start = PAGE_SIZE, .vm_end = BIT_ULL(42),
			.vm_kernel_min_size = SZ_32M,
			.max_commands_per_submission = 1, .max_attachments = 0,
			/* Asahi's user timestamp unit is ns. The bootstrap's raw
			 * 24 MHz counters are private; timestamp commands are not
			 * admitted by this submission subset yet.
			 */
			.command_timestamp_frequency_hz = 1000000000,
		},
		.usc_generation = 3, .gpu_hal_generation = 200,
	};

	if (args->pad || args->param_group || args->size > SZ_4K)
		return -EINVAL;
	/* This is a bring-up device, not a general G16 admission predicate. Its
	 * identity comes from the same powered probe as the tested bootstrap.
	 */
	if (!smp_load_acquire(&g16_render_service_ready))
		return -EAGAIN;
	if ((g16_drm_id_version >> 16) != 0x0a02 || !g16_drm_core_mask ||
	    (g16_drm_id_counts & 0xff) != 10 ||
	    ((g16_drm_id_counts >> 8) & 0xff) != 1 ||
	    ((g16_drm_id_counts >> 16) & 0xf) != 1 ||
	    g16_drm_core_mask & ~GENMASK(9, 0))
		return -ENODEV;
	params.base.gpu_revision = (g16_drm_id_version >> 8) & 0xff;
	params.base.num_dies = 1;
	params.base.num_clusters_total = 1;
	params.base.num_cores_per_cluster = 10;
	params.base.core_masks[0] = g16_drm_core_mask;
	return copy_to_user(u64_to_user_ptr(args->pointer), &params,
		min_t(u64, args->size, sizeof(params))) ? -EFAULT : 0;
}
#endif
