// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/mutex.h>
#include "m4_dcpext_routes.h"

static DEFINE_MUTEX(route_owners_lock);
static int route_owners[2] = { -1, -1 };

bool m4_dcpext_route_claim(unsigned int controller, unsigned int phy)
{
	bool available;

	if (controller > 1 || phy > 1)
		return false;
	mutex_lock(&route_owners_lock);
	available = route_owners[phy] < 0 || route_owners[phy] == controller;
	if (available)
		route_owners[phy] = controller;
	mutex_unlock(&route_owners_lock);
	return available;
}

void m4_dcpext_route_release(unsigned int controller, unsigned int phy)
{
	if (controller > 1 || phy > 1)
		return;
	mutex_lock(&route_owners_lock);
	if (route_owners[phy] == controller)
		route_owners[phy] = -1;
	mutex_unlock(&route_owners_lock);
}
