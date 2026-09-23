/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef PHY_APPLE_T8132_H
#define PHY_APPLE_T8132_H

#include <linux/types.h>
#include <linux/usb/typec.h>

struct phy;

/* Type-C state, not evidence that the display link has trained. Generation
 * changes on route/HPD changes and each HPD IRQ. irq_count preserves pulses
 * when the consumer coalesces work; both counters use unsigned wraparound.
 */
struct apple_t8132_dp_connection {
	u64 generation;
	u64 irq_count;
	enum typec_orientation orientation;
	bool pin_d;
	bool allowed;
	bool hpd;
};

typedef void (*apple_t8132_dp_notify_t)(void *cookie,
			const struct apple_t8132_dp_connection *connection);

/* One display owner per PHY. Registration synchronously replays current
 * state, under the same lock as subsequent notifications, so an already
 * connected monitor cannot be missed. Hold a generic-PHY reference throughout.
 *
 * Callbacks run under the provider's ATC mutex: copy the snapshot and queue
 * work only. Do not call PHY APIs, take the display route lock, or register /
 * unregister from a callback. The snapshot pointer expires at callback return.
 * Unsubscribe waits for in-flight callbacks. Then cancel consumer work before
 * freeing its state or dropping the PHY reference. Call these functions without
 * consumer locks that callbacks or the provider could acquire.
 */
int apple_t8132_dp_subscribe(struct phy *phy, apple_t8132_dp_notify_t notify,
			    void *cookie);
int apple_t8132_dp_unsubscribe(struct phy *phy, apple_t8132_dp_notify_t notify,
			      void *cookie);

void __iomem *apple_t8132_dp_get_regs(struct phy *phy);

#endif
