/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Diagnostic adapter; register sequencing lives in the PHY driver. */
#ifndef DCPEXT_ATC_LANE_POWER_H
#define DCPEXT_ATC_LANE_POWER_H
#include "atc-t8132-lane-power.h"
#define atc_lane_power_field(...) apple_t8132_atc_lane_power_field(atc_core, __VA_ARGS__)
#define atc_lane_tx_power(...) apple_t8132_atc_lane_tx_power(atc_core, __VA_ARGS__)
#define atc_lane_rx_power(...) apple_t8132_atc_lane_rx_power(atc_core, __VA_ARGS__)
#endif
