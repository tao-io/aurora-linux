// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/soc/apple/pmp-temps.h>

/* Share the validated firmware-record decoder with the native GPU cooler. */
__rust_helper int rust_helper_apple_pmp_temp_hotspot(void __iomem *sram, long *temp)
{
	return apple_pmp_temp_hotspot(sram, temp);
}

__rust_helper int rust_helper_apple_pmp_temp_hotspot_channel(void __iomem *sram,
							   long *temp, int *channel)
{
	return apple_pmp_temp_hotspot_channel(sram, temp, channel);
}
