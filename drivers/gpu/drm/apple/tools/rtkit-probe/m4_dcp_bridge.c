// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Root-only RPC transport for bounded firmware diagnostics, not a display ABI. */
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/uaccess.h>

#include "../../dcp-link.h"
#include "m4_dcp_bridge.h"

#define MAX_EVENTS 32
#define MAX_CALLBACKS 8

/* Write op 1: submit, message=0 (normal) or 1 (nested), data=RPC packet.
 * Write op 2: callback reply, message=original message, data=output bytes.
 * Read kind 1: command reply, kind 2: callback, kind 3: unexpected message.
 * All headers are little endian. One complete record per read/write.
 */
struct record_header {
	__le32 kind;
	__le32 size;
	__le64 message;
};

struct bridge_event {
	struct list_head list;
	struct record_header header;
	u8 data[];
};

struct m4_dcp_bridge {
	struct miscdevice misc;
	struct device *dev;
	struct apple_rtkit *rtkit;
	void *rpc;
	struct mutex lock;
	wait_queue_head_t wait;
	struct list_head events;
	unsigned int event_count, callback_count;
	bool failed;
	bool enabled;
	struct {
		bool active;
		u32 size;
		struct apple_dcp_link_rpc_header header;
	} calls[2];
	struct {
		u64 message;
		void *output;
		u32 size;
	} callbacks[MAX_CALLBACKS];
};

static void fail(struct m4_dcp_bridge *b)
{
	b->failed = true;
	dev_err(b->dev, "DCP bridge stopped; retain firmware buffers until reboot\n");
	wake_up_interruptible(&b->wait);
}

static void event(struct m4_dcp_bridge *b, u32 kind, u64 message,
		  const void *data, u32 size)
{
	struct bridge_event *e;

	if (b->event_count >= MAX_EVENTS || size > APPLE_DCP_LINK_STREAM_BUFFER_SIZE) {
		fail(b);
		return;
	}
	e = kmalloc(sizeof(*e) + size, GFP_KERNEL);
	if (!e) {
		fail(b);
		return;
	}
	e->header = (struct record_header) {
		.kind = cpu_to_le32(kind), .size = cpu_to_le32(size),
		.message = cpu_to_le64(message),
	};
	if (size)
		memcpy(e->data, data, size);
	list_add_tail(&e->list, &b->events);
	b->event_count++;
	wake_up_interruptible(&b->wait);
}

void m4_dcp_bridge_receive(struct m4_dcp_bridge *b, u64 message)
{
	struct apple_dcp_link_stream_layout layout;
	struct apple_dcp_link_rpc_view view;
	u8 stream = apple_dcp_link_message_stream(message);
	bool remote = apple_dcp_link_message_is_remote(message);
	int ret;

	mutex_lock(&b->lock);
	if (b->failed)
		goto out;
	if ((message & 3) != APPLE_DCP_LINK_MSG_RPC) {
		event(b, 3, message, NULL, 0);
		goto out;
	}
	dma_rmb();
	if (message & BIT_ULL(6)) {
		if (stream || !b->calls[remote].active ||
		    apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, 0, remote, &layout))
			goto error;
		if (memcmp(b->rpc + layout.local_offset, &b->calls[remote].header,
			   sizeof(b->calls[remote].header)))
			goto error;
		event(b, 1, message, b->rpc + layout.local_offset, b->calls[remote].size);
		b->calls[remote].active = false;
		goto out;
	}
	if (b->callback_count >= MAX_CALLBACKS ||
	    apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, stream, true, &layout))
		goto error;
	ret = apple_dcp_link_rpc_decode(message, b->rpc + layout.remote_offset,
				      layout.capacity, &view);
	if (ret)
		goto error;
	if ((view.call >> 24) != 'D') {
		u32 off;

		/* Preserve stream-placement evidence without acknowledging bad data. */
		for (off = 0; off < APPLE_DCP_LINK_HEAP_OFFSET;
		     off += APPLE_DCP_LINK_STREAM_BUFFER_SIZE) {
			const __le32 *h = b->rpc + off;

			dev_err(b->dev, "M4 RPC header offset=%#x words=%#x/%#x/%#x\n",
				off, le32_to_cpu(h[0]), le32_to_cpu(h[1]), le32_to_cpu(h[2]));
		}
		goto error;
	}
	b->callbacks[b->callback_count].message = message;
	b->callbacks[b->callback_count].output = view.output;
	b->callbacks[b->callback_count].size = view.output_size;
	b->callback_count++;
	dev_info(b->dev, "M4 DCP callback tag=%#x input=%u output=%u\n",
		 view.call, view.input_size, view.output_size);
	event(b, 2, message, b->rpc + layout.remote_offset + view.offset, view.total_size);
	goto out;
error:
	fail(b);
out:
	mutex_unlock(&b->lock);
}

static ssize_t bridge_read(struct file *file, char __user *user, size_t count, loff_t *pos)
{
	struct m4_dcp_bridge *b = container_of(file->private_data, struct m4_dcp_bridge, misc);
	struct bridge_event *e;
	size_t size;
	int ret;

again:
	mutex_lock(&b->lock);
	if (list_empty(&b->events)) {
		ret = b->failed ? -EIO : -EAGAIN;
		mutex_unlock(&b->lock);
		if (ret == -EIO || file->f_flags & O_NONBLOCK)
			return ret;
		ret = wait_event_interruptible(b->wait, READ_ONCE(b->event_count) || READ_ONCE(b->failed));
		if (ret)
			return ret;
		goto again;
	}
	e = list_first_entry(&b->events, struct bridge_event, list);
	size = sizeof(e->header) + le32_to_cpu(e->header.size);
	ret = -EMSGSIZE;
	if (count < size)
		goto out;
	ret = -EFAULT;
	if (copy_to_user(user, &e->header, size))
		goto out;
	list_del(&e->list);
	b->event_count--;
	kfree(e);
	ret = size;
out:
	mutex_unlock(&b->lock);
	return ret;
}

