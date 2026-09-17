/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Diagnostic adapter; register sequencing lives in the PHY driver. */
#ifndef DCPEXT_ATC_DP_MAIN_H
#define DCPEXT_ATC_DP_MAIN_H
#include "dcpext_atc_dp_pll.h"
#include "dcpext_atc_lane_power.h"
#include "atc-t8132-dp-main.h"
#define dp_lane_update(...) apple_t8132_dp_lane_update(atc_core, __VA_ARGS__)
#define dp_lane_tx_initial_eq(...) apple_t8132_dp_lane_tx_initial_eq(atc_core, __VA_ARGS__)
#define dp_lane_rx_initial_eq(...) apple_t8132_dp_lane_rx_initial_eq(atc_core, __VA_ARGS__)
#define dp_lane_tx_prepare(...) apple_t8132_dp_lane_tx_prepare(atc_core, __VA_ARGS__)
#define dp_lane_rx_prepare(...) apple_t8132_dp_lane_rx_prepare(atc_core, __VA_ARGS__)
#define atc_dp_main_prepare(...) apple_t8132_atc_dp_main_prepare(atc_core, __VA_ARGS__)
#define atc_dp_main_stop(...) apple_t8132_atc_dp_main_stop(atc_core, __VA_ARGS__)
#endif
