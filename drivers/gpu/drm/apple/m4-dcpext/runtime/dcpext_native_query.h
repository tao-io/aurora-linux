/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Bounded external DCPLink profile query. Framing from Asahi dcp-link.h and
 * our qualified internal M4 m4_dcp_rtkit_main.c; no external startup/swap ABI
 * is assumed. Only READY and A412 is_main_display are admitted.
 */
#ifndef DCPEXT_NATIVE_QUERY_H
#define DCPEXT_NATIVE_QUERY_H
#include "m4_dcpext_afk.h"
#define DCPEXT_NATIVE_RPC_BYTES 0xc0000
struct dcpext_native_query_ops {
	int (*allocate)(void *cookie, u32 bytes, u8 **cpu, u64 *dva);
	int (*send)(void *cookie, u8 endpoint, u64 message);
};
struct dcpext_native_query {
	u8 *rpc;
	u64 dva;
	u32 phase, hash, alignment, is_main, init_messages;
	int error;
};
int dcpext_native_query_start(struct dcpext_native_query *s,
			    const struct dcpext_native_query_ops *ops, void *cookie);
int dcpext_native_query_receive(struct dcpext_native_query *s,
			      const struct dcpext_native_query_ops *ops,
			      void *cookie, u64 message);
#endif
