/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Private scanout mappings in SYS SID23 and FE SID0. DART encodings/flush
 * ordering follow Asahi io-pgtable-dart.c. Ordinary pages are contiguous only
 * in DVA space; streaming DMA synchronization brackets each CPU update.
 * Firmware quiescence is required before mappings or allocations are freed.
 */
#include <linux/vmalloc.h>
#include "m4_dcpext_modes.h"
/* Keep the bootstrap image at 1080p; DRM may select any admitted larger mode. */
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_STRIDE (FRAME_WIDTH * 4)
#define FRAME_BYTES ALIGN(M4_DCPEXT_MAX_PITCH * M4_DCPEXT_MAX_HEIGHT + 24 * LEASE_PAGE, LEASE_PAGE)
#define FRAME_PAGES (FRAME_BYTES / LEASE_PAGE)
#define FRAME_PTES (LEASE_PAGE / sizeof(u64))
/* The communication arena retains its old slot. Scanout owns separate slots,
 * including the leaf boundaries inside a 4K buffer and between two buffers. */
#define FRAME_DVA (LEASE_DVA + 0x2000000)
#define FRAME_SLOT (FRAME_DVA >> 25)
#define FRAME_MAX_TABLES DIV_ROUND_UP(2 * FRAME_PAGES, FRAME_PTES)

static bool frame_map;
static unsigned int frame_buffers = 1;
module_param(frame_buffers, uint, 0400);
module_param(frame_map, bool, 0400);
MODULE_PARM_DESC(frame_map, "Map private up-to-4K scanout pages in SYS SID23 and FE SID0");
static void *frame_pixels, *frame_pixels_b, *frame_ram;
static dma_addr_t *frame_dma[2];
static u32 frame_mapped[2], frame_tables;
static u64 *frame_leaves[FRAME_MAX_TABLES];
static u8 *frame_saved;
static u64 *frame_root;
static void __iomem *frame_regs;
static struct resource frame_reservation;
static u32 frame_ttbr, frame_tcr, frame_protect;
static bool frame_published;
static int flush_sid(void);

static void frame_cleanup(void)
{
	if (frame_ram)
		memunmap(frame_ram);
	kfree(frame_saved);
	for (u32 b = 0; b < 2; b++) {
		for (u32 i = 0; i < frame_mapped[b]; i++)
			dma_unmap_page(dma_dev, frame_dma[b][i], LEASE_PAGE, DMA_TO_DEVICE);
		kfree(frame_dma[b]);
	}
	vfree(frame_pixels);
	vfree(frame_pixels_b);
	for (u32 i = 0; i < frame_tables; i++)
		if (frame_leaves[i])
			free_page((unsigned long)frame_leaves[i]);
}

static int frame_flush(void)
{
	u32 status;
	dma_wmb();
	writel(0x100, frame_regs + 0x80); /* SID0 only; SID16 untouched. */
	return readl_poll_timeout(frame_regs + 0x80, status, !(status & BIT(31)), 1, 10000);
}

static bool frame_registers_match(void)
{
	return readl(frame_regs + 0x1400) == frame_ttbr &&
		readl(frame_regs + 0x1000) == frame_tcr &&
		readl(frame_regs + 0x200) == frame_protect && !readl(frame_regs + 0x100);
}

/* Caller already checked the extent against FRAME_BYTES and owns the slot
 * through a matching native swap completion. TO_DEVICE mappings are read-only
 * for scanout; explicit synchronization is still required on a noncoherent host. */
static void frame_sync(unsigned int slot, size_t bytes, bool for_cpu)
{
	for (u32 i = 0; i < DIV_ROUND_UP(bytes, LEASE_PAGE); i++) {
		if (for_cpu)
			dma_sync_single_for_cpu(dma_dev, frame_dma[slot][i], LEASE_PAGE, DMA_TO_DEVICE);
		else
			dma_sync_single_for_device(dma_dev, frame_dma[slot][i], LEASE_PAGE, DMA_TO_DEVICE);
	}
}

static int frame_set_page(u32 index, dma_addr_t address)
{
	u64 *entry;

	if (index >= frame_buffers * FRAME_PAGES ||
	    (address & (LEASE_PAGE - 1)) || address >= BIT_ULL(42))
		return -EINVAL;
	entry = &frame_leaves[index / FRAME_PTES][index % FRAME_PTES];
	if (*entry)
		return -EBUSY;
	*entry = encode_address(address) | PTE_RW;
	return 0;
}

