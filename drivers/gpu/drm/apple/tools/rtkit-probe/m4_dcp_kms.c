// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* J713 native SID0 scanout. Explicit-load, fixed inherited panel mode only. */
#include <linux/dma-mapping.h>
#include <linux/aperture.h>
#include <linux/of_address.h>
#include <linux/dma-buf.h>
#include <linux/debugfs.h>
#include <linux/iosys-map.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include "m4_dcp_native.h"
#include "m4_dcp_kms.h"

/* Diagnostic history capacity, unrelated to submission or buffering limits. */
#define M4_PRESENT_HISTORY 256
struct m4_present_record {
	u64 sequence, submitted_ns, completed_ns;
	u32 framebuffer;
	s32 status;
};

struct m4_kms {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_display_mode mode;
	struct m4_dcp_native *native;
	/* Keep scanout pinned independently of atomic state until real completion. */
	struct drm_framebuffer *active_fb, *failed_fb;
	struct mutex present_lock;
	bool failed;
	u64 completed;
	u64 swap_total_ns, swap_max_ns;
	spinlock_t history_lock;
	u64 history_count;
	struct m4_present_record history[M4_PRESENT_HISTORY];
};

static struct m4_kms *to_m4(struct drm_device *drm)
{
	return container_of(drm, struct m4_kms, drm);
}

static enum drm_mode_status m4_mode_valid(struct drm_crtc *crtc,
					const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &to_m4(crtc->dev)->mode) ? MODE_OK : MODE_BAD;
}

static int m4_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *c;
	struct drm_gem_dma_object *obj;
	u64 bytes;
	int ret;

	if (READ_ONCE(to_m4(plane->dev)->failed))
		return -EIO;
	if (!p->crtc)
		return 0;
	c = drm_atomic_get_crtc_state(state, p->crtc);
	if (IS_ERR(c))
		return PTR_ERR(c);
	ret = drm_atomic_helper_check_plane_state(p, c, DRM_PLANE_NO_SCALING,
						DRM_PLANE_NO_SCALING, false, true);
	if (ret || !p->visible)
		return ret;
	/* Only the qualified full-panel, uncompressed, single-plane layout. */
	if (p->src_x || p->src_y || p->crtc_x || p->crtc_y ||
	    p->src_w != 2560U << 16 || p->src_h != 1664U << 16 ||
	    p->crtc_w != 2560 || p->crtc_h != 1664 ||
	    p->fb->width != 2560 || p->fb->height != 1664 ||
	    p->fb->modifier != DRM_FORMAT_MOD_LINEAR || p->fb->offsets[0] ||
	    p->fb->pitches[0] < 10240 || (p->fb->pitches[0] & 63))
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(p->fb, 0);
	bytes = (u64)p->fb->pitches[0] * p->fb->height;
	if (!obj || bytes > obj->base.size || bytes > U32_MAX || !obj->dma_addr)
		return -EINVAL;
	return 0;
}

static void m4_plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	/* One RPC contains the complete atomic state, submitted by CRTC flush. */
}

static const struct drm_plane_helper_funcs m4_plane_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = m4_plane_check,
	.atomic_update = m4_plane_update,
	.atomic_disable = m4_plane_update,
};

static const struct drm_plane_funcs m4_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static void m4_surface(u8 *s, struct drm_framebuffer *fb)
{
	u32 stride = fb->pitches[0], bytes = stride * fb->height;

	memset(s, 0, 0x22c);
	/* DCP has no XRGB8. Premultiplied RGB over black preserves XRGB pixels. */
	s[2] = fb->format->format == DRM_FORMAT_XRGB8888;
	put_unaligned_le32(1, s + 3);
	put_unaligned_le32(1, s + 7);
	put_unaligned_le32(0x42475241, s + 0xb);
	s[0x13] = 13;
	s[0x14] = 12;
	put_unaligned_le32(stride, s + 0x15);
	put_unaligned_le16(1, s + 0x19);
	s[0x1b] = s[0x1c] = 1;
	put_unaligned_le32(fb->width, s + 0x21);
	put_unaligned_le32(fb->height, s + 0x25);
	put_unaligned_le32(bytes, s + 0x29);
	put_unaligned_le32(1, s + 0x35);
	put_unaligned_le64(1, s + 0x51);
	put_unaligned_le32(fb->width, s + 0x59);
	put_unaligned_le32(fb->height, s + 0x5d);
	put_unaligned_le32(stride, s + 0x69);
	put_unaligned_le32(bytes, s + 0x6d);
	put_unaligned_le16(4, s + 0x71);
	s[0x73] = s[0x74] = 1;
	put_unaligned_le64(1, s + 0x149);
}

