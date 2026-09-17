/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded experiment policy; calibration and register operations are shared. */
#include "atc-t8132-common.h"
#define atc_component_tuning apple_t8132_tuning
static struct atc_component_tuning atc_component_tunings[] = { APPLE_T8132_COMMON_TUNINGS };
static int (*atc_component_observer)(void);
static bool atc_component_route_pin_d;
#define atc_component_select(cells, count) apple_t8132_atc_component_select(atc_component_tunings, cells, count)
#define atc_component_update(...) apple_t8132_atc_component_update(atc_core, __VA_ARGS__)
#define atc_route_pin_d_flipped() apple_t8132_atc_route_pin_d_flipped(atc_core)
#define atc_common_start() apple_t8132_atc_common_start(atc_core)
#define atc_common_stop() apple_t8132_atc_common_stop(atc_core)
#define atc_aux_start() apple_t8132_atc_aux_start(atc_core)
#define atc_aux_stop() apple_t8132_atc_aux_stop(atc_core)
#include "atc-t8132-session.h"
static int atc_common_roundtrip(void)
{
	struct apple_t8132_atc_session state = { .regs = atc_core };
	int ret;

	memcpy(state.tuning, atc_component_tunings, sizeof(state.tuning));
	ret = apple_t8132_atc_session_idle(&state);
	if (ret)
		return ret;
	__module_get(THIS_MODULE);
	atc_pinned = true;
	ret = apple_t8132_atc_session_start(&state, atc_component_route_pin_d);
	application_result = ret;
	if (ret)
		return ret;
	if (atc_component_observer) {
		ret = atc_component_observer();
		if (ret)
			return ret; /* Consumer has not established a safe PHY stop. */
	}
	ret = apple_t8132_atc_session_stop(&state);
	if (!ret)
		pr_info("m4_atc_probe: common/AUX session restored all sampled registers; application=0\n");
	return ret;
}
