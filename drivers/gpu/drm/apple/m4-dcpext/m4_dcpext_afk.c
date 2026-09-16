// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* See m4_dcpext_afk.h for attribution, protocol facts and locking. */
#include "m4_dcpext_afk.h"

#ifdef __KERNEL__
#include <linux/align.h>
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#else
/* Host shim for the offline tests: no barriers, no concurrency. */
#include <assert.h>
#include <errno.h>
#include <string.h>
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define dma_rmb() do {} while (0)
#define dma_wmb() do {} while (0)
#define dma_mb() do {} while (0)
#define is_power_of_2(x) ((x) && !((x) & ((x) - 1)))
#define mutex_init(m) ((m)->held = 0)
#define mutex_lock(m) do { assert(!(m)->held); (m)->held = 1; } while (0)
#define mutex_unlock(m) do { assert((m)->held); (m)->held = 0; } while (0)
static u16 get_unaligned_le16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static void put_unaligned_le16(u16 v, void *p) { memcpy(p, &v, 2); }
static void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
#endif

#define AFK_MAGIC 0x20504f49 /* "IOP " */
#define AFK_RING_TX 0
#define AFK_RING_RX 1
#define AFK_MSG_START 0x80
#define AFK_MSG_DRAIN 0x85
#define AFK_MSG_RUNNING 0x86
#define AFK_MSG_BUFFER 0x89
#define AFK_MSG_RING_TX 0x8a
#define AFK_MSG_RING_RX 0x8b
#define AFK_MSG_NOOP 0x8c
#define AFK_MSG_HELLO 0xa0
#define AFK_MSG_BUFFER_REPLY 0xa1
#define AFK_MSG_SUBMIT 0xa2
#define AFK_MSG_RINGS_READY 0xa3
#define AFK_MSG_SHUTDOWN 0xc0
#define AFK_MSG_SHUTDOWN_ACK 0xc1
#define AFK_MAX_MESSAGES 128
#define AFK_MAX_DRAIN 64

static int afk_fail(struct m4_dcpext_afk *afk, int error)
{
	if (!afk->error)
		afk->error = error;
	return afk->error;
}

static int afk_send(struct m4_dcpext_afk *afk, u32 type, u64 payload)
{
	int ret = afk->ops->send(afk->cookie, afk->endpoint, (u64)type << 48 | payload);

	return ret ? afk_fail(afk, ret) : 0;
}

void m4_dcpext_afk_init(struct m4_dcpext_afk *afk, const struct m4_dcpext_afk_ops *ops,
			void *cookie, u8 endpoint)
{
	memset(afk, 0, sizeof(*afk));
	mutex_init(&afk->lock);
	afk->ops = ops;
	afk->cookie = cookie;
	afk->endpoint = endpoint;
}

int m4_dcpext_afk_start(struct m4_dcpext_afk *afk)
{
	int ret;

	mutex_lock(&afk->lock);
	if (afk->started || !afk->ops || !afk->ops->send || !afk->ops->allocate ||
	    afk->endpoint < 0x20 || afk->endpoint > 0x3f) {
		ret = -EINVAL;
		goto out;
	}
	afk->started = true; /* Published start may take effect even on send error. */
	ret = afk->ops->send(afk->cookie, 0, (5ULL << 52) | ((u64)afk->endpoint << 32) | 2);
	ret = ret ? afk_fail(afk, ret) : afk_send(afk, AFK_MSG_START, 0);
out:
	mutex_unlock(&afk->lock);
	return ret;
}

static bool afk_tx_empty(struct m4_dcpext_afk *afk)
{
	struct m4_dcpext_afk_ring *r = &afk->rings[AFK_RING_TX];
	u32 rd, wr;

	if (!r->header)
		return false;
	rd = READ_ONCE(*(u32 *)(r->header + r->block));
	wr = READ_ONCE(*(u32 *)(r->header + 2 * r->block));
	dma_rmb();
	return rd < r->size && !(rd & (r->block - 1)) && rd == wr;
}