static void m4_finish_event(struct drm_crtc *crtc, struct drm_atomic_state *state, bool success)
{
	struct drm_crtc_state *c = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event = c->event;
	unsigned long flags;

	c->event = NULL;
	if (!event)
		return;
	if (!success) {
		/* Release helper waiters without reporting a successful page flip. */
		if (event->base.fence) {
			dma_fence_set_error(event->base.fence, -EIO);
			dma_fence_signal(event->base.fence);
		}
		if (event->base.completion) {
			complete_all(event->base.completion);
			event->base.completion_release(event->base.completion);
		}
		drm_event_cancel_free(crtc->dev, &event->base);
		return;
	}
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static bool m4_present(struct m4_kms *m4, struct drm_framebuffer *fb)
{
	u8 surface[0x22c];
	dma_addr_t dva = 0;
	u64 start, elapsed;
	int ret;

	mutex_lock(&m4->present_lock);
	if (m4->failed) {
		mutex_unlock(&m4->present_lock);
		return false;
	}
	if (fb) {
		drm_framebuffer_get(fb);
		m4_surface(surface, fb);
		dva = drm_fb_dma_get_gem_obj(fb, 0)->dma_addr;
	}
	start = ktime_get_ns();
	ret = m4_dcp_native_swap(m4->native, fb ? surface : NULL, dva,
				fb ? fb->width : 0, fb ? fb->height : 0);
	elapsed = ktime_get_ns() - start;
	/* Never make a timing reader wait on present_lock or copy scanout pixels. */
	spin_lock(&m4->history_lock);
	m4->history[m4->history_count % M4_PRESENT_HISTORY] =
		(struct m4_present_record) {
			.sequence = m4->history_count + 1,
			.submitted_ns = start,
			.completed_ns = start + elapsed,
			.framebuffer = fb ? fb->base.id : 0,
			.status = ret,
		};
	m4->history_count++;
	spin_unlock(&m4->history_lock);
	if (ret) {
		/* Either buffer may still be scanned out. Never unmap them on error. */
		m4->failed_fb = fb;
		WRITE_ONCE(m4->failed, true);
		drm_err(&m4->drm, "native DCP flip failed %d; buffers pinned until reboot\n", ret);
		mutex_unlock(&m4->present_lock);
		return false;
	}
	if (m4->active_fb)
		drm_framebuffer_put(m4->active_fb);
	m4->active_fb = fb;
	m4->completed++;
	m4->swap_total_ns += elapsed;
	m4->swap_max_ns = max(m4->swap_max_ns, elapsed);
	if (m4->completed <= 3 || !(m4->completed % 600))
		drm_info(&m4->drm, "native KMS completed=%llu surface=%d imported=%d\n",
			 m4->completed, !!fb, fb && !!fb->obj[0]->import_attach);
	mutex_unlock(&m4->present_lock);
	return true;
}

static void m4_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct m4_kms *m4 = to_m4(crtc->dev);
	bool success;

	if (!drm_atomic_get_new_crtc_state(state, crtc)->active)
		return;
	success = m4_present(m4, drm_atomic_get_new_plane_state(state, &m4->plane)->fb);
	if (success)
		drm_crtc_handle_vblank(crtc);
	m4_finish_event(crtc, state, success);
}

static void m4_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	const struct drm_vblank_crtc_config config = { .offdelay_ms = 0 };

	drm_crtc_vblank_on_config(crtc, &config);
}

