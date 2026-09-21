// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Independent DCPEXT1 state, using the same transport, KMS and lifecycle code.
 * The builtin parameter prefix keeps both instances independently observable.
 * Shared helpers carry explicit state; only physical port arbitration is global.
 */
#define pr_fmt(fmt) "dcpext1: " fmt
#include <linux/module.h>
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "m4_dcpext_secondary."
#define DCPEXT_INSTANCE 1
#define DCPEXT_WITH_SESSION
#define DCPEXT_DESKTOP
#include "m4_dcpext_dma_probe.c"
