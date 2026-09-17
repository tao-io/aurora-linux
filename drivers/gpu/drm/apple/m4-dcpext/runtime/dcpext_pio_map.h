/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Diagnostic mirror of the private SYS23 arena in external FE SID16.
 * PIODMA ownership/role: preserved J713 m1n1 reservations and internal M4
 * bridge D201. DART encodings/flush ordering: Asahi io-pgtable-dart.c.
 * Never edits inherited mappings, TTBR, TCR or lock registers. SID0 untouched.
 */
static bool pio_map;
module_param(pio_map, bool, 0400);
static void __iomem *pio_regs;
static void *pio_ram, *pio_saved, *pio_leaf;
static u64 *pio_root;
static struct resource pio_reservation;
static u32 pio_ttbr, pio_tcr, pio_protect;
static bool pio_published;

static void pio_cleanup(void)
{
	if (pio_ram)
		memunmap(pio_ram);
	kfree(pio_saved);
	if (pio_leaf)
		free_page((unsigned long)pio_leaf);
}
static bool pio_registers_match(void)
{
	return readl(pio_regs + 0x1440) == pio_ttbr &&
	       readl(pio_regs + 0x1040) == pio_tcr &&
	       readl(pio_regs + 0x200) == pio_protect &&
	       !readl(pio_regs + 0x100);
}
static int pio_flush(void)
{
	u32 status;
	dma_wmb();
	writel(0x110, pio_regs + 0x80); /* SID16 */
	return readl_poll_timeout(pio_regs + 0x80, status, !(status & BIT(31)),
				  1, 10000);
}
static int pio_prepare(struct device_node *dcp, int count)
{
	struct device_node *ram;
	phys_addr_t root_pa;
	int ret = -EINVAL;

	if (!pio_map)
		return 0;
	if (port || count < 3)
		return -EINVAL;
	ram = of_parse_phandle(dcp, "memory-region", count - 1);
	if (!ram || !of_device_is_compatible(ram, "apple,dart-mem") ||
	    !of_property_read_bool(ram, "no-map") ||
	    of_address_to_resource(ram, 0, &pio_reservation))
		goto out;
	if (resource_size(&pio_reservation) < LEASE_PAGE ||
	    resource_size(&pio_reservation) > 0x100000 ||
	    (pio_reservation.start | resource_size(&pio_reservation)) &
		    (LEASE_PAGE - 1))
		goto out;
	ret = fe_prepare();
	if (ret)
		goto out;
	pio_regs = fe_regs;
	pio_ttbr = readl(pio_regs + 0x1440);
	pio_tcr = readl(pio_regs + 0x1040);
	pio_protect = readl(pio_regs + 0x200);
	root_pa = (u64)((pio_ttbr & GENMASK(29, 2)) >> 2) << 14;
	ret = -EINVAL;
	if (!(pio_protect & 1) || pio_tcr != 1 || !(pio_ttbr & 1) ||
	    root_pa < pio_reservation.start || root_pa > pio_reservation.end ||
	    pio_reservation.end - root_pa + 1 < LEASE_PAGE ||
	    readl(pio_regs + 0x100))
		goto out;
	pio_ram = memremap(pio_reservation.start,
			   resource_size(&pio_reservation), MEMREMAP_WB);
	ret = -ENOMEM;
	if (!pio_ram)
		goto out;
	pio_root = pio_ram + root_pa - pio_reservation.start;
	pio_saved =
		kmemdup(pio_ram, resource_size(&pio_reservation), GFP_KERNEL);
	if (!pio_saved)
		goto out;
	pio_leaf = (void *)get_zeroed_page(GFP_KERNEL);
	if (!pio_leaf)
		goto out;
	/* Copy only the communication arena. Framebuffer pages in the SYS
	 * leaf must not become reachable through SID16 as a side effect. */
	memcpy(pio_leaf, leaf, LEASE_SIZE / LEASE_PAGE * sizeof(u64));
	root_pa = virt_to_phys(pio_leaf);
	ret = -EINVAL;
	if (root_pa & (LEASE_PAGE - 1) || root_pa >= BIT_ULL(42))
		goto out;
	ret = READ_ONCE(pio_root[LEASE_SLOT]) ? -EBUSY : 0;
out:
	of_node_put(ram);
	return ret;
}
static int pio_publish(void)
{
	u64 entry;
	if (!pio_map)
		return 0;
	if (!pinned || !pio_registers_match() ||
	    memcmp(pio_ram, pio_saved, resource_size(&pio_reservation)))
		return -EUCLEAN;
	entry = encode_address(virt_to_phys(pio_leaf)) | 1;
	if (cmpxchg64_relaxed(&pio_root[LEASE_SLOT], 0, entry))
		return -EBUSY;
	pio_published =
		true; /* Includes failed invalidation: never free uncertain tables. */
	pr_info("m4_dcpext_pio: private SYS23 arena mirrored at SID16 DVA=%#llx bytes=%#x\n",
		LEASE_DVA, LEASE_SIZE);
	return pio_flush();
}
static int pio_remove(void)
{
	u64 entry;
	int ret;
	if (!pio_map)
		return 0;
	if (!pio_published || !pio_registers_match())
		return -EUCLEAN;
	entry = encode_address(virt_to_phys(pio_leaf)) | 1;
	if (cmpxchg64_relaxed(&pio_root[LEASE_SLOT], entry, 0) != entry)
		return -EAGAIN;
	ret = pio_flush();
	if (ret)
		return ret;
	if (!pio_registers_match() ||
	    memcmp(pio_ram, pio_saved, resource_size(&pio_reservation)))
		return -EIO;
	pio_published = false;
	pr_info("m4_dcpext_pio: SID16 mapping removed; full inherited reservation unchanged\n");
	return 0;
}