static void m4_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	bool success = m4_present(to_m4(crtc->dev), NULL);

	if (success)
		drm_crtc_handle_vblank(crtc);
	m4_finish_event(crtc, state, success);
	drm_crtc_vblank_off(crtc);
}

static int m4_enable_vblank(struct drm_crtc *crtc) { return 0; }
static void m4_disable_vblank(struct drm_crtc *crtc) { }

static const struct drm_crtc_helper_funcs m4_crtc_helpers = {
	.mode_valid = m4_mode_valid,
	.atomic_enable = m4_crtc_enable,
	.atomic_disable = m4_crtc_disable,
	.atomic_flush = m4_crtc_flush,
};

static const struct drm_crtc_funcs m4_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = m4_enable_vblank,
	.disable_vblank = m4_disable_vblank,
};

static int m4_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev,
							 &to_m4(connector->dev)->mode);
	if (!mode)
		return 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static enum drm_connector_status m4_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_helper_funcs m4_connector_helpers = {
	.get_modes = m4_get_modes,
};

static const struct drm_connector_funcs m4_connector_funcs = {
	.detect = m4_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_encoder_funcs m4_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int m4_atomic_check(struct drm_device *drm, struct drm_atomic_state *state)
{
	struct m4_kms *m4 = to_m4(drm);
	struct drm_crtc_state *c;
	int ret;

	if (READ_ONCE(m4->failed))
		return -EIO;
	c = drm_atomic_get_crtc_state(state, &m4->crtc);
	if (IS_ERR(c))
		return PTR_ERR(c);
	ret = drm_atomic_add_affected_planes(state, &m4->crtc);
	return ret ?: drm_atomic_helper_check(drm, state);
}

static void m4_commit_tail(struct drm_atomic_state *state)
{
	/* Each flush waits for its matching D590 before releasing the previous FB.
	 * Waiting for an additional vblank here would stall an idle display.
	 */
	drm_atomic_helper_commit_modeset_disables(state->dev, state);
	drm_atomic_helper_commit_modeset_enables(state->dev, state);
	drm_atomic_helper_commit_planes(state->dev, state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_cleanup_planes(state->dev, state);
}

static const struct drm_mode_config_funcs m4_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = m4_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};
static const struct drm_mode_config_helper_funcs m4_mode_helpers = {
	.atomic_commit_tail = m4_commit_tail,
};

static int m4_dumb_create(struct drm_file *file, struct drm_device *drm,
			  struct drm_mode_create_dumb *args)
{
	if (args->bpp != 32 || !args->width || args->width > 2560 ||
	    !args->height || args->height > 1664)
		return -EINVAL;
	args->pitch = ALIGN(args->width * 4, 64);
	args->size = PAGE_ALIGN((u64)args->pitch * args->height);
	return drm_gem_dma_dumb_create_internal(file, drm, args);
}

DEFINE_DRM_GEM_DMA_FOPS(m4_fops);
static const struct drm_driver m4_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(m4_dumb_create),
	.fops = &m4_fops,
	.name = "m4-dcp",
	.desc = "J713 native DCP scanout",
	.major = 0,
	.minor = 1,
};


static int m4_status(struct seq_file *seq, void *unused)
{
	struct drm_debugfs_entry *entry = seq->private;
	struct m4_kms *m4 = to_m4(entry->dev);
	struct drm_framebuffer *fb;

	mutex_lock(&m4->present_lock);
	fb = m4->active_fb;
	seq_printf(seq, "completed %llu\nfailed %u\nactive_fb %u\nimported %u\n",
		   m4->completed, m4->failed, fb ? fb->base.id : 0,
		   fb && !!fb->obj[0]->import_attach);
	seq_printf(seq, "swap_total_us %llu\nswap_max_us %llu\n",
		   m4->swap_total_ns / 1000, m4->swap_max_ns / 1000);
	mutex_unlock(&m4->present_lock);
	return 0;
}

static int m4_analytics(struct seq_file *seq, void *unused)
{
	struct drm_debugfs_entry *entry = seq->private;
	struct m4_kms *m4 = to_m4(entry->dev);

	m4_dcp_native_analytics_show(m4->native, seq);
	return 0;
}