bool m4_dcpext_afk_tx_empty(struct m4_dcpext_afk *afk)
{
	bool empty;

	mutex_lock(&afk->lock);
	empty = afk_tx_empty(afk);
	mutex_unlock(&afk->lock);
	return empty;
}

/* Refuse split/dummy-wrap publication. Bounded packets fit; the caller must
 * handle -ENOSPC. No shared state changes on admission failure; a published
 * packet stays in the ring even if the mailbox notification fails.
 */
static int afk_write(struct m4_dcpext_afk *afk, const u8 *data, u32 bytes)
{
	struct m4_dcpext_afk_ring *r = &afk->rings[AFK_RING_TX];
	u32 rd, wr, total, next;
	u8 *qe;

	if (!afk->running || afk->stopping || afk->error || !r->header ||
	    bytes < M4_DCPEXT_AFK_REPORT ||
	    bytes > (afk->max_payload ? afk->max_payload : M4_DCPEXT_AFK_DEFAULT_PAYLOAD) ||
	    bytes > M4_DCPEXT_AFK_MAX_PAYLOAD)
		return -EINVAL;
	total = ALIGN(bytes + 16, r->block);
	rd = READ_ONCE(*(u32 *)(r->header + r->block));
	wr = READ_ONCE(*(u32 *)(r->header + 2 * r->block));
	dma_rmb();
	if (rd >= r->size || wr >= r->size || (rd | wr) & (r->block - 1))
		return -EPROTO;
	if (total >= r->size || total > r->size - wr ||
	    (wr < rd && total >= rd - wr))
		return -ENOSPC;
	next = wr + total;
	if (next == r->size)
		next = 0;
	if (next == rd)
		return -ENOSPC;
	qe = r->data + wr;
	memset(qe, 0, total);
	put_unaligned_le32(AFK_MAGIC, qe);
	put_unaligned_le32(bytes, qe + 4);
	memcpy(qe + 16, data, bytes);
	dma_wmb();
	WRITE_ONCE(*(u32 *)(r->header + 2 * r->block), next);
	dma_wmb();
	return afk_send(afk, AFK_MSG_SUBMIT, next);
}

int m4_dcpext_afk_write(struct m4_dcpext_afk *afk, const u8 *payload, u32 bytes)
{
	int ret;

	mutex_lock(&afk->lock);
	ret = afk_write(afk, payload, bytes);
	mutex_unlock(&afk->lock);
	return ret;
}

/* Flat OSSerialize dictionaries emitted by the observed V2 publications.
 * Tags/alignment follow m1n1 fw/common.py (MIT). Reject nested objects and
 * malformed bounds; never select a unit from the truncated 32-byte name.
 */
int m4_dcpext_afk_publication_named(const u8 *p, u32 bytes, u16 interface,
				    const char *target)
{
	u32 count, pos = 64, tag, keylen, type, len;
	const u8 *key, *value;
	bool matched = false, have_id = false, have_name = false;

	if (bytes < 64 || get_unaligned_le32(p + 56) != 0xd3)
		return -EPROTO;
	tag = get_unaligned_le32(p + 60);
	count = tag & 0xffffff;
	if ((tag & 0xff000000) != 0x81000000 || !count || count > 32)
		return -EPROTO;
	if (!target)
		matched = !memcmp(p + 24, "disp0-service\0", 14);
	for (u32 i = 0; i < count; i++) {
		if (pos > bytes || bytes - pos < 4)
			return -EPROTO;
		tag = get_unaligned_le32(p + pos);
		pos += 4;
		keylen = tag & 0xffffff;
		if ((tag & 0xff000000) != 0x09000000 || !keylen || keylen > 64 ||
		    ALIGN(keylen, 4) > bytes - pos)
			return -EPROTO;
		key = p + pos;
		pos += ALIGN(keylen, 4);
		if (bytes - pos < 4)
			return -EPROTO;
		tag = get_unaligned_le32(p + pos);
		pos += 4;
		type = (tag >> 24) & 0x7f;
		len = tag & 0xffffff;
		if (!!(tag & 0x80000000) != (i == count - 1))
			return -EPROTO;
		if (type == 4) {
			if (len != 64)
				return -EPROTO;
			len = 8;
		} else if (type == 11) {
			if (len > 1)
				return -EPROTO;
			len = 0;
		} else if (type != 9 && type != 10) {
			return -EOPNOTSUPP;
		}
		if (len > bytes - pos || ALIGN(len, 4) > bytes - pos)
			return -EPROTO;
		value = p + pos;
		pos += ALIGN(len, 4);
		if (keylen == 12 && !memcmp(key, "interface-id", 12)) {
			if (have_id || type != 4 || get_unaligned_le64(value) != interface)
				return -EPROTO;
			have_id = true;
		} else if (keylen == 14 && !memcmp(key, "interface-name", 14)) {
			if (have_name || type != 9)
				return -EPROTO;
			have_name = true;
			if (target)
				matched = len == strlen(target) && !memcmp(value, target, len);
		}
	}
	return pos == bytes && have_id ? matched : -EPROTO;
}

