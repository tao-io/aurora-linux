/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * AFKEP V2 endpoint channel for the J713 external DCP (DCPEXT).
 *
 * Ring/handshake protocol adapted from Sven Peter's 2022 Asahi Linux afk.c /
 * afk.h (kernel fba91e5d9577), Asahi Linux Contributors. Dynamic ring block
 * sizing (64/128/256 bytes, negotiated per ring) follows Eileen Yoon's
 * MIT-licensed m1n1 proxyclient/m1n1/fw/afk/rbep.py (m1n1 eebff23b5aba).
 * The V2 transport/report/command framing is expressed independently from
 * the J713 native client analysis recorded in DISPLAYPORT-POC.md ("Native
 * AFKEP V2 command reference") and DISPLAY_PORT_FINAL_IMPLEMENTATION.md
 * section 6; OSSerialize publication tags follow m1n1 fw/common.py (MIT).
 * The getModeCount payload used by the offline tests comes from Asahi m1n1
 * fw/dcp/iboot.py (MIT). Predecessors: tools/m4-dcpext-probe/
 * dcpext_afk_probe.h, dcpext_afk_channel.h and dcpext_afk_query.h.
 *
 * Protocol facts encoded here (do not "simplify" them away):
 *  - Every ring packet is {magic "IOP " LE32, payload length LE32, channel
 *    LE32, type LE32} followed by the payload, rounded up to the ring's
 *    negotiated block size. The ring header holds {data size, rptr, wptr},
 *    one block apart. The block size is derived from the firmware's ring
 *    layout message, never assumed to be 64 bytes.
 *  - The payload starts with the eight-byte V2 transport header: sequence
 *    at byte 0, interface id LE16 at 2, following length LE32 at 4.
 *    Byte 1 is not initialised by the native client and is never used as a
 *    version discriminator.
 *  - The 16-byte common header follows: identifier LE64 at 8, type at 16,
 *    category at 17 (report 0 / command 1 / response 2), report flags at 18.
 *    Publications are report 0x11; open/close are reports 0x12 / 0x13 with
 *    identifier 0 and report flag 1.
 *  - Commands/responses add eight bytes: flags at 24 (bit 0 = out-of-band
 *    buffers), tag at 25, response capacity (command) or status (response)
 *    LE32 at 28. Responses are matched by tag; the common 64-bit field need
 *    not echo zero. A 32-byte status-only response envelope is valid.
 *  - Close-report TX consumption and the AFK shutdown ACK (0xc1) are distinct
 *    facts; a separate peer close reply is recorded if it arrives but is
 *    never required or invented.
 *
 * Locking: every public entry point takes afk->lock (a mutex; the RTKit
 * receive path runs from its workqueue and the mailbox send may sleep).
 * The ops callbacks run with afk->lock held and must not call back into
 * this channel. No other lock is taken by this file; the owner serialises
 * lifetime (init/reset) against all callers itself.
 *
 * Ownership: the channel never frees anything. The ring buffer obtained
 * through ops->allocate stays mapped until the owner has seen the AFK
 * shutdown ACK and RTKit quiescence; a failed channel keeps it too.
 */
#ifndef M4_DCPEXT_AFK_H
#define M4_DCPEXT_AFK_H

#ifdef __KERNEL__
#include <linux/mutex.h>
#include <linux/types.h>
#else
/* Host build for the offline tests (tools/test-displayport-afk*.py). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
struct mutex { int held; };
#endif

#define M4_DCPEXT_AFK_CAPTURE 32768	/* Retained raw RX evidence, bytes */
#define M4_DCPEXT_AFK_COMMANDS 8	/* Bounded outstanding commands */
#define M4_DCPEXT_AFK_DEFAULT_PAYLOAD 256
#define M4_DCPEXT_AFK_MAX_PAYLOAD 4096
#define M4_DCPEXT_AFK_HEADER 32		/* V2 + common + command headers */
#define M4_DCPEXT_AFK_REPORT 24		/* V2 + common headers */
#define M4_DCPEXT_AFK_TYPE_PUBLISH 0x11
#define M4_DCPEXT_AFK_TYPE_OPEN 0x12
#define M4_DCPEXT_AFK_TYPE_CLOSE 0x13
#define M4_DCPEXT_AFK_TYPE_COMMAND 0xc0
#define M4_DCPEXT_AFK_STATUS_UNSUPPORTED 0xe00002c7 /* kIOReturnUnsupported */

struct m4_dcpext_afk;

/* Descriptor handed to ops->receive for validated service packets. */
struct m4_dcpext_afk_packet {
	const u8 *data;		/* Whole payload, V2 header first */
	u32 bytes;
	u16 interface;
	u8 type, category;	/* Common header bytes 16 and 17 */
	u8 tag;			/* Commands and responses only */
	u32 status;		/* Responses only, LE32 at 28 */
};

struct m4_dcpext_afk_ops {
	/* Mailbox message to endpoint ep (endpoint 0 for the start message). */
	int (*send)(void *cookie, u8 endpoint, u64 message);
	/* Zeroed, DMA-mapped host buffer of size bytes for the rings. The owner
	 * keeps it mapped; the channel only records the pointer.
	 */
	int (*allocate)(void *cookie, u32 size, u8 **buffer, u64 *dva);
	/* Optional: every consumed RX payload, raw, before interpretation. */
	int (*record)(void *cookie, const u8 *payload, u32 bytes);
	/* Optional: packets on the selected interface after channel-level
	 * validation: matched responses, firmware commands (answer them with
	 * m4_dcpext_afk_respond) and reports other than publication/close.
	 */
	int (*receive)(void *cookie, const struct m4_dcpext_afk_packet *packet);
};

struct m4_dcpext_afk_ring {
	u8 *header, *data;
	u32 size, block, offset, extent;
};

struct m4_dcpext_afk_command {
	u8 tag;			/* 0 = free slot */
	u8 type;
	bool oob;
};

struct m4_dcpext_afk {
	struct mutex lock;
	const struct m4_dcpext_afk_ops *ops;
	void *cookie;
	u8 endpoint;
	/* Transport (0x80 start .. 0xc1 shutdown ACK). */
	bool started, running, stopping, stopped;
	int error;		/* Sticky first error */
	u32 messages, buffer_size, tag;
	u32 max_payload;	/* 0 keeps the 256-byte default; at most 4096 */
	u8 *buffer;
	struct m4_dcpext_afk_ring rings[2];	/* TX, RX */
	/* Service (publication, open/close reports, commands). */
	bool selecting;		/* Publication matching armed by _select */
	const char *service;	/* Full name; NULL = iBoot "disp0-service" */
	u16 interface;		/* 0 until the matching publication arrived */
	u8 sequence, next_tag;
	bool opened, closing, closed;
	struct m4_dcpext_afk_command commands[M4_DCPEXT_AFK_COMMANDS];
	/* Bounded raw RX evidence: {channel, type, bytes, payload} records. */
	u32 records, capture_size;
	u8 capture[M4_DCPEXT_AFK_CAPTURE];
};

/* Allocate with kvzalloc (the capture makes this larger than a page). */
void m4_dcpext_afk_init(struct m4_dcpext_afk *afk, const struct m4_dcpext_afk_ops *ops,
			void *cookie, u8 endpoint);
/* Starts the endpoint (RTKit start message then 0x80). */
int m4_dcpext_afk_start(struct m4_dcpext_afk *afk);
/* Every mailbox message for this endpoint after start. */
int m4_dcpext_afk_receive(struct m4_dcpext_afk *afk, u64 message);
/* Requests the acknowledged shutdown once TX is consumed. */
int m4_dcpext_afk_stop(struct m4_dcpext_afk *afk);
bool m4_dcpext_afk_tx_empty(struct m4_dcpext_afk *afk);
/* Shared publication primitive for a negotiated, mapped TX ring. Caller
 * serializes access and notifies firmware using the returned write pointer.
 * An admission failure leaves the ring and output pointer unchanged. On
 * success all memory must remain owned even if notification later fails.
 */
int m4_dcpext_afk_ring_write(struct m4_dcpext_afk_ring *ring,
			     const u8 *payload, u32 bytes, u32 *published);
/* Raw TX ring publication of a complete payload (framed by the caller). */
int m4_dcpext_afk_write(struct m4_dcpext_afk *afk, const u8 *payload, u32 bytes);
/* Consumes the RX ring; exposed for the offline ring tests. */
int m4_dcpext_afk_drain(struct m4_dcpext_afk *afk);

/* Arms publication matching for one service (see struct field). */
int m4_dcpext_afk_select(struct m4_dcpext_afk *afk, const char *service);
int m4_dcpext_afk_open(struct m4_dcpext_afk *afk);
int m4_dcpext_afk_close(struct m4_dcpext_afk *afk);
/* True once the close report was written and firmware consumed all TX. */
bool m4_dcpext_afk_close_consumed(struct m4_dcpext_afk *afk);
/* Frames and publishes a command whose service payload the caller placed
 * from byte 32 on; bytes covers the whole packet. capacity is the response
 * payload capacity written at 28. Returns the allocated tag (1..255).
 */
int m4_dcpext_afk_command(struct m4_dcpext_afk *afk, u8 *packet, u32 bytes,
			  u32 capacity, bool oob);
/* Frames and publishes the response to a firmware command: identifier, type
 * and tag are copied from command; the caller placed the body from byte 32.
 */
int m4_dcpext_afk_respond(struct m4_dcpext_afk *afk, const u8 *command,
			  u8 *reply, u32 bytes, u32 status);

/* Flat OSSerialize publication parser: 1 matched, 0 other service, -errno.
 * target NULL selects the iBoot "disp0-service" record by its 32-byte name
 * field (it publishes no interface-name property); otherwise the full
 * interface-name property must equal target. Interface ids are dynamic and
 * the publication's interface-id property must equal interface.
 */
int m4_dcpext_afk_publication_named(const u8 *p, u32 bytes, u16 interface,
				    const char *target);
#endif
