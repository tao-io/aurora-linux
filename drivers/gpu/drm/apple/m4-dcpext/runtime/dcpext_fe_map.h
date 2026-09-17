/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* One register owner for external FE SID0 (scanout) and SID16 (PIODMA).
 * The session serializes both users; each keeps its own inherited root and
 * invalidates only its SID. Cleanup is allowed only after both are retired. */
static void __iomem *fe_regs;
static phys_addr_t fe_base;
static bool fe_owned;

static int fe_prepare(void)
{
	phys_addr_t base = port ? 0x48a300000ULL : 0x286300000ULL;

	if (fe_regs)
		return fe_base == base ? 0 : -EINVAL;
	if (fe_owned)
		return -EUCLEAN;
	if (!request_mem_region(base, 0xc000, "m4-dcpext-fe"))
		return -EBUSY;
	fe_base = base;
	fe_owned = true;
	fe_regs = ioremap(base, 0xc000);
	return fe_regs ? 0 : -ENOMEM;
}

static void fe_cleanup(void)
{
	if (fe_regs)
		iounmap(fe_regs);
	if (fe_owned)
		release_mem_region(fe_base, 0xc000);
	fe_regs = NULL;
	fe_owned = false;
}
