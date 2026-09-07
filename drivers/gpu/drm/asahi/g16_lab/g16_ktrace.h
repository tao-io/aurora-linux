/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * J713 25G83 firmware execution trace, verified against firmware 0x245bc
 * (writer), 0x248f0 (overflow report), 0x3430 (mask import). The older
 * RawKTraceMsg is not this ABI: the slot stride here is 0x48, not 0x38.
 * Preserve every byte, including the currently unexplained tail.
 */
static int g16_lab_drain_ktrace(void)
{
	u8 record[J713_KTRACE_RECORD_SIZE];
	u8 *state;
	u32 consumer, producer;
	unsigned int drained = 0;
	int ret;

	if (!firmware_ktrace_mask || !g16_ktrace_state_first)
		return 0;
	if (!g16_ktrace_copy || !g16_ktrace_data_first)
		return -EINVAL;
	state = g16_lab_live_object_ptr(g16_ktrace_state_first, 0);
	g16_lab_dcache_invalidate(state, J713_KTRACE_STATE_SIZE);
	consumer = READ_ONCE(*(u32 *)state);
	producer = READ_ONCE(*(u32 *)(state + 0x20));
	if (consumer >= J713_KTRACE_COUNT || producer >= J713_KTRACE_COUNT) {
		pr_err("g16_lab: invalid KTrace ring consumer=%u producer=%u\n",
		       consumer, producer);
		return -EPROTO;
	}
	dma_rmb();
	/* Snapshot producer: a noisy firmware cannot keep this loop running. */
	while (consumer != producer) {
		ret = g16_lab_copy_live_object(g16_ktrace_data_first,
			J713_KTRACE_DATA_SIZE, consumer * sizeof(record),
			record, sizeof(record));
		if (ret)
			return ret;
		if (get_unaligned_le32(record) != 5) {
			pr_err("g16_lab: invalid KTrace type=%u slot=%u\n",
			       get_unaligned_le32(record), consumer);
			return -EPROTO;
		}
		if (g16_ktrace_count < J713_KTRACE_CAPTURE_COUNT) {
			memcpy((u8 *)g16_ktrace_copy + g16_ktrace_count * sizeof(record),
			       record, sizeof(record));
			g16_ktrace_count++;
		} else {
			g16_ktrace_host_dropped++;
		}
		/* Stream compact evidence independently of the post-run export. */
		pr_debug("g16_lab: KTrace tick=%llu event=%06x thread=%u phase=%u args=%llx/%llx/%llx/%llx\n",
			 get_unaligned_le64(record + 4),
			 get_unaligned_le32(record + 44) & 0xffffff, record[47],
			 get_unaligned_le32(record + 48),
			 get_unaligned_le64(record + 12), get_unaligned_le64(record + 20),
			 get_unaligned_le64(record + 28), get_unaligned_le64(record + 36));
		consumer = (consumer + 1) & (J713_KTRACE_COUNT - 1);
		drained++;
	}
	if (drained) {
		dma_wmb();
		WRITE_ONCE(*(u32 *)state, consumer);
		g16_lab_dcache_clean(state, sizeof(u32));
	}
	return 0;
}

/* Called after the bounded firmware run has stopped, before freeing its UAT. */
static int g16_lab_export_ktrace(void)
{
	int ret = g16_lab_drain_ktrace();

	if (!g16_ktrace_copy)
		return ret;
	g16_ktrace_blob.data = g16_ktrace_copy;
	g16_ktrace_blob.size = g16_ktrace_count * J713_KTRACE_RECORD_SIZE;
	g16_ktrace_file = debugfs_create_blob("g16_lab_ktrace", 0400, NULL,
					    &g16_ktrace_blob);
	if (IS_ERR_OR_NULL(g16_ktrace_file)) {
		int error = g16_ktrace_file ? PTR_ERR(g16_ktrace_file) : -ENOMEM;

		g16_ktrace_file = NULL;
		return ret ? ret : error;
	}
	pr_info("g16_lab: KTrace captured=%u host-dropped=%u drain-result=%d bytes=%lu\n",
		g16_ktrace_count, g16_ktrace_host_dropped, ret,
		g16_ktrace_blob.size);
	return ret;
}
