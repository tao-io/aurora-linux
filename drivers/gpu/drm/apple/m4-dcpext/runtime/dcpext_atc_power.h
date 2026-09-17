/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Diagnostic ownership wrapper; sequence shared with normal PHY code. */
#include "atc-t8132-power.h"
static int atc_power_up_reset(void)
{
	int ret = apple_t8132_atc_power_idle(atc_core);
	if (ret)
		return ret;
	__module_get(THIS_MODULE);
	atc_pinned = true;
	return apple_t8132_atc_power_up_reset(atc_core);
}
static int atc_power_down(void)
{
	return apple_t8132_atc_power_down(atc_core);
}
static inline int atc_power_roundtrip(void)
{
	int ret = atc_power_up_reset();
	return ret ? ret : atc_power_down();
}