static int frame_prepare(struct device_node *dcp, int count)
{
	struct device_node *ram;
	phys_addr_t root_pa, leaf_pa;
	int ret = -EINVAL;
	if (!frame_map)
		return 0;
	if (frame_buffers < 1 || frame_buffers > 2 ||
	    FRAME_SLOT + FRAME_MAX_TABLES > LEASE_PAGE / sizeof(u64))
		return -EINVAL;
	/* Identity is already restricted to J713/T8132, qualified firmware,
	 * disabled external DCP, exact reservation count and powered FE/SYS. */
	ram = of_parse_phandle(dcp, "memory-region", count - 2);
	if (!ram || !of_device_is_compatible(ram, "apple,dart-mem") ||
	    !of_property_read_bool(ram, "no-map") ||
	    of_address_to_resource(ram, 0, &frame_reservation))
		goto out;
	if (resource_size(&frame_reservation) > 0x100000 ||
	    resource_size(&frame_reservation) < LEASE_PAGE ||
	    (frame_reservation.start | resource_size(&frame_reservation)) & (LEASE_PAGE - 1))
		goto out;
	ret = fe_prepare();
	if (ret)
		goto out;
	frame_regs = fe_regs;
	frame_ttbr = readl(frame_regs + 0x1400);
	frame_tcr = readl(frame_regs + 0x1000);
	frame_protect = readl(frame_regs + 0x200);
	root_pa = (u64)((frame_ttbr & GENMASK(29, 2)) >> 2) << 14;
	ret = -EINVAL;
	if (!(frame_protect & 1) || frame_tcr != 1 || !(frame_ttbr & 1) ||
	    root_pa < frame_reservation.start || root_pa > frame_reservation.end ||
	    frame_reservation.end - root_pa + 1 < LEASE_PAGE || readl(frame_regs + 0x100))
		goto out;
	frame_ram = memremap(frame_reservation.start, resource_size(&frame_reservation), MEMREMAP_WB);
	ret = -ENOMEM;
	if (!frame_ram)
		goto out;
	frame_root = frame_ram + root_pa - frame_reservation.start;
	frame_saved = kmemdup(frame_ram, resource_size(&frame_reservation), GFP_KERNEL);
	if (!frame_saved)
		goto out;
	frame_tables = DIV_ROUND_UP(frame_buffers * FRAME_PAGES, FRAME_PTES);
	for (u32 i = 0; i < frame_tables; i++) {
		if (READ_ONCE(frame_root[FRAME_SLOT + i]) || READ_ONCE(root[FRAME_SLOT + i])) {
			ret = -EBUSY;
			goto out;
		}
		frame_leaves[i] = (void *)get_zeroed_page(GFP_KERNEL);
		ret = -ENOMEM;
		if (!frame_leaves[i])
			goto out;
		leaf_pa = virt_to_phys(frame_leaves[i]);
		if ((leaf_pa & (LEASE_PAGE - 1)) || leaf_pa >= BIT_ULL(42)) {
			ret = -EINVAL;
			goto out;
		}
	}
	/* Scanout need only be contiguous in DVA space. Avoid 32+ MiB CMA
	 * allocations; each ordinary page gets an explicit DMA ownership map. */
	frame_pixels = vzalloc(FRAME_BYTES);
	if (frame_buffers == 2)
		frame_pixels_b = vzalloc(FRAME_BYTES);
	ret = -ENOMEM;
	if (!frame_pixels || (frame_buffers == 2 && !frame_pixels_b))
		goto out;
	/* Opaque BGRA vertical bars with a white border. Padding remains zero. */
	for (u32 y = 0; y < FRAME_HEIGHT; y++)
		for (u32 x = 0; x < FRAME_WIDTH; x++) {
			static const u32 bars[] = { 0xffff0000, 0xff00ff00, 0xff0000ff,
				0xffffff00, 0xffff00ff, 0xff00ffff, 0xffffffff, 0xff202020 };
			((u32 *)frame_pixels)[y * FRAME_WIDTH + x] =
				x < 8 || y < 8 || x >= FRAME_WIDTH - 8 || y >= FRAME_HEIGHT - 8 ?
				0xffffffff : bars[x * 8 / FRAME_WIDTH];
			if (frame_pixels_b)
				((u32 *)frame_pixels_b)[y * FRAME_WIDTH + x] =
					((u32 *)frame_pixels)[y * FRAME_WIDTH + x] ^ 0x00ffffff;
		}
	for (u32 b = 0; b < frame_buffers; b++) {
		void *pixels = b ? frame_pixels_b : frame_pixels;

		frame_dma[b] = kcalloc(FRAME_PAGES, sizeof(*frame_dma[b]), GFP_KERNEL);
		if (!frame_dma[b]) {
			ret = -ENOMEM;
			goto out;
		}
		for (u32 i = 0; i < FRAME_PAGES; i++) {
			u32 index = b * FRAME_PAGES + i;
			dma_addr_t address = dma_map_page(dma_dev,
				vmalloc_to_page(pixels + i * LEASE_PAGE), 0,
				LEASE_PAGE, DMA_TO_DEVICE);

			if (dma_mapping_error(dma_dev, address)) {
				ret = -ENOMEM;
				goto out;
			}
			frame_dma[b][i] = address;
			frame_mapped[b]++;
			if ((address & (LEASE_PAGE - 1)) || address >= BIT_ULL(42) ||
			    dma_to_phys(dma_dev, address) != address) {
				ret = -EINVAL;
				goto out;
			}
			ret = frame_set_page(index, address);
			if (ret)
				goto out;
		}
	}
	ret = frame_registers_match() &&
		!memcmp(frame_ram, frame_saved, resource_size(&frame_reservation)) ? 0 : -EAGAIN;
out:
	of_node_put(ram);
	return ret;
}