static struct m4_dcpext_afk_command *afk_command_slot(struct m4_dcpext_afk *afk, u8 tag)
{
	for (unsigned int i = 0; i < M4_DCPEXT_AFK_COMMANDS; i++)
		if (afk->commands[i].tag == tag)
			return &afk->commands[i];
	return NULL;
}

/* Service-level interpretation of one consumed payload. Without an armed
 * selection every payload stays opaque evidence (record callback only): a
 * V2 sequence byte of 2 once collided with the old EPIC version heuristic
 * and misclassified a publication during shutdown (afk-ep24-20260914-1).
 */
static int afk_dispatch(struct m4_dcpext_afk *afk, const u8 *p, u32 bytes)
{
	struct m4_dcpext_afk_packet packet = { .data = p, .bytes = bytes };
	struct m4_dcpext_afk_command *command;
	int ret;

	if (!afk->selecting)
		return 0;
	if (bytes < M4_DCPEXT_AFK_REPORT || get_unaligned_le32(p + 4) != bytes - 8)
		return -EPROTO;
	packet.interface = get_unaligned_le16(p + 2);
	packet.type = p[16];
	packet.category = p[17];
	if (packet.type == M4_DCPEXT_AFK_TYPE_PUBLISH && !packet.category) {
		if (!packet.interface)
			return -EPROTO;
		ret = m4_dcpext_afk_publication_named(p, bytes, packet.interface, afk->service);
		if (ret <= 0)
			return ret;
		if (afk->interface)
			return -EPROTO; /* A second matching publication is not expected. */
		afk->interface = packet.interface;
		return 0;
	}
	/* Interface ids are dynamic: anything for another or stale id is an error. */
	if (!afk->interface || packet.interface != afk->interface)
		return -EPROTO;
	if (packet.type == M4_DCPEXT_AFK_TYPE_CLOSE && !packet.category && (p[18] & 1) &&
	    afk->closing) {
		afk->closed = true;
		return 0;
	}
	if (packet.category == 2) {
		if (bytes < M4_DCPEXT_AFK_HEADER)
			return -EPROTO;
		packet.tag = p[25];
		command = afk_command_slot(afk, packet.tag);
		if (!command || command->type != packet.type || !!(p[24] & 1) != command->oob)
			return -EPROTO;
		/* Matched by tag, like the native handleClientResponse; the
		 * common 64-bit field at 8 need not echo the request's zero.
		 */
		command->tag = 0;
		packet.status = get_unaligned_le32(p + 28);
	} else if (packet.category == 1) {
		if (bytes < M4_DCPEXT_AFK_HEADER)
			return -EPROTO;
		packet.tag = p[25];
	}
	return afk->ops->receive ? afk->ops->receive(afk->cookie, &packet) : 0;
}

