// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Hash-qualified J713 DCP protocol; a single kernel client owns all RPCs. */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include "m4_dcp_bridge.h"
#include "m4_dcp_native.h"

#define A(n) M4_DCP_TAG('A', n)
#define D(n) M4_DCP_TAG('D', n)
#define MAX_PROPERTIES 256
#define MAX_RAW_PROPERTIES 32

struct m4_dcp_property {
	u32 service;
	char key[64];
	u64 value;
};

struct m4_dcp_native {
	struct device *dev;
	struct m4_dcp_bridge *bridge;
	struct mutex lock;
	bool failed;
	u32 stride;
	struct m4_dcp_property properties[MAX_PROPERTIES];
	u32 property_count;
	struct { u32 service; char key[64]; void *data; u32 size; } raw[MAX_RAW_PROPERTIES];
	u32 raw_count, raw_bytes;
	void *chunk;
	u32 chunk_size, chunk_offset;
};

static int callback(struct m4_dcp_bridge *b, void *cookie, u32 tag,
		    const void *input, u32 in_size, void *output, u32 out_size);

static int call(struct m4_dcp_native *dcp, u32 tag, const void *in, u32 ins,
		void *out, u32 outs, u32 completion)
{
	int ret;

	if (dcp->failed)
		return -EIO;
	ret = m4_dcp_bridge_call(dcp->bridge, tag, in, ins, out, outs,
				completion, callback, dcp);
	if (ret) {
		dcp->failed = true;
		dev_err(dcp->dev, "M4 kernel RPC %#x failed: %d; recovery requires reboot\n", tag, ret);
	}
	return ret;
}

static int simple_call(struct m4_dcp_native *dcp, u32 tag, bool input, bool reply, bool expected)
{
	__le32 one = cpu_to_le32(1), result = 0;
	int ret = call(dcp, tag, input ? &one : NULL, input ? 4 : 0,
		       reply ? &result : NULL, reply ? 4 : 0, 0);

	if (!ret && reply && le32_to_cpu(result) != expected)
		return -EPROTO;
	return ret;
}

