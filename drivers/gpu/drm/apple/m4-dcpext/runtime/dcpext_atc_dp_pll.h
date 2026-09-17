/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Diagnostic adapter; register sequencing lives in the PHY driver. */
#ifndef DCPEXT_ATC_DP_PLL_H
#define DCPEXT_ATC_DP_PLL_H
#include "atc-t8132-dp-pll.h"
#define dp_pll_update(...) apple_t8132_dp_pll_update(atc_core, __VA_ARGS__)
#define dp_pll_ack(...) apple_t8132_dp_pll_ack(atc_core, __VA_ARGS__)
#define dp_pll_request_begin() apple_t8132_dp_pll_request_begin(atc_core)
#define dp_pll_prepare(...) apple_t8132_dp_pll_prepare(atc_core, __VA_ARGS__)
#define dp_pll_stop_after_lanes() apple_t8132_dp_pll_stop_after_lanes(atc_core)
#endif