/* Copy evidence before advancing rptr; the reference consumes first. */
static int afk_drain(struct m4_dcpext_afk *afk)
{
	struct m4_dcpext_afk_ring *r = &afk->rings[AFK_RING_RX];
	u32 rd, wr, bytes, total;
	u8 *qe;
	int ret;

	if (!afk->running || !r->header)
		return -EPROTO;
	for (u32 n = 0; n < AFK_MAX_DRAIN; n++) {
		rd = READ_ONCE(*(u32 *)(r->header + r->block));
		wr = READ_ONCE(*(u32 *)(r->header + 2 * r->block));
		dma_rmb();
		if (rd >= r->size || wr >= r->size || (rd | wr) & (r->block - 1))
			return -EPROTO;
		if (rd == wr)
			return 0;
		qe = r->data + rd;
		if (get_unaligned_le32(qe) != AFK_MAGIC)
			return -EPROTO;
		bytes = get_unaligned_le32(qe + 4);
		if (bytes > r->size - 16)
			return -EPROTO;
		if (bytes + 16 > r->size - rd) {
			if (wr >= rd || !wr)
				return -EPROTO;
			rd = 0;
			qe = r->data;
			if (get_unaligned_le32(qe) != AFK_MAGIC)
				return -EPROTO;
			bytes = get_unaligned_le32(qe + 4);
		}
		if (bytes < 8 || bytes > r->size - rd - 16)
			return -EPROTO;
		total = ALIGN(bytes + 16, r->block);
		if (total > r->size - rd || (rd < wr && total > wr - rd) ||
		    bytes + 12 > sizeof(afk->capture) - afk->capture_size)
			return -EOVERFLOW;
		/* Capture {channel, type, payload length, payload}: the framing
		 * tools/displayport-afk-capture.py decodes.
		 */
		memcpy(afk->capture + afk->capture_size, qe + 8, 8);
		put_unaligned_le32(bytes, afk->capture + afk->capture_size + 8);
		memcpy(afk->capture + afk->capture_size + 12, qe + 16, bytes);
		afk->capture_size += bytes + 12;
		afk->records++;
		ret = afk->ops->record ? afk->ops->record(afk->cookie, qe + 16, bytes) : 0;
		if (!ret)
			ret = afk_dispatch(afk, qe + 16, bytes);
		rd += total;
		if (rd == r->size)
			rd = 0;
		dma_mb();
		WRITE_ONCE(*(u32 *)(r->header + r->block), rd);
		if (ret)
			return ret;
	}
	return -E2BIG;
}

int m4_dcpext_afk_drain(struct m4_dcpext_afk *afk)
{
	int ret;

	mutex_lock(&afk->lock);
	ret = afk_drain(afk);
	mutex_unlock(&afk->lock);
	return ret;
}