static int m4_present_history(struct seq_file *seq, void *unused)
{
	struct drm_debugfs_entry *entry = seq->private;
	struct m4_kms *m4 = to_m4(entry->dev);
	struct m4_present_record *records;
	u64 count, first, i;

	records = kmalloc(sizeof(m4->history), GFP_KERNEL);
	if (!records)
		return -ENOMEM;
	spin_lock(&m4->history_lock);
	count = m4->history_count;
	memcpy(records, m4->history, sizeof(m4->history));
	spin_unlock(&m4->history_lock);
	seq_puts(seq, "sequence submitted_ns completed_ns framebuffer status\n");
	first = count > M4_PRESENT_HISTORY ? count - M4_PRESENT_HISTORY : 0;
	for (i = first; i < count; i++) {
		struct m4_present_record *r = &records[i % M4_PRESENT_HISTORY];

		seq_printf(seq, "%llu %llu %llu %u %d\n", r->sequence,
			   r->submitted_ns, r->completed_ns, r->framebuffer, r->status);
	}
	kfree(records);
	return 0;
}

struct m4_snapshot {
	size_t size;
	u8 bytes[];
};

static int m4_snapshot_open(struct inode *inode, struct file *file)
{
	struct m4_kms *m4 = inode->i_private;
	struct m4_snapshot *snapshot;
	struct drm_framebuffer *fb;
	struct dma_buf *dmabuf = NULL;
	struct iosys_map map = {};
	u32 bytes;
	int ret;

	ret = mutex_lock_interruptible(&m4->present_lock);
	if (ret)
		return ret;
	fb = m4->active_fb;
	if (m4->failed || !fb) {
		ret = -ENODATA;
		goto unlock;
	}
	bytes = fb->pitches[0] * fb->height;
	if (bytes > SZ_32M) {
		ret = -E2BIG;
		goto unlock;
	}
	snapshot = kvmalloc(sizeof(*snapshot) + 32 + bytes, GFP_KERNEL);
	if (!snapshot) {
		ret = -ENOMEM;
		goto unlock;
	}
	/* Only this root-only diagnostic maps or reads imported GPU pixels. */
	if (fb->obj[0]->import_attach) {
		dmabuf = fb->obj[0]->import_attach->dmabuf;
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
		if (ret)
			goto free;
		ret = dma_buf_vmap_unlocked(dmabuf, &map);
		if (ret) {
			dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
			goto free;
		}
	} else {
		ret = drm_gem_dma_vmap(drm_fb_dma_get_gem_obj(fb, 0), &map);
		if (ret)
			goto free;
	}
	snapshot->size = 32 + bytes;
	put_unaligned_le32(0x4246344d, snapshot->bytes); /* M4FB */
	put_unaligned_le32(fb->width, snapshot->bytes + 4);
	put_unaligned_le32(fb->height, snapshot->bytes + 8);
	put_unaligned_le32(fb->pitches[0], snapshot->bytes + 12);
	put_unaligned_le32(fb->format->format, snapshot->bytes + 16);
	put_unaligned_le32(fb->base.id, snapshot->bytes + 20);
	put_unaligned_le32(!!dmabuf, snapshot->bytes + 24);
	put_unaligned_le32(bytes, snapshot->bytes + 28);
	iosys_map_memcpy_from(snapshot->bytes + 32, &map, 0, bytes);
	if (dmabuf) {
		dma_buf_vunmap_unlocked(dmabuf, &map);
		ret = dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
		if (ret)
			goto free;
	}
	file->private_data = snapshot;
	mutex_unlock(&m4->present_lock);
	return 0;
free:
	kvfree(snapshot);
unlock:
	mutex_unlock(&m4->present_lock);
	return ret;
}

static ssize_t m4_snapshot_read(struct file *file, char __user *data, size_t size, loff_t *pos)
{
	struct m4_snapshot *snapshot = file->private_data;

	return simple_read_from_buffer(data, size, pos, snapshot->bytes, snapshot->size);
}

static int m4_snapshot_release(struct inode *inode, struct file *file)
{
	kvfree(file->private_data);
	return 0;
}

