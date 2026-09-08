/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCP_BRIDGE_H
#define M4_DCP_BRIDGE_H

struct device;
struct apple_rtkit;
struct m4_dcp_bridge;

struct m4_dcp_bridge *m4_dcp_bridge_create(struct device *dev,
					struct apple_rtkit *rtkit, void *rpc);
void m4_dcp_bridge_receive(struct m4_dcp_bridge *bridge, u64 message);
void m4_dcp_bridge_activate(struct m4_dcp_bridge *bridge);

#endif
