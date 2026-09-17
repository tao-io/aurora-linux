// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include "dcpext_native_query.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/dma-mapping.h>
#else
#include <errno.h>
#include <string.h>
static void put_unaligned_le32(u32 v,void *p) {memcpy(p,&v,4);}
static u32 get_unaligned_le32(const void *p) {u32 v;memcpy(&v,p,4);return v;}
#define dma_rmb() do { } while (0)
#define dma_wmb() do { } while (0)
#endif

int dcpext_native_query_start(struct dcpext_native_query *s,
			    const struct dcpext_native_query_ops *ops, void *cookie)
{
	int ret;

	if (s->error)
		return s->error;
	if (s->phase || !ops || !ops->allocate || !ops->send)
		return -EINVAL;
	ret = ops->allocate(cookie, DCPEXT_NATIVE_RPC_BYTES, &s->rpc, &s->dva);
	if (ret)
		return s->error = ret;
	if (!s->rpc || s->dva < (1ULL << 40) || (s->dva & 0x3fff) ||
	    s->dva - (1ULL << 40) > (1ULL << 36) - DCPEXT_NATIVE_RPC_BYTES)
		return s->error = -ERANGE;
	memset(s->rpc, 0, DCPEXT_NATIVE_RPC_BYTES);
	/* M4 descriptor offset 12 is alignment, not the historical version 4. */
	put_unaligned_le32(64, s->rpc + 12);
	s->phase = 1; /* Even an uncertain send retains the mapping. */
	ret = ops->send(cookie, 0, (5ULL << 52) | (0x37ULL << 32) | 2);
	if (!ret) {
		dma_wmb();
		ret = ops->send(cookie, 0x37, (s->dva << 16) | 0x40);
	}
	return s->error = ret;
}

int dcpext_native_query_receive(struct dcpext_native_query *s,
			      const struct dcpext_native_query_ops *ops,
			      void *cookie, u64 message)
{
	int ret = -EPROTO;

	if (s->error)
		return s->error;
	if (s->phase == 1) {
		if (!message && !s->init_messages++)
			return 0;
		s->alignment = message >> 48;
		s->hash = (message >> 16) & 0xffffffff;
		/* Exact READY low flags observed on both internal M4 (20260908)
		 * and external native-query-1 (20260917): 0x101. The bit 8 is
		 * present in READY; it does not make the following A412 remote. */
		if ((message & 0xffff) != 0x101 || s->alignment != 64 || s->hash != 0x45bc9ef4)
			goto out;
		put_unaligned_le32(0x41343132, s->rpc); /* A412 */
		put_unaligned_le32(0, s->rpc + 4);
		put_unaligned_le32(4, s->rpc + 8);
		put_unaligned_le32(0xa5, s->rpc + 12);
		s->phase = 2;
		dma_wmb();
		ret = ops->send(cookie, 0x37, (16ULL << 32) | 2);
	} else if (s->phase == 2) {
		dma_rmb();
		s->is_main = get_unaligned_le32(s->rpc + 12);
		if (message != 0x42 || get_unaligned_le32(s->rpc) != 0x41343132 ||
		    get_unaligned_le32(s->rpc + 4) || get_unaligned_le32(s->rpc + 8) != 4 ||
		    s->is_main != 0)
			goto out;
		s->phase = 3;
		ret = 0;
	}
 out:
	return s->error = ret;
}
