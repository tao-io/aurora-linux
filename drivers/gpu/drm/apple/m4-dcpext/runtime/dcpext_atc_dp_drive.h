/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Diagnostic adapter; register sequencing lives in the PHY driver. */
#ifndef DCPEXT_ATC_DP_DRIVE_H
#define DCPEXT_ATC_DP_DRIVE_H
#include "dcpext_atc_dp_pll.h"
#include "atc-t8132-dp-drive.h"
#define atc_dp_drive_preset(...) apple_t8132_atc_dp_drive_preset(atc_core, __VA_ARGS__)
#endif