static int afk_receive(struct m4_dcpext_afk *afk, u64 message)
{
	u32 type = message >> 48, size, offset, index, data_size, block;
	struct m4_dcpext_afk_ring *r, *other;
	u64 dva;
	int ret = 0;

	if (afk->error)
		return afk->error;
	if (!afk->started || afk->stopped || ++afk->messages > AFK_MAX_MESSAGES)
		return afk_fail(afk, -EPROTO);
	switch (type) {
	case AFK_MSG_HELLO:
	case AFK_MSG_NOOP: /* The m1n1 AFK client treats 0x8c as a no-op. */
		break;
	case AFK_MSG_BUFFER:
		size = ((message >> 16) & 0xffff) * 64;
		if (afk->buffer || afk->stopping || !size || size > 65536)
			return afk_fail(afk, -EINVAL);
		ret = afk->ops->allocate(afk->cookie, size, &afk->buffer, &dva);
		if (ret)
			break;
		afk->buffer_size = size;
		afk->tag = message & 0xffff;
		/* M4 DART wire addresses keep their 1-TiB bias, 16-KiB aligned. */
		if (!afk->buffer || dva < (1ULL << 40) ||
		    dva - (1ULL << 40) >= (1ULL << 36) || dva & 0x3fff)
			return afk_fail(afk, -EINVAL);
		ret = afk_send(afk, AFK_MSG_BUFFER_REPLY, dva);
		break;
	case AFK_MSG_RING_TX:
	case AFK_MSG_RING_RX:
		index = type == AFK_MSG_RING_TX ? AFK_RING_TX : AFK_RING_RX;
		r = &afk->rings[index];
		other = &afk->rings[!index];
		offset = ((message >> 32) & 0xffff) * 64;
		size = ((message >> 16) & 0xffff) * 64;
		if (!afk->buffer || r->header || afk->stopping || size < 256 ||
		    (message & 0xffff) != afk->tag || offset >= afk->buffer_size ||
		    size > afk->buffer_size - offset || (other->header &&
		    offset < other->offset + other->extent && other->offset < offset + size))
			return afk_fail(afk, -EINVAL);
		/* Ring extent = header (three blocks) + data; the firmware's
		 * data size word gives the negotiated block size.
		 */
		data_size = READ_ONCE(*(u32 *)(afk->buffer + offset));
		if (data_size >= size || (size - data_size) % 3)
			return afk_fail(afk, -EPROTO);
		block = (size - data_size) / 3;
		if (block < 64 || block > 256 || !is_power_of_2(block) ||
		    data_size < block || data_size % block)
			return afk_fail(afk, -EPROTO);
		*r = (struct m4_dcpext_afk_ring){ .header = afk->buffer + offset,
			.data = afk->buffer + offset + 3 * block, .size = data_size,
			.block = block, .offset = offset, .extent = size };
		if (other->header)
			ret = afk_send(afk, AFK_MSG_RINGS_READY, 0);
		break;
	case AFK_MSG_RUNNING:
		if (!afk->rings[AFK_RING_TX].header || !afk->rings[AFK_RING_RX].header ||
		    afk->running || afk->stopping)
			return afk_fail(afk, -EPROTO);
		afk->running = true;
		break;
	case AFK_MSG_DRAIN:
		ret = afk_drain(afk);
		break;
	case AFK_MSG_SHUTDOWN_ACK:
		if (!afk->stopping || !afk_tx_empty(afk))
			return afk_fail(afk, -EPROTO);
		afk->stopped = true;
		break;
	default:
		ret = -EOPNOTSUPP;
	}
	return ret ? afk_fail(afk, ret) : 0;
}

int m4_dcpext_afk_receive(struct m4_dcpext_afk *afk, u64 message)
{
	int ret;

	mutex_lock(&afk->lock);
	ret = afk_receive(afk, message);
	mutex_unlock(&afk->lock);
	return ret;
}

int m4_dcpext_afk_stop(struct m4_dcpext_afk *afk)
{
	int ret;

	mutex_lock(&afk->lock);
	if (afk->error)
		ret = afk->error;
	else if (!afk->running || afk->stopping || !afk_tx_empty(afk))
		ret = -EAGAIN;
	else {
		afk->stopping = true;
		ret = afk_send(afk, AFK_MSG_SHUTDOWN, 0);
	}
	mutex_unlock(&afk->lock);
	return ret;
}

int m4_dcpext_afk_select(struct m4_dcpext_afk *afk, const char *service)
{
	int ret = 0;

	mutex_lock(&afk->lock);
	if (afk->selecting || afk->interface)
		ret = -EBUSY;
	else {
		afk->selecting = true;
		afk->service = service;
	}
	mutex_unlock(&afk->lock);
	return ret;
}

/* V2 transport header plus common header; the caller zeroed the packet. */
static void afk_frame(struct m4_dcpext_afk *afk, u8 *packet, u32 bytes, u8 type, u8 category)
{
	packet[0] = afk->sequence++;
	put_unaligned_le16(afk->interface, packet + 2);
	put_unaligned_le32(bytes - 8, packet + 4);
	packet[16] = type;
	packet[17] = category;
	packet[18] = category ? 0 : 1; /* native open/close report flag */
}

