/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCPEXT_ROUTES_H
#define M4_DCPEXT_ROUTES_H

/* Claim before mapping or programming a PHY. Release only after its clocks,
 * route and MMIO ownership have been retired; faults retain the claim.
 */
bool m4_dcpext_route_claim(unsigned int controller, unsigned int phy);
void m4_dcpext_route_release(unsigned int controller, unsigned int phy);
bool m4_dcpext_primary_enabled(void);

#endif