static const struct file_operations m4_snapshot_fops = {
	.owner = THIS_MODULE,
	.open = m4_snapshot_open,
	.read = m4_snapshot_read,
	.release = m4_snapshot_release,
	.llseek = default_llseek,
};

int m4_dcp_kms_register(struct m4_dcp_native *native, bool takeover)
{
	static const u32 formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
	static const u64 modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct device_node *node;
	struct platform_device *pdev;
	struct m4_kms *m4;
	struct drm_device *drm;
	const void *timing;
	u32 size;
	int ret;

	node = of_find_node_by_path("disp0");
	if (!node || !of_device_is_available(node) ||
	    !of_device_is_compatible(node, "apple,t8132-display-diagnostics") ||
	    !of_property_read_bool(node, "apple,j713-inherited-mappings")) {
		of_node_put(node);
		return -ENODEV;
	}
	pdev = of_find_device_by_node(node);
	if (!pdev) {
		of_node_put(node);
		return -ENODEV;
	}
	/* Native client and its device mappings remain pinned until reboot. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(42));
	if (!ret)
		ret = of_dma_configure(&pdev->dev, node, true);
	of_node_put(node);
	if (ret)
		return ret;
	m4 = devm_drm_dev_alloc(&pdev->dev, &m4_driver, struct m4_kms, drm);
	if (IS_ERR(m4))
		return PTR_ERR(m4);
	drm = &m4->drm;
	m4->native = native;
	mutex_init(&m4->present_lock);
	spin_lock_init(&m4->history_lock);
	timing = m4_dcp_native_property(native, "PreferredTimingElements", &size);
	if (!timing)
		return -ENODATA;
	ret = m4_dcp_preferred_mode(timing, size, &m4->mode);
	kfree(timing);
	if (ret)
		return ret;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = drm->mode_config.max_width = 2560;
	drm->mode_config.min_height = drm->mode_config.max_height = 1664;
	drm->mode_config.funcs = &m4_mode_funcs;
	drm->mode_config.helper_private = &m4_mode_helpers;
	ret = drm_universal_plane_init(drm, &m4->plane, 1, &m4_plane_funcs, formats,
				       ARRAY_SIZE(formats), modifiers, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&m4->plane, &m4_plane_helpers);
	ret = drm_crtc_init_with_planes(drm, &m4->crtc, &m4->plane, NULL, &m4_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&m4->crtc, &m4_crtc_helpers);
	ret = drm_encoder_init(drm, &m4->encoder, &m4_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	m4->encoder.possible_crtcs = 1;
	ret = drm_connector_init(drm, &m4->connector, &m4_connector_funcs, DRM_MODE_CONNECTOR_eDP);
	if (ret)
		return ret;
	drm_connector_helper_add(&m4->connector, &m4_connector_helpers);
	ret = drm_connector_attach_encoder(&m4->connector, &m4->encoder);
	if (ret)
		return ret;
	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	if (takeover) {
		struct resource fb;

		node = of_find_compatible_node(NULL, NULL, "simple-framebuffer");
		ret = node ? of_address_to_resource(node, 0, &fb) : -ENODEV;
		of_node_put(node);
		if (ret)
			return ret;
		ret = aperture_remove_conflicting_devices(fb.start, resource_size(&fb), m4_driver.name);
		if (ret)
			return ret;
	}
	drm_debugfs_add_file(drm, "m4_status", m4_status, NULL);
	drm_debugfs_add_file(drm, "m4_analytics", m4_analytics, NULL);
	drm_debugfs_add_file(drm, "m4_present_history", m4_present_history, NULL);
	ret = drm_dev_register(drm, 0);
	if (!ret)
		debugfs_create_file("m4_scanout", 0400, drm->debugfs_root, m4, &m4_snapshot_fops);
	if (!ret)
		drm_info(drm, "M4 native KMS ready: %ux%u at %d Hz, SID0 DMA scanout\n",
			 m4->mode.hdisplay, m4->mode.vdisplay, drm_mode_vrefresh(&m4->mode));
	return ret;
}

MODULE_IMPORT_NS("DMA_BUF");