static ssize_t bridge_write(struct file *file, const char __user *user, size_t count, loff_t *pos)
{
	struct m4_dcp_bridge *b = container_of(file->private_data, struct m4_dcp_bridge, misc);
	struct apple_dcp_link_stream_layout layout;
	struct apple_dcp_link_rpc_header *rpc;
	struct record_header *record;
	u64 message, control;
	u32 kind, size, total;
	bool nested;
	int ret;

	if (count < sizeof(*record) || count > sizeof(*record) + APPLE_DCP_LINK_STREAM_BUFFER_SIZE)
		return -EMSGSIZE;
	record = memdup_user(user, count);
	if (IS_ERR(record))
		return PTR_ERR(record);
	kind = le32_to_cpu(record->kind);
	size = le32_to_cpu(record->size);
	control = le64_to_cpu(record->message);
	ret = -EINVAL;
	if (size != count - sizeof(*record))
		goto free_record;
	mutex_lock(&b->lock);
	if (!b->enabled) {
		ret = -EAGAIN;
		goto out;
	}
	if (b->failed) {
		ret = -EIO;
		goto out;
	}
	if (kind == 2) {
		unsigned int top;

		if (!b->callback_count)
			goto out;
		top = b->callback_count - 1;
		if (control != b->callbacks[top].message || size != b->callbacks[top].size)
			goto out;
		memcpy(b->callbacks[top].output, record + 1, size);
		dma_wmb();
		ret = apple_rtkit_send_message(b->rtkit, APPLE_DCP_LINK_ENDPOINT,
					      apple_dcp_link_rpc_reply(control), NULL, false);
		if (ret)
			fail(b);
		else
			b->callback_count--;
		goto out;
	}
	if (kind != 1 || control > 1 || size < sizeof(*rpc))
		goto out;
	nested = control;
	if (b->calls[nested].active || (nested && !b->callback_count) ||
	    (!nested && b->callback_count)) {
		ret = -EBUSY;
		goto out;
	}
	rpc = (void *)(record + 1);
	ret = apple_dcp_link_rpc_payload_size(le32_to_cpu(rpc->input_size),
					    le32_to_cpu(rpc->output_size), &total);
	if (ret || total != size) {
		ret = -EMSGSIZE;
		goto out;
	}
	ret = apple_dcp_link_stream_layout(APPLE_DCP_LINK_SIDE_AP, 0, nested, &layout);
	if (ret)
		goto out;
	memcpy(b->rpc + layout.local_offset, rpc, size);
	memset(b->rpc + layout.local_offset + sizeof(*rpc) + le32_to_cpu(rpc->input_size),
	       0, le32_to_cpu(rpc->output_size));
	b->calls[nested].header = *rpc;
	b->calls[nested].size = size;
	b->calls[nested].active = true;
	apple_dcp_link_rpc_message(0, size, nested, &message);
	apple_dcp_link_stream_message(message, 0, nested, &message);
	dma_wmb();
	ret = apple_rtkit_send_message(b->rtkit, APPLE_DCP_LINK_ENDPOINT, message, NULL, false);
	if (ret)
		fail(b);
out:
	mutex_unlock(&b->lock);
free_record:
	kfree(record);
	return ret ? ret : count;
}

static __poll_t bridge_poll(struct file *file, poll_table *wait)
{
	struct m4_dcp_bridge *b = container_of(file->private_data, struct m4_dcp_bridge, misc);
	__poll_t mask = 0;

	poll_wait(file, &b->wait, wait);
	if (READ_ONCE(b->event_count))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(b->failed))
		mask |= EPOLLERR;
	return mask;
}

static const struct file_operations bridge_fops = {
	.owner = THIS_MODULE,
	.read = bridge_read,
	.write = bridge_write,
	.poll = bridge_poll,
};

struct m4_dcp_bridge *m4_dcp_bridge_create(struct device *dev,
					struct apple_rtkit *rtkit, void *rpc)
{
	struct m4_dcp_bridge *b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	int ret;

	if (!b)
		return ERR_PTR(-ENOMEM);
	b->dev = dev;
	b->rtkit = rtkit;
	b->rpc = rpc;
	mutex_init(&b->lock);
	init_waitqueue_head(&b->wait);
	INIT_LIST_HEAD(&b->events);
	b->misc = (struct miscdevice) {
		.minor = MISC_DYNAMIC_MINOR, .name = "m4-dcp-rpc", .mode = 0600,
		.fops = &bridge_fops, .parent = dev,
	};
	ret = misc_register(&b->misc);
	if (ret)
		return ERR_PTR(ret);
	return b;
}

void m4_dcp_bridge_activate(struct m4_dcp_bridge *b)
{
	mutex_lock(&b->lock);
	b->enabled = true;
	mutex_unlock(&b->lock);
	dev_info(b->dev, "M4 DCP RPC bridge /dev/m4-dcp-rpc ready\n");
}
