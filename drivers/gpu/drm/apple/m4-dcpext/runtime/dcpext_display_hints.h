/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Native handleSetDisplayHints copies a 108-byte IOAVDisplayHints from service
 * offset64, passes allocation at176 and writes status at208 (not at64).
 * Field offsets below are qualified by captured J713 HPD-on/off callbacks.
 * This records DCP's parsed monitor data, not a raw EDID block/checksum.
 */
static bool dptx_monitor_seen;
static char dptx_monitor_name[17];
static u32 dptx_monitor_width, dptx_monitor_height;
module_param_named(monitor_seen, dptx_monitor_seen, bool, 0400);
module_param_string(monitor_name, dptx_monitor_name, sizeof(dptx_monitor_name), 0400);
module_param_named(monitor_width, dptx_monitor_width, uint, 0400);
module_param_named(monitor_height, dptx_monitor_height, uint, 0400);

static int dptx_record_display_hints(const u8 *data, u32 size)
{
	u32 width, height;
	unsigned int length = 0;
	if (size != 160)
		return -EPROTO;
	if (!(get_unaligned_le32(data) & 1))
		return 0; /* Retain the last identified monitor across HPD-off. */
	width = get_unaligned_le32(data + 56);
	height = get_unaligned_le32(data + 60);
	if (!width || width > 16384 || !height || height > 16384)
		return -EPROTO;
	while (length < 16 && data[92 + length]) {
		if (data[92 + length] < 0x20 || data[92 + length] > 0x7e)
			return -EPROTO;
		length++;
	}
	if (!length || length == 16)
		return -EPROTO;
	memcpy(dptx_monitor_name, data + 92, length);
	dptx_monitor_name[length] = 0;
	dptx_monitor_width = width;
	dptx_monitor_height = height;
	dptx_monitor_seen = true;
	pr_info("m4_dcpext_session: external monitor '%s' %ux%u (DCP display hints; raw EDID not captured)\n",
		dptx_monitor_name, width, height);
	return 0;
}
