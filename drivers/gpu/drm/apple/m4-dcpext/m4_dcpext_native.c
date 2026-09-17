// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* External J713 native client, adapted from the working internal protocol
 * client. READY/A412, A401 and A457 are externally qualified by the retained
 * native-startup-dart-1 trace. Swaps/modes remain gated on further validation;
 * this component is not yet attached to a DRM scanout owner.
 * One owner serializes startup, polling and swaps; buffers remain owned by it.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include "m4_dcpext_rpc.h"
#include "m4_dcpext_native.h"

#define A(n) M4_DCPEXT_TAG('A', n)
#define D(n) M4_DCPEXT_TAG('D', n)
#define MAX_PROPERTIES 256
#define MAX_RAW_PROPERTIES 32

struct m4_dcp_property {
	u32 service;
	char key[64];
	u64 value;
};

struct m4_dcpext_native {
	struct device *dev;
	struct m4_dcpext_rpc *bridge;
	struct mutex lock;
	bool failed;
	bool stopped;
	bool connected;
	bool console_requested;
	u64 mode_generation;
	u64 requested_generation;
	u64 active_mode_generation;
	u64 powered_mode_generation;
	struct m4_dcpext_native_mode *modes;
	u32 mode_count;
	u32 stride;
	struct m4_dcp_property properties[MAX_PROPERTIES];
	u32 property_count;
	struct {
		u32 service;
		char key[64];
		void *data;
		u32 size;
	} raw[MAX_RAW_PROPERTIES];
	u32 raw_count, raw_bytes;
	void *chunk;
	u32 chunk_size, chunk_offset;
	u64 analytics_count;
	u8 analytics_last[4164];
	u64 intent_count;
	u8 intent_last[20];
};

static int callback(struct m4_dcpext_rpc *b, void *cookie, u32 tag,
		    const void *input, u32 in_size, void *output, u32 out_size);
static bool power_request_valid(void *cookie);
static bool swap_request_valid(void *cookie);

static int call(struct m4_dcpext_native *dcp, u32 tag, const void *in, u32 ins,
		void *out, u32 outs, u32 completion)
{
	int ret;

	if (dcp->failed)
		return -EIO;
	ret = m4_dcpext_rpc_call(dcp->bridge, tag, in, ins, out, outs,
				 completion, callback, dcp);
	if (ret) {
		dcp->failed = true;
		dev_err(dcp->dev,
			"DCPEXT native RPC %#x failed: %d; recovery requires reboot\n",
			tag, ret);
	}
	return ret;
}

static int simple_call(struct m4_dcpext_native *dcp, u32 tag, bool input,
		       bool reply, bool expected)
{
	__le32 one = cpu_to_le32(1), result = 0;
	int ret = call(dcp, tag, input ? &one : NULL, input ? 4 : 0,
		       reply ? &result : NULL, reply ? 4 : 0, 0);

	if (!ret && reply && le32_to_cpu(result) != expected) {
		dev_err(dcp->dev, "external RPC %#x result=%#x expected=%u\n",
			tag, le32_to_cpu(result), expected);
		return -EPROTO;
	}
	return ret;
}

static struct m4_dcp_property *property(struct m4_dcpext_native *dcp,
					u32 service, const u8 *key, bool create)
{
	struct m4_dcp_property *p;
	u32 i;

	if (!memchr(key, 0, 64))
		return ERR_PTR(-EINVAL);
	for (i = 0; i < dcp->property_count; i++) {
		p = &dcp->properties[i];
		if (p->service == service && !strcmp(p->key, key))
			return p;
	}
	if (!create)
		return NULL;
	if (dcp->property_count == MAX_PROPERTIES)
		return ERR_PTR(-ENOSPC);
	p = &dcp->properties[dcp->property_count++];
	p->service = service;
	strscpy(p->key, key, sizeof(p->key));
	return p;
}

