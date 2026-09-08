/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M4_DCP_BRIDGE_H
#define M4_DCP_BRIDGE_H

struct device;
struct apple_rtkit;
struct m4_dcp_bridge;

struct m4_dcp_buffer {
	u64 size, physical, dva;
	u32 id, flags;
};

typedef int (*m4_dcp_callback_fn)(struct m4_dcp_bridge *bridge, void *cookie,
				u32 tag, const void *input, u32 input_size,
				void *output, u32 output_size);

struct m4_dcp_bridge *m4_dcp_bridge_create(struct device *dev,
					struct apple_rtkit *rtkit, void *rpc, bool kernel_client);
void m4_dcp_bridge_receive(struct m4_dcp_bridge *bridge, u64 message);
void m4_dcp_bridge_activate(struct m4_dcp_bridge *bridge);
void m4_dcp_bridge_crashed(struct m4_dcp_bridge *bridge);
int m4_dcp_bridge_call(struct m4_dcp_bridge *bridge, u32 tag,
		      const void *input, u32 input_size, void *output, u32 output_size,
		      u32 completion_id, m4_dcp_callback_fn callback, void *cookie);
int m4_dcp_bridge_pump(struct m4_dcp_bridge *bridge, unsigned long timeout,
		      m4_dcp_callback_fn callback, void *cookie);
int m4_dcp_bridge_alloc(struct m4_dcp_bridge *bridge, struct m4_dcp_buffer *info);
int m4_dcp_bridge_map(struct m4_dcp_bridge *bridge, struct m4_dcp_buffer *info);
int m4_dcp_bridge_retire(struct m4_dcp_bridge *bridge, u32 id);

#endif