static struct m4_dcp_property *property(struct m4_dcp_native *dcp,
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

static int raw_property(struct m4_dcp_native *dcp, u32 service, const u8 *key,
			const void *data, u32 size)
{
	void *copy;
	u32 i, old_size = 0;

	if (!memchr(key, 0, 64) || size > SZ_1M)
		return -EINVAL;
	for (i = 0; i < dcp->raw_count; i++)
		if (dcp->raw[i].service == service && !strcmp(dcp->raw[i].key, key))
			break;
	if (i == MAX_RAW_PROPERTIES)
		return -ENOSPC;
	if (i < dcp->raw_count)
		old_size = dcp->raw[i].size;
	if (dcp->raw_bytes - old_size + size > SZ_8M)
		return -ENOSPC;
	copy = kvmemdup(data, size ?: 1, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	if (i == dcp->raw_count)
		dcp->raw_count++;
	kvfree(dcp->raw[i].data);
	dcp->raw[i].service = service;
	strscpy(dcp->raw[i].key, key, 64);
	dcp->raw[i].data = copy;
	dcp->raw[i].size = size;
	dcp->raw_bytes += size - old_size;
	return 0;
}

static int callback(struct m4_dcp_bridge *b, void *cookie, u32 tag,
		    const void *input, u32 in_size, void *output, u32 out_size)
{
	struct m4_dcp_native *dcp = cookie;
	const u8 *in = input;
	u8 *out = output;
	struct m4_dcp_property *p;
	struct m4_dcp_buffer buffer = {};
	u32 count, offset, service, key_offset;
	int ret;

#define SHAPE(i, o) (in_size == (i) && out_size == (o))
	/* The transport zeroes output, so absent optional metadata stays absent. */
	if ((tag == D(588) && SHAPE(0, 0)) || (tag == D(590) && SHAPE(0x730, 0)))
		return 0;
	if (tag == D(599) && SHAPE(0, 0))
		return simple_call(dcp, A(412), false, true, true);
	if ((tag == D(108) || tag == D(109) || tag == D(110) || tag == D(111) ||
	     tag == D(112) || tag == D(113) || tag == D(0) || tag == D(1)) && SHAPE(0, 4)) {
		out[0] = 1;
		return 0;
	}
	if (tag == D(575) && SHAPE(88, 76)) {
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
		dcp->chunk = kvzalloc(max_t(u32, 1, dcp->chunk_size), GFP_KERNEL);
		if (!dcp->chunk)
			return -ENOMEM;
		out[0] = 1;
		return 0;
	}
	if (tag == D(128) && SHAPE(0x1008, 4)) {
		offset = get_unaligned_le32(in + 0x1000);
		count = get_unaligned_le32(in + 0x1004);
		if (!dcp->chunk || offset != dcp->chunk_offset || count > 4096 ||
		    offset > dcp->chunk_size || count > dcp->chunk_size - offset)
			return -EINVAL;
		memcpy(dcp->chunk + offset, in, count);
		dcp->chunk_offset += count;
		out[0] = 1;
		return 0;
	}
	if (tag == D(129) && SHAPE(64, 4)) {
		if (!dcp->chunk || dcp->chunk_offset != dcp->chunk_size || !memchr(in, 0, 64) ||
		    dcp->raw_count == MAX_RAW_PROPERTIES || dcp->raw_bytes + dcp->chunk_size > SZ_8M)
			return -EINVAL;
		ret = raw_property(dcp, 0, in, dcp->chunk, dcp->chunk_size);
		if (ret)
			return ret;
		kvfree(dcp->chunk);
		dcp->chunk = NULL;
		dev_info(dcp->dev, "M4 property %.64s: %u bytes\n", in, dcp->chunk_size);
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
	if ((tag == D(122) || tag == D(123)) && SHAPE(0, 4))
		return 0;
	if (tag == D(121) && SHAPE(0, 4)) {
		static const struct { u32 tag; bool in, reply, expected; } sequence[] = {
			{A(389)}, {A(446), false, true, true}, {A(29)},
			{A(468), true}, {A(0), true, true, true}, {A(465), false, true, true},
		};
		for (count = 0; count < ARRAY_SIZE(sequence); count++) {
			ret = simple_call(dcp, sequence[count].tag, sequence[count].in,
					  sequence[count].reply, sequence[count].expected);
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
		put_unaligned_le32(1, out + 56); /* Explicitly decline bandwidth service. */
		return 0;
	}
	if (tag == D(451) && SHAPE(20, 28)) {
		if (get_unaligned_le32(in) != 0x703 || get_unaligned_le32(in + 12) != 4 ||
		    in[16] || in[17] || in[18])
			return -EINVAL;
		buffer.size = get_unaligned_le64(in + 4);
		ret = m4_dcp_bridge_alloc(b, &buffer);
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
		ret = m4_dcp_bridge_map(b, &buffer);
		if (ret)
			return ret;
		put_unaligned_le32(buffer.id, out);
		put_unaligned_le64(buffer.dva, out + 4);
		return 0;
	}
	if (tag == D(454) && SHAPE(4, 4)) {
		ret = m4_dcp_bridge_retire(b, get_unaligned_le32(in));
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if (tag == D(582) && SHAPE(8, 4))
		return 0; /* Optional default surface is not allocated by this host. */
	if (tag == D(400) && SHAPE(76, 0xc04)) {
		count = get_unaligned_le32(in + 68);
		service = get_unaligned_le32(in);
		if (count > 0xc00 || (in[72] & 1) || !memchr(in + 4, 0, 64))
			return -EINVAL;
		for (offset = 0; offset < dcp->raw_count; offset++) {
			if (dcp->raw[offset].service != service || strcmp(dcp->raw[offset].key, in + 4))
				continue;
			if (dcp->raw[offset].size > count)
				return -ENOSPC;
			memcpy(out, dcp->raw[offset].data, dcp->raw[offset].size);
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
		ret = raw_property(dcp, service, in + key_offset, in + key_offset + 64, count);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if ((tag == D(563) && SHAPE(76, 4)) || (tag == D(565) && SHAPE(72, 4)) ||
	    (tag == D(414) && SHAPE(80, 4)) || (tag == D(415) && SHAPE(76, 4)) ||
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
		p->value = count == 8 ? get_unaligned_le64(in + key_offset + 64) :
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
			memset(&dcp->properties[dcp->property_count], 0, sizeof(*p));
		}
		return 0;
	}
	if ((tag == D(206) || tag == D(207)) && SHAPE(0, 4)) {
		ret = simple_call(dcp, tag == D(206) ? A(131) : A(132), false, true, false);
		if (!ret)
			out[0] = 1;
		return ret;
	}
	if (tag == D(100) && SHAPE(0, 0))
		return simple_call(dcp, A(390), false, true, false);
	dev_err(dcp->dev, "M4 unhandled kernel callback %#x %u/%u\n", tag, in_size, out_size);
	return -EOPNOTSUPP;
#undef SHAPE
}

struct m4_dcp_native *m4_dcp_native_start(struct device *dev, struct m4_dcp_bridge *bridge)
{
	struct m4_dcp_native *dcp;
	struct device_node *fb;
	int ret;

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return ERR_PTR(-ENOMEM);
	dcp->dev = dev;
	dcp->bridge = bridge;
	mutex_init(&dcp->lock);
	fb = of_find_compatible_node(NULL, NULL, "simple-framebuffer");
	ret = fb ? of_property_read_u32(fb, "stride", &dcp->stride) : -ENODEV;
	of_node_put(fb);
	if (ret || dcp->stride != 10240)
		return ERR_PTR(-EINVAL);
	ret = simple_call(dcp, A(401), false, true, true);
	if (!ret)
		ret = simple_call(dcp, A(457), false, false, false);
	if (ret)
		return ERR_PTR(ret);
	dev_info(dev, "M4 DCP kernel startup and first-client-open complete\n");
	return dcp;
}

int m4_dcp_native_swap(struct m4_dcp_native *dcp, const void *surface, u64 dva,
		       u32 width, u32 height)
{
	__le32 start[4] = {}, started[2], result[3];
	u8 *swap;
	u32 id;
	int ret;

	if (surface && (!dva || !width || width > 2560 || !height || height > 1664))
		return -EINVAL;
	swap = kzalloc(0x1bd8, GFP_KERNEL);
	if (!swap)
		return -ENOMEM;
	mutex_lock(&dcp->lock);
	ret = call(dcp, A(406), start, sizeof(start), started, sizeof(started), 0);
	if (ret || le32_to_cpu(started[1])) {
		ret = ret ?: -EIO;
		goto out;
	}
	id = le32_to_cpu(started[0]);
	if (!id) {
		ret = -EPROTO;
		goto out;
	}
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
	ret = call(dcp, A(407), swap, 0x1bd8, result, sizeof(result), id);
	if (!ret && get_unaligned_le32((u8 *)result + 5))
		ret = -EIO;
	if (!ret && !surface)
		dev_info(dcp->dev, "M4 kernel swap %u completed, surface=%d\n", id, !!surface);
out:
	if (ret)
		dcp->failed = true;
	mutex_unlock(&dcp->lock);
	kfree(swap);
	return ret;
}

const void *m4_dcp_native_property(struct m4_dcp_native *dcp, const char *key, u32 *size)
{
	u32 i;

	for (i = 0; i < dcp->raw_count; i++) {
		if (!dcp->raw[i].service && !strcmp(dcp->raw[i].key, key)) {
			*size = dcp->raw[i].size;
			return dcp->raw[i].data;
		}
	}
	return NULL;
}