static int raw_property(struct m4_dcpext_native *dcp, u32 service,
			const u8 *key, const void *data, u32 size)
{
	void *copy;
	struct m4_dcpext_native_mode *modes = NULL;
	u32 mode_count = 0;
	u32 i, old_size = 0;
	int ret;

	if (!memchr(key, 0, 64) || size > SZ_1M)
		return -EINVAL;
	for (i = 0; i < dcp->raw_count; i++)
		if (dcp->raw[i].service == service &&
		    !strcmp(dcp->raw[i].key, key))
			break;
	if (i == MAX_RAW_PROPERTIES)
		return -ENOSPC;
	if (i < dcp->raw_count)
		old_size = dcp->raw[i].size;
	if (dcp->raw_bytes - old_size + size > SZ_8M)
		return -ENOSPC;
	copy = kvzalloc(size ?: 1, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	if (size)
		memcpy(copy, data, size);
	if (!service && !strcmp(key, "TimingElements")) {
		modes = kcalloc(M4_DCPEXT_NATIVE_MAX_MODES, sizeof(*modes), GFP_KERNEL);
		if (!modes) {
			kvfree(copy);
			return -ENOMEM;
		}
		ret = m4_dcpext_native_modes_parse(copy, size, modes,
						 M4_DCPEXT_NATIVE_MAX_MODES, &mode_count);
		if (ret) {
			kfree(modes);
			kvfree(copy);
			return ret;
		}
	}
	if (i == dcp->raw_count)
		dcp->raw_count++;
	kvfree(dcp->raw[i].data);
	dcp->raw[i].service = service;
	strscpy(dcp->raw[i].key, key, 64);
	dcp->raw[i].data = copy;
	dcp->raw[i].size = size;
	dcp->raw_bytes += size - old_size;
	if (modes) {
		kfree(dcp->modes);
		dcp->modes = modes;
		dcp->mode_count = mode_count;
		dcp->mode_generation++;
	}
	return 0;
}

static void remove_raw_property(struct m4_dcpext_native *dcp, const char *key)
{
	for (u32 i = 0; i < dcp->raw_count; i++) {
		if (dcp->raw[i].service || strcmp(dcp->raw[i].key, key))
			continue;
		dcp->raw_bytes -= dcp->raw[i].size;
		kvfree(dcp->raw[i].data);
		dcp->raw[i] = dcp->raw[--dcp->raw_count];
		memset(&dcp->raw[dcp->raw_count], 0, sizeof(dcp->raw[0]));
		break;
	}
	if (!strcmp(key, "TimingElements")) {
		kfree(dcp->modes);
		dcp->modes = NULL;
		dcp->mode_count = 0;
		dcp->active_mode_generation = 0;
		dcp->mode_generation++;
	}
}

static int callback(struct m4_dcpext_rpc *b, void *cookie, u32 tag,
		    const void *input, u32 in_size, void *output, u32 out_size)
{
	struct m4_dcpext_native *dcp = cookie;
	const u8 *in = input;
	u8 *out = output;
	struct m4_dcp_property *p;
	struct m4_dcpext_buffer buffer = {};
	u32 count, offset, service, key_offset;
	int ret;

#define SHAPE(i, o) (in_size == (i) && out_size == (o))
	/* The transport zeroes output, so absent optional metadata stays absent. */
	if (tag == D(592) && SHAPE(20, 0)) {
		/* 25G83 a97be08 -> swap_complete_intent_gated (a968a0c).
		 * Packed u32 id, bool, u32 intent, u32 width, u32 height;
		 * bytes 17..19 are padding. Native accepts intents 6/7/15.
		 * These are firmware-generated intent swaps, distinct from the
		 * submitted A407/D590 fence. Retain their metadata, never use
		 * them to retire a submitted framebuffer or finish its wait. */
		u32 intent = get_unaligned_le32(in + 5);

		if (intent != 6 && intent != 7 && intent != 15)
			return -EOPNOTSUPP;
		dcp->intent_count++;
		memcpy(dcp->intent_last, in, sizeof(dcp->intent_last));
		dev_info(dcp->dev, "DCPEXT intent swap id=%u flag=%u intent=%u size=%ux%u; no submitted buffer retired\n",
			 get_unaligned_le32(in), in[4] & 1, intent,
			 get_unaligned_le32(in + 9), get_unaligned_le32(in + 13));
		return 0;
	}
	if (tag == D(209) && SHAPE(0, 8)) {
		/* 25G83 UPPipe D209 -> get_calendar_time_ms (976f21c):
		 * microtime, seconds * 1000 + usec / 1000.
		 * This is wall-clock time, not a scanout retirement timestamp. */
		put_unaligned_le64(ktime_to_ms(ktime_get_real()), out);
		return 0;
	}
	if (tag == D(115) && SHAPE(16, 8)) {
		/* 25G83 UnifiedPipeline D115 (97841f4): get_tiling_state,
		 * not the older firmware's D115 setter. Optional u32 in/out at
		 * input8/output0, nullable byte12, success bool at output4.
		 * Shim cases1/11 read tiling flags (977d490/977d408). This
		 * single untiled sink has neither flag. Unknown queries return
		 * false and preserve the optional value, never invented state. */
		u32 event = get_unaligned_le32(in);
		u32 param = get_unaligned_le32(in + 4);

		if (in[12] > 1)
			return -EINVAL;
		if (!in[12]) {
			memcpy(out, in + 8, 4);
			if (event == 1 && (param == 1 || param == 11)) {
				put_unaligned_le32(0, out);
				out[4] = 1;
			}
		}
		return 0;
	}
	if (tag == D(114) && SHAPE(4164, 4100)) {
		/* J713 CoreAnalyticsSendEvent: name[64], serialized dictionary[4096],
		 * nullable flag. Retain the latest event locally. The dictionary is
		 * in/out: firmware parses it even on failure, so a zero-filled reply
		 * is invalid. Return its own serialized dictionary without changes.
		 * The final u32 is the method status (zero = accepted).
		 */
		if (!memchr(in, 0, 64) || in[4160] > 1 ||
		    (!in[4160] && in[64] != 'd'))
			return -EINVAL;
		memcpy(dcp->analytics_last, in, sizeof(dcp->analytics_last));
		dcp->analytics_count++;
		if (!in[4160])
			memcpy(out, in + 64, 4096);
		put_unaligned_le32(0, out + 4096);
		dev_info(dcp->dev,
			 "M4 analytics event %llu: %.64s (retained locally)\n",
			 dcp->analytics_count, in);
		return 0;
	}
	if ((tag == D(588) && SHAPE(0, 0)) ||
	    (tag == D(590) && SHAPE(0x730, 0)))
		return 0;
	if (tag == D(599) && SHAPE(0, 0))
		return simple_call(dcp, A(412), false, true, false);
	if (tag == D(598) && SHAPE(0, 4)) {
		/* 25G83 a97c470 reads AP+0x20cc (remote subobject+0x203c).
		 * setProperties at a95cc40/a95cc88 names the flag
		 * IOMFBPreserveContents. This external owner does not request
		 * preserving a closed client's image. False is not a DMA fence. */
		out[0] = 0;
		return 0;
	}
	if (tag == D(595) && SHAPE(4, 0)) {
		/* 25G83 a97c178 -> setSystemConsoleMode. This is an AP console
		 * handoff notification, not a request to change the wire mode.
		 * We register no boot/default console surface (D582), so retain
		 * the request without substituting or retiring a DRM buffer. */
		if (in[0] > 1)
			return -EINVAL;
		dcp->console_requested = in[0];
		dev_info(dcp->dev, "DCPEXT console handoff=%u; no default console surface\n",
			 dcp->console_requested);
		return 0;
	}
	if ((tag == D(108) || tag == D(109) || tag == D(110) || tag == D(111) ||
	     tag == D(112) || tag == D(113) || tag == D(0) || tag == D(1)) &&
	    SHAPE(0, 4)) {
		out[0] = 1;
		return 0;
	}
	if (tag == D(572) && SHAPE(4, 4)) {
		/* Firmware writes only the bool and its nullable byte; the
		 * remaining two bytes are uninitialized padding. */
		if (in[0] > 1 || in[1])
			return -EINVAL;
		return m4_dcpext_rpc_dart_power(b, in[0]);
	}
	/* will_power_off_signal: notification only, not a DMA fence. */
	if (tag == D(2) && SHAPE(0, 0))
		return 0;
	if (tag == D(575) && SHAPE(88, 76)) {
		/* 25G83 callback a97abb4 loads a full u64 argument for
		 * hotPlug_notify_gated (a96103c), not a byte-sized bool.
		 * Only states 0/1 are qualified for this external route. */
		u64 connected = get_unaligned_le64(in);

		if (connected > 1)
			return -EINVAL;
		dcp->connected = connected;
		dcp->mode_generation++;
		if (!dcp->connected)
			remove_raw_property(dcp, "TimingElements");
		if (!(in[84] & 1))
			memcpy(out, in + 8, 76);
		return 0;
	}
	if ((tag == D(576) && SHAPE(4, 0)) || (tag == D(300) && SHAPE(16, 0)))
		return 0;
	if (tag == D(127) && SHAPE(4, 4)) {
		count = get_unaligned_le32(in);
		if (dcp->chunk || !count || count > 0x100001)
			return -EINVAL;
		dcp->chunk_size = count - 1;
		dcp->chunk_offset = 0;
		dcp->chunk =
			kvzalloc(max_t(u32, 1, dcp->chunk_size), GFP_KERNEL);
		if (!dcp->chunk)
			return -ENOMEM;
		out[0] = 1;
		return 0;
	}
	if (tag == D(128) && SHAPE(0x1008, 4)) {
		offset = get_unaligned_le32(in + 0x1000);
		count = get_unaligned_le32(in + 0x1004);
		if (!dcp->chunk || offset != dcp->chunk_offset ||
		    count > 4096 || offset > dcp->chunk_size ||
		    count > dcp->chunk_size - offset)
			return -EINVAL;
		memcpy(dcp->chunk + offset, in, count);
		dcp->chunk_offset += count;
		out[0] = 1;
		return 0;
	}
	if (tag == D(129) && SHAPE(64, 4)) {
		if (!dcp->chunk || dcp->chunk_offset != dcp->chunk_size ||
		    !memchr(in, 0, 64) ||
		    dcp->raw_count == MAX_RAW_PROPERTIES ||
		    dcp->raw_bytes + dcp->chunk_size > SZ_8M)
			return -EINVAL;
		ret = raw_property(dcp, 0, in, dcp->chunk, dcp->chunk_size);
		if (ret)
			return ret;
		kvfree(dcp->chunk);
		dcp->chunk = NULL;
		dev_info(dcp->dev, "M4 property %.64s: %u bytes\n", in,
			 dcp->chunk_size);
		out[0] = 1;
		return 0;
	}
	if (tag == D(408) && SHAPE(8, 8)) {
		count = get_unaligned_le32(in + 4);
		if (memcmp(in, "VORP", 4) || count > 1)
			return -EINVAL;
		/* Qualified host-less fallbacks, not active panel clock metadata. */
		put_unaligned_le64(count ? 24000000 : 48000000, out);
		return 0;
	}
	if (tag == D(125) && SHAPE(100, 36)) {
		count = get_unaligned_le32(in + 64);
		if (count > 8)
			return -EINVAL;
		memcpy(out, in + 68, count * 4);
		return 0;
	}
	if (tag == D(6) && SHAPE(84, 80)) {
		if (!(in[80] & 1))
			memcpy(out, in, 80);
		return 0;
	}
	/* Native D134 calls vi_is_displayX1. This RGB/EDID owner provides
	 * no proprietary display-X1 interface (PHL237E7 on the qualified route). */
	if (tag == D(134) && SHAPE(0, 4))
		return 0;
	if ((tag == D(122) || tag == D(123)) && SHAPE(0, 4))
		return 0;
	if (tag == D(121) && SHAPE(0, 4)) {
		static const struct {
			u32 tag;
			bool in, reply, expected;
		} sequence[] = {
			{ A(389) },
			/* DFB enable-state query, not an operation status.
			 * DCPEXT has no inherited boot framebuffer. */
			{ A(446), false, true, false },
			{ A(29) },
			{ A(468), true },
			{ A(0), true, true, true },
			{ A(465), false, true, true },
		};
		for (count = 0; count < ARRAY_SIZE(sequence); count++) {
			ret = simple_call(dcp, sequence[count].tag,
					  sequence[count].in,
					  sequence[count].reply,
					  sequence[count].expected);
			if (ret)
				return ret;
		}
		out[0] = 1;
		return 0;
	}
	if (tag == D(101) && SHAPE(0, 4)) {
		put_unaligned_le32(dcp->stride, out);
		return 0;
	}
	if (tag == D(3) && SHAPE(4, 60)) {
		put_unaligned_le32(
			1,
			out + 56); /* Explicitly decline bandwidth service. */
		return 0;
	}
	if (tag == D(451) && SHAPE(20, 28)) {
		if (get_unaligned_le32(in) != 0x703 ||
		    get_unaligned_le32(in + 12) != 4 || in[16] || in[17] ||
		    in[18])
			return -EINVAL;
		buffer.size = get_unaligned_le64(in + 4);
		ret = m4_dcpext_rpc_alloc(b, &buffer);
		if (ret)
			return ret;
		put_unaligned_le64(buffer.physical, out);
		put_unaligned_le64(buffer.dva, out + 8);
		put_unaligned_le64(buffer.size, out + 16);
		put_unaligned_le32(buffer.id, out + 24);
		return 0;
	}
	if (tag == D(201) && SHAPE(12, 16)) {
		u64 id = get_unaligned_le64(in);

		if (id > U32_MAX || get_unaligned_le32(in + 8) != 1)
			return -EINVAL;
		buffer.id = id;
		ret = m4_dcpext_rpc_map(b, &buffer);
		if (ret)
			return ret;
		put_unaligned_le32(buffer.id, out);
		put_unaligned_le64(buffer.dva, out + 4);
		return 0;
	}
	if (tag == D(454) && SHAPE(4, 4)) {
		ret = m4_dcpext_rpc_retire(b, get_unaligned_le32(in));
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if (tag == D(582) && SHAPE(8, 4))
		return 0; /* Optional default surface is not allocated by this host. */
	if (tag == D(583) && SHAPE(0, 0))
		/* 25G83 a97b370 -> clear_default_surface at a95af78.
		 * D582 above never allocates an optional default surface, so
		 * there is nothing to clear. Submitted scanout is separate. */
		return 0;
	if (tag == D(400) && SHAPE(76, 0xc04)) {
		count = get_unaligned_le32(in + 68);
		service = get_unaligned_le32(in);
		if (count > 0xc00 || (in[72] & 1) || !memchr(in + 4, 0, 64))
			return -EINVAL;
		for (offset = 0; offset < dcp->raw_count; offset++) {
			if (dcp->raw[offset].service != service ||
			    strcmp(dcp->raw[offset].key, in + 4))
				continue;
			if (dcp->raw[offset].size > count)
				return -ENOSPC;
			memcpy(out, dcp->raw[offset].data,
			       dcp->raw[offset].size);
			put_unaligned_le32(dcp->raw[offset].size, out + 0xc00);
			break;
		}
		return 0;
	}
	if (tag == D(401) && SHAPE(80, 12)) {
		p = property(dcp, get_unaligned_le32(in), in + 4, false);
		if (IS_ERR(p))
			return PTR_ERR(p);
		if (p) {
			put_unaligned_le64(p->value, out);
			out[8] = 1;
		}
		return 0;
	}
	if ((tag == D(413) && SHAPE(4168, 4)) ||
	    ((tag == D(552) || tag == D(561)) && SHAPE(4164, 4)) ||
	    (tag == D(567) && SHAPE(128, 4))) {
		key_offset = tag == D(413) ? 4 : 0;
		service = key_offset ? get_unaligned_le32(in) : 0;
		if (tag != D(567) && (in[key_offset + 4160] & 1)) {
			out[0] = 1;
			return 0;
		}
		count = tag == D(567) ? strnlen(in + 64, 64) : 4096;
		ret = raw_property(dcp, service, in + key_offset,
				   in + key_offset + 64, count);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if ((tag == D(563) && SHAPE(76, 4)) ||
	    (tag == D(565) && SHAPE(72, 4)) ||
	    (tag == D(414) && SHAPE(80, 4)) ||
	    (tag == D(415) && SHAPE(76, 4)) ||
	    ((tag == D(102) || tag == D(104)) && SHAPE(68, 0))) {
		key_offset = (tag == D(414) || tag == D(415)) ? 4 : 0;
		service = key_offset ? get_unaligned_le32(in) : 0;
		count = (tag == D(563) || tag == D(414)) ? 8 : 4;
		if (out_size && (in[key_offset + 64 + count] & 1)) {
			out[0] = 1;
			return 0;
		}
		p = property(dcp, service, in + key_offset, true);
		if (IS_ERR(p))
			return PTR_ERR(p);
		p->value = count == 8 ?
				   get_unaligned_le64(in + key_offset + 64) :
				   get_unaligned_le32(in + key_offset + 64);
		if (tag == D(104))
			p->value = in[64];
		if (out_size)
			out[0] = 1;
		return 0;
	}
	if (tag == D(406) && SHAPE(72, 0))
		return 0;
	if (tag == D(107) && SHAPE(64, 0)) {
		p = property(dcp, 0, in, false);
		if (IS_ERR(p))
			return PTR_ERR(p);
		if (p) {
			*p = dcp->properties[--dcp->property_count];
			memset(&dcp->properties[dcp->property_count], 0,
			       sizeof(*p));
		}
		/* D107 invalidates data publications as well as scalar properties.
		 * The native unplug capture removes TimingElements before D575. */
		remove_raw_property(dcp, in);
		return 0;
	}
	if ((tag == D(206) || tag == D(207)) && SHAPE(0, 4)) {
		/* External native-startup-trace-1 and native-startup-pmu-1:
		 * A131/A132 service-match notifications both return true. The
		 * internal panel's false results are not a universal contract. */
		ret = simple_call(dcp, tag == D(206) ? A(131) : A(132), false,
				  true, true);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if (tag == D(100) && SHAPE(0, 0))
		return simple_call(dcp, A(390), false, true, false);
	dev_err(dcp->dev, "M4 unhandled kernel callback %#x %u/%u\n", tag,
		in_size, out_size);
	return -EOPNOTSUPP;
#undef SHAPE
}

struct m4_dcpext_native *m4_dcpext_native_start(struct device *dev,
						struct m4_dcpext_rpc *bridge,
						u32 stride)
{
	struct m4_dcpext_native *dcp;
	int ret;

	if (!stride || stride > 7680 || (stride & 63))
		return ERR_PTR(-EINVAL);
	dcp = kzalloc(sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return ERR_PTR(-ENOMEM);
	dcp->dev = dev;
	dcp->bridge = bridge;
	mutex_init(&dcp->lock);
	dcp->stride = stride;
	ret = simple_call(dcp, A(401), false, true, true);
	if (!ret)
		ret = simple_call(dcp, A(457), false, false, false);
	if (ret) {
		m4_dcpext_native_destroy(dcp);
		return ERR_PTR(ret);
	}
	dev_info(dev, "M4 DCP kernel startup and first-client-open complete\n");
	return dcp;
}

int m4_dcpext_native_swap(struct m4_dcpext_native *dcp, u64 generation, const void *surface,
			  u64 dva, u32 width, u32 height)
{
	__le32 start[4] = {}, started[2], result[3];
	u8 *swap;
	u32 id;
	int ret;
	bool started_swap = false;

	if (surface &&
	    (!dva || !width || width > 1920 || !height || height > 1080))
		return -EINVAL;
	swap = kzalloc(0x1bd8, GFP_KERNEL);
	if (!swap)
		return -ENOMEM;
	mutex_lock(&dcp->lock);
	if (dcp->failed || dcp->stopped) {
		ret = -ESHUTDOWN;
		goto out;
	}
	dcp->requested_generation = generation;
	ret = m4_dcpext_rpc_call_checked(dcp->bridge, A(406), start, sizeof(start),
					started, sizeof(started), 0, callback, dcp,
					swap_request_valid);
	if (ret || le32_to_cpu(started[1])) {
		ret = ret ?: -EIO;
		goto out;
	}
	id = le32_to_cpu(started[0]);
	if (!id) {
		ret = -EPROTO;
		goto out;
	}
	started_swap = true;
	put_unaligned_le32(id, swap + 0x98);
	put_unaligned_le32(0x80000007, swap + 0x14c);
	put_unaligned_le32(0x80000007, swap + 0x150);
	put_unaligned_le32(surface ? 0xff000000 : 0xff402060, swap + 0x154);
	memset(swap + 0x1bcb, 1, 10);
	swap[0x1bd6] = swap[0x1bd7] = 1;
	if (surface) {
		memcpy(swap + 0x588, surface, 0x22c);
		put_unaligned_le64(dva, swap + 0xe38);
		put_unaligned_le32(1, swap + 0x9c);
		put_unaligned_le32(width, swap + 0xb4);
		put_unaligned_le32(height, swap + 0xb8);
		put_unaligned_le32(width, swap + 0x114);
		put_unaligned_le32(height, swap + 0x118);
		swap[0x1bcb] = 0;
	}
	ret = m4_dcpext_rpc_call_checked(dcp->bridge, A(407), swap, 0x1bd8,
					result, sizeof(result), id, callback, dcp,
					swap_request_valid);
	if (!ret && get_unaligned_le32((u8 *)result + 5))
		ret = -EIO;
	if (!ret && !surface)
		dev_info(dcp->dev, "M4 kernel swap %u completed, surface=%d\n",
			 id, !!surface);
out:
	/* Once A406 allocated a swap, an unplug before A407 leaves a live
	 * obligation without a qualified cancel operation. Retain the session. */
	if (ret && ret != -ESHUTDOWN && (ret != -ESTALE || started_swap))
		dcp->failed = true;
	mutex_unlock(&dcp->lock);
	kfree(swap);
	return ret;
}

struct m4_dcpext_native_mode *m4_dcpext_native_modes(struct m4_dcpext_native *dcp,
						  u32 *count, u64 *generation)
{
	struct m4_dcpext_native_mode *copy;

	if (!count || !generation)
		return ERR_PTR(-EINVAL);
	*count = 0;
	*generation = 0;
	mutex_lock(&dcp->lock);
	if (dcp->failed || dcp->stopped)
		copy = ERR_PTR(-ESHUTDOWN);
	else if (!dcp->connected || !dcp->mode_count)
		copy = ERR_PTR(-ENODATA);
	else {
		copy = kmemdup(dcp->modes, dcp->mode_count * sizeof(*copy), GFP_KERNEL);
		if (!copy)
			copy = ERR_PTR(-ENOMEM);
		else {
			*count = dcp->mode_count;
			*generation = dcp->mode_generation;
		}
	}
	mutex_unlock(&dcp->lock);
	return copy;
}

static bool mode_request_valid(void *cookie)
{
	struct m4_dcpext_native *dcp = cookie;

	return !dcp->failed && !dcp->stopped && dcp->connected &&
	       dcp->mode_generation == dcp->requested_generation;
}

int m4_dcpext_native_set_mode(struct m4_dcpext_native *dcp, u64 generation,
			     u32 color_id, u32 timing_id, u32 pitch)
{
	__le32 request[2] = { cpu_to_le32(color_id), cpu_to_le32(timing_id) };
	__le32 result = 0;
	int ret = -EINVAL;
	u32 i;

	mutex_lock(&dcp->lock);
	if (dcp->failed || dcp->stopped) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (!dcp->connected || generation != dcp->mode_generation) {
		ret = -ESTALE;
		goto out;
	}
	for (i = 0; i < dcp->mode_count; i++)
		if (dcp->modes[i].color_id == color_id && dcp->modes[i].timing_id == timing_id)
			break;
	if (i == dcp->mode_count || pitch < dcp->modes[i].geometry.width * 4 ||
	    pitch > 7680 || (pitch & 63))
		goto out;
	dcp->stride = pitch;
	dcp->requested_generation = generation;
	dcp->active_mode_generation = 0;
	dcp->powered_mode_generation = 0;
	/* Native producer a96bb74 and captured A413: color then timing ID.
	 * Revalidate after the transport drains queued hotplug callbacks.
	 * The result is operation status, not presentation completion. */
	ret = m4_dcpext_rpc_call_checked(dcp->bridge, A(413), request, sizeof(request),
					&result, sizeof(result), 0, callback, dcp,
					mode_request_valid);
	if (ret && ret != -ESTALE)
		dcp->failed = true;
	if (!ret && le32_to_cpu(result)) {
		dev_err(dcp->dev, "DCPEXT native mode %u/%u status %#x\n",
			color_id, timing_id, le32_to_cpu(result));
		dcp->failed = true;
		ret = -EREMOTEIO;
	}
	if (!ret && (!dcp->connected || generation != dcp->mode_generation))
		ret = -ESTALE;
	if (!ret)
		dcp->active_mode_generation = generation;
out:
	mutex_unlock(&dcp->lock);
	return ret;
}

static bool power_request_valid(void *cookie)
{
	struct m4_dcpext_native *dcp = cookie;

	return mode_request_valid(cookie) &&
	       dcp->active_mode_generation == dcp->requested_generation;
}

static bool swap_request_valid(void *cookie)
{
	struct m4_dcpext_native *dcp = cookie;

	return power_request_valid(cookie) &&
	       dcp->powered_mode_generation == dcp->requested_generation;
}

int m4_dcpext_native_set_power(struct m4_dcpext_native *dcp, bool on, u64 generation)
{
	u8 request[16] = {};
	__le32 result[2] = {};
	int ret;

	mutex_lock(&dcp->lock);
	if (dcp->failed || dcp->stopped) {
		ret = -ESHUTDOWN;
		goto out;
	}
	dcp->requested_generation = generation;
	if (on && !power_request_valid(dcp)) {
		ret = -ESTALE;
		goto out;
	}
	/* Native a96e334 packs a u64 state and four bools; the optional
	 * result precedes status. The existing Asahi power-on policy selects
	 * state1 with all optional flags clear; this uses the qualified M4
	 * 16/8 ABI rather than the older firmware's shorter request. */
	put_unaligned_le64(on ? 1 : 0, request);
	dcp->powered_mode_generation = 0;
	ret = m4_dcpext_rpc_call_checked(dcp->bridge, A(473), request, sizeof(request),
					result, sizeof(result), 0, callback, dcp,
					on ? power_request_valid : NULL);
	if (!ret && le32_to_cpu(result[1]))
		ret = -EREMOTEIO;
	if (ret && ret != -ESTALE)
		dcp->failed = true;
	if (!ret && on && !power_request_valid(dcp))
		ret = -ESTALE;
	if (!ret && on)
		dcp->powered_mode_generation = generation;
	dev_info(dcp->dev, "DCPEXT native power=%u result=%#x status=%#x ret=%d; DMA retained\n",
		 on, le32_to_cpu(result[0]), le32_to_cpu(result[1]), ret);
out:
	mutex_unlock(&dcp->lock);
	return ret;
}

int m4_dcpext_native_stop(struct m4_dcpext_native *dcp)
{
	/* 25G83 IOMobileFramebufferAP::last_client_close_dcp at a96da88:
	 * byte 0 indicates a NULL result pointer; the other bytes are padding.
	 * Ask for the optional result, but do not interpret it as a status. */
	u8 close_request[4] = {};
	/* 25G83 setPowerState at a96e334: u64 state, four bools,
	 * nullable-result byte, three padding bytes. Native physical-unplug
	 * and software-disable snapshots independently show all fields zero.
	 * Only the native stack padding differs (0xaa versus zero here). */
	u8 power_request[16] = {};
	__le32 closed = 0, power_result[2] = {};
	int ret = 0;

	mutex_lock(&dcp->lock);
	if (dcp->failed) {
		ret = -EIO;
		goto out;
	}
	if (dcp->stopped)
		goto out;
	ret = call(dcp, A(458), close_request, sizeof(close_request),
		   &closed, sizeof(closed), 0);
	if (ret)
		goto out;
	ret = call(dcp, A(473), power_request, sizeof(power_request),
		   power_result, sizeof(power_result), 0);
	if (!ret && le32_to_cpu(power_result[1])) {
		dev_err(dcp->dev, "DCPEXT native power-off status %#x\n",
			le32_to_cpu(power_result[1]));
		ret = -EREMOTEIO;
	}
	if (ret) {
		dcp->failed = true;
		goto out;
	}
	dcp->stopped = true;
	dev_info(dcp->dev, "DCPEXT native client closed (result %#x), power-off acknowledged; owner quiescence still required\n",
		 le32_to_cpu(closed));
out:
	mutex_unlock(&dcp->lock);
	return ret;
}

void *m4_dcpext_native_property(struct m4_dcpext_native *dcp, const char *key,
				u32 *size)
{
	void *copy = NULL;
	u32 i;

	mutex_lock(&dcp->lock);
	for (i = 0; i < dcp->raw_count; i++) {
		if (!dcp->raw[i].service && !strcmp(dcp->raw[i].key, key)) {
			*size = dcp->raw[i].size;
			copy = kmemdup(dcp->raw[i].data, *size, GFP_KERNEL);
			break;
		}
	}
	mutex_unlock(&dcp->lock);
	return copy;
}

void m4_dcpext_native_analytics_show(struct m4_dcpext_native *dcp,
				     struct seq_file *seq)
{
	mutex_lock(&dcp->lock);
	seq_printf(seq, "events %llu\n", dcp->analytics_count);
	if (dcp->analytics_count) {
		seq_printf(seq, "last_event %.64s\ndictionary_present %u\n",
			   dcp->analytics_last, !dcp->analytics_last[4160]);
		if (!dcp->analytics_last[4160])
			seq_hex_dump(seq, "", DUMP_PREFIX_OFFSET, 16, 1,
				     dcp->analytics_last + 64, 4096, false);
	}
	mutex_unlock(&dcp->lock);
}

/* Poll handles only already queued asynchronous notifications. The owner
 * supplies mailbox events separately and invokes this under its route lock. */
int m4_dcpext_native_poll(struct m4_dcpext_native *dcp)
{
	int ret;
	mutex_lock(&dcp->lock);
	ret = dcp->failed ? -EIO :
			    m4_dcpext_rpc_pump(dcp->bridge, 0, callback, dcp);
	if (ret && ret != -ETIMEDOUT && ret != -EAGAIN)
		dcp->failed = true;
	mutex_unlock(&dcp->lock);
	return ret;
}
void m4_dcpext_native_destroy(struct m4_dcpext_native *dcp)
{
	/* Only CPU-side metadata: DMA allocations belong to the route owner. */
	for (u32 i = 0; i < dcp->raw_count; i++)
		kvfree(dcp->raw[i].data);
	kvfree(dcp->chunk);
	kfree(dcp->modes);
	kfree(dcp);
}