static int frame_publish(void)
{
	int ret;
	if (!frame_map)
		return 0;
	if (!pinned || !frame_registers_match() ||
	    memcmp(frame_ram, frame_saved, resource_size(&frame_reservation)))
		return -EAGAIN;
	for (u32 i = 0; i < frame_tables; i++)
		if (READ_ONCE(root[FRAME_SLOT + i]) || READ_ONCE(frame_root[FRAME_SLOT + i]))
			return -EAGAIN;
	dma_wmb();
	/* Retain all resources after even a partial publication or failed flush. */
	frame_published = true;
	for (u32 i = 0; i < frame_tables; i++) {
		u64 entry = encode_address(virt_to_phys(frame_leaves[i])) | 1;

		if (cmpxchg64_relaxed(&root[FRAME_SLOT + i], 0, entry) ||
		    cmpxchg64_relaxed(&frame_root[FRAME_SLOT + i], 0, entry))
			return -EAGAIN;
	}
	pr_info("m4_dcpext_frame: SYS23/FE0 DVA=%#llx bytes=%#x buffers=%u leaves=%u (paged scanout)\n",
		FRAME_DVA, FRAME_BYTES, frame_buffers, frame_tables);
	ret = flush_sid();
	return ret ? ret : frame_flush();
}

static int frame_remove(void)
{
	int ret;
	if (!frame_map)
		return 0;
	if (!frame_published || !frame_registers_match())
		return -EUCLEAN;
	/* Check the entire ownership set before removing any entry. */
	for (u32 i = 0; i < frame_tables; i++) {
		u64 entry = encode_address(virt_to_phys(frame_leaves[i])) | 1;

		if (READ_ONCE(root[FRAME_SLOT + i]) != entry ||
		    READ_ONCE(frame_root[FRAME_SLOT + i]) != entry)
			return -EAGAIN;
	}
	for (u32 i = 0; i < frame_tables; i++) {
		u64 entry = encode_address(virt_to_phys(frame_leaves[i])) | 1;

		if (cmpxchg64_relaxed(&root[FRAME_SLOT + i], entry, 0) != entry ||
		    cmpxchg64_relaxed(&frame_root[FRAME_SLOT + i], entry, 0) != entry)
			return -EAGAIN;
	}
	ret = flush_sid();
	if (!ret)
		ret = frame_flush();
	if (ret)
		return ret;
	if (!frame_registers_match() ||
	    memcmp(frame_ram, frame_saved, resource_size(&frame_reservation)))
		return -EIO;
	frame_published = false;
	pr_info("m4_dcpext_frame: FE SID0 and SYS scanout removed; inherited reservation unchanged\n");
	return 0;
}
