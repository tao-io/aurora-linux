// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* First connected-monitor desktop driver. Shares the qualified handoff,
 * mapping, native display and DRM implementation with bounded qualification.
 * TODO: normal platform/PHY ownership, hotplug/retraining and power saving.
 */
#define DCPEXT_WITH_SESSION
#define DCPEXT_DESKTOP
#include "m4_dcpext_dma_probe.c"

#ifndef MODULE
bool m4_dcpext_primary_enabled(void)
{
	return enable;
}
#endif
