/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Private external framebuffer mappings. SID0 + SYS SID23 follow Hector
 * Martin / Asahi m1n1 display_map_fb(), with Janne Grunau's external display
 * support. DART encodings/flush ordering follow Sven Peter / Asahi Linux.
 * No inherited mapping is replaced. All allocations survive uncertain DMA.
 */
#define FRAME_DVA (LEASE_DVA + 0x800000)
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_STRIDE (FRAME_WIDTH * 4)
#define FRAME_BYTES ALIGN(FRAME_STRIDE * FRAME_HEIGHT + 24 * LEASE_PAGE, LEASE_PAGE)
#define FRAME_FIRST ((FRAME_DVA - LEASE_DVA) / LEASE_PAGE)

static bool frame_map;
static unsigned int frame_buffers = 1;
module_param(frame_buffers, uint, 0400);
module_param(frame_map, bool, 0400);
MODULE_PARM_DESC(frame_map, "Map private 1080p buffer in external SYS SID23 and FE SID0; no image without separate opt-in");
static void *frame_pixels, *frame_leaf, *frame_ram;
static void *frame_pixels_b;
static dma_addr_t frame_dma_b;
static u8 *frame_saved;
static u64 *frame_root;
static dma_addr_t frame_dma;
static void __iomem *frame_regs;
static struct resource frame_reservation;
static u32 frame_ttbr, frame_tcr, frame_protect;
static bool frame_published;

static void frame_cleanup(void)
{
	if (frame_ram)
		memunmap(frame_ram);
	kfree(frame_saved);
	if (frame_leaf)
		free_page((unsigned long)frame_leaf);
	if (frame_pixels)
		dma_free_coherent(dma_dev, FRAME_BYTES, frame_pixels, frame_dma);
	if (frame_pixels_b)
		dma_free_coherent(dma_dev, FRAME_BYTES, frame_pixels_b, frame_dma_b);
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

static int frame_prepare(struct device_node *dcp, int count)
{
	struct device_node *ram;
	phys_addr_t root_pa, leaf_pa;
	u64 entry;
	int ret = -EINVAL;
	if (!frame_map)
		return 0;
	if (frame_buffers < 1 || frame_buffers > 2 ||
	    FRAME_FIRST + frame_buffers * (FRAME_BYTES / LEASE_PAGE) > LEASE_PAGE / sizeof(u64))
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
	frame_leaf = (void *)get_zeroed_page(GFP_KERNEL);
	if (!frame_saved || !frame_leaf)
		goto out;
	ret = -EBUSY;
	if (READ_ONCE(frame_root[LEASE_SLOT]))
		goto out;
	frame_pixels = dma_alloc_coherent(dma_dev, FRAME_BYTES, &frame_dma, GFP_KERNEL);
	ret = -ENOMEM;
	if (!frame_pixels)
		goto out;
	if (frame_buffers == 2) {
		frame_pixels_b = dma_alloc_coherent(dma_dev, FRAME_BYTES, &frame_dma_b, GFP_KERNEL);
		if (!frame_pixels_b)
			goto out;
		memset(frame_pixels_b, 0, FRAME_BYTES);
	}
	leaf_pa = virt_to_phys(frame_leaf);
	ret = -EINVAL;
	if ((frame_dma | leaf_pa) & (LEASE_PAGE - 1) ||
	    dma_to_phys(dma_dev, frame_dma) != frame_dma ||
	    frame_dma + FRAME_BYTES > BIT_ULL(42) || leaf_pa >= BIT_ULL(42) ||
	    FRAME_FIRST + FRAME_BYTES / LEASE_PAGE > LEASE_PAGE / sizeof(u64))
		goto out;
	if (frame_buffers == 2 && ((frame_dma_b & (LEASE_PAGE - 1)) ||
	    dma_to_phys(dma_dev, frame_dma_b) != frame_dma_b ||
	    frame_dma_b + FRAME_BYTES > BIT_ULL(42)))
		goto out;
	memset(frame_pixels, 0, FRAME_BYTES);
	for (u32 i = 0; i < frame_buffers * (FRAME_BYTES / LEASE_PAGE); i++) {
		u32 page = i % (FRAME_BYTES / LEASE_PAGE);
		dma_addr_t base = i < FRAME_BYTES / LEASE_PAGE ? frame_dma : frame_dma_b;
		entry = encode_address(base + page * LEASE_PAGE) | PTE_RW;
		if (((u64 *)leaf)[FRAME_FIRST + i]) {
			ret = -EBUSY;
			goto out;
		}
		((u64 *)leaf)[FRAME_FIRST + i] = entry;
		((u64 *)frame_leaf)[FRAME_FIRST + i] = entry;
	}
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
	ret = frame_registers_match() &&
		!memcmp(frame_ram, frame_saved, resource_size(&frame_reservation)) ? 0 : -EAGAIN;
out:
	of_node_put(ram);
	return ret;
}

static int frame_publish(void)
{
	u64 entry;
	if (!frame_map)
		return 0;
	if (!pinned || !frame_registers_match() ||
	    memcmp(frame_ram, frame_saved, resource_size(&frame_reservation)))
		return -EAGAIN;
	entry = encode_address(virt_to_phys(frame_leaf)) | 1;
	dma_wmb();
	if (cmpxchg64_relaxed(&frame_root[LEASE_SLOT], 0, entry))
		return -EAGAIN;
	frame_published = true;
	pr_info("m4_dcpext_frame: SYS23/FE0 DVA=%#llx bytes=%#x frame=%ux%u stride=%u\n",
		FRAME_DVA, FRAME_BYTES, FRAME_WIDTH, FRAME_HEIGHT, FRAME_STRIDE);
	return frame_flush();
}

static int frame_remove(void)
{
	u64 entry;
	int ret;
	if (!frame_map)
		return 0;
	if (!frame_published || !frame_registers_match())
		return -EUCLEAN;
	entry = encode_address(virt_to_phys(frame_leaf)) | 1;
	if (cmpxchg64_relaxed(&frame_root[LEASE_SLOT], entry, 0) != entry)
		return -EAGAIN;
	ret = frame_flush();
	if (ret)
		return ret;
	if (!frame_registers_match() ||
	    memcmp(frame_ram, frame_saved, resource_size(&frame_reservation)))
		return -EIO;
	frame_published = false;
	pr_info("m4_dcpext_frame: FE SID0 removed; full inherited reservation unchanged\n");
	return 0;
}