static int afk_report(struct m4_dcpext_afk *afk, u8 type)
{
	u8 packet[M4_DCPEXT_AFK_REPORT] = { 0 };

	if (!afk->interface)
		return -ENOLINK;
	afk_frame(afk, packet, sizeof(packet), type, 0);
	return afk_write(afk, packet, sizeof(packet));
}

int m4_dcpext_afk_open(struct m4_dcpext_afk *afk)
{
	int ret;

	mutex_lock(&afk->lock);
	if (afk->opened)
		ret = -EALREADY;
	else {
		ret = afk_report(afk, M4_DCPEXT_AFK_TYPE_OPEN);
		if (!ret)
			afk->opened = true;
	}
	mutex_unlock(&afk->lock);
	return ret;
}

int m4_dcpext_afk_close(struct m4_dcpext_afk *afk)
{
	int ret;

	mutex_lock(&afk->lock);
	if (!afk->opened || afk->closing)
		ret = -EALREADY;
	else {
		ret = afk_report(afk, M4_DCPEXT_AFK_TYPE_CLOSE);
		if (!ret)
			afk->closing = true;
	}
	mutex_unlock(&afk->lock);
	return ret;
}

bool m4_dcpext_afk_close_consumed(struct m4_dcpext_afk *afk)
{
	bool consumed;

	mutex_lock(&afk->lock);
	consumed = afk->closing && afk_tx_empty(afk);
	mutex_unlock(&afk->lock);
	return consumed;
}

int m4_dcpext_afk_command(struct m4_dcpext_afk *afk, u8 *packet, u32 bytes,
			  u32 capacity, bool oob)
{
	struct m4_dcpext_afk_command *slot;
	u8 tag;
	int ret;

	mutex_lock(&afk->lock);
	if (bytes < M4_DCPEXT_AFK_HEADER) {
		ret = -EINVAL;
		goto out;
	}
	if (!afk->opened || afk->closing) {
		ret = -ENOLINK;
		goto out;
	}
	slot = afk_command_slot(afk, 0);
	if (!slot) {
		ret = -EBUSY;
		goto out;
	}
	/* Tags 1..255; skip tags still outstanding. */
	tag = afk->next_tag;
	do {
		tag = tag == 255 ? 1 : tag + 1;
	} while (afk_command_slot(afk, tag));
	afk_frame(afk, packet, bytes, M4_DCPEXT_AFK_TYPE_COMMAND, 1);
	packet[24] = oob ? 1 : 0;
	packet[25] = tag;
	put_unaligned_le32(capacity, packet + 28);
	ret = afk_write(afk, packet, bytes);
	if (ret) /* Not outstanding; the sequence byte stays consumed as in the PoC. */
		goto out;
	afk->next_tag = tag;
	*slot = (struct m4_dcpext_afk_command){ .tag = tag,
		.type = M4_DCPEXT_AFK_TYPE_COMMAND, .oob = oob };
	ret = tag;
out:
	mutex_unlock(&afk->lock);
	return ret;
}

int m4_dcpext_afk_respond(struct m4_dcpext_afk *afk, const u8 *command,
			  u8 *reply, u32 bytes, u32 status)
{
	int ret;

	mutex_lock(&afk->lock);
	if (bytes < M4_DCPEXT_AFK_HEADER) {
		ret = -EINVAL;
		goto out;
	}
	/* AFKEPCommandRemote::getRspHeaders keeps the incoming identifier and
	 * tag, replaces capacity with status and prepends the common header.
	 */
	afk_frame(afk, reply, bytes, command[16], 2);
	memcpy(reply + 8, command + 8, 8);
	reply[24] = 0; /* Inline replies only */
	reply[25] = command[25];
	put_unaligned_le32(status, reply + 28);
	ret = afk_write(afk, reply, bytes);
out:
	mutex_unlock(&afk->lock);
	return ret;
}

#ifdef __KERNEL__
MODULE_DESCRIPTION("J713 external DisplayPort (DCPEXT) driver");
MODULE_LICENSE("Dual MIT/GPL");
#endif
