// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022-2024, Advanced Micro Devices, Inc.
 */

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/build_bug.h>
#include <linux/interrupt.h>
#include <linux/dev_printk.h>
#if defined(CONFIG_DEBUG_FS)
#include <linux/seq_file.h>
#endif
#include <linux/irqreturn.h>
#include <linux/pci.h>
#include "amdxdna_mailbox.h"
#include "amdxdna_trace.h"

#define MB_ERR(chann, fmt, args...) \
({ \
	typeof(chann) _chann = chann; \
	dev_err((_chann)->mb->dev, "xdna_mailbox.%d: "fmt, \
		(_chann)->msix_irq, ##args); \
})
#define MB_DBG(chann, fmt, args...) \
({ \
	typeof(chann) _chann = chann; \
	dev_dbg((_chann)->mb->dev, "xdna_mailbox.%d: "fmt, \
		(_chann)->msix_irq, ##args); \
})

#define ASYNC_MSG_START_ID		0x80000000U
/* Message IDs should not be greater than ASYNC_MSG_START_ID */
#define MAGIC_VAL			0x1D000000U
#define MAGIC_VAL_MASK			0xFF000000
#define MAX_MSG_ID_ENTRIES		256
#define MSG_RX_TIMER			200 /* milliseconds */
#define MAILBOX_NAME			"xdna_mailbox"

enum channel_res_type {
	CHAN_RES_X2I,
	CHAN_RES_I2X,
	CHAN_RES_NUM
};

struct mailbox {
	struct device		*dev;
	struct xdna_mailbox_res	res;
	/* protect channel list */
	struct mutex		mbox_lock;
	struct list_head        chann_list;

#if defined(CONFIG_DEBUG_FS)
	struct list_head        res_records;
#endif /* CONFIG_DEBUG_FS */
};

struct mailbox_channel {
	struct mailbox			*mb;
#if defined(CONFIG_DEBUG_FS)
	struct list_head		chann_entry;
#endif
	struct xdna_mailbox_chann_res	res[CHAN_RES_NUM];
	int				msix_irq;
	u32				iohub_int_addr;
	struct idr			chan_idr;
	spinlock_t			chan_idr_lock; /* protect idr operations */
	u32				x2i_tail;

	/* Received msg related fields */
	struct workqueue_struct		*work_q;
	struct work_struct		rx_work;
	u32				i2x_head;

	/* protect async message list */
	spinlock_t			async_msg_lock;
	struct list_head		async_msg_list;
	struct completion		async_comp;
};

struct xdna_msg_header {
	u32 total_size;
	u32 size		: 11;
	u32 rsvd0		: 5;
	u32 protocol_version	: 8;
	u32 rsvd1		: 8;
	u32 id;
	u32 opcode;
} __packed;

static_assert(sizeof(struct xdna_msg_header) == 16);

struct mailbox_pkg {
	struct xdna_msg_header	header;
	u32			payload[];
};

/* The protocol version. */
#define MSG_PROTOCOL_VERSION	0x1
/* The tombstone value. */
#define TOMBSTONE		0xDEADFACE

struct mailbox_msg {
	void			*handle;
	void			(*notify_cb)(void *handle, const u32 *data, size_t size);
	size_t			pkg_size; /* package size in bytes */
	struct mailbox_pkg	pkg;
};

struct mailbox_async_msg {
	struct list_head	 entry;
	struct xdna_mailbox_async msg;
};

#if defined(CONFIG_DEBUG_FS)
struct mailbox_res_record {
	struct list_head		re_entry;
	struct xdna_mailbox_chann_res	re_x2i;
	struct xdna_mailbox_chann_res	re_i2x;
	int				re_irq;
};
#endif /* CONFIG_DEBUG_FS */

static void mailbox_reg_write(struct mailbox_channel *mb_chann, u32 mbox_reg, u32 data)
{
	struct xdna_mailbox_res *mb_res = &mb_chann->mb->res;
	u64 ringbuf_addr = mb_res->mbox_base + mbox_reg;

	iowrite32(data, (void *)ringbuf_addr);
}

static u32 mailbox_reg_read(struct mailbox_channel *mb_chann, u32 mbox_reg)
{
	struct xdna_mailbox_res *mb_res = &mb_chann->mb->res;
	u64 ringbuf_addr = mb_res->mbox_base + mbox_reg;

	return ioread32((void *)ringbuf_addr);
}

static inline void
mailbox_set_headptr(struct mailbox_channel *mb_chann, u32 headptr_val)
{
	mailbox_reg_write(mb_chann, mb_chann->res[CHAN_RES_I2X].mb_head_ptr_reg, headptr_val);
	mb_chann->i2x_head = headptr_val;
}

static inline void
mailbox_set_tailptr(struct mailbox_channel *mb_chann, u32 tailptr_val)
{
	mailbox_reg_write(mb_chann, mb_chann->res[CHAN_RES_X2I].mb_tail_ptr_reg, tailptr_val);
	mb_chann->x2i_tail = tailptr_val;
}

static inline u32
mailbox_get_headptr(struct mailbox_channel *mb_chann, enum channel_res_type type)
{
	return mailbox_reg_read(mb_chann, mb_chann->res[type].mb_head_ptr_reg);
}

static inline u32
mailbox_get_tailptr(struct mailbox_channel *mb_chann, enum channel_res_type type)
{
	return mailbox_reg_read(mb_chann, mb_chann->res[type].mb_tail_ptr_reg);
}

static inline u32
mailbox_get_ringbuf_size(struct mailbox_channel *mb_chann, enum channel_res_type type)
{
	return mb_chann->res[type].rb_size;
}

static inline int mailbox_validate_msgid(int msg_id)
{
	return (msg_id & MAGIC_VAL_MASK) == MAGIC_VAL;
}

static int mailbox_acquire_msgid(struct mailbox_channel *mb_chann, struct mailbox_msg *mb_msg)
{
	unsigned long flags;
	int msg_id;

	spin_lock_irqsave(&mb_chann->chan_idr_lock, flags);
	msg_id = idr_alloc_cyclic(&mb_chann->chan_idr, mb_msg, 0,
				  MAX_MSG_ID_ENTRIES, GFP_NOWAIT);
	spin_unlock_irqrestore(&mb_chann->chan_idr_lock, flags);
	if (msg_id < 0)
		return msg_id;

	/*
	 * The IDR becomes less efficient when dealing with larger IDs.
	 * Thus, add MAGIC_VAL to the higher bits.
	 */
	msg_id |= MAGIC_VAL;
	return msg_id;
}

static void mailbox_release_msgid(struct mailbox_channel *mb_chann, int msg_id)
{
	unsigned long flags = 0;

	msg_id &= ~MAGIC_VAL_MASK;
	spin_lock_irqsave(&mb_chann->chan_idr_lock, flags);
	idr_remove(&mb_chann->chan_idr, msg_id);
	spin_unlock_irqrestore(&mb_chann->chan_idr_lock, flags);
}

static int mailbox_release_msg(int id, void *p, void *data)
{
	struct mailbox_channel *mb_chann = data;
	struct mailbox_msg *mb_msg = p;

	MB_DBG(mb_chann, "msg_id 0x%x msg opcode 0x%x",
	       mb_msg->pkg.header.id, mb_msg->pkg.header.opcode);
	if (mb_msg->notify_cb)
		mb_msg->notify_cb(mb_msg->handle, NULL, 0);
	kfree(mb_msg);

	return 0;
}

static inline int
mailbox_send_msg(struct mailbox_channel *mb_chann, struct mailbox_msg *mb_msg)
{
	u32 ringbuf_size;
	u32 head, tail;
	u32 start_addr;
	u64 write_addr;
	u32 tmp_tail;

	head = mailbox_get_headptr(mb_chann, CHAN_RES_X2I);
	tail = mb_chann->x2i_tail;
	ringbuf_size = mailbox_get_ringbuf_size(mb_chann, CHAN_RES_X2I);
	start_addr = mb_chann->res[CHAN_RES_X2I].rb_start_addr;
	tmp_tail = tail + mb_msg->pkg_size;

	if (tail < head && tmp_tail >= head)
		goto no_space;

	if (tail >= head && (tmp_tail > ringbuf_size - sizeof(u32) &&
			     mb_msg->pkg_size >= head))
		goto no_space;

	if (tail >= head && tmp_tail > ringbuf_size - sizeof(u32)) {
		write_addr = mb_chann->mb->res.ringbuf_base + start_addr + tail;
		iowrite32(TOMBSTONE, (void *)write_addr);

		/* tombstone is set. Write from the start of the ringbuf */
		tail = 0;
	}

	write_addr = mb_chann->mb->res.ringbuf_base + start_addr + tail;
	memcpy_toio((void *)write_addr, &mb_msg->pkg, mb_msg->pkg_size);
	mailbox_set_tailptr(mb_chann, tail + mb_msg->pkg_size);

	trace_mbox_set_tail(MAILBOX_NAME, mb_chann->msix_irq,
			    mb_msg->pkg.header.opcode,
			    mb_msg->pkg.header.id);

	return 0;

no_space:
	return -ENOSPC;
}

static inline void
mailbox_get_resp(struct mailbox_channel *mb_chann, struct xdna_msg_header *header,
		 void *data)
{
	struct mailbox_msg *mb_msg;
	unsigned long flags;
	int msg_id;

	msg_id = header->id;
	if (!mailbox_validate_msgid(msg_id)) {
		MB_DBG(mb_chann, "Bad message ID 0x%x", msg_id);
		return;
	}

	msg_id &= ~MAGIC_VAL_MASK;
	spin_lock_irqsave(&mb_chann->chan_idr_lock, flags);
	mb_msg = idr_find(&mb_chann->chan_idr, msg_id);
	if (!mb_msg) {
		WARN_ONCE(1, "Cannot find msg 0x%x\n", msg_id);
		spin_unlock_irqrestore(&mb_chann->chan_idr_lock, flags);
		return;
	}
	idr_remove(&mb_chann->chan_idr, msg_id);
	spin_unlock_irqrestore(&mb_chann->chan_idr_lock, flags);

	if (mb_msg->notify_cb)
		mb_msg->notify_cb(mb_msg->handle, data, header->size);

	kfree(mb_msg);
}

static inline void
mailbox_get_async_msg(struct mailbox_channel *mb_chann, struct xdna_msg_header *header,
		      u32 *data)
{
	struct mailbox_async_msg *async_msg;
	unsigned long flags;

	/*
	 * Async message is error handling request from device. This is not
	 * critical path. Don't worry about performance.
	 */
	async_msg = kzalloc(sizeof(*async_msg), GFP_KERNEL);
	if (!async_msg)
		return;

	async_msg->msg.opcode = header->opcode;
	memcpy_fromio(async_msg->msg.payload, data, header->size);

	spin_lock_irqsave(&mb_chann->async_msg_lock, flags);
	list_add_tail(&async_msg->entry, &mb_chann->async_msg_list);
	spin_unlock_irqrestore(&mb_chann->async_msg_lock, flags);
	complete(&mb_chann->async_comp);
}

static inline int mailbox_get_msg(struct mailbox_channel *mb_chann)
{
	struct xdna_msg_header header;
	u32 ringbuf_size;
	u32 head, tail;
	u32 start_addr;
	u64 read_addr;
	u32 msg_size;
	u32 val;

	tail = mailbox_get_tailptr(mb_chann, CHAN_RES_I2X);
	head = mb_chann->i2x_head;
	ringbuf_size = mailbox_get_ringbuf_size(mb_chann, CHAN_RES_I2X);
	start_addr = mb_chann->res[CHAN_RES_I2X].rb_start_addr;

	/* ringbuf empty */
	if ((head & (ringbuf_size - 1)) == (tail & (ringbuf_size - 1)))
		return -ENOENT;

	if (head == ringbuf_size)
		head = 0;

	/* Peek size of the message or TOMBSTONE */
	read_addr = mb_chann->mb->res.ringbuf_base + start_addr + head;
	val = ioread32((void *)read_addr);

	/* The first word could be total size or TOMBSTONE */
	if (val == TOMBSTONE) {
		mailbox_set_headptr(mb_chann, 0);
		return 0;
	}

	msg_size = val;
	memcpy_fromio(&header, (void *)read_addr, sizeof(header));
	if (msg_size + sizeof(header) > tail - head) {
		WARN_ONCE(1, "Invalid message size %d, tail %d, head %d\n",
			  msg_size, tail, head);
		return -EINVAL;
	}

	read_addr += sizeof(header);
	if (header.id < ASYNC_MSG_START_ID)
		mailbox_get_resp(mb_chann, &header, (u32 *)read_addr);
	else
		mailbox_get_async_msg(mb_chann, &header, (u32 *)read_addr);

	mailbox_set_headptr(mb_chann, head + sizeof(header) + msg_size);
	/* After update head, it can equal to ringbuf_size. This is expected. */
	trace_mbox_set_head(MAILBOX_NAME, mb_chann->msix_irq,
			    header.opcode, header.id);

	return 0;
}

static irqreturn_t mailbox_irq_handler(int irq, void *p)
{
	struct mailbox_channel *mb_chann = p;

	trace_mbox_irq_handle(MAILBOX_NAME, irq);
	/* Schedule a rx_work to call the callback functions */
	queue_work(mb_chann->work_q, &mb_chann->rx_work);
	/* Clear IOHUB register */
	mailbox_reg_write(mb_chann, mb_chann->iohub_int_addr, 0);

	return IRQ_HANDLED;
}

static void mailbox_rx_worker(struct work_struct *rx_work)
{
	struct mailbox_channel *mb_chann;
	int ret;

	mb_chann = container_of(rx_work, struct mailbox_channel, rx_work);

	while (1) {
		/*
		 * If return is 0, keep consuming next message, until there is
		 * no messages or an error happened.
		 */
		ret = mailbox_get_msg(mb_chann);
		if (ret)
			break;
	}
}

int xdna_mailbox_send_msg(struct mailbox_channel *mb_chann,
			  const struct xdna_mailbox_msg *msg, u64 tx_timeout)
{
	struct xdna_msg_header *header;
	struct mailbox_msg *mb_msg;
	size_t pkg_size;
	int ret;

	pkg_size = sizeof(struct xdna_msg_header) + msg->send_size;
	if (pkg_size > mailbox_get_ringbuf_size(mb_chann, CHAN_RES_X2I)) {
		MB_ERR(mb_chann, "Message size larger than ringbuf size");
		return -EINVAL;
	}

	if (unlikely(!IS_ALIGNED(msg->send_size, 4))) {
		MB_ERR(mb_chann, "Message must be 4 bytes align");
		return -EINVAL;
	}

	/* The fist word in payload can NOT be TOMBSTONE */
	if  (unlikely(((u32 *)msg->send_data)[0] == TOMBSTONE)) {
		MB_ERR(mb_chann, "Tomb stone in data");
		return -EINVAL;
	}

	mb_msg = kzalloc(sizeof(*mb_msg) + pkg_size, GFP_KERNEL);
	if (!mb_msg)
		return -ENOMEM;

	mb_msg->handle = msg->handle;
	mb_msg->notify_cb = msg->notify_cb;
	mb_msg->pkg_size = pkg_size;

	header = &mb_msg->pkg.header;
	/*
	 * Hardware use total_size and size to split huge message.
	 * We do not support it here. Thus the values are the same.
	 */
	header->total_size = msg->send_size;
	header->size = msg->send_size;
	header->opcode = msg->opcode;
	header->protocol_version = MSG_PROTOCOL_VERSION;
	memcpy(mb_msg->pkg.payload, msg->send_data, msg->send_size);

	ret = mailbox_acquire_msgid(mb_chann, mb_msg);
	if (unlikely(ret < 0)) {
		MB_ERR(mb_chann, "mailbox_acquire_msgid failed");
		goto msg_id_failed;
	}
	header->id = ret;

	MB_DBG(mb_chann, "opcode 0x%x size %d id 0x%x",
	       header->opcode, header->total_size, header->id);

	ret = mailbox_send_msg(mb_chann, mb_msg);
	if (ret) {
		MB_DBG(mb_chann, "Error in mailbox send msg, ret %d", ret);
		goto release_id;
	}

	return 0;

release_id:
	mailbox_release_msgid(mb_chann, header->id);
msg_id_failed:
	kfree(mb_msg);
	return ret;
}

int xdna_mailbox_wait_async_msg(struct mailbox_channel *mb_chann,
				struct xdna_mailbox_async *buf, bool blocking)
{
	struct mailbox_async_msg *async_msg;
	unsigned long flags;
	int ret = 0;

	if (!blocking)
		goto skip_wait;

	ret = wait_for_completion_interruptible(&mb_chann->async_comp);
	if (ret)
		return ret;

skip_wait:
	spin_lock_irqsave(&mb_chann->async_msg_lock, flags);
	async_msg = list_first_entry_or_null(&mb_chann->async_msg_list,
					     typeof(*async_msg), entry);
	if (!async_msg) {
		ret = -EAGAIN;
		goto unlock_and_out;
	}
	list_del(&async_msg->entry);

unlock_and_out:
	spin_unlock_irqrestore(&mb_chann->async_msg_lock, flags);

	if (ret)
		return ret;

	memcpy(buf, &async_msg->msg, sizeof(*buf));
	kfree(async_msg);

	return 0;
}

#if defined(CONFIG_DEBUG_FS)
int xdna_mailbox_info_show(struct mailbox *mb, struct seq_file *m)
{
	static const char ring_fmt[] = "%4d  %3s  %5d  0x%08x  0x%04x  ";
	static const char mbox_fmt[] = "0x%08x  0x%08x  0x%04x    0x%04x\n";
	struct mailbox_res_record *record;
	struct mailbox_channel *chann;

	/* If below two puts changed, make sure update fmt[] as well */
	seq_puts(m, "mbox  dir  alive  ring addr   size    ");
	seq_puts(m, "head ptr    tail ptr    head val  tail val\n");

#define xdna_mbox_dump_queue(_dir, _act) \
	{ \
		u32 head_ptr, tail_ptr, head_val, tail_val; \
		u32 rb_start, rb_size; \
		u32 mbox_irq; \
		mbox_irq = record->re_irq; \
		rb_start = record->re_##_dir.rb_start_addr; \
		rb_size = record->re_##_dir.rb_size; \
		head_ptr = record->re_##_dir.mb_head_ptr_reg; \
		tail_ptr = record->re_##_dir.mb_tail_ptr_reg; \
		head_val = ioread32((void *)(mb->res.mbox_base + head_ptr)); \
		tail_val = ioread32((void *)(mb->res.mbox_base + tail_ptr)); \
		seq_printf(m, ring_fmt, mbox_irq, #_dir, _act, rb_start, rb_size); \
		seq_printf(m, mbox_fmt, head_ptr, tail_ptr, head_val, tail_val); \
	}
	mutex_lock(&mb->mbox_lock);
	list_for_each_entry(record, &mb->res_records, re_entry) {
		int active = 0;

		list_for_each_entry(chann, &mb->chann_list, chann_entry) {
			if (record->re_irq == chann->msix_irq)
				active = 1;
		}
		xdna_mbox_dump_queue(x2i, active);
		xdna_mbox_dump_queue(i2x, active);
	}
	mutex_unlock(&mb->mbox_lock);

	return 0;
}

int xdna_mailbox_ringbuf_show(struct mailbox *mb, struct seq_file *m)
{
	struct mailbox_res_record *record;
	const int size = 0x1000;
	void __iomem *base;
	char pfx[15];
	void *buf;

	buf = vzalloc(size);
	if (!buf)
		return -ENOMEM;

#define xdna_mbox_dump_ringbuf(_dir) \
	do { \
		snprintf(pfx, sizeof(pfx), "%s %d: ", #_dir, record->re_irq); \
		memcpy_fromio(buf, base + record->re_##_dir.rb_start_addr, size); \
		seq_hex_dump(m, pfx, DUMP_PREFIX_OFFSET, 16, 4, buf, size, true); \
	} while (0)
	mutex_lock(&mb->mbox_lock);
	base = (void *)mb->res.ringbuf_base;
	list_for_each_entry(record, &mb->res_records, re_entry) {
		xdna_mbox_dump_ringbuf(x2i);
		xdna_mbox_dump_ringbuf(i2x);
	}
	mutex_unlock(&mb->mbox_lock);

	vfree(buf);
	return 0;
}
#endif /* CONFIG_DEBUG_FS */

struct mailbox_channel *
xdna_mailbox_create_channel(struct mailbox *mb,
			    const struct xdna_mailbox_chann_res *x2i,
			    const struct xdna_mailbox_chann_res *i2x,
			    u32 iohub_int_addr,
			    int mb_irq)
{
	struct mailbox_channel *mb_chann = NULL;
	int ret;
#if defined(CONFIG_DEBUG_FS)
	struct mailbox_res_record *record;
	int record_found = 0;

	mutex_lock(&mb->mbox_lock);
	list_for_each_entry(record, &mb->res_records, re_entry) {
		if (record->re_irq != mb_irq)
			continue;

		record_found = 1;
		break;
	}

	if (record_found)
		goto skip_record;

	record = kzalloc(sizeof(*record), GFP_KERNEL);
	if (!record) {
		MB_ERR(mb_chann, "No memory for record");
		mutex_unlock(&mb->mbox_lock);
		return NULL;
	}

	memcpy(&record->re_x2i, x2i, sizeof(*x2i));
	memcpy(&record->re_i2x, i2x, sizeof(*i2x));
	record->re_irq = mb_irq;

	/* Record will be released when mailbox device destroy*/
	list_add_tail(&record->re_entry, &mb->res_records);

skip_record:
	mutex_unlock(&mb->mbox_lock);
#endif /* CONFIG_DEBUG_FS */

	if (!is_power_of_2(x2i->rb_size) || !is_power_of_2(i2x->rb_size)) {
		pr_err("Ring buf size must be power of 2");
		return NULL;
	}

	mb_chann = kzalloc(sizeof(*mb_chann), GFP_KERNEL);
	if (!mb_chann)
		return NULL;

	mb_chann->mb = mb;
	mb_chann->msix_irq = mb_irq;
	mb_chann->iohub_int_addr = iohub_int_addr;
	memcpy(&mb_chann->res[CHAN_RES_X2I], x2i, sizeof(*x2i));
	memcpy(&mb_chann->res[CHAN_RES_I2X], i2x, sizeof(*i2x));

	INIT_LIST_HEAD(&mb_chann->async_msg_list);
	spin_lock_init(&mb_chann->chan_idr_lock);
	spin_lock_init(&mb_chann->async_msg_lock);
	init_completion(&mb_chann->async_comp);
	idr_init(&mb_chann->chan_idr);
	mb_chann->x2i_tail = mailbox_get_tailptr(mb_chann, CHAN_RES_X2I);

	INIT_WORK(&mb_chann->rx_work, mailbox_rx_worker);
	mb_chann->work_q = create_singlethread_workqueue(MAILBOX_NAME);
	if (!mb_chann->work_q) {
		MB_ERR(mb_chann, "Create workqueue failed");
		goto free_and_out;
	}

	/* Everything look good. Time to enable irq handler */
	ret = request_irq(mb_irq, mailbox_irq_handler, 0, MAILBOX_NAME, mb_chann);
	if (ret) {
		MB_ERR(mb_chann, "Failed to request irq %d ret %d", mb_irq, ret);
		goto destroy_wq;
	}

	mutex_lock(&mb->mbox_lock);
	list_add(&mb_chann->chann_entry, &mb->chann_list);
	mutex_unlock(&mb->mbox_lock);

	MB_DBG(mb_chann, "Mailbox channel created (irq: %d)", mb_chann->msix_irq);
	return mb_chann;

destroy_wq:
	destroy_workqueue(mb_chann->work_q);
free_and_out:
	kfree(mb_chann);
	return NULL;
}

int xdna_mailbox_destroy_channel(struct mailbox_channel *mb_chann)
{
	struct mailbox_async_msg *async_msg;
	struct mailbox_async_msg *next;

	if (!mb_chann)
		return 0;

	mutex_lock(&mb_chann->mb->mbox_lock);
	list_del(&mb_chann->chann_entry);
	mutex_unlock(&mb_chann->mb->mbox_lock);

	free_irq(mb_chann->msix_irq, mb_chann);
	destroy_workqueue(mb_chann->work_q);
	/* We can clean up and release resources */

	idr_for_each(&mb_chann->chan_idr, mailbox_release_msg, mb_chann);
	idr_destroy(&mb_chann->chan_idr);

	list_for_each_entry_safe(async_msg, next, &mb_chann->async_msg_list, entry) {
		list_del(&async_msg->entry);
		kfree(async_msg);
	}

	MB_DBG(mb_chann, "Mailbox channel destroyed, irq: %d", mb_chann->msix_irq);
	kfree(mb_chann);
	return 0;
}

struct mailbox *xdna_mailbox_create(struct device *dev,
				    const struct xdna_mailbox_res *res)
{
	struct mailbox *mb;

	mb = devm_kzalloc(dev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return NULL;
	mb->dev = dev;

	/* mailbox and ring buf base and size information */
	memcpy(&mb->res, res, sizeof(*res));

	mutex_init(&mb->mbox_lock);
	INIT_LIST_HEAD(&mb->chann_list);

#if defined(CONFIG_DEBUG_FS)
	INIT_LIST_HEAD(&mb->res_records);
#endif /* CONFIG_DEBUG_FS */

	return mb;
}

void xdna_mailbox_destroy(struct mailbox *mb)
{
#if defined(CONFIG_DEBUG_FS)
	struct mailbox_res_record *record;
	struct mailbox_res_record *next;

	if (list_empty(&mb->res_records))
		goto done_release_record;

	list_for_each_entry_safe(record, next, &mb->res_records, re_entry) {
		list_del(&record->re_entry);
		kfree(record);
	}
done_release_record:
#endif /* CONFIG_DEBUG_FS */

	mutex_lock(&mb->mbox_lock);
	if (!list_empty(&mb->chann_list))
		WARN_ON("Channel not destroy");
	mutex_unlock(&mb->mbox_lock);

	mutex_destroy(&mb->mbox_lock);
}
