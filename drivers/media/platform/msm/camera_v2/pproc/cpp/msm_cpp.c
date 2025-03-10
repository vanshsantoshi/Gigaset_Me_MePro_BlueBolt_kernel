/* Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "MSM-CPP %s:%d " fmt, __func__, __LINE__


#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/ion.h>
#include <linux/proc_fs.h>
#include <linux/msm_ion.h>
#include <linux/iommu.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/msm_iommu_domains.h>
#include <linux/clk/msm-clk.h>
#include <linux/qcom_iommu.h>
#include <media/msm_isp.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/msmb_camera.h>
#include <media/msmb_generic_buf_mgr.h>
#include <media/msmb_pproc.h>
#include <linux/clk/msm-clk-provider.h>
#include "msm_cpp.h"
#include "msm_isp_util.h"
#include "msm_camera_io_util.h"
#include <linux/debugfs.h>

#define MSM_CPP_DRV_NAME "msm_cpp"

#define MSM_CPP_MAX_BUFF_QUEUE	16

#define CONFIG_MSM_CPP_DBG	0

#define ENABLE_CPP_LOW		0

#define CPP_CMD_TIMEOUT_MS	300
#define MSM_CPP_INVALID_OFFSET	0x00000000
#define MSM_CPP_NOMINAL_CLOCK	266670000
#define MSM_CPP_TURBO_CLOCK	320000000

#define CPP_FW_VERSION_1_2_0	0x10020000
#define CPP_FW_VERSION_1_4_0	0x10040000
#define CPP_FW_VERSION_1_6_0	0x10060000
#define CPP_FW_VERSION_1_8_0	0x10080000
#define CPP_FW_VERSION_1_10_0	0x10100000

/* stripe information offsets in frame command */
#define STRIPE_BASE_FW_1_2_0	130
#define STRIPE_BASE_FW_1_4_0	140
#define STRIPE_BASE_FW_1_6_0	464
#define STRIPE_BASE_FW_1_8_0	493
#define STRIPE_BASE_FW_1_10_0	553

#define PLANE_BASE_FW_1_8_0	478
#define PLANE_BASE_FW_1_10_0	481

/* dump the frame command before writing to the hardware */
#define  MSM_CPP_DUMP_FRM_CMD 0

#define CPP_CLK_INFO_MAX 16

static int msm_cpp_buffer_ops(struct cpp_device *cpp_dev,
	uint32_t buff_mgr_ops, struct msm_buf_mngr_info *buff_mgr_info);
static int msm_cpp_send_frame_to_hardware(struct cpp_device *cpp_dev,
	struct msm_queue_cmd *frame_qcmd);
static int msm_cpp_send_command_to_hardware(struct cpp_device *cpp_dev,
	uint32_t *cmd_msg, uint32_t payload_size);

#if CONFIG_MSM_CPP_DBG
#define CPP_DBG(fmt, args...) pr_err(fmt, ##args)
#else
#define CPP_DBG(fmt, args...) pr_debug(fmt, ##args)
#endif

#define CPP_LOW(fmt, args...) do { \
	if (ENABLE_CPP_LOW) \
		pr_info(fmt, ##args); \
	} while (0)

#define ERR_USER_COPY(to) pr_err("copy %s user\n", \
			((to) ? "to" : "from"))
#define ERR_COPY_FROM_USER() ERR_USER_COPY(0)

/* CPP bus bandwidth definitions */
static struct msm_bus_vectors msm_cpp_init_vectors[] = {
	{
		.src = MSM_BUS_MASTER_CPP,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_vectors msm_cpp_ping_vectors[] = {
	{
		.src = MSM_BUS_MASTER_CPP,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_vectors msm_cpp_pong_vectors[] = {
	{
		.src = MSM_BUS_MASTER_CPP,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};



static struct msm_bus_paths msm_cpp_bus_client_config[] = {
	{
		ARRAY_SIZE(msm_cpp_init_vectors),
		msm_cpp_init_vectors,
	},
	{
		ARRAY_SIZE(msm_cpp_ping_vectors),
		msm_cpp_ping_vectors,
	},
	{
		ARRAY_SIZE(msm_cpp_pong_vectors),
		msm_cpp_pong_vectors,
	},
};

static struct msm_bus_scale_pdata msm_cpp_bus_scale_data = {
	msm_cpp_bus_client_config,
	ARRAY_SIZE(msm_cpp_bus_client_config),
	.name = "msm_camera_cpp",
};

#define msm_dequeue(queue, member) ({	   \
	unsigned long flags;		  \
	struct msm_device_queue *__q = (queue);	 \
	struct msm_queue_cmd *qcmd = 0;	   \
	spin_lock_irqsave(&__q->lock, flags);	 \
	if (!list_empty(&__q->list)) {		\
		__q->len--;		 \
		qcmd = list_first_entry(&__q->list,   \
		struct msm_queue_cmd, member);  \
		list_del_init(&qcmd->member);	 \
	}			 \
	spin_unlock_irqrestore(&__q->lock, flags);  \
	qcmd;			 \
})

#define MSM_CPP_MAX_TIMEOUT_TRIAL 0

struct msm_cpp_timer_data_t {
	struct cpp_device *cpp_dev;
	struct msm_cpp_frame_info_t *processed_frame[MAX_CPP_PROCESSING_FRAME];
	spinlock_t processed_frame_lock;
};

struct msm_cpp_timer_t {
	atomic_t used;
	struct msm_cpp_timer_data_t data;
	struct timer_list cpp_timer;
};

struct msm_cpp_timer_t cpp_timer;

static int msm_cpp_is_tnr_enabled(struct cpp_device *cpp_dev,
	uint32_t *cpp_frame_msg)
{
	if (cpp_frame_msg && cpp_dev) {
		if (((cpp_dev->fw_version & 0xffff0000) ==
			CPP_FW_VERSION_1_8_0) &&
			((cpp_frame_msg[PLANE_BASE_FW_1_8_0] & 0x40) ||
			(cpp_frame_msg[PLANE_BASE_FW_1_8_0 + 5] & 0x40) ||
			(cpp_frame_msg[PLANE_BASE_FW_1_8_0 + 10] & 0x40))) {
			return 1;
		} else if (((cpp_dev->fw_version & 0xffff0000) ==
			CPP_FW_VERSION_1_10_0) &&
			((cpp_frame_msg[PLANE_BASE_FW_1_10_0] & 0x40) ||
			(cpp_frame_msg[PLANE_BASE_FW_1_10_0 + 24] & 0x40) ||
			(cpp_frame_msg[PLANE_BASE_FW_1_10_0 + 48] & 0x40))) {
			return 1;
		}
	}
	return 0;
}

static int msm_cpp_init_bandwidth_mgr(struct cpp_device *cpp_dev)
{
	int rc = 0;

	cpp_dev->bus_client =
		msm_bus_scale_register_client(&msm_cpp_bus_scale_data);
	if (!cpp_dev->bus_client) {
		pr_err("Fail to register bus client\n");
		return -ENOENT;
	}

	rc = msm_bus_scale_client_update_request(cpp_dev->bus_client, 1);
	if (rc < 0) {
		pr_err("Fail bus scale update %d\n", rc);
		return -EINVAL;
	}
	cpp_dev->bus_idx = 1;

	return 0;
}

static int msm_cpp_update_bandwidth(struct cpp_device *cpp_dev,
	uint64_t ab, uint64_t ib)
{

	int rc;
	struct msm_bus_paths *path;

	path = &(msm_cpp_bus_scale_data.usecase[cpp_dev->bus_idx]);
	path->vectors[0].ab = ab;
	path->vectors[0].ib = ib;

	rc = msm_bus_scale_client_update_request(cpp_dev->bus_client,
		 cpp_dev->bus_idx);
	if (rc < 0) {
		pr_err("Fail bus scale update %d\n", rc);
		return -EINVAL;
	}
	cpp_dev->bus_idx = 3 - cpp_dev->bus_idx;

	return 0;
}

void msm_cpp_deinit_bandwidth_mgr(struct cpp_device *cpp_dev)
{
	if (cpp_dev->bus_client) {
		msm_bus_scale_unregister_client(cpp_dev->bus_client);
		cpp_dev->bus_client = 0;
	}
}

static void msm_queue_init(struct msm_device_queue *queue, const char *name)
{
	CPP_DBG("E\n");
	spin_lock_init(&queue->lock);
	queue->len = 0;
	queue->max = 0;
	queue->name = name;
	INIT_LIST_HEAD(&queue->list);
	init_waitqueue_head(&queue->wait);
}

static void msm_enqueue(struct msm_device_queue *queue,
			struct list_head *entry)
{
	unsigned long flags;
	spin_lock_irqsave(&queue->lock, flags);
	queue->len++;
	if (queue->len > queue->max) {
		queue->max = queue->len;
		pr_info("queue %s new max is %d\n", queue->name, queue->max);
	}
	list_add_tail(entry, &queue->list);
	wake_up(&queue->wait);
	CPP_DBG("woke up %s\n", queue->name);
	spin_unlock_irqrestore(&queue->lock, flags);
}

#define msm_cpp_empty_list(queue, member) { \
	unsigned long flags; \
	struct msm_queue_cmd *qcmd = NULL; \
	if (queue) { \
		spin_lock_irqsave(&queue->lock, flags); \
		while (!list_empty(&queue->list)) { \
			queue->len--; \
			qcmd = list_first_entry(&queue->list, \
				struct msm_queue_cmd, member); \
			list_del_init(&qcmd->member); \
			kfree(qcmd); \
		} \
		spin_unlock_irqrestore(&queue->lock, flags); \
	} \
}

static struct msm_cam_clk_info cpp_clk_info[CPP_CLK_INFO_MAX];

static int get_clock_index(const char *clk_name)
{
	uint32_t i = 0;
	for (i = 0; i < ARRAY_SIZE(cpp_clk_info); i++) {
		if (!strcmp(clk_name, cpp_clk_info[i].clk_name))
			return i;
	}
	return -EINVAL;
}


static int msm_cpp_notify_frame_done(struct cpp_device *cpp_dev,
	uint8_t put_buf);
static void cpp_load_fw(struct cpp_device *cpp_dev, char *fw_name_bin);
static void cpp_timer_callback(unsigned long data);

uint8_t induce_error;
static int msm_cpp_enable_debugfs(struct cpp_device *cpp_dev);

static void msm_cpp_write(u32 data, void __iomem *cpp_base)
{
	writel_relaxed((data), cpp_base + MSM_CPP_MICRO_FIFO_RX_DATA);
}

static void msm_cpp_clear_timer(struct cpp_device *cpp_dev)
{
	uint32_t i = 0;

	if (atomic_read(&cpp_timer.used)) {
		atomic_set(&cpp_timer.used, 0);
		del_timer(&cpp_timer.cpp_timer);
		for (i = 0; i < MAX_CPP_PROCESSING_FRAME; i++)
			cpp_timer.data.processed_frame[i] = NULL;
		cpp_dev->timeout_trial_cnt = 0;
	}
}

static void msm_cpp_timer_queue_update(struct cpp_device *cpp_dev)
{
	uint32_t i;
	unsigned long flags;
	CPP_DBG("Frame done qlen %d\n", cpp_dev->processing_q.len);
	if (cpp_dev->processing_q.len <= 1) {
		msm_cpp_clear_timer(cpp_dev);
	} else {
		spin_lock_irqsave(&cpp_timer.data.processed_frame_lock, flags);
		for (i = 0; i < cpp_dev->processing_q.len - 1; i++)
			cpp_timer.data.processed_frame[i] =
				cpp_timer.data.processed_frame[i + 1];
		cpp_timer.data.processed_frame[i] = NULL;
		cpp_dev->timeout_trial_cnt = 0;
		spin_unlock_irqrestore(&cpp_timer.data.processed_frame_lock,
			flags);

		mod_timer(&cpp_timer.cpp_timer,
			jiffies + msecs_to_jiffies(CPP_CMD_TIMEOUT_MS));
	}
}

static uint32_t msm_cpp_read(void __iomem *cpp_base)
{
	uint32_t tmp, retry = 0;
	do {
		tmp = msm_camera_io_r(cpp_base + MSM_CPP_MICRO_FIFO_TX_STAT);
	} while (((tmp & 0x2) == 0x0) && (retry++ < 10));
	if (retry < 10) {
		tmp = msm_camera_io_r(cpp_base + MSM_CPP_MICRO_FIFO_TX_DATA);
		CPP_DBG("Read data: 0%x\n", tmp);
	} else {
		CPP_DBG("Read failed\n");
		tmp = 0xDEADBEEF;
	}

	return tmp;
}

static struct msm_cpp_buff_queue_info_t *msm_cpp_get_buff_queue_entry(
	struct cpp_device *cpp_dev, uint32_t session_id, uint32_t stream_id)
{
	uint32_t i = 0;
	struct msm_cpp_buff_queue_info_t *buff_queue_info = NULL;

	for (i = 0; i < cpp_dev->num_buffq; i++) {
		if ((cpp_dev->buff_queue[i].used == 1) &&
			(cpp_dev->buff_queue[i].session_id == session_id) &&
			(cpp_dev->buff_queue[i].stream_id == stream_id)) {
			buff_queue_info = &cpp_dev->buff_queue[i];
			break;
		}
	}

	if (buff_queue_info == NULL) {
		CPP_DBG("buffer queue entry for sess:%d strm:%d not found\n",
			session_id, stream_id);
	}
	return buff_queue_info;
}

static unsigned long msm_cpp_get_phy_addr(struct cpp_device *cpp_dev,
	struct msm_cpp_buff_queue_info_t *buff_queue_info, uint32_t buff_index,
	uint8_t native_buff, int32_t *fd)
{
	unsigned long phy_add = 0;
	struct list_head *buff_head;
	struct msm_cpp_buffer_map_list_t *buff, *save;

	if (native_buff)
		buff_head = &buff_queue_info->native_buff_head;
	else
		buff_head = &buff_queue_info->vb2_buff_head;

	list_for_each_entry_safe(buff, save, buff_head, entry) {
		if (buff->map_info.buff_info.index == buff_index) {
			phy_add = buff->map_info.phy_addr;
			*fd = buff->map_info.buff_info.fd;
			break;
		}
	}

	return phy_add;
}

static unsigned long msm_cpp_queue_buffer_info(struct cpp_device *cpp_dev,
	struct msm_cpp_buff_queue_info_t *buff_queue,
	struct msm_cpp_buffer_info_t *buffer_info)
{
	struct list_head *buff_head;
	struct msm_cpp_buffer_map_list_t *buff, *save;
	int rc = 0;

	if (buffer_info->native_buff)
		buff_head = &buff_queue->native_buff_head;
	else
		buff_head = &buff_queue->vb2_buff_head;

	list_for_each_entry_safe(buff, save, buff_head, entry) {
		if (buff->map_info.buff_info.index == buffer_info->index) {
			pr_err("error buffer index already queued\n");
			return -EINVAL;
		}
	}

	buff = kzalloc(
		sizeof(struct msm_cpp_buffer_map_list_t), GFP_KERNEL);
	if (!buff) {
		pr_err("error allocating memory\n");
		return -EINVAL;
	}

	buff->map_info.buff_info = *buffer_info;
	buff->map_info.ion_handle = ion_import_dma_buf(cpp_dev->client,
		buffer_info->fd);
	if (IS_ERR_OR_NULL(buff->map_info.ion_handle)) {
		pr_err("ION import failed\n");
		goto queue_buff_error1;
	}
	rc = ion_map_iommu(cpp_dev->client, buff->map_info.ion_handle,
		cpp_dev->domain_num, 0, SZ_4K, 0,
		&buff->map_info.phy_addr,
		&buff->map_info.len, 0, 0);
	if (rc < 0) {
		pr_err("ION mmap failed\n");
		goto queue_buff_error2;
	}

	INIT_LIST_HEAD(&buff->entry);
	list_add_tail(&buff->entry, buff_head);

	return buff->map_info.phy_addr;

queue_buff_error2:
	ion_free(cpp_dev->client, buff->map_info.ion_handle);
queue_buff_error1:
	buff->map_info.ion_handle = NULL;
	kzfree(buff);

	return 0;
}

static void msm_cpp_dequeue_buffer_info(struct cpp_device *cpp_dev,
	struct msm_cpp_buffer_map_list_t *buff)
{

	ion_unmap_iommu(cpp_dev->client, buff->map_info.ion_handle,
		cpp_dev->domain_num, 0);
	ion_free(cpp_dev->client, buff->map_info.ion_handle);
	buff->map_info.ion_handle = NULL;

	list_del_init(&buff->entry);
	kzfree(buff);

	return;
}

static unsigned long msm_cpp_fetch_buffer_info(struct cpp_device *cpp_dev,
	struct msm_cpp_buffer_info_t *buffer_info, uint32_t session_id,
	uint32_t stream_id, int32_t *fd)
{
	unsigned long phy_addr = 0;
	struct msm_cpp_buff_queue_info_t *buff_queue_info;
	uint8_t native_buff = buffer_info->native_buff;

	buff_queue_info = msm_cpp_get_buff_queue_entry(cpp_dev, session_id,
		stream_id);
	if (buff_queue_info == NULL) {
		pr_err("error finding buffer queue entry for sessid:%d strmid:%d\n",
			session_id, stream_id);
		return phy_addr;
	}

	phy_addr = msm_cpp_get_phy_addr(cpp_dev, buff_queue_info,
		buffer_info->index, native_buff, fd);
	if ((phy_addr == 0) && (native_buff)) {
		phy_addr = msm_cpp_queue_buffer_info(cpp_dev, buff_queue_info,
			buffer_info);
		*fd = buffer_info->fd;
	}
	return phy_addr;
}

static int32_t msm_cpp_dequeue_buff_info_list(struct cpp_device *cpp_dev,
	struct msm_cpp_buff_queue_info_t *buff_queue_info)
{
	struct msm_cpp_buffer_map_list_t *buff, *save;
	struct list_head *buff_head;

	buff_head = &buff_queue_info->native_buff_head;
	list_for_each_entry_safe(buff, save, buff_head, entry) {
		msm_cpp_dequeue_buffer_info(cpp_dev, buff);
	}

	buff_head = &buff_queue_info->vb2_buff_head;
	list_for_each_entry_safe(buff, save, buff_head, entry) {
		msm_cpp_dequeue_buffer_info(cpp_dev, buff);
	}

	return 0;
}

static int32_t msm_cpp_dequeue_buff(struct cpp_device *cpp_dev,
	struct msm_cpp_buff_queue_info_t *buff_queue_info, uint32_t buff_index,
	uint8_t native_buff)
{
	struct msm_cpp_buffer_map_list_t *buff, *save;
	struct list_head *buff_head;

	if (native_buff)
		buff_head = &buff_queue_info->native_buff_head;
	else
		buff_head = &buff_queue_info->vb2_buff_head;

	list_for_each_entry_safe(buff, save, buff_head, entry) {
		if (buff->map_info.buff_info.index == buff_index) {
			msm_cpp_dequeue_buffer_info(cpp_dev, buff);
			break;
		}
	}

	return 0;
}

static int32_t msm_cpp_add_buff_queue_entry(struct cpp_device *cpp_dev,
	uint16_t session_id, uint16_t stream_id)
{
	uint32_t i;
	struct msm_cpp_buff_queue_info_t *buff_queue_info;

	for (i = 0; i < cpp_dev->num_buffq; i++) {
		if (cpp_dev->buff_queue[i].used == 0) {
			buff_queue_info = &cpp_dev->buff_queue[i];
			buff_queue_info->used = 1;
			buff_queue_info->session_id = session_id;
			buff_queue_info->stream_id = stream_id;
			INIT_LIST_HEAD(&buff_queue_info->vb2_buff_head);
			INIT_LIST_HEAD(&buff_queue_info->native_buff_head);
			return 0;
		}
	}
	pr_err("buffer queue full. error for sessionid: %d streamid: %d\n",
		session_id, stream_id);
	return -EINVAL;
}

static int32_t msm_cpp_free_buff_queue_entry(struct cpp_device *cpp_dev,
	uint32_t session_id, uint32_t stream_id)
{
	struct msm_cpp_buff_queue_info_t *buff_queue_info;

	buff_queue_info = msm_cpp_get_buff_queue_entry(cpp_dev, session_id,
		stream_id);
	if (buff_queue_info == NULL) {
		pr_err("error finding buffer queue entry for sessid:%d strmid:%d\n",
			session_id, stream_id);
		return -EINVAL;
	}

	buff_queue_info->used = 0;
	buff_queue_info->session_id = 0;
	buff_queue_info->stream_id = 0;
	INIT_LIST_HEAD(&buff_queue_info->vb2_buff_head);
	INIT_LIST_HEAD(&buff_queue_info->native_buff_head);
	return 0;
}

static int32_t msm_cpp_create_buff_queue(struct cpp_device *cpp_dev,
	uint32_t num_buffq)
{
	struct msm_cpp_buff_queue_info_t *buff_queue;
	buff_queue = kzalloc(
		sizeof(struct msm_cpp_buff_queue_info_t) * num_buffq,
		GFP_KERNEL);
	if (!buff_queue) {
		pr_err("Buff queue allocation failure\n");
		return -ENOMEM;
	}

	if (cpp_dev->buff_queue) {
		pr_err("Buff queue not empty\n");
		kzfree(buff_queue);
		return -EINVAL;
	} else {
		cpp_dev->buff_queue = buff_queue;
		cpp_dev->num_buffq = num_buffq;
	}
	return 0;
}

static void msm_cpp_delete_buff_queue(struct cpp_device *cpp_dev)
{
	uint32_t i;

	for (i = 0; i < cpp_dev->num_buffq; i++) {
		if (cpp_dev->buff_queue[i].used == 1) {
			pr_info("Queue not free sessionid: %d, streamid: %d\n",
				cpp_dev->buff_queue[i].session_id,
				cpp_dev->buff_queue[i].stream_id);
			msm_cpp_dequeue_buff_info_list
				(cpp_dev, &cpp_dev->buff_queue[i]);
			msm_cpp_free_buff_queue_entry(cpp_dev,
				cpp_dev->buff_queue[i].session_id,
				cpp_dev->buff_queue[i].stream_id);
		}
	}
	kzfree(cpp_dev->buff_queue);
	cpp_dev->buff_queue = NULL;
	cpp_dev->num_buffq = 0;
	return;
}

static void msm_cpp_poll(void __iomem *cpp_base, u32 val)
{
	uint32_t tmp, retry = 0;
	do {
		usleep_range(1000, 2000);
		tmp = msm_cpp_read(cpp_base);
		if (tmp != 0xDEADBEEF)
			CPP_LOW("poll: 0%x\n", tmp);
	} while ((tmp != val) && (retry++ < MSM_CPP_POLL_RETRIES));
	if (retry < MSM_CPP_POLL_RETRIES)
		CPP_LOW("Poll finished\n");
	else
		pr_err("Poll failed: expect: 0x%x\n", val);
}

static void msm_cpp_poll_rx_empty(void __iomem *cpp_base)
{
	uint32_t tmp, retry = 0;

	tmp = msm_camera_io_r(cpp_base + MSM_CPP_MICRO_FIFO_RX_STAT);
	while (((tmp & 0x2) != 0x0) && (retry++ < MSM_CPP_POLL_RETRIES)) {
		/*
		* Below usleep values are chosen based on experiments
		* and this was the smallest number which works. This
		* sleep is needed to leave enough time for Microcontroller
		* to read rx fifo.
		*/
		usleep_range(200, 300);
		tmp = msm_camera_io_r(cpp_base + MSM_CPP_MICRO_FIFO_RX_STAT);
	}

	if (retry < MSM_CPP_POLL_RETRIES)
		CPP_LOW("Poll rx empty\n");
	else
		pr_err("Poll rx empty failed\n");
}

void cpp_release_ion_client(struct kref *ref)
{
	struct cpp_device *cpp_dev = container_of(ref,
		struct cpp_device, refcount);
	pr_err("Calling ion_client_destroy\n");
	ion_client_destroy(cpp_dev->client);
}

static int cpp_init_mem(struct cpp_device *cpp_dev)
{
	int rc = 0;

	kref_init(&cpp_dev->refcount);
	#ifndef VENDOR_EDIT
	/*liuyan 2015/7/21 delete, memleak*/
	//kref_get(&cpp_dev->refcount);
	#endif
	cpp_dev->client = msm_ion_client_create("cpp");

	CPP_DBG("E\n");
	if (!cpp_dev->domain) {
		pr_err("domain / iommu context not found\n");
		return  -ENODEV;
	}

	CPP_DBG("X\n");
	return rc;
}

static void cpp_deinit_mem(struct cpp_device *cpp_dev)
{
	CPP_DBG("E\n");
	kref_put(&cpp_dev->refcount, cpp_release_ion_client);
	CPP_DBG("X\n");
}

static irqreturn_t msm_cpp_irq(int irq_num, void *data)
{
	unsigned long flags;
	uint32_t tx_level;
	uint32_t irq_status;
	uint32_t i;
	uint32_t tx_fifo[MSM_CPP_TX_FIFO_LEVEL];
	struct cpp_device *cpp_dev = data;
	struct msm_cpp_tasklet_queue_cmd *queue_cmd;
	irq_status = msm_camera_io_r(cpp_dev->base + MSM_CPP_MICRO_IRQGEN_STAT);

	if (irq_status & 0x8) {
		tx_level = msm_camera_io_r(cpp_dev->base +
			MSM_CPP_MICRO_FIFO_TX_STAT) >> 2;
		for (i = 0; i < tx_level; i++) {
			tx_fifo[i] = msm_camera_io_r(cpp_dev->base +
				MSM_CPP_MICRO_FIFO_TX_DATA);
		}
		spin_lock_irqsave(&cpp_dev->tasklet_lock, flags);
		queue_cmd = &cpp_dev->tasklet_queue_cmd[cpp_dev->taskletq_idx];
		if (queue_cmd->cmd_used) {
			pr_err("%s: cpp tasklet queue overflow\n", __func__);
			list_del(&queue_cmd->list);
		} else {
			atomic_add(1, &cpp_dev->irq_cnt);
		}
		queue_cmd->irq_status = irq_status;
		queue_cmd->tx_level = tx_level;
		memset(&queue_cmd->tx_fifo[0], 0, sizeof(queue_cmd->tx_fifo));
		for (i = 0; i < tx_level; i++)
			queue_cmd->tx_fifo[i] = tx_fifo[i];

		queue_cmd->cmd_used = 1;
		cpp_dev->taskletq_idx =
			(cpp_dev->taskletq_idx + 1) % MSM_CPP_TASKLETQ_SIZE;
		list_add_tail(&queue_cmd->list, &cpp_dev->tasklet_q);
		spin_unlock_irqrestore(&cpp_dev->tasklet_lock, flags);

		tasklet_schedule(&cpp_dev->cpp_tasklet);
	} else if (irq_status & 0x7C0) {
		pr_debug("irq_status: 0x%x\n", irq_status);
		pr_debug("DEBUG_SP: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x40));
		pr_debug("DEBUG_T: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x44));
		pr_debug("DEBUG_N: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x48));
		pr_debug("DEBUG_R: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x4C));
		pr_debug("DEBUG_OPPC: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x50));
		pr_debug("DEBUG_MO: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x54));
		pr_debug("DEBUG_TIMER0: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x60));
		pr_debug("DEBUG_TIMER1: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x64));
		pr_debug("DEBUG_GPI: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x70));
		pr_debug("DEBUG_GPO: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x74));
		pr_debug("DEBUG_T0: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x80));
		pr_debug("DEBUG_R0: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x84));
		pr_debug("DEBUG_T1: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x88));
		pr_debug("DEBUG_R1: 0x%x\n",
			msm_camera_io_r(cpp_dev->base + 0x8C));
	}
	msm_camera_io_w(irq_status, cpp_dev->base + MSM_CPP_MICRO_IRQGEN_CLR);
	return IRQ_HANDLED;
}

void msm_cpp_do_tasklet(unsigned long data)
{
	unsigned long flags;
	uint32_t irq_status;
	uint32_t tx_level;
	uint32_t msg_id, cmd_len;
	uint32_t i;
	uint32_t tx_fifo[MSM_CPP_TX_FIFO_LEVEL];
	struct cpp_device *cpp_dev = (struct cpp_device *) data;
	struct msm_cpp_tasklet_queue_cmd *queue_cmd;

	while (atomic_read(&cpp_dev->irq_cnt)) {
		spin_lock_irqsave(&cpp_dev->tasklet_lock, flags);
		queue_cmd = list_first_entry(&cpp_dev->tasklet_q,
		struct msm_cpp_tasklet_queue_cmd, list);
		if (!queue_cmd) {
			atomic_set(&cpp_dev->irq_cnt, 0);
			spin_unlock_irqrestore(&cpp_dev->tasklet_lock, flags);
			return;
		}
		atomic_sub(1, &cpp_dev->irq_cnt);
		list_del(&queue_cmd->list);
		queue_cmd->cmd_used = 0;
		irq_status = queue_cmd->irq_status;
		tx_level = queue_cmd->tx_level;
		for (i = 0; i < tx_level; i++)
			tx_fifo[i] = queue_cmd->tx_fifo[i];

		spin_unlock_irqrestore(&cpp_dev->tasklet_lock, flags);

		for (i = 0; i < tx_level; i++) {
			if (tx_fifo[i] == MSM_CPP_MSG_ID_CMD) {
				cmd_len = tx_fifo[i+1];
				msg_id = tx_fifo[i+2];
				if (msg_id == MSM_CPP_MSG_ID_FRAME_ACK) {
					CPP_DBG("Frame done!!\n");
					/* delete CPP timer */
					CPP_DBG("delete timer.\n");
					msm_cpp_timer_queue_update(cpp_dev);
					msm_cpp_notify_frame_done(cpp_dev, 0);
				} else if (msg_id ==
					MSM_CPP_MSG_ID_FRAME_NACK) {
					pr_err("NACK error from hw!!\n");
					CPP_DBG("delete timer.\n");
					msm_cpp_timer_queue_update(cpp_dev);
					msm_cpp_notify_frame_done(cpp_dev, 0);
				}
				i += cmd_len + 2;
			}
		}
	}
}

static void cpp_get_clk_freq_tbl(struct clk *clk, struct cpp_hw_info *hw_info,
	uint32_t min_clk_rate)
{
	uint32_t i;
	uint32_t idx = 0;
	signed long freq_tbl_entry = 0;

	if ((clk == NULL) || (hw_info == NULL) || (clk->ops == NULL) ||
		(clk->ops->list_rate == NULL)) {
		pr_err("Bad parameter\n");
		return;
	}

	for (i = 0; i < MAX_FREQ_TBL; i++) {
		freq_tbl_entry = clk->ops->list_rate(clk, i);
		pr_debug("entry=%ld\n", freq_tbl_entry);
		if (freq_tbl_entry >= 0) {
			if (freq_tbl_entry >= min_clk_rate) {
				hw_info->freq_tbl[idx++] = freq_tbl_entry;
				pr_debug("tbl[%d]=%ld\n", idx-1,
					freq_tbl_entry);
			}
		} else {
			pr_debug("freq table returned invalid entry/end %ld\n",
				freq_tbl_entry);
			break;
		}
	}

	pr_debug("%s: idx %d", __func__, idx);
	hw_info->freq_tbl_count = idx;
}

static void msm_cpp_calculate_stripe_base(struct cpp_device *cpp_dev)
{
	if ((cpp_dev->fw_version & 0xffff0000) ==
		CPP_FW_VERSION_1_2_0) {
		cpp_dev->stripe_base = STRIPE_BASE_FW_1_2_0;
		cpp_dev->stripe_info_offset = 12;
		cpp_dev->stripe_size = 27;
		cpp_dev->rd_pntr = 5;
		cpp_dev->wr_0_pntr = 11;
		cpp_dev->wr_1_pntr = 12;
		cpp_dev->wr_2_pntr = 13;
		cpp_dev->wr_3_pntr = 14;
		cpp_dev->rd_ref_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_ref_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_0_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_1_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_2_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_3_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
	} else if ((cpp_dev->fw_version & 0xffff0000) ==
		CPP_FW_VERSION_1_4_0) {
		cpp_dev->stripe_base = STRIPE_BASE_FW_1_4_0;
		cpp_dev->stripe_info_offset = 12;
		cpp_dev->stripe_size = 27;
		cpp_dev->rd_pntr = 5;
		cpp_dev->wr_0_pntr = 11;
		cpp_dev->wr_1_pntr = 12;
		cpp_dev->wr_2_pntr = 13;
		cpp_dev->wr_3_pntr = 14;
		cpp_dev->rd_ref_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_ref_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_0_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_1_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_2_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_3_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
	} else if ((cpp_dev->fw_version & 0xffff0000) ==
		CPP_FW_VERSION_1_6_0) {
		cpp_dev->stripe_base = STRIPE_BASE_FW_1_6_0;
		cpp_dev->stripe_info_offset = 12;
		cpp_dev->stripe_size = 27;
		cpp_dev->rd_pntr = 5;
		cpp_dev->wr_0_pntr = 11;
		cpp_dev->wr_1_pntr = 12;
		cpp_dev->wr_2_pntr = 13;
		cpp_dev->wr_3_pntr = 14;
		cpp_dev->rd_ref_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_ref_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_0_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_1_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_2_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_3_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
	} else if ((cpp_dev->fw_version & 0xffff0000) ==
		CPP_FW_VERSION_1_8_0) {
		cpp_dev->stripe_base = STRIPE_BASE_FW_1_8_0;
		cpp_dev->stripe_info_offset = 9;
		cpp_dev->stripe_size = 48;
		cpp_dev->rd_pntr = 8;
		cpp_dev->wr_0_pntr = 20;
		cpp_dev->wr_1_pntr = 21;
		cpp_dev->wr_2_pntr = 22;
		cpp_dev->wr_3_pntr = 23;
		cpp_dev->rd_ref_pntr = 14;
		cpp_dev->wr_ref_pntr = 30;
		cpp_dev->wr_0_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_1_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_2_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
		cpp_dev->wr_3_meta_data_wr_pntr = MSM_CPP_INVALID_OFFSET;
	} else if ((cpp_dev->fw_version & 0xffff0000) ==
		CPP_FW_VERSION_1_10_0) {
		cpp_dev->stripe_base = STRIPE_BASE_FW_1_10_0;
		cpp_dev->stripe_info_offset = 9;
		cpp_dev->stripe_size = 61;
		cpp_dev->rd_pntr = 11;
		cpp_dev->wr_0_pntr = 23;
		cpp_dev->wr_1_pntr = 24;
		cpp_dev->wr_2_pntr = 25;
		cpp_dev->wr_3_pntr = 26;
		cpp_dev->rd_ref_pntr = 17;
		cpp_dev->wr_ref_pntr = 36;
		cpp_dev->wr_0_meta_data_wr_pntr = 42;
		cpp_dev->wr_1_meta_data_wr_pntr = 43;
		cpp_dev->wr_2_meta_data_wr_pntr = 44;
		cpp_dev->wr_3_meta_data_wr_pntr = 45;
	}
	return;
}

static int cpp_init_hardware(struct cpp_device *cpp_dev)
{
	int rc = 0;
	uint32_t msm_cpp_core_clk_idx;
	uint32_t msm_micro_iface_idx;
	uint32_t vbif_version;

	if (cpp_dev->bus_master_flag)
		rc = msm_cpp_init_bandwidth_mgr(cpp_dev);
	else
		rc = msm_isp_init_bandwidth_mgr(ISP_CPP);
	if (rc < 0) {
		pr_err("%s: Bandwidth registration Failed!\n", __func__);
		goto bus_scale_register_failed;
	}

	if (cpp_dev->fs_cpp == NULL) {
		cpp_dev->fs_cpp =
			regulator_get(&cpp_dev->pdev->dev, "vdd");
		if (IS_ERR(cpp_dev->fs_cpp)) {
			pr_err("Regulator cpp vdd get failed %ld\n",
				PTR_ERR(cpp_dev->fs_cpp));
			rc = -EINVAL;
			cpp_dev->fs_cpp = NULL;
			goto fs_failed;
		}

		rc = regulator_enable(cpp_dev->fs_cpp);
		if (rc != 0) {
			pr_err("Regulator cpp vdd enable failed\n");
			regulator_put(cpp_dev->fs_cpp);
			cpp_dev->fs_cpp = NULL;
			goto fs_failed;
		}
	}
	msm_micro_iface_idx = get_clock_index("micro_iface_clk");
	if (msm_micro_iface_idx < 0)  {
		pr_err("Fail to get clock index\n");
		rc = msm_micro_iface_idx;
		goto fs_failed;
	}

	cpp_dev->cpp_clk[msm_micro_iface_idx] =
		clk_get(&cpp_dev->pdev->dev,
		cpp_clk_info[msm_micro_iface_idx].clk_name);
	if (IS_ERR(cpp_dev->cpp_clk[msm_micro_iface_idx])) {
		pr_err("%s get failed\n",
		cpp_clk_info[msm_micro_iface_idx].clk_name);
		rc = PTR_ERR(cpp_dev->cpp_clk[msm_micro_iface_idx]);
		goto remap_failed;
	}

	rc = clk_reset(cpp_dev->cpp_clk[msm_micro_iface_idx],
		CLK_RESET_ASSERT);
	if (rc) {
		pr_err("%s:micro_iface_clk assert failed\n",
		__func__);
		clk_put(cpp_dev->cpp_clk[msm_micro_iface_idx]);
		goto remap_failed;
	}
	/*Below usleep values are chosen based on experiments
	and this was the smallest number which works. This
	sleep is needed to leave enough time for Microcontroller
	to resets all its registers.*/
	usleep_range(10000, 12000);

	rc = clk_reset(cpp_dev->cpp_clk[msm_micro_iface_idx],
		CLK_RESET_DEASSERT);
	if (rc) {
		pr_err("%s:micro_iface_clk assert failed\n", __func__);
		clk_put(cpp_dev->cpp_clk[msm_micro_iface_idx]);
		goto remap_failed;
	}
	/*Below usleep values are chosen based on experiments and
	this was the smallest number which works. This sleep is
	needed to leave enough time for Microcontroller to
	resets all its registers.*/
	usleep_range(1000, 1200);

	clk_put(cpp_dev->cpp_clk[msm_micro_iface_idx]);

	rc = msm_cam_clk_enable(&cpp_dev->pdev->dev, cpp_clk_info,
			cpp_dev->cpp_clk, cpp_dev->num_clk, 1);
	if (rc < 0) {
		pr_err("clk enable failed\n");
		goto clk_failed;
	}

	cpp_dev->base = ioremap(cpp_dev->mem->start,
		resource_size(cpp_dev->mem));
	if (!cpp_dev->base) {
		rc = -ENOMEM;
		pr_err("ioremap failed\n");
		goto remap_failed;
	}

	cpp_dev->vbif_base = ioremap(cpp_dev->vbif_mem->start,
		resource_size(cpp_dev->vbif_mem));
	if (!cpp_dev->vbif_base) {
		rc = -ENOMEM;
		pr_err("ioremap failed\n");
		goto vbif_remap_failed;
	}

	cpp_dev->cpp_hw_base = ioremap(cpp_dev->cpp_hw_mem->start,
		resource_size(cpp_dev->cpp_hw_mem));
	if (!cpp_dev->cpp_hw_base) {
		rc = -ENOMEM;
		pr_err("ioremap failed\n");
		goto cpp_hw_remap_failed;
	}

	if (cpp_dev->state != CPP_STATE_BOOT) {
		rc = request_irq(cpp_dev->irq->start, msm_cpp_irq,
			IRQF_TRIGGER_RISING, "cpp", cpp_dev);
		if (rc < 0) {
			pr_err("irq request fail\n");
			goto req_irq_fail;
		}
		cpp_dev->buf_mgr_subdev = msm_buf_mngr_get_subdev();

		rc = msm_cpp_buffer_ops(cpp_dev,
			VIDIOC_MSM_BUF_MNGR_INIT, NULL);
		if (rc < 0) {
			pr_err("buf mngr init failed\n");
			free_irq(cpp_dev->irq->start, cpp_dev);
			goto req_irq_fail;
		}
	}

	cpp_dev->hw_info.cpp_hw_version =
		msm_camera_io_r(cpp_dev->cpp_hw_base);
	if (cpp_dev->hw_info.cpp_hw_version == CPP_HW_VERSION_4_1_0) {
		vbif_version = msm_camera_io_r(cpp_dev->vbif_base);
		if (vbif_version == VBIF_VERSION_2_3_0)
			cpp_dev->hw_info.cpp_hw_version = CPP_HW_VERSION_4_0_0;
	}
	pr_info("CPP HW Version: 0x%x\n", cpp_dev->hw_info.cpp_hw_version);
	cpp_dev->hw_info.cpp_hw_caps =
		msm_camera_io_r(cpp_dev->cpp_hw_base + 0x4);
	msm_cpp_core_clk_idx = get_clock_index("cpp_core_clk");
	if (msm_cpp_core_clk_idx < 0)  {
		pr_err("cpp_core_clk: fail to get clock index\n");
		rc = msm_cpp_core_clk_idx;
		goto req_irq_fail;
	}
	cpp_get_clk_freq_tbl(cpp_dev->cpp_clk[msm_cpp_core_clk_idx],
		&cpp_dev->hw_info, cpp_dev->min_clk_rate);
	pr_debug("CPP HW Caps: 0x%x\n", cpp_dev->hw_info.cpp_hw_caps);
	msm_camera_io_w(0x1, cpp_dev->vbif_base + 0x4);
	cpp_dev->taskletq_idx = 0;
	atomic_set(&cpp_dev->irq_cnt, 0);
	rc = msm_cpp_create_buff_queue(cpp_dev, MSM_CPP_MAX_BUFF_QUEUE);
	if (rc < 0) {
		pr_err("%s: create buff queue failed with err %d\n",
			__func__, rc);
		goto req_irq_fail;
	}
	pr_err("stream_cnt:%d\n", cpp_dev->stream_cnt);
	cpp_dev->stream_cnt = 0;
	if (cpp_dev->is_firmware_loaded == 1) {
		disable_irq(cpp_dev->irq->start);
		cpp_load_fw(cpp_dev, cpp_dev->fw_name_bin);
		enable_irq(cpp_dev->irq->start);
		msm_camera_io_w_mb(0x7C8, cpp_dev->base +
			MSM_CPP_MICRO_IRQGEN_MASK);
		msm_camera_io_w_mb(0xFFFF, cpp_dev->base +
			MSM_CPP_MICRO_IRQGEN_CLR);
	}

	return rc;
req_irq_fail:
	iounmap(cpp_dev->cpp_hw_base);
cpp_hw_remap_failed:
	iounmap(cpp_dev->vbif_base);
vbif_remap_failed:
	iounmap(cpp_dev->base);
remap_failed:
	msm_cam_clk_enable(&cpp_dev->pdev->dev, cpp_clk_info,
		cpp_dev->cpp_clk, cpp_dev->num_clk, 0);
clk_failed:
	regulator_disable(cpp_dev->fs_cpp);
	regulator_put(cpp_dev->fs_cpp);
fs_failed:
		if (cpp_dev->bus_master_flag)
			msm_cpp_deinit_bandwidth_mgr(cpp_dev);
		else
			msm_isp_deinit_bandwidth_mgr(ISP_CPP);
bus_scale_register_failed:
	return rc;
}

static void cpp_release_hardware(struct cpp_device *cpp_dev)
{
	int32_t rc;
	if (cpp_dev->state != CPP_STATE_BOOT) {
		rc = msm_cpp_buffer_ops(cpp_dev,
			VIDIOC_MSM_BUF_MNGR_DEINIT, NULL);
		if (rc < 0) {
			pr_err("error in buf mngr deinit\n");
			rc = -EINVAL;
		}
		free_irq(cpp_dev->irq->start, cpp_dev);
		tasklet_kill(&cpp_dev->cpp_tasklet);
		atomic_set(&cpp_dev->irq_cnt, 0);
	}
	msm_cpp_delete_buff_queue(cpp_dev);
	iounmap(cpp_dev->base);
	iounmap(cpp_dev->vbif_base);
	iounmap(cpp_dev->cpp_hw_base);
	msm_cam_clk_enable(&cpp_dev->pdev->dev, cpp_clk_info,
		cpp_dev->cpp_clk, cpp_dev->num_clk, 0);
	regulator_disable(cpp_dev->fs_cpp);
	regulator_put(cpp_dev->fs_cpp);
	cpp_dev->fs_cpp = NULL;
	if (cpp_dev->stream_cnt > 0) {
		pr_info("stream count active\n");
		if (cpp_dev->bus_master_flag)
			rc = msm_cpp_update_bandwidth(cpp_dev, 0, 0);
		else
			rc = msm_isp_update_bandwidth(ISP_CPP, 0, 0);
	}
	cpp_dev->stream_cnt = 0;
	if (cpp_dev->bus_master_flag)
		msm_cpp_deinit_bandwidth_mgr(cpp_dev);
	else
		msm_isp_deinit_bandwidth_mgr(ISP_CPP);
}

static void cpp_load_fw(struct cpp_device *cpp_dev, char *fw_name_bin)
{
	uint32_t i;
	uint32_t *ptr_bin = NULL;
	int32_t rc = -EFAULT;
	const struct firmware *fw = NULL;
	struct device *dev = &cpp_dev->pdev->dev;

	msm_camera_io_w(0x1, cpp_dev->base + MSM_CPP_MICRO_CLKEN_CTL);
	msm_camera_io_w(0x1, cpp_dev->base +
				 MSM_CPP_MICRO_BOOT_START);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);

	if (fw_name_bin) {
		pr_debug("%s: FW file: %s\n", __func__, fw_name_bin);
		rc = request_firmware(&fw, fw_name_bin, dev);
		if (rc) {
			dev_err(dev,
				"Fail to loc blob %s from dev %pK, Error: %d\n",
				fw_name_bin, dev, rc);
		}
		if (NULL != fw)
			ptr_bin = (uint32_t *)fw->data;

		msm_camera_io_w(0x1, cpp_dev->base +
					 MSM_CPP_MICRO_BOOT_START);
		msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
		msm_camera_io_w(0xFFFFFFFF, cpp_dev->base +
			MSM_CPP_MICRO_IRQGEN_CLR);

		/*Start firmware loading*/
		msm_cpp_write(MSM_CPP_CMD_FW_LOAD, cpp_dev->base);
		if (fw)
			msm_cpp_write(fw->size, cpp_dev->base);
		else
			msm_cpp_write(MSM_CPP_END_ADDRESS, cpp_dev->base);
		msm_cpp_write(MSM_CPP_START_ADDRESS, cpp_dev->base);

		if (ptr_bin) {
			msm_cpp_poll_rx_empty(cpp_dev->base);
			for (i = 0; i < fw->size/4; i++) {
				msm_cpp_write(*ptr_bin, cpp_dev->base);
				if (i % MSM_CPP_RX_FIFO_LEVEL == 0)
					msm_cpp_poll_rx_empty(cpp_dev->base);
				ptr_bin++;
			}
		}
		if (fw)
			release_firmware(fw);
		msm_camera_io_w_mb(0x00, cpp_dev->cpp_hw_base + 0xC);
		msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_OK);
		msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
	}

	/*Trigger MC to jump to start address*/
	msm_cpp_write(MSM_CPP_CMD_EXEC_JUMP, cpp_dev->base);
	msm_cpp_write(MSM_CPP_JUMP_ADDRESS, cpp_dev->base);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
	msm_cpp_poll(cpp_dev->base, 0x1);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_JUMP_ACK);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_TRAILER);

	/*Get Bootloader Version*/
	msm_cpp_write(MSM_CPP_CMD_GET_BOOTLOADER_VER, cpp_dev->base);
	pr_info("MC Bootloader Version: 0x%x\n",
		   msm_cpp_read(cpp_dev->base));

	/*Get Firmware Version*/
	msm_cpp_write(MSM_CPP_CMD_GET_FW_VER, cpp_dev->base);
	msm_cpp_write(MSM_CPP_MSG_ID_CMD, cpp_dev->base);
	msm_cpp_write(0x1, cpp_dev->base);
	msm_cpp_write(MSM_CPP_CMD_GET_FW_VER, cpp_dev->base);
	msm_cpp_write(MSM_CPP_MSG_ID_TRAILER, cpp_dev->base);

	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_CMD);
	msm_cpp_poll(cpp_dev->base, 0x2);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_FW_VER);
	cpp_dev->fw_version = msm_cpp_read(cpp_dev->base);
	pr_info("CPP FW Version: 0x%08x\n", cpp_dev->fw_version);
	msm_cpp_poll(cpp_dev->base, MSM_CPP_MSG_ID_TRAILER);

	/* Update the payload offsets */
	msm_cpp_calculate_stripe_base(cpp_dev);

	/*Disable MC clock*/
	/*msm_camera_io_w(0x0, cpp_dev->base +
					   MSM_CPP_MICRO_CLKEN_CTL);*/
}

static int cpp_open_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	int rc;
	uint32_t i;
	struct cpp_device *cpp_dev = NULL;
	CPP_DBG("E\n");

	if (!sd || !fh) {
		pr_err("Wrong input parameters sd %pK fh %pK!",
			sd, fh);
		return -EINVAL;
	}
	cpp_dev = v4l2_get_subdevdata(sd);
	if (!cpp_dev) {
		pr_err("failed: cpp_dev %pK\n", cpp_dev);
		return -EINVAL;
	}
	mutex_lock(&cpp_dev->mutex);
	if (cpp_dev->cpp_open_cnt == MAX_ACTIVE_CPP_INSTANCE) {
		pr_err("No free CPP instance\n");
		mutex_unlock(&cpp_dev->mutex);
		return -ENODEV;
	}

	for (i = 0; i < MAX_ACTIVE_CPP_INSTANCE; i++) {
		if (cpp_dev->cpp_subscribe_list[i].active == 0) {
			cpp_dev->cpp_subscribe_list[i].active = 1;
			cpp_dev->cpp_subscribe_list[i].vfh = &fh->vfh;
			break;
		}
	}
	if (i == MAX_ACTIVE_CPP_INSTANCE) {
		pr_err("No free instance\n");
		mutex_unlock(&cpp_dev->mutex);
		return -ENODEV;
	}

	CPP_DBG("open %d %pK\n", i, &fh->vfh);
	cpp_dev->cpp_open_cnt++;
	if (cpp_dev->cpp_open_cnt == 1) {
		rc = cpp_init_hardware(cpp_dev);
		if (rc < 0) {
			cpp_dev->cpp_open_cnt--;
			cpp_dev->cpp_subscribe_list[i].active = 0;
			cpp_dev->cpp_subscribe_list[i].vfh = NULL;
			mutex_unlock(&cpp_dev->mutex);
			return rc;
		}

		cpp_init_mem(cpp_dev);
		cpp_dev->state = CPP_STATE_IDLE;
	}
	mutex_unlock(&cpp_dev->mutex);
	return 0;
}

static int cpp_close_node(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	uint32_t i;
	struct cpp_device *cpp_dev = NULL;
	struct msm_device_queue *processing_q = NULL;
	struct msm_device_queue *eventData_q = NULL;

	if (!sd) {
		pr_err("Wrong input sd parameter");
		return -EINVAL;
	}
	cpp_dev =  v4l2_get_subdevdata(sd);

	if (!cpp_dev) {
		pr_err("failed: cpp_dev %pK\n", cpp_dev);
		return -EINVAL;
	}

	mutex_lock(&cpp_dev->mutex);

	processing_q = &cpp_dev->processing_q;
	eventData_q = &cpp_dev->eventData_q;

	if (cpp_dev->cpp_open_cnt == 0) {
		mutex_unlock(&cpp_dev->mutex);
		return 0;
	}

	for (i = 0; i < MAX_ACTIVE_CPP_INSTANCE; i++) {
		if (cpp_dev->cpp_subscribe_list[i].active == 1) {
			cpp_dev->cpp_subscribe_list[i].active = 0;
			cpp_dev->cpp_subscribe_list[i].vfh = NULL;
			break;
		}
	}
	if (i == MAX_ACTIVE_CPP_INSTANCE) {
		pr_err("Invalid close\n");
		mutex_unlock(&cpp_dev->mutex);
		return -ENODEV;
	}

	cpp_dev->cpp_open_cnt--;
	if (cpp_dev->cpp_open_cnt == 0) {
		pr_debug("irq_status: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x4));
		pr_debug("DEBUG_SP: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x40));
		pr_debug("DEBUG_T: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x44));
		pr_debug("DEBUG_N: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x48));
		pr_debug("DEBUG_R: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x4C));
		pr_debug("DEBUG_OPPC: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x50));
		pr_debug("DEBUG_MO: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x54));
		pr_debug("DEBUG_TIMER0: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x60));
		pr_debug("DEBUG_TIMER1: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x64));
		pr_debug("DEBUG_GPI: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x70));
		pr_debug("DEBUG_GPO: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x74));
		pr_debug("DEBUG_T0: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x80));
		pr_debug("DEBUG_R0: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x84));
		pr_debug("DEBUG_T1: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x88));
		pr_debug("DEBUG_R1: 0x%x\n",
			msm_camera_io_r(cpp_dev->cpp_hw_base + 0x8C));
		msm_camera_io_w(0x0, cpp_dev->base + MSM_CPP_MICRO_CLKEN_CTL);
		msm_cpp_clear_timer(cpp_dev);
		cpp_release_hardware(cpp_dev);
		if (cpp_dev->iommu_state == CPP_IOMMU_STATE_ATTACHED) {
			iommu_detach_device(cpp_dev->domain,
				cpp_dev->iommu_ctx);
			cpp_dev->iommu_state = CPP_IOMMU_STATE_DETACHED;
		}
		cpp_deinit_mem(cpp_dev);
		msm_cpp_empty_list(processing_q, list_frame);
		msm_cpp_empty_list(eventData_q, list_eventdata);
		cpp_dev->state = CPP_STATE_OFF;
	}

	mutex_unlock(&cpp_dev->mutex);
	return 0;
}

static const struct v4l2_subdev_internal_ops msm_cpp_internal_ops = {
	.open = cpp_open_node,
	.close = cpp_close_node,
};

static int msm_cpp_buffer_ops(struct cpp_device *cpp_dev,
	uint32_t buff_mgr_ops, struct msm_buf_mngr_info *buff_mgr_info)
{
	int rc = -EINVAL;

	rc = v4l2_subdev_call(cpp_dev->buf_mgr_subdev, core, ioctl,
		buff_mgr_ops, buff_mgr_info);
	if (rc < 0)
		pr_debug("%s: line %d rc = %d\n", __func__, __LINE__, rc);
	return rc;
}

static int msm_cpp_notify_frame_done(struct cpp_device *cpp_dev,
	uint8_t put_buf)
{
	struct v4l2_event v4l2_evt;
	struct msm_queue_cmd *frame_qcmd = NULL;
	struct msm_queue_cmd *event_qcmd = NULL;
	struct msm_cpp_frame_info_t *processed_frame = NULL;
	struct msm_device_queue *queue = &cpp_dev->processing_q;
	struct msm_buf_mngr_info buff_mgr_info;
	int rc = 0;

	frame_qcmd = msm_dequeue(queue, list_frame);
	if (frame_qcmd) {
		processed_frame = frame_qcmd->command;
		do_gettimeofday(&(processed_frame->out_time));
		kfree(frame_qcmd);
		event_qcmd = kzalloc(sizeof(struct msm_queue_cmd), GFP_ATOMIC);
		if (!event_qcmd) {
			pr_err("Insufficient memory\n");
			return -ENOMEM;
		}
		atomic_set(&event_qcmd->on_heap, 1);
		event_qcmd->command = processed_frame;
		CPP_DBG("fid %d\n", processed_frame->frame_id);
		msm_enqueue(&cpp_dev->eventData_q, &event_qcmd->list_eventdata);

		if ((processed_frame->partial_frame_indicator != 0) &&
			(processed_frame->last_payload == 0))
			goto NOTIFY_FRAME_DONE;

		if (!processed_frame->output_buffer_info[0].processed_divert &&
			!processed_frame->output_buffer_info[0].native_buff &&
			!processed_frame->we_disable) {
			memset(&buff_mgr_info, 0 ,
				sizeof(struct msm_buf_mngr_info));
			buff_mgr_info.session_id =
				((processed_frame->identity >> 16) & 0xFFFF);
			buff_mgr_info.stream_id =
				(processed_frame->identity & 0xFFFF);
			buff_mgr_info.frame_id = processed_frame->frame_id;
			buff_mgr_info.timestamp = processed_frame->timestamp;
			/*
			 * Update the reserved field (cds information) to buffer
			 * manager structure so it is propogated back to HAL
			 */
			buff_mgr_info.reserved = processed_frame->reserved;
			if (processed_frame->batch_info.batch_mode ==
				BATCH_MODE_VIDEO) {
				buff_mgr_info.index =
					processed_frame->batch_info.cont_idx;
			} else {
				buff_mgr_info.index = processed_frame->
					output_buffer_info[0].index;
			}
			if (put_buf) {
				rc = msm_cpp_buffer_ops(cpp_dev,
					VIDIOC_MSM_BUF_MNGR_PUT_BUF,
					&buff_mgr_info);
				if (rc < 0) {
					pr_err("error putting buffer\n");
					rc = -EINVAL;
				}
			} else {
				rc = msm_cpp_buffer_ops(cpp_dev,
					VIDIOC_MSM_BUF_MNGR_BUF_DONE,
					&buff_mgr_info);
				if (rc < 0) {
					pr_err("error putting buffer\n");
					rc = -EINVAL;
				}
			}
		}

		if (processed_frame->duplicate_output  &&
			!processed_frame->
				duplicate_buffer_info.processed_divert &&
			!processed_frame->we_disable) {
			memset(&buff_mgr_info, 0 ,
				sizeof(struct msm_buf_mngr_info));
			buff_mgr_info.session_id =
			((processed_frame->duplicate_identity >> 16) & 0xFFFF);
			buff_mgr_info.stream_id =
				(processed_frame->duplicate_identity & 0xFFFF);
			buff_mgr_info.frame_id = processed_frame->frame_id;
			buff_mgr_info.timestamp = processed_frame->timestamp;
			buff_mgr_info.index =
				processed_frame->duplicate_buffer_info.index;
			/*
			 * Update the reserved field (cds information) to buffer
			 * manager structure so it is propogated back to HAL
			 */
			buff_mgr_info.reserved = processed_frame->reserved;
			if (put_buf) {
				rc = msm_cpp_buffer_ops(cpp_dev,
					VIDIOC_MSM_BUF_MNGR_PUT_BUF,
					&buff_mgr_info);
				if (rc < 0) {
					pr_err("error putting buffer\n");
					rc = -EINVAL;
				}
			} else {
				rc = msm_cpp_buffer_ops(cpp_dev,
					VIDIOC_MSM_BUF_MNGR_BUF_DONE,
					&buff_mgr_info);
				if (rc < 0) {
					pr_err("error putting buffer\n");
					rc = -EINVAL;
				}
			}
		}
NOTIFY_FRAME_DONE:
		v4l2_evt.id = processed_frame->inst_id;
		v4l2_evt.type = V4L2_EVENT_CPP_FRAME_DONE;
		v4l2_event_queue(cpp_dev->msm_sd.sd.devnode, &v4l2_evt);
	}
	return rc;
}

#if MSM_CPP_DUMP_FRM_CMD
static int msm_cpp_dump_frame_cmd(struct msm_cpp_frame_info_t *frame_info)
{
	int i, i1, i2;
	struct cpp_device *cpp_dev = cpp_timer.data.cpp_dev;
	CPP_DBG("-- start: cpp frame cmd for identity=0x%x, frame_id=%d --\n",
		frame_info->identity, frame_info->frame_id);

	CPP_DBG("msg[%03d] = 0x%08x\n", 0, 0x6);
	/* send top level and plane level */
	for (i = 0; i < cpp_dev->stripe_base; i++)
		CPP_DBG("msg[%03d] = 0x%08x\n", i,
			frame_info->cpp_cmd_msg[i]);
	/* send stripes */
	i1 = cpp_dev->stripe_base +
		cpp_dev->stripe_size * frame_info->first_stripe_index;
	i2 = cpp_dev->stripe_size * (frame_info->last_stripe_index -
		frame_info->first_stripe_index + 1);
	for (i = 0; i < i2; i++)
		CPP_DBG("msg[%03d] = 0x%08x\n", i+i1,
			frame_info->cpp_cmd_msg[i+i1]);
	/* send trailer */
	CPP_DBG("msg[%03d] = 0x%08x\n", i+i1, MSM_CPP_MSG_ID_TRAILER);
	CPP_DBG("--   end: cpp frame cmd for identity=0x%x, frame_id=%d --\n",
		frame_info->identity, frame_info->frame_id);
	return 0;
}
#else
static int msm_cpp_dump_frame_cmd(struct msm_cpp_frame_info_t *frame_info)
{
	return 0;
}
#endif

static void msm_cpp_do_timeout_work(struct work_struct *work)
{
	uint32_t j = 0, i = 0, i1 = 0, i2 = 0;
	int32_t queue_len = 0;
	struct msm_device_queue *queue = NULL;
	struct msm_cpp_frame_info_t *processed_frame[MAX_CPP_PROCESSING_FRAME];
	struct cpp_device *cpp_dev;

	pr_info("cpp_timer_callback called. (jiffies=%lu)\n",
		jiffies);
	if (!work || cpp_timer.data.cpp_dev->state != CPP_STATE_ACTIVE) {
		pr_err("Invalid work:%pK or state:%d\n", work,
			cpp_timer.data.cpp_dev->state);
		return;
	}
	if (!atomic_read(&cpp_timer.used)) {
		pr_info("Delayed trigger, IRQ serviced\n");
		return;
	}

	disable_irq(cpp_timer.data.cpp_dev->irq->start);
	pr_info("Reloading firmware\n");
	cpp_load_fw(cpp_timer.data.cpp_dev,
		cpp_timer.data.cpp_dev->fw_name_bin);
	pr_info("Firmware loading done\n");
	enable_irq(cpp_timer.data.cpp_dev->irq->start);
	msm_camera_io_w_mb(0x8, cpp_timer.data.cpp_dev->base +
		MSM_CPP_MICRO_IRQGEN_MASK);
	msm_camera_io_w_mb(0xFFFF,
		cpp_timer.data.cpp_dev->base +
		MSM_CPP_MICRO_IRQGEN_CLR);

	if (!atomic_read(&cpp_timer.used)) {
		pr_info("Delayed trigger, IRQ serviced\n");
		return;
	}

	queue = &cpp_timer.data.cpp_dev->processing_q;
	queue_len = queue->len;
	cpp_dev = cpp_timer.data.cpp_dev;

	mutex_lock(&cpp_dev->mutex);

	if (cpp_dev->timeout_trial_cnt >=
		cpp_dev->max_timeout_trial_cnt) {
		pr_info("Max trial reached\n");
		while (queue_len) {
			msm_cpp_notify_frame_done(cpp_dev, 1);
			queue_len--;
		}
		atomic_set(&cpp_timer.used, 0);
		for (i = 0; i < MAX_CPP_PROCESSING_FRAME; i++)
			cpp_timer.data.processed_frame[i] = NULL;
		cpp_dev->timeout_trial_cnt = 0;
		mutex_unlock(&cpp_dev->mutex);
		pr_info("exit\n");
		return;
	}

	atomic_set(&cpp_timer.used, 1);
	pr_info("Starting timer to fire in %d ms. (jiffies=%lu)\n",
		CPP_CMD_TIMEOUT_MS, jiffies);
	mod_timer(&cpp_timer.cpp_timer,
		jiffies + msecs_to_jiffies(CPP_CMD_TIMEOUT_MS));

	for (i = 0; i < MAX_CPP_PROCESSING_FRAME; i++)
		processed_frame[i] = cpp_timer.data.processed_frame[i];

	for (i = 0; i < queue_len; i++) {
		pr_info("Rescheduling for identity=0x%x, frame_id=%03d\n",
			processed_frame[i]->identity,
			processed_frame[i]->frame_id);

		msm_cpp_write(0x6, cpp_dev->base);
		/* send top level and plane level */
		for (j = 0; j < cpp_dev->stripe_base; j++) {
			if (j % MSM_CPP_RX_FIFO_LEVEL == 0)
				msm_cpp_poll_rx_empty(cpp_dev->base);
			msm_cpp_write(processed_frame[i]->cpp_cmd_msg[j],
				cpp_dev->base);
		}
		/* send stripes */
		i1 = cpp_dev->stripe_base +
			cpp_dev->stripe_size *
			processed_frame[i]->first_stripe_index;
		i2 = cpp_dev->stripe_size *
			(processed_frame[i]->last_stripe_index -
			processed_frame[i]->first_stripe_index + 1);
		for (j = 0; j < i2; j++) {
			if (j % MSM_CPP_RX_FIFO_LEVEL == 0)
				msm_cpp_poll_rx_empty(cpp_dev->base);
			msm_cpp_write(processed_frame[i]->cpp_cmd_msg[j+i1],
				cpp_dev->base);
		}
		/* send trailer */
		msm_cpp_write(0xabcdefaa, cpp_dev->base);
		pr_info("After frame:%d write\n", i+1);
	}

	cpp_timer.data.cpp_dev->timeout_trial_cnt++;
	mutex_unlock(&cpp_timer.data.cpp_dev->mutex);

	pr_info("exit\n");
	return;
}

void cpp_timer_callback(unsigned long data)
{
	struct msm_cpp_work_t *work =
		cpp_timer.data.cpp_dev->work;
	queue_work(cpp_timer.data.cpp_dev->timer_wq,
		(struct work_struct *)work);
}

static int msm_cpp_send_frame_to_hardware(struct cpp_device *cpp_dev,
	struct msm_queue_cmd *frame_qcmd)
{
	unsigned long flags;
	uint32_t i, i1, i2;
	int32_t rc = -EAGAIN;
	int ret;
	struct msm_cpp_frame_info_t *process_frame;
	uint32_t queue_len = 0;

	if (cpp_dev->processing_q.len < MAX_CPP_PROCESSING_FRAME) {
		process_frame = frame_qcmd->command;
		msm_cpp_dump_frame_cmd(process_frame);
		spin_lock_irqsave(&cpp_timer.data.processed_frame_lock, flags);
		msm_enqueue(&cpp_dev->processing_q,
			&frame_qcmd->list_frame);
		cpp_timer.data.processed_frame[cpp_dev->processing_q.len - 1] =
			process_frame;
		queue_len = cpp_dev->processing_q.len;
		spin_unlock_irqrestore(&cpp_timer.data.processed_frame_lock,
			flags);
		if (queue_len == 1)
			atomic_set(&cpp_timer.used, 1);

		CPP_DBG("Starting timer to fire in %d ms. (jiffies=%lu)\n",
			CPP_CMD_TIMEOUT_MS, jiffies);
		ret = mod_timer(&cpp_timer.cpp_timer,
			jiffies + msecs_to_jiffies(CPP_CMD_TIMEOUT_MS));
		if (ret)
			CPP_DBG("Timer has not expired yet\n");

		msm_cpp_write(0x6, cpp_dev->base);
		/* send top level and plane level */
		for (i = 0; i < cpp_dev->stripe_base; i++) {
			if (i % MSM_CPP_RX_FIFO_LEVEL == 0)
				msm_cpp_poll_rx_empty(cpp_dev->base);
			msm_cpp_write(process_frame->cpp_cmd_msg[i],
				cpp_dev->base);
		}
		/* send stripes */
		i1 = cpp_dev->stripe_base + cpp_dev->stripe_size *
			process_frame->first_stripe_index;
		i2 = cpp_dev->stripe_size * (process_frame->last_stripe_index -
			process_frame->first_stripe_index + 1);
		for (i = 0; i < i2; i++) {
			if (i % MSM_CPP_RX_FIFO_LEVEL == 0)
				msm_cpp_poll_rx_empty(cpp_dev->base);
			msm_cpp_write(process_frame->cpp_cmd_msg[i+i1],
				cpp_dev->base);
		}
		/* send trailer */
		msm_cpp_write(MSM_CPP_MSG_ID_TRAILER, cpp_dev->base);

		do_gettimeofday(&(process_frame->in_time));
		rc = 0;
	}
	if (rc < 0)
		pr_err("process queue full. drop frame\n");
	return rc;
}

static int msm_cpp_send_command_to_hardware(struct cpp_device *cpp_dev,
	uint32_t *cmd_msg, uint32_t payload_size)
{
	uint32_t i;

	for (i = 0; i < payload_size; i++) {
		msm_cpp_write(cmd_msg[i],
			cpp_dev->base);
	}
	return 0;
}

static int msm_cpp_flush_frames(struct cpp_device *cpp_dev)
{
	return 0;
}

static struct msm_cpp_frame_info_t *msm_cpp_get_frame(
	struct msm_camera_v4l2_ioctl_t *ioctl_ptr)
{
	uint32_t *cpp_frame_msg;
	struct msm_cpp_frame_info_t *new_frame = NULL;
	int32_t rc = 0;

	new_frame = kzalloc(sizeof(struct msm_cpp_frame_info_t), GFP_KERNEL);

	if (!new_frame) {
		pr_err("Insufficient memory\n");
		rc = -ENOMEM;
		goto no_mem_err;
	}

	rc = (copy_from_user(new_frame, (void __user *)ioctl_ptr->ioctl_ptr,
			sizeof(struct msm_cpp_frame_info_t)) ? -EFAULT : 0);
	if (rc) {
		ERR_COPY_FROM_USER();
		goto frame_err;
	}

	if ((new_frame->msg_len == 0) ||
		(new_frame->msg_len > MSM_CPP_MAX_FRAME_LENGTH)) {
		pr_err("%s:%d: Invalid frame len:%d\n", __func__,
			__LINE__, new_frame->msg_len);
		goto frame_err;
	}

	cpp_frame_msg = kzalloc(sizeof(uint32_t) * new_frame->msg_len,
		GFP_KERNEL);
	if (!cpp_frame_msg) {
		pr_err("Insufficient memory\n");
		goto frame_err;
	}

	rc = (copy_from_user(cpp_frame_msg,
		(void __user *)new_frame->cpp_cmd_msg,
		sizeof(uint32_t) * new_frame->msg_len) ? -EFAULT : 0);
	if (rc) {
		ERR_COPY_FROM_USER();
		goto frame_msg_err;
	}
	new_frame->cpp_cmd_msg = cpp_frame_msg;
	return new_frame;

frame_msg_err:
	kfree(cpp_frame_msg);
frame_err:
	kfree(new_frame);
no_mem_err:
	return NULL;
}

static int msm_cpp_check_buf_type(struct msm_buf_mngr_info *buff_mgr_info,
	struct msm_cpp_frame_info_t *new_frame)
{
	int32_t num_output_bufs = 0;
	uint32_t i = 0;
	if (buff_mgr_info->type == MSM_CAMERA_BUF_MNGR_BUF_USER) {
		new_frame->batch_info.cont_idx =
			buff_mgr_info->index;
		num_output_bufs = buff_mgr_info->user_buf.buf_cnt;
		if (buff_mgr_info->user_buf.buf_cnt <
			new_frame->batch_info.batch_size) {
			/* Less bufs than Input buffer */
			num_output_bufs = buff_mgr_info->user_buf.buf_cnt;
		} else {
			/* More or equal bufs as Input buffer */
			num_output_bufs = new_frame->batch_info.batch_size;
		}
		if (num_output_bufs > MSM_OUTPUT_BUF_CNT)
			return 0;
		for (i = 0; i < num_output_bufs; i++) {
			new_frame->output_buffer_info[i].index =
				buff_mgr_info->user_buf.buf_idx[i];
		}
	} else {
		/* For non-group case use first buf slot */
		new_frame->output_buffer_info[0].index = buff_mgr_info->index;
		num_output_bufs = 1;
	}

	return num_output_bufs;
}
static int msm_cpp_cfg_frame(struct cpp_device *cpp_dev,
	struct msm_cpp_frame_info_t *new_frame)
{
	int32_t rc = 0;
	struct msm_queue_cmd *frame_qcmd = NULL;
	uint32_t *cpp_frame_msg;
	unsigned long in_phyaddr, out_phyaddr0 = (unsigned long)NULL;
	unsigned long out_phyaddr1;
	unsigned long tnr_scratch_buffer0, tnr_scratch_buffer1;
	uint16_t num_stripes = 0;
	struct msm_buf_mngr_info buff_mgr_info, dup_buff_mgr_info;
	int32_t in_fd;
	int32_t i = 0, num_output_bufs = 1;
	uint32_t stripe_base = 0;
	uint32_t rd_pntr, wr_0_pntr, wr_1_pntr, wr_2_pntr, wr_3_pntr;
	uint32_t wr_0_meta_data_wr_pntr, wr_1_meta_data_wr_pntr,
		wr_2_meta_data_wr_pntr, wr_3_meta_data_wr_pntr;
	uint32_t rd_ref_pntr, wr_ref_pntr, stripe_info_offset, stripe_size;
	uint8_t tnr_enabled, ubwc_enabled;
	enum msm_camera_buf_mngr_buf_type buf_type =
		MSM_CAMERA_BUF_MNGR_BUF_PLANAR;

	if (!new_frame) {
		pr_err("%s: Frame is Null\n", __func__);
		return -EINVAL;
	}
	cpp_frame_msg = new_frame->cpp_cmd_msg;

	stripe_base = cpp_dev->stripe_base;
	stripe_info_offset = cpp_dev->stripe_info_offset;
	stripe_size = cpp_dev->stripe_size;
	rd_pntr = cpp_dev->rd_pntr;
	wr_0_pntr = cpp_dev->wr_0_pntr;
	wr_1_pntr = cpp_dev->wr_1_pntr;
	wr_2_pntr = cpp_dev->wr_2_pntr;
	wr_3_pntr = cpp_dev->wr_3_pntr;
	rd_ref_pntr = cpp_dev->rd_ref_pntr;
	wr_ref_pntr = cpp_dev->wr_ref_pntr;
	wr_0_meta_data_wr_pntr = cpp_dev->wr_0_meta_data_wr_pntr;
	wr_1_meta_data_wr_pntr = cpp_dev->wr_1_meta_data_wr_pntr;
	wr_2_meta_data_wr_pntr = cpp_dev->wr_2_meta_data_wr_pntr;
	wr_3_meta_data_wr_pntr = cpp_dev->wr_3_meta_data_wr_pntr;

	if (cpp_frame_msg == NULL ||
		(new_frame->msg_len < MSM_CPP_MIN_FRAME_LENGTH)) {
		pr_err("Length is not correct or frame message is missing\n");
		return -EINVAL;
	}

	if (cpp_frame_msg[new_frame->msg_len - 1] !=
		MSM_CPP_MSG_ID_TRAILER) {
		pr_err("Invalid frame message\n");
		return -EINVAL;
	}

	/* Stripe index starts at zero */
	if ((!new_frame->num_strips) ||
		(new_frame->first_stripe_index >= new_frame->num_strips) ||
		(new_frame->last_stripe_index  >= new_frame->num_strips) ||
		(new_frame->first_stripe_index >
			new_frame->last_stripe_index)) {
		pr_err("Invalid frame message, #stripes=%d, stripe indices=[%d,%d]\n",
			new_frame->num_strips,
			new_frame->first_stripe_index,
			new_frame->last_stripe_index);
		return -EINVAL;
	}

	if (!stripe_size) {
		pr_err("Invalid frame message, invalid stripe_size (%d)!\n",
			stripe_size);
		return -EINVAL;
	}

	if ((stripe_base == UINT_MAX) ||
		(new_frame->num_strips >
			(UINT_MAX - 1 - stripe_base) / stripe_size)) {
		pr_err("Invalid frame message, num_strips %d is large\n",
			new_frame->num_strips);
		return -EINVAL;
	}

	if ((stripe_base + new_frame->num_strips * stripe_size + 1) !=
		new_frame->msg_len) {
		pr_err("Invalid frame message,len=%d,expected=%d\n",
			new_frame->msg_len,
			(stripe_base +
			new_frame->num_strips * stripe_size + 1));
		return -EINVAL;
	}

	if (cpp_dev->iommu_state != CPP_IOMMU_STATE_ATTACHED) {
		pr_err("IOMMU is not attached\n");
		return -EAGAIN;
	}

	in_phyaddr = msm_cpp_fetch_buffer_info(cpp_dev,
		&new_frame->input_buffer_info,
		((new_frame->input_buffer_info.identity >> 16) & 0xFFFF),
		(new_frame->input_buffer_info.identity & 0xFFFF), &in_fd);
	if (!in_phyaddr) {
		pr_err("%s: error gettting input physical address\n", __func__);
		rc = -EINVAL;
		goto frame_msg_err;
	}

	if (new_frame->we_disable == 0) {
		if ((new_frame->output_buffer_info[0].native_buff == 0) &&
			(new_frame->first_payload)) {
			memset(&buff_mgr_info, 0,
				sizeof(struct msm_buf_mngr_info));
			if (new_frame->batch_info.batch_mode ==
				BATCH_MODE_VIDEO)
				buf_type = MSM_CAMERA_BUF_MNGR_BUF_USER;

			buff_mgr_info.session_id =
				((new_frame->identity >> 16) & 0xFFFF);
			buff_mgr_info.stream_id =
				(new_frame->identity & 0xFFFF);
			buff_mgr_info.type = buf_type;
			rc = msm_cpp_buffer_ops(cpp_dev,
				VIDIOC_MSM_BUF_MNGR_GET_BUF,
				&buff_mgr_info);
			if (rc < 0) {
				rc = -EAGAIN;
				pr_debug("%s: error getting buffer rc:%d\n",
					__func__, rc);
				goto frame_msg_err;
			}
			num_output_bufs =
				msm_cpp_check_buf_type(&buff_mgr_info,
					new_frame);
			if (!num_output_bufs) {
				pr_err("%s: error getting buffer %d\n",
					__func__, num_output_bufs);
				rc = -EINVAL;
				goto phyaddr_err;
			}
		}

		out_phyaddr0 = msm_cpp_fetch_buffer_info(cpp_dev,
			&new_frame->output_buffer_info[0],
			((new_frame->identity >> 16) & 0xFFFF),
			(new_frame->identity & 0xFFFF),
			&new_frame->output_buffer_info[0].fd);
		if (!out_phyaddr0) {
			pr_err("%s: error gettting output physical address\n",
				__func__);
			rc = -EINVAL;
			goto phyaddr_err;
		}
	}
	out_phyaddr1 = out_phyaddr0;

	/* get buffer for duplicate output */
	if (new_frame->duplicate_output) {
		CPP_DBG("duplication enabled, dup_id=0x%x",
			new_frame->duplicate_identity);
		memset(&new_frame->duplicate_buffer_info, 0,
			sizeof(struct msm_cpp_buffer_info_t));
		memset(&dup_buff_mgr_info, 0, sizeof(struct msm_buf_mngr_info));
		dup_buff_mgr_info.session_id =
			((new_frame->duplicate_identity >> 16) & 0xFFFF);
		dup_buff_mgr_info.stream_id =
			(new_frame->duplicate_identity & 0xFFFF);
		dup_buff_mgr_info.type =
			MSM_CAMERA_BUF_MNGR_BUF_PLANAR;
		rc = msm_cpp_buffer_ops(cpp_dev, VIDIOC_MSM_BUF_MNGR_GET_BUF,
			&dup_buff_mgr_info);
		if (rc < 0) {
			rc = -EAGAIN;
			pr_err("%s: error getting buffer rc:%d\n",
				__func__, rc);
			goto phyaddr_err;
		}
		new_frame->duplicate_buffer_info.index =
			dup_buff_mgr_info.index;
		out_phyaddr1 = msm_cpp_fetch_buffer_info(cpp_dev,
			&new_frame->duplicate_buffer_info,
			((new_frame->duplicate_identity >> 16) & 0xFFFF),
			(new_frame->duplicate_identity & 0xFFFF),
			&new_frame->duplicate_buffer_info.fd);
		if (!out_phyaddr1) {
			pr_err("error gettting output physical address\n");
			rc = -EINVAL;
			msm_cpp_buffer_ops(cpp_dev, VIDIOC_MSM_BUF_MNGR_PUT_BUF,
				&dup_buff_mgr_info);
			goto phyaddr_err;
		}
		/* set duplicate enable bit */
		cpp_frame_msg[5] |= 0x1;
		CPP_DBG("out_phyaddr1= %08x\n", (uint32_t)out_phyaddr1);
	}

	if ((BATCH_MODE_VIDEO == new_frame->batch_info.batch_mode) &&
		(new_frame->batch_info.batch_size > 1)) {
		/*create and send Set Group Buffer with Duplicate command*/
		uint32_t set_group_buffer_w_duplication[73];
		uint32_t *ptr = &set_group_buffer_w_duplication[0];
		unsigned long out_phyaddr, out_phyaddr_new, distance;

		memset(ptr, 0, sizeof(set_group_buffer_w_duplication));
		*ptr++ = MSM_CPP_CMD_GROUP_BUFFER;
		*ptr++ = MSM_CPP_MSG_ID_CMD;
		*ptr++ = MSM_CPP_GROUP_CMD_LEN;
		*ptr++ = MSM_CPP_CMD_GROUP_BUFFER;
		*ptr++ = 0;
		out_phyaddr = out_phyaddr0;
		for (i = 1; i < num_output_bufs; i++) {
			out_phyaddr_new = msm_cpp_fetch_buffer_info(cpp_dev,
				&new_frame->output_buffer_info[i],
				((new_frame->identity >> 16) & 0xFFFF),
				(new_frame->identity & 0xFFFF),
				&new_frame->output_buffer_info[i].fd);
			if (!out_phyaddr_new) {
				pr_err("%s: error getting o/p phy addr\n",
					__func__);
				rc = -EINVAL;
				goto phyaddr_err;
			}
			distance = out_phyaddr_new - out_phyaddr;
			out_phyaddr = out_phyaddr_new;
			*ptr++ = distance;
			*ptr++ = distance;
			*ptr++ = distance;
		}
		if (new_frame->duplicate_output)
			set_group_buffer_w_duplication[38] =
				1 << new_frame->batch_info.pick_preview_idx;
		else
			set_group_buffer_w_duplication[38] = 0;

		set_group_buffer_w_duplication[72] = MSM_CPP_MSG_ID_TRAILER;
		rc = msm_cpp_send_command_to_hardware(cpp_dev,
			&set_group_buffer_w_duplication[0], 73);
		if (rc < 0) {
			pr_err("%s: Send Command Error\n", __func__);
			rc = -EINVAL;
			goto phyaddr_err;
		}
	}

	tnr_enabled = msm_cpp_is_tnr_enabled(cpp_dev, cpp_frame_msg);
	pr_debug("%s: feature_mask 0x%x\n", __func__, new_frame->feature_mask);
	ubwc_enabled = ((new_frame->feature_mask & 0x20) >> 5);
	if (tnr_enabled) {
		tnr_scratch_buffer0 = msm_cpp_fetch_buffer_info(cpp_dev,
			&new_frame->tnr_scratch_buffer_info[0],
			((new_frame->identity >> 16) & 0xFFFF),
			(new_frame->identity & 0xFFFF),
			&new_frame->tnr_scratch_buffer_info[0].fd);
		if (!tnr_scratch_buffer0) {
			pr_err("error getting scratch buffer physical address\n");
			rc = -EINVAL;
			goto phyaddr_err;
		}

		tnr_scratch_buffer1 = msm_cpp_fetch_buffer_info(cpp_dev,
			&new_frame->tnr_scratch_buffer_info[1],
			((new_frame->identity >> 16) & 0xFFFF),
			(new_frame->identity & 0xFFFF),
			&new_frame->tnr_scratch_buffer_info[1].fd);
		if (!tnr_scratch_buffer1) {
			pr_err("error getting scratch buffer physical address\n");
			rc = -EINVAL;
			goto phyaddr_err;
		}
	} else {
		tnr_scratch_buffer0 = 0;
		tnr_scratch_buffer1 = 0;
	}

	for (i = 0; i < new_frame->num_strips; i++) {
		cpp_frame_msg[stripe_base + rd_pntr + i * stripe_size] +=
			(uint32_t) in_phyaddr;
		cpp_frame_msg[stripe_base + wr_0_pntr + i * stripe_size] +=
			(uint32_t) out_phyaddr0;
		cpp_frame_msg[stripe_base + wr_1_pntr + i * stripe_size] +=
			(uint32_t) out_phyaddr1;
		cpp_frame_msg[stripe_base + wr_2_pntr + i * stripe_size] +=
			(uint32_t) out_phyaddr0;
		cpp_frame_msg[stripe_base + wr_3_pntr + i * stripe_size] +=
			(uint32_t) out_phyaddr1;
		if (tnr_enabled) {
			cpp_frame_msg[stripe_base + rd_ref_pntr +
				i * stripe_size] +=
				(uint32_t)tnr_scratch_buffer0;
			cpp_frame_msg[stripe_base + wr_ref_pntr +
				i * stripe_size] +=
				(uint32_t)tnr_scratch_buffer1;
		}
		if (ubwc_enabled) {
			cpp_frame_msg[stripe_base + wr_0_meta_data_wr_pntr +
				i * stripe_size] += (uint32_t) out_phyaddr0;
			cpp_frame_msg[stripe_base + wr_1_meta_data_wr_pntr +
				i * stripe_size] += (uint32_t) out_phyaddr1;
			cpp_frame_msg[stripe_base + wr_2_meta_data_wr_pntr +
				i * stripe_size] += (uint32_t) out_phyaddr0;
			cpp_frame_msg[stripe_base + wr_3_meta_data_wr_pntr +
				i * stripe_size] += (uint32_t) out_phyaddr1;

		}
	}

	if (tnr_enabled) {
		cpp_frame_msg[10] = tnr_scratch_buffer1 -
			tnr_scratch_buffer0;
	}

	num_stripes = new_frame->last_stripe_index -
		new_frame->first_stripe_index + 1;
	cpp_frame_msg[1] = stripe_base - 2 + num_stripes * stripe_size;

	frame_qcmd = kzalloc(sizeof(struct msm_queue_cmd), GFP_KERNEL);
	if (!frame_qcmd) {
		pr_err("%s: Insufficient memory\n", __func__);
		rc = -ENOMEM;
		goto qcmd_err;
	}

	atomic_set(&frame_qcmd->on_heap, 1);
	frame_qcmd->command = new_frame;
	rc = msm_cpp_send_frame_to_hardware(cpp_dev, frame_qcmd);
	if (rc < 0) {
		pr_err("%s: error cannot send frame to hardware\n", __func__);
		rc = -EINVAL;
		goto qcmd_err;
	}

	return rc;
qcmd_err:
	kfree(frame_qcmd);
phyaddr_err:
	if (new_frame->output_buffer_info[0].native_buff == 0)
		msm_cpp_buffer_ops(cpp_dev, VIDIOC_MSM_BUF_MNGR_PUT_BUF,
			&buff_mgr_info);
frame_msg_err:
	kfree(cpp_frame_msg);
	kfree(new_frame);
	return rc;
}

static int msm_cpp_cfg(struct cpp_device *cpp_dev,
	struct msm_camera_v4l2_ioctl_t *ioctl_ptr)
{
	struct msm_cpp_frame_info_t *frame = NULL;
	struct msm_cpp_frame_info_t k_frame_info;
	int32_t rc = 0;
	if (copy_from_user(&k_frame_info,
			(void __user *)ioctl_ptr->ioctl_ptr,
			sizeof(k_frame_info)))
		return -EFAULT;

	frame = msm_cpp_get_frame(ioctl_ptr);
	if (!frame) {
		pr_err("%s: Error allocating frame\n", __func__);
		rc = -EINVAL;
	} else {
		rc = msm_cpp_cfg_frame(cpp_dev, frame);
	}

	ioctl_ptr->trans_code = rc;

	if (copy_to_user((void __user *)k_frame_info.status, &rc,
		sizeof(int32_t)))
		pr_err("error cannot copy error\n");

	return rc;
}

void msm_cpp_clean_queue(struct cpp_device *cpp_dev)
{
	struct msm_queue_cmd *frame_qcmd = NULL;
	struct msm_cpp_frame_info_t *processed_frame = NULL;
	struct msm_device_queue *queue = NULL;

	while (cpp_dev->processing_q.len) {
		pr_info("queue len:%d\n", cpp_dev->processing_q.len);
		queue = &cpp_dev->processing_q;
		frame_qcmd = msm_dequeue(queue, list_frame);
		if (frame_qcmd) {
			processed_frame = frame_qcmd->command;
			kfree(frame_qcmd);
			if (processed_frame)
				kfree(processed_frame->cpp_cmd_msg);
			kfree(processed_frame);
		}
	}
}

#ifdef CONFIG_COMPAT
static int msm_cpp_copy_from_ioctl_ptr(void *dst_ptr,
	struct msm_camera_v4l2_ioctl_t *ioctl_ptr)
{
	int ret;
	if ((ioctl_ptr->ioctl_ptr == NULL) || (ioctl_ptr->len == 0)) {
		pr_err("%s: Wrong ioctl_ptr %pK / len %zu\n", __func__,
			ioctl_ptr, ioctl_ptr->len);
		return -EINVAL;
	}

	/* For compat task, source ptr is in kernel space */
	if (is_compat_task()) {
		memcpy(dst_ptr, ioctl_ptr->ioctl_ptr, ioctl_ptr->len);
		ret = 0;
	} else {
		ret = copy_from_user(dst_ptr,
			(void __user *)ioctl_ptr->ioctl_ptr, ioctl_ptr->len);
		if (ret)
			pr_err("Copy from user fail %d\n", ret);
	}
	return ret ? -EFAULT : 0;
}
#else
static int msm_cpp_copy_from_ioctl_ptr(void *dst_ptr,
	struct msm_camera_v4l2_ioctl_t *ioctl_ptr)
{
	int ret;
	if ((ioctl_ptr->ioctl_ptr == NULL) || (ioctl_ptr->len == 0)) {
		pr_err("%s: Wrong ioctl_ptr %pK / len %zu\n", __func__,
			ioctl_ptr, ioctl_ptr->len);
		return -EINVAL;
	}

	ret = copy_from_user(dst_ptr,
		(void __user *)ioctl_ptr->ioctl_ptr, ioctl_ptr->len);
	if (ret)
		pr_err("Copy from user fail %d\n", ret);

	return ret ? -EFAULT : 0;
}
#endif


static int msm_cpp_validate_ioctl_input(unsigned int cmd, void *arg,
	struct msm_camera_v4l2_ioctl_t **ioctl_ptr)
{
	switch (cmd) {
	case MSM_SD_SHUTDOWN:
	case VIDIOC_MSM_CPP_IOMMU_ATTACH:
	case VIDIOC_MSM_CPP_IOMMU_DETACH:
		break;
	default: {
		if (ioctl_ptr == NULL) {
			pr_err("Wrong ioctl_ptr for cmd %u\n", cmd);
			return -EINVAL;
		}

		*ioctl_ptr = arg;
		if (((*ioctl_ptr) == NULL) ||
			((*ioctl_ptr)->ioctl_ptr == NULL) ||
			((*ioctl_ptr)->len == 0)) {
			pr_err("Error invalid ioctl argument cmd %u", cmd);
			return -EINVAL;
		}
		break;
	}
	}
	return 0;
}

long msm_cpp_subdev_ioctl(struct v4l2_subdev *sd,
			unsigned int cmd, void *arg)
{
	struct cpp_device *cpp_dev = NULL;
	struct msm_camera_v4l2_ioctl_t *ioctl_ptr = NULL;
	int rc = 0;

	if (sd == NULL) {
		pr_err("sd %pK\n", sd);
		return -EINVAL;
	}

	if (_IOC_DIR(cmd) == _IOC_NONE) {
		pr_err("Invalid ioctl/subdev cmd %u", cmd);
		return -EINVAL;
	}

	rc = msm_cpp_validate_ioctl_input(cmd, arg, &ioctl_ptr);
	if (rc != 0) {
		pr_err("input validation failed\n");
		return rc;
	}

	cpp_dev = v4l2_get_subdevdata(sd);
	if (cpp_dev == NULL) {
		pr_err("cpp_dev is null\n");
		return -EINVAL;
	}
	mutex_lock(&cpp_dev->mutex);
	CPP_DBG("E cmd: 0x%x\n", cmd);
	switch (cmd) {
	case VIDIOC_MSM_CPP_GET_HW_INFO: {
		CPP_DBG("VIDIOC_MSM_CPP_GET_HW_INFO\n");
		if (copy_to_user((void __user *)ioctl_ptr->ioctl_ptr,
			&cpp_dev->hw_info,
			sizeof(struct cpp_hw_info))) {
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		break;
	}

	case VIDIOC_MSM_CPP_LOAD_FIRMWARE: {
		CPP_DBG("VIDIOC_MSM_CPP_LOAD_FIRMWARE\n");
		if (cpp_dev->is_firmware_loaded == 0) {
			if (cpp_dev->fw_name_bin != NULL) {
				kfree(cpp_dev->fw_name_bin);
				cpp_dev->fw_name_bin = NULL;
			}
			if ((ioctl_ptr->len == 0) ||
				(ioctl_ptr->len > MSM_CPP_MAX_FW_NAME_LEN)) {
				pr_err("ioctl_ptr->len is 0\n");
				mutex_unlock(&cpp_dev->mutex);
				return -EINVAL;
			}
			cpp_dev->fw_name_bin = kzalloc(ioctl_ptr->len+1,
				GFP_KERNEL);
			if (!cpp_dev->fw_name_bin) {
				pr_err("%s:%d: malloc error\n", __func__,
					__LINE__);
				mutex_unlock(&cpp_dev->mutex);
				return -EINVAL;
			}
			if (ioctl_ptr->ioctl_ptr == NULL) {
				pr_err("ioctl_ptr->ioctl_ptr=NULL\n");
				kfree(cpp_dev->fw_name_bin);
				cpp_dev->fw_name_bin = NULL;
				mutex_unlock(&cpp_dev->mutex);
				return -EINVAL;
			}
			rc = (copy_from_user(cpp_dev->fw_name_bin,
				(void __user *)ioctl_ptr->ioctl_ptr,
				ioctl_ptr->len) ? -EFAULT : 0);
			if (rc) {
				ERR_COPY_FROM_USER();
				kfree(cpp_dev->fw_name_bin);
				cpp_dev->fw_name_bin = NULL;
				mutex_unlock(&cpp_dev->mutex);
				return -EINVAL;
			}
			*(cpp_dev->fw_name_bin+ioctl_ptr->len) = '\0';
			disable_irq(cpp_dev->irq->start);
			cpp_load_fw(cpp_dev, cpp_dev->fw_name_bin);
			enable_irq(cpp_dev->irq->start);
			cpp_dev->is_firmware_loaded = 1;
		}
		break;
	}
	case VIDIOC_MSM_CPP_CFG:
		CPP_DBG("VIDIOC_MSM_CPP_CFG\n");
		rc = msm_cpp_cfg(cpp_dev, ioctl_ptr);
		break;
	case VIDIOC_MSM_CPP_FLUSH_QUEUE:
		CPP_DBG("VIDIOC_MSM_CPP_FLUSH_QUEUE\n");
		rc = msm_cpp_flush_frames(cpp_dev);
		break;
	case VIDIOC_MSM_CPP_DELETE_STREAM_BUFF:
	case VIDIOC_MSM_CPP_APPEND_STREAM_BUFF_INFO:
	case VIDIOC_MSM_CPP_ENQUEUE_STREAM_BUFF_INFO: {
		uint32_t j;
		struct msm_cpp_stream_buff_info_t *u_stream_buff_info;
		struct msm_cpp_stream_buff_info_t k_stream_buff_info;
		struct msm_cpp_buff_queue_info_t *buff_queue_info;
		CPP_DBG("VIDIOC_MSM_CPP_ENQUEUE_STREAM_BUFF_INFO\n");
		if (sizeof(struct msm_cpp_stream_buff_info_t) !=
			ioctl_ptr->len) {
			pr_err("%s:%d: invalid length\n", __func__, __LINE__);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		u_stream_buff_info = kzalloc(ioctl_ptr->len, GFP_KERNEL);
		if (!u_stream_buff_info) {
			pr_err("%s:%d: malloc error\n", __func__, __LINE__);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		rc = msm_cpp_copy_from_ioctl_ptr(u_stream_buff_info,
			ioctl_ptr);
		if (rc) {
			ERR_COPY_FROM_USER();
			kfree(u_stream_buff_info);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		if (u_stream_buff_info->num_buffs == 0) {
			pr_err("%s:%d: Invalid number of buffers\n", __func__,
				__LINE__);
			kfree(u_stream_buff_info);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		k_stream_buff_info.num_buffs = u_stream_buff_info->num_buffs;
		k_stream_buff_info.identity = u_stream_buff_info->identity;

		if (k_stream_buff_info.num_buffs > MSM_CAMERA_MAX_STREAM_BUF) {
			pr_err("%s:%d: unexpected large num buff requested\n",
				__func__, __LINE__);
			kfree(u_stream_buff_info);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		k_stream_buff_info.buffer_info =
			kzalloc(k_stream_buff_info.num_buffs *
			sizeof(struct msm_cpp_buffer_info_t), GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(k_stream_buff_info.buffer_info)) {
			pr_err("%s:%d: malloc error\n", __func__, __LINE__);
			kfree(u_stream_buff_info);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		rc = (copy_from_user(k_stream_buff_info.buffer_info,
				(void __user *)u_stream_buff_info->buffer_info,
				k_stream_buff_info.num_buffs *
				sizeof(struct msm_cpp_buffer_info_t)) ?
				-EFAULT : 0);
		if (rc) {
			ERR_COPY_FROM_USER();
			kfree(k_stream_buff_info.buffer_info);
			kfree(u_stream_buff_info);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		buff_queue_info = msm_cpp_get_buff_queue_entry(cpp_dev,
				(k_stream_buff_info.identity >> 16) & 0xFFFF,
				k_stream_buff_info.identity & 0xFFFF);

		if (buff_queue_info == NULL) {
			if (cmd == VIDIOC_MSM_CPP_DELETE_STREAM_BUFF)
				goto STREAM_BUFF_END;

			rc = msm_cpp_add_buff_queue_entry(cpp_dev,
				((k_stream_buff_info.identity >> 16) & 0xFFFF),
				(k_stream_buff_info.identity & 0xFFFF));

			if (rc)
				goto STREAM_BUFF_END;

			if (cpp_dev->stream_cnt == 0) {
				cpp_dev->state = CPP_STATE_ACTIVE;
				msm_cpp_clear_timer(cpp_dev);
				msm_cpp_clean_queue(cpp_dev);
			}
			cpp_dev->stream_cnt++;
			CPP_DBG("stream_cnt:%d\n", cpp_dev->stream_cnt);
		}
		buff_queue_info = msm_cpp_get_buff_queue_entry(cpp_dev,
			((k_stream_buff_info.identity >> 16) & 0xFFFF),
			(k_stream_buff_info.identity & 0xFFFF));
		if (buff_queue_info == NULL) {
			pr_err("error finding buffer queue entry identity:%d\n",
				k_stream_buff_info.identity);
			kfree(k_stream_buff_info.buffer_info);
			kfree(u_stream_buff_info);
			cpp_dev->stream_cnt--;
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		if (VIDIOC_MSM_CPP_DELETE_STREAM_BUFF == cmd) {
			for (j = 0; j < k_stream_buff_info.num_buffs; j++) {
				msm_cpp_dequeue_buff(cpp_dev, buff_queue_info,
				k_stream_buff_info.buffer_info[j].index,
				k_stream_buff_info.buffer_info[j].native_buff);
			}
		} else {
			for (j = 0; j < k_stream_buff_info.num_buffs; j++) {
				msm_cpp_queue_buffer_info(cpp_dev,
					buff_queue_info,
					&k_stream_buff_info.buffer_info[j]);
			}
		}

STREAM_BUFF_END:
		kfree(k_stream_buff_info.buffer_info);
		kfree(u_stream_buff_info);

		break;
	}
	case VIDIOC_MSM_CPP_DEQUEUE_STREAM_BUFF_INFO: {
		uint32_t identity;
		struct msm_cpp_buff_queue_info_t *buff_queue_info;
		CPP_DBG("VIDIOC_MSM_CPP_DEQUEUE_STREAM_BUFF_INFO\n");
		if (ioctl_ptr->len != sizeof(uint32_t)) {
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		rc = msm_cpp_copy_from_ioctl_ptr(&identity, ioctl_ptr);
		if (rc) {
			ERR_COPY_FROM_USER();
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		buff_queue_info = msm_cpp_get_buff_queue_entry(cpp_dev,
			((identity >> 16) & 0xFFFF), (identity & 0xFFFF));
		if (buff_queue_info == NULL) {
			pr_err("error finding buffer queue entry for identity:%d\n",
				identity);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		msm_cpp_dequeue_buff_info_list(cpp_dev, buff_queue_info);
		rc = msm_cpp_free_buff_queue_entry(cpp_dev,
			buff_queue_info->session_id,
			buff_queue_info->stream_id);
		if (cpp_dev->stream_cnt > 0) {
			cpp_dev->stream_cnt--;
			pr_info("stream_cnt:%d\n", cpp_dev->stream_cnt);
			if (cpp_dev->stream_cnt == 0) {
				if (cpp_dev->bus_master_flag)
					rc = msm_cpp_update_bandwidth(cpp_dev,
						 0, 0);
				else
					rc = msm_isp_update_bandwidth(ISP_CPP,
						 0, 0);
				if (rc < 0)
					pr_err("Bandwidth Reset Failed!\n");
				cpp_dev->state = CPP_STATE_IDLE;
				msm_cpp_clear_timer(cpp_dev);
				msm_cpp_clean_queue(cpp_dev);
			}
		} else {
			pr_err("error: stream count underflow %d\n",
				cpp_dev->stream_cnt);
		}
		break;
	}
	case VIDIOC_MSM_CPP_GET_EVENTPAYLOAD: {
		struct msm_device_queue *queue = &cpp_dev->eventData_q;
		struct msm_queue_cmd *event_qcmd;
		struct msm_cpp_frame_info_t *process_frame;
		CPP_DBG("VIDIOC_MSM_CPP_GET_EVENTPAYLOAD\n");
		event_qcmd = msm_dequeue(queue, list_eventdata);
		if (!event_qcmd) {
			pr_err("no queue cmd available");
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		process_frame = event_qcmd->command;
		CPP_DBG("fid %d\n", process_frame->frame_id);
		if (copy_to_user((void __user *)ioctl_ptr->ioctl_ptr,
				process_frame,
				sizeof(struct msm_cpp_frame_info_t))) {
					mutex_unlock(&cpp_dev->mutex);
					kfree(process_frame->cpp_cmd_msg);
					kfree(process_frame);
					kfree(event_qcmd);
					return -EINVAL;
		}

		kfree(process_frame->cpp_cmd_msg);
		kfree(process_frame);
		kfree(event_qcmd);
		break;
	}
	case VIDIOC_MSM_CPP_SET_CLOCK: {
		uint32_t msm_cpp_core_clk_idx;
		struct msm_cpp_clock_settings_t clock_settings;
		unsigned long clock_rate = 0;
		CPP_DBG("VIDIOC_MSM_CPP_SET_CLOCK\n");
		if (ioctl_ptr->len == 0) {
			pr_err("ioctl_ptr->len is 0\n");
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		if (ioctl_ptr->ioctl_ptr == NULL) {
			pr_err("ioctl_ptr->ioctl_ptr is NULL\n");
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		if (ioctl_ptr->len != sizeof(struct msm_cpp_clock_settings_t)) {
			pr_err("Not valid ioctl_ptr->len\n");
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		rc = msm_cpp_copy_from_ioctl_ptr(&clock_settings, ioctl_ptr);
		if (rc) {
			ERR_COPY_FROM_USER();
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		if (clock_settings.clock_rate > 0) {
			msm_cpp_core_clk_idx = get_clock_index("cpp_core_clk");
			if (msm_cpp_core_clk_idx < 0) {
				pr_err(" Fail to get clock index\n");
				return -EINVAL;
			}
			if (cpp_dev->bus_master_flag)
				rc = msm_cpp_update_bandwidth(cpp_dev,
					clock_settings.avg,
					clock_settings.inst);
			else
				rc = msm_isp_update_bandwidth(ISP_CPP,
					clock_settings.avg,
					clock_settings.inst);
			if (rc < 0) {
				pr_err("Bandwidth Set Failed!\n");
				if (cpp_dev->bus_master_flag)
					rc = msm_cpp_update_bandwidth(cpp_dev,
						0, 0);
				else
					rc = msm_isp_update_bandwidth(ISP_CPP,
						0, 0);
				mutex_unlock(&cpp_dev->mutex);
				return -EINVAL;
			}
			clock_rate = clk_round_rate(
				cpp_dev->cpp_clk[msm_cpp_core_clk_idx],
				clock_settings.clock_rate);
			if (clock_rate != clock_settings.clock_rate)
				pr_err("clock rate differ from settings\n");
			clk_set_rate(cpp_dev->cpp_clk[msm_cpp_core_clk_idx],
				clock_rate);
			msm_isp_util_update_clk_rate(clock_rate);
		}
		break;
	}
	case MSM_SD_SHUTDOWN:
		CPP_DBG("MSM_SD_SHUTDOWN\n");
		mutex_unlock(&cpp_dev->mutex);
		pr_info("shutdown cpp node. open cnt:%d\n",
			cpp_dev->cpp_open_cnt);

		if (atomic_read(&cpp_timer.used))
			pr_info("Timer state not cleared\n");

		while (cpp_dev->cpp_open_cnt != 0)
			cpp_close_node(sd, NULL);
		mutex_lock(&cpp_dev->mutex);
		rc = 0;
		break;
	case VIDIOC_MSM_CPP_QUEUE_BUF: {
		struct msm_pproc_queue_buf_info queue_buf_info;
		CPP_DBG("VIDIOC_MSM_CPP_QUEUE_BUF\n");

		if (ioctl_ptr->len != sizeof(struct msm_pproc_queue_buf_info)) {
			pr_err("%s: Not valid ioctl_ptr->len\n", __func__);
			return -EINVAL;
		}
		rc = msm_cpp_copy_from_ioctl_ptr(&queue_buf_info, ioctl_ptr);
		if (rc) {
			ERR_COPY_FROM_USER();
			break;
		}

		if (queue_buf_info.is_buf_dirty) {
			rc = msm_cpp_buffer_ops(cpp_dev,
				VIDIOC_MSM_BUF_MNGR_PUT_BUF,
				&queue_buf_info.buff_mgr_info);
		} else {
			rc = msm_cpp_buffer_ops(cpp_dev,
				VIDIOC_MSM_BUF_MNGR_BUF_DONE,
				&queue_buf_info.buff_mgr_info);
		}
		if (rc < 0) {
			pr_err("error in buf done\n");
			rc = -EINVAL;
		}

		break;
	}
	case VIDIOC_MSM_CPP_POP_STREAM_BUFFER: {
		struct msm_buf_mngr_info buff_mgr_info;
		struct msm_cpp_frame_info_t frame_info;
		if (ioctl_ptr->ioctl_ptr == NULL ||
			(ioctl_ptr->len !=
			sizeof(struct msm_cpp_frame_info_t))) {
			rc = -EINVAL;
			break;
		}

		rc = msm_cpp_copy_from_ioctl_ptr(&frame_info, ioctl_ptr);
		if (rc) {
			ERR_COPY_FROM_USER();
			break;
		}

		memset(&buff_mgr_info, 0, sizeof(struct msm_buf_mngr_info));
		buff_mgr_info.session_id =
			((frame_info.identity >> 16) & 0xFFFF);
		buff_mgr_info.stream_id = (frame_info.identity & 0xFFFF);
		buff_mgr_info.type =
			MSM_CAMERA_BUF_MNGR_BUF_PLANAR;
		rc = msm_cpp_buffer_ops(cpp_dev, VIDIOC_MSM_BUF_MNGR_GET_BUF,
			&buff_mgr_info);
		if (rc < 0) {
			rc = -EAGAIN;
			pr_err_ratelimited("error getting buffer rc:%d\n", rc);
			break;
		}
		buff_mgr_info.frame_id = frame_info.frame_id;
		rc = msm_cpp_buffer_ops(cpp_dev, VIDIOC_MSM_BUF_MNGR_BUF_DONE,
			&buff_mgr_info);
		if (rc < 0) {
			pr_err("error in buf done\n");
			rc = -EAGAIN;
		}
		break;
	}
	default:
		pr_err_ratelimited("invalid value: cmd=0x%x\n", cmd);
		break;
	case VIDIOC_MSM_CPP_IOMMU_ATTACH: {
		if (cpp_dev->iommu_state == CPP_IOMMU_STATE_DETACHED) {
			rc = iommu_attach_device(cpp_dev->domain,
				cpp_dev->iommu_ctx);
			if (rc < 0) {
				pr_err("%s:%dError iommu_attach_device failed\n",
					__func__, __LINE__);
				rc = -EINVAL;
				break;
			}
			cpp_dev->iommu_state = CPP_IOMMU_STATE_ATTACHED;
		} else {
			pr_err("%s:%d IOMMMU attach triggered in invalid state\n",
				__func__, __LINE__);
			rc = -EINVAL;
		}
		break;
	}
	case VIDIOC_MSM_CPP_IOMMU_DETACH: {
		if ((cpp_dev->iommu_state == CPP_IOMMU_STATE_ATTACHED) &&
			(cpp_dev->stream_cnt == 0)) {
			iommu_detach_device(cpp_dev->domain,
				cpp_dev->iommu_ctx);
			if (rc < 0) {
				pr_err("%s:%dError iommu detach failed\n",
					__func__, __LINE__);
				rc = -EINVAL;
				break;
			}
			cpp_dev->iommu_state = CPP_IOMMU_STATE_DETACHED;
		} else {
			pr_err("%s:%d IOMMMU attach triggered in invalid state\n",
				__func__, __LINE__);
			rc = -EINVAL;
		}
		break;
	}
	}
	mutex_unlock(&cpp_dev->mutex);
	CPP_DBG("X\n");
	return rc;
}

int msm_cpp_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
	struct v4l2_event_subscription *sub)
{
	CPP_DBG("Called\n");
	return v4l2_event_subscribe(fh, sub, MAX_CPP_V4l2_EVENTS, NULL);
}

int msm_cpp_unsubscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
	struct v4l2_event_subscription *sub)
{
	CPP_DBG("Called\n");
	return v4l2_event_unsubscribe(fh, sub);
}

static struct v4l2_subdev_core_ops msm_cpp_subdev_core_ops = {
	.ioctl = msm_cpp_subdev_ioctl,
	.subscribe_event = msm_cpp_subscribe_event,
	.unsubscribe_event = msm_cpp_unsubscribe_event,
};

static const struct v4l2_subdev_ops msm_cpp_subdev_ops = {
	.core = &msm_cpp_subdev_core_ops,
};

static long msm_cpp_subdev_do_ioctl(
	struct file *file, unsigned int cmd, void *arg)
{
	struct video_device *vdev;
	struct v4l2_subdev *sd;
	struct v4l2_fh *vfh = NULL;

	if ((arg == NULL) || (file == NULL)) {
		pr_err("Invalid input parameters arg %pK, file %pK\n", arg, file);
		return -EINVAL;
	}
	vdev = video_devdata(file);
	sd = vdev_to_v4l2_subdev(vdev);

	if (sd == NULL) {
		pr_err("Invalid input parameter sd %pK\n", sd);
		return -EINVAL;
	}
	vfh = file->private_data;

	switch (cmd) {
	case VIDIOC_DQEVENT:
		if (!(sd->flags & V4L2_SUBDEV_FL_HAS_EVENTS))
			return -ENOIOCTLCMD;

		return v4l2_event_dequeue(vfh, arg, file->f_flags & O_NONBLOCK);

	case VIDIOC_SUBSCRIBE_EVENT:
		return v4l2_subdev_call(sd, core, subscribe_event, vfh, arg);

	case VIDIOC_UNSUBSCRIBE_EVENT:
		return v4l2_subdev_call(sd, core, unsubscribe_event, vfh, arg);

	case VIDIOC_MSM_CPP_GET_INST_INFO: {
		uint32_t i;
		struct cpp_device *cpp_dev = v4l2_get_subdevdata(sd);
		struct msm_camera_v4l2_ioctl_t *ioctl_ptr = arg;
		struct msm_cpp_frame_info_t inst_info;
		memset(&inst_info, 0, sizeof(struct msm_cpp_frame_info_t));
		for (i = 0; i < MAX_ACTIVE_CPP_INSTANCE; i++) {
			if (cpp_dev->cpp_subscribe_list[i].vfh == vfh) {
				inst_info.inst_id = i;
				break;
			}
		}
		if (copy_to_user(
				(void __user *)ioctl_ptr->ioctl_ptr, &inst_info,
				sizeof(struct msm_cpp_frame_info_t))) {
			return -EINVAL;
		}
	}
	break;
	default:
		return v4l2_subdev_call(sd, core, ioctl, cmd, arg);
	}

	return 0;
}

static long msm_cpp_subdev_fops_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	return video_usercopy(file, cmd, arg, msm_cpp_subdev_do_ioctl);
}


#ifdef CONFIG_COMPAT
static struct msm_cpp_frame_info_t *get_64bit_cpp_frame_from_compat(
	struct msm_camera_v4l2_ioctl_t *kp_ioctl)
{
	struct msm_cpp_frame_info32_t *new_frame32 = NULL;
	struct msm_cpp_frame_info_t *new_frame = NULL;
	uint32_t *cpp_frame_msg;
	void *cpp_cmd_msg_64bit;
	int32_t rc, i;

	new_frame32 = kzalloc(sizeof(struct msm_cpp_frame_info32_t),
		GFP_KERNEL);
	if (!new_frame32) {
		pr_err("Insufficient memory\n");
		goto no_mem32;
	}
	new_frame = kzalloc(sizeof(struct msm_cpp_frame_info_t), GFP_KERNEL);
	if (!new_frame) {
		pr_err("Insufficient memory\n");
		goto no_mem;
	}

	rc = (copy_from_user(new_frame32, (void __user *)kp_ioctl->ioctl_ptr,
			sizeof(struct msm_cpp_frame_info32_t)) ? -EFAULT : 0);
	if (rc) {
		ERR_COPY_FROM_USER();
		goto frame_err;
	}

	new_frame->frame_id = new_frame32->frame_id;
	new_frame->inst_id = new_frame32->inst_id;
	new_frame->client_id = new_frame32->client_id;
	new_frame->frame_type = new_frame32->frame_type;
	new_frame->num_strips = new_frame32->num_strips;
	new_frame->strip_info = compat_ptr(new_frame32->strip_info);

	new_frame->src_fd =  new_frame32->src_fd;
	new_frame->dst_fd =  new_frame32->dst_fd;

	new_frame->timestamp.tv_sec =
		(unsigned long)new_frame32->timestamp.tv_sec;
	new_frame->timestamp.tv_usec =
		(unsigned long)new_frame32->timestamp.tv_usec;

	new_frame->in_time.tv_sec =
		(unsigned long)new_frame32->in_time.tv_sec;
	new_frame->in_time.tv_usec =
		(unsigned long)new_frame32->in_time.tv_usec;

	new_frame->out_time.tv_sec =
		(unsigned long)new_frame32->out_time.tv_sec;
	new_frame->out_time.tv_usec =
		(unsigned long)new_frame32->out_time.tv_usec;

	new_frame->msg_len = new_frame32->msg_len;
	new_frame->identity = new_frame32->identity;
	new_frame->input_buffer_info = new_frame32->input_buffer_info;
	new_frame->output_buffer_info[0] =
		new_frame32->output_buffer_info[0];
	new_frame->output_buffer_info[1] =
		new_frame32->output_buffer_info[1];
	new_frame->output_buffer_info[2] =
		new_frame32->output_buffer_info[2];
	new_frame->output_buffer_info[3] =
		new_frame32->output_buffer_info[3];
	new_frame->output_buffer_info[4] =
		new_frame32->output_buffer_info[4];
	new_frame->output_buffer_info[5] =
		new_frame32->output_buffer_info[5];
	new_frame->output_buffer_info[6] =
		new_frame32->output_buffer_info[6];
	new_frame->output_buffer_info[7] =
		new_frame32->output_buffer_info[7];
	new_frame->duplicate_buffer_info =
		new_frame32->duplicate_buffer_info;
	new_frame->tnr_scratch_buffer_info[0] =
		new_frame32->tnr_scratch_buffer_info[0];
	new_frame->tnr_scratch_buffer_info[1] =
		new_frame32->tnr_scratch_buffer_info[1];
	new_frame->duplicate_output = new_frame32->duplicate_output;
	new_frame->we_disable = new_frame32->we_disable;
	new_frame->duplicate_identity = new_frame32->duplicate_identity;
	new_frame->feature_mask = new_frame32->feature_mask;
	new_frame->reserved = new_frame32->reserved;
	new_frame->partial_frame_indicator =
		new_frame32->partial_frame_indicator;
	new_frame->first_payload = new_frame32->first_payload;
	new_frame->last_payload = new_frame32->last_payload;
	new_frame->first_stripe_index = new_frame32->first_stripe_index;
	new_frame->last_stripe_index = new_frame32->last_stripe_index;
	new_frame->stripe_info_offset =
		new_frame32->stripe_info_offset;
	new_frame->stripe_info = new_frame32->stripe_info;
	new_frame->batch_info.batch_mode =
		new_frame32->batch_info.batch_mode;
	new_frame->batch_info.batch_size =
		new_frame32->batch_info.batch_size;
	new_frame->batch_info.cont_idx =
		new_frame32->batch_info.cont_idx;
	for (i = 0; i < MAX_PLANES; i++)
		new_frame->batch_info.intra_plane_offset[i] =
			new_frame32->batch_info.intra_plane_offset[i];
	new_frame->batch_info.pick_preview_idx =
		new_frame32->batch_info.pick_preview_idx;

	/* Convert the 32 bit pointer to 64 bit pointer */
	new_frame->cookie = compat_ptr(new_frame32->cookie);
	cpp_cmd_msg_64bit = compat_ptr(new_frame32->cpp_cmd_msg);
	if ((new_frame->msg_len == 0) ||
		(new_frame->msg_len > MSM_CPP_MAX_FRAME_LENGTH)) {
		pr_err("%s:%d: Invalid frame len:%d\n", __func__,
			__LINE__, new_frame->msg_len);
		goto frame_err;
	}

	cpp_frame_msg = kzalloc(sizeof(uint32_t)*new_frame->msg_len,
		GFP_KERNEL);
	if (!cpp_frame_msg) {
		pr_err("Insufficient memory\n");
		goto frame_err;
	}

	rc = (copy_from_user(cpp_frame_msg,
		(void __user *)cpp_cmd_msg_64bit,
		sizeof(uint32_t)*new_frame->msg_len) ? -EFAULT : 0);
	if (rc) {
		ERR_COPY_FROM_USER();
		goto frame_msg_err;
	}
	new_frame->cpp_cmd_msg = cpp_frame_msg;

	kfree(new_frame32);
	return new_frame;

frame_msg_err:
	kfree(cpp_frame_msg);
frame_err:
	kfree(new_frame);
no_mem:
	kfree(new_frame32);
no_mem32:
	return NULL;
}

static void get_compat_frame_from_64bit(struct msm_cpp_frame_info_t *frame,
	struct msm_cpp_frame_info32_t *k32_frame)
{
	int32_t i;

	k32_frame->frame_id = frame->frame_id;
	k32_frame->inst_id = frame->inst_id;
	k32_frame->client_id = frame->client_id;
	k32_frame->frame_type = frame->frame_type;
	k32_frame->num_strips = frame->num_strips;
	k32_frame->strip_info = ptr_to_compat(frame->strip_info);

	k32_frame->src_fd = frame->src_fd;
	k32_frame->dst_fd = frame->dst_fd;

	k32_frame->timestamp.tv_sec = (uint32_t)frame->timestamp.tv_sec;
	k32_frame->timestamp.tv_usec = (uint32_t)frame->timestamp.tv_usec;

	k32_frame->in_time.tv_sec = (uint32_t)frame->in_time.tv_sec;
	k32_frame->in_time.tv_usec = (uint32_t)frame->in_time.tv_usec;

	k32_frame->out_time.tv_sec = (uint32_t)frame->out_time.tv_sec;
	k32_frame->out_time.tv_usec = (uint32_t)frame->out_time.tv_usec;

	k32_frame->msg_len = frame->msg_len;
	k32_frame->identity = frame->identity;
	k32_frame->input_buffer_info = frame->input_buffer_info;
	k32_frame->output_buffer_info[0] = frame->output_buffer_info[0];
	k32_frame->output_buffer_info[1] = frame->output_buffer_info[1];
	k32_frame->output_buffer_info[2] = frame->output_buffer_info[2];
	k32_frame->output_buffer_info[3] = frame->output_buffer_info[3];
	k32_frame->output_buffer_info[4] = frame->output_buffer_info[4];
	k32_frame->output_buffer_info[5] = frame->output_buffer_info[5];
	k32_frame->output_buffer_info[6] = frame->output_buffer_info[6];
	k32_frame->output_buffer_info[7] = frame->output_buffer_info[7];
	k32_frame->duplicate_buffer_info = frame->duplicate_buffer_info;
	k32_frame->duplicate_output = frame->duplicate_output;
	k32_frame->we_disable = frame->we_disable;
	k32_frame->duplicate_identity = frame->duplicate_identity;
	k32_frame->feature_mask = frame->feature_mask;
	k32_frame->reserved = frame->reserved;
	k32_frame->cookie = ptr_to_compat(frame->cookie);
	k32_frame->partial_frame_indicator = frame->partial_frame_indicator;
	k32_frame->first_payload = frame->first_payload;
	k32_frame->last_payload = frame->last_payload;
	k32_frame->first_stripe_index = frame->first_stripe_index;
	k32_frame->last_stripe_index = frame->last_stripe_index;
	k32_frame->stripe_info_offset = frame->stripe_info_offset;
	k32_frame->stripe_info = frame->stripe_info;
	k32_frame->batch_info.batch_mode = frame->batch_info.batch_mode;
	k32_frame->batch_info.batch_size = frame->batch_info.batch_size;
	k32_frame->batch_info.cont_idx = frame->batch_info.cont_idx;
	for (i = 0; i < MAX_PLANES; i++)
		k32_frame->batch_info.intra_plane_offset[i] =
			frame->batch_info.intra_plane_offset[i];
	k32_frame->batch_info.pick_preview_idx =
		frame->batch_info.pick_preview_idx;
}

static long msm_cpp_subdev_fops_compat_ioctl(struct file *file,
	unsigned int cmd, unsigned long arg)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_subdev *sd = vdev_to_v4l2_subdev(vdev);
	struct cpp_device *cpp_dev = NULL;

	int32_t rc = 0;
	struct msm_camera_v4l2_ioctl_t kp_ioctl;
	struct msm_camera_v4l2_ioctl32_t up32_ioctl;
	struct msm_cpp_clock_settings_t clock_settings;
	struct msm_pproc_queue_buf_info k_queue_buf;
	struct msm_cpp_stream_buff_info_t k_cpp_buff_info;
	struct msm_cpp_frame_info32_t k32_frame_info;
	struct msm_cpp_frame_info_t k64_frame_info;
	void __user *up = (void __user *)arg;
	bool is_copytouser_req = true;

	if (sd == NULL) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return -EINVAL;
	}
	cpp_dev = v4l2_get_subdevdata(sd);
	if (!vdev || !cpp_dev) {
		pr_err("Invalid vdev %pK or cpp_dev %pK structures!",
			vdev, cpp_dev);
		return -EINVAL;
	}
	/*
	 * copy the user space 32 bit pointer to kernel space 32 bit compat
	 * pointer
	 */
	if (copy_from_user(&up32_ioctl, (void __user *)up,
		sizeof(up32_ioctl)))
		return -EFAULT;

	/* copy the data from 32 bit compat to kernel space 64 bit pointer */
	kp_ioctl.id = up32_ioctl.id;
	kp_ioctl.len = up32_ioctl.len;
	kp_ioctl.trans_code = up32_ioctl.trans_code;
	/* Convert the 32 bit pointer to 64 bit pointer */
	kp_ioctl.ioctl_ptr = compat_ptr(up32_ioctl.ioctl_ptr);
	if (!kp_ioctl.ioctl_ptr) {
		pr_err("%s: Invalid ioctl pointer\n", __func__);
		return -EINVAL;
	}

	/*
	 * Convert 32 bit IOCTL ID's to 64 bit IOCTL ID's
	 * except VIDIOC_MSM_CPP_CFG32, which needs special
	 * processing
	 */
	switch (cmd) {
	case VIDIOC_MSM_CPP_CFG32:
	{
		struct msm_cpp_frame_info32_t k32_frame_info;
		struct msm_cpp_frame_info_t *cpp_frame = NULL;
		int32_t *status;

		if (copy_from_user(&k32_frame_info,
			(void __user *)kp_ioctl.ioctl_ptr,
			sizeof(k32_frame_info)))
			return -EFAULT;
		/* Get the cpp frame pointer */
		cpp_frame = get_64bit_cpp_frame_from_compat(&kp_ioctl);

		/* Configure the cpp frame */
		if (cpp_frame) {
			rc = msm_cpp_cfg_frame(cpp_dev, cpp_frame);
			/* Cpp_frame can be free'd by cfg_frame in error */
			if (rc >= 0) {
				k32_frame_info.output_buffer_info[0] =
					cpp_frame->output_buffer_info[0];
				k32_frame_info.output_buffer_info[1] =
					cpp_frame->output_buffer_info[1];
			}
		} else {
			pr_err("%s: Error getting frame\n", __func__);
			rc = -EINVAL;
		}

		kp_ioctl.trans_code = rc;

		/* Convert the 32 bit pointer to 64 bit pointer */
		status = compat_ptr(k32_frame_info.status);

		if (copy_to_user((void __user *)status, &rc,
			sizeof(int32_t)))
			pr_err("error cannot copy error\n");

		if (copy_to_user((void __user *)kp_ioctl.ioctl_ptr,
			&k32_frame_info,
			sizeof(k32_frame_info)))
			return -EFAULT;

		cmd = VIDIOC_MSM_CPP_CFG;
		break;
	}
	case VIDIOC_MSM_CPP_GET_HW_INFO32:
	{
		struct cpp_hw_info_32_t u32_cpp_hw_info;
		uint32_t i;

		u32_cpp_hw_info.cpp_hw_version =
			cpp_dev->hw_info.cpp_hw_version;
		u32_cpp_hw_info.cpp_hw_caps = cpp_dev->hw_info.cpp_hw_caps;
		memset(&u32_cpp_hw_info.freq_tbl, 0x00,
				sizeof(u32_cpp_hw_info.freq_tbl));
		for (i = 0; i < cpp_dev->hw_info.freq_tbl_count; i++)
			u32_cpp_hw_info.freq_tbl[i] =
				cpp_dev->hw_info.freq_tbl[i];

		u32_cpp_hw_info.freq_tbl_count =
			cpp_dev->hw_info.freq_tbl_count;
		if (copy_to_user((void __user *)kp_ioctl.ioctl_ptr,
			&u32_cpp_hw_info, sizeof(struct cpp_hw_info_32_t)))
			return -EINVAL;

		cmd = VIDIOC_MSM_CPP_GET_HW_INFO;
		break;
	}
	case VIDIOC_MSM_CPP_LOAD_FIRMWARE32:
		cmd = VIDIOC_MSM_CPP_LOAD_FIRMWARE;
		break;
	case VIDIOC_MSM_CPP_GET_INST_INFO32:
	{
		struct cpp_device *cpp_dev = v4l2_get_subdevdata(sd);
		struct msm_cpp_frame_info32_t inst_info;
		struct v4l2_fh *vfh = NULL;
		uint32_t i;
		vfh = file->private_data;
		memset(&inst_info, 0, sizeof(struct msm_cpp_frame_info32_t));
		for (i = 0; i < MAX_ACTIVE_CPP_INSTANCE; i++) {
			if (cpp_dev->cpp_subscribe_list[i].vfh == vfh) {
				inst_info.inst_id = i;
				break;
			}
		}
		if (copy_to_user(
				(void __user *)kp_ioctl.ioctl_ptr, &inst_info,
				sizeof(struct msm_cpp_frame_info32_t))) {
			return -EINVAL;
		}
		cmd = VIDIOC_MSM_CPP_GET_INST_INFO;
		break;
	}
	case VIDIOC_MSM_CPP_FLUSH_QUEUE32:
		cmd = VIDIOC_MSM_CPP_FLUSH_QUEUE;
		break;
	case VIDIOC_MSM_CPP_APPEND_STREAM_BUFF_INFO32:
	case VIDIOC_MSM_CPP_ENQUEUE_STREAM_BUFF_INFO32:
	case VIDIOC_MSM_CPP_DELETE_STREAM_BUFF32:
	{
		struct msm_cpp_stream_buff_info32_t *u32_cpp_buff_info =
		  (struct msm_cpp_stream_buff_info32_t *)kp_ioctl.ioctl_ptr;

		k_cpp_buff_info.identity = u32_cpp_buff_info->identity;
		k_cpp_buff_info.num_buffs = u32_cpp_buff_info->num_buffs;
		k_cpp_buff_info.buffer_info =
			compat_ptr(u32_cpp_buff_info->buffer_info);

		kp_ioctl.ioctl_ptr = (void *)&k_cpp_buff_info;
		if (is_compat_task()) {
			if (kp_ioctl.len != sizeof(
				struct msm_cpp_stream_buff_info32_t))
				return -EINVAL;
			else
				kp_ioctl.len =
				  sizeof(struct msm_cpp_stream_buff_info_t);
		}
		is_copytouser_req = false;
		if (cmd == VIDIOC_MSM_CPP_ENQUEUE_STREAM_BUFF_INFO32)
			cmd = VIDIOC_MSM_CPP_ENQUEUE_STREAM_BUFF_INFO;
		else if (cmd == VIDIOC_MSM_CPP_DELETE_STREAM_BUFF32)
			cmd = VIDIOC_MSM_CPP_DELETE_STREAM_BUFF;
		else
			cmd = VIDIOC_MSM_CPP_APPEND_STREAM_BUFF_INFO;
		break;
	}
	case VIDIOC_MSM_CPP_DEQUEUE_STREAM_BUFF_INFO32:
		cmd = VIDIOC_MSM_CPP_DEQUEUE_STREAM_BUFF_INFO;
		break;
	case VIDIOC_MSM_CPP_GET_EVENTPAYLOAD32:
	{
		struct msm_device_queue *queue = &cpp_dev->eventData_q;
		struct msm_queue_cmd *event_qcmd;
		struct msm_cpp_frame_info_t *process_frame;
		struct msm_cpp_frame_info32_t k32_process_frame;

		CPP_DBG("VIDIOC_MSM_CPP_GET_EVENTPAYLOAD\n");
		mutex_lock(&cpp_dev->mutex);
		event_qcmd = msm_dequeue(queue, list_eventdata);
		if (!event_qcmd) {
			pr_err("no queue cmd available");
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}
		process_frame = event_qcmd->command;

		memset(&k32_process_frame, 0, sizeof(k32_process_frame));
		get_compat_frame_from_64bit(process_frame, &k32_process_frame);

		CPP_DBG("fid %d\n", process_frame->frame_id);
		if (copy_to_user((void __user *)kp_ioctl.ioctl_ptr,
			&k32_process_frame,
			sizeof(struct msm_cpp_frame_info32_t))) {
			kfree(process_frame->cpp_cmd_msg);
			kfree(process_frame);
			kfree(event_qcmd);
			mutex_unlock(&cpp_dev->mutex);
			return -EINVAL;
		}

		kfree(process_frame->cpp_cmd_msg);
		kfree(process_frame);
		kfree(event_qcmd);
		cmd = VIDIOC_MSM_CPP_GET_EVENTPAYLOAD;
		mutex_unlock(&cpp_dev->mutex);
		break;
	}
	case VIDIOC_MSM_CPP_SET_CLOCK32:
	{
		struct msm_cpp_clock_settings32_t *clock_settings32 =
			(struct msm_cpp_clock_settings32_t *)kp_ioctl.ioctl_ptr;
		clock_settings.clock_rate = clock_settings32->clock_rate;
		clock_settings.avg = clock_settings32->avg;
		clock_settings.inst = clock_settings32->inst;
		kp_ioctl.ioctl_ptr = (void *)&clock_settings;
		if (is_compat_task()) {
			if (kp_ioctl.len != sizeof(
				struct msm_cpp_clock_settings32_t))
				return -EINVAL;
			else
				kp_ioctl.len =
					sizeof(struct msm_cpp_clock_settings_t);
		}
		is_copytouser_req = false;
		cmd = VIDIOC_MSM_CPP_SET_CLOCK;
		break;
	}
	case VIDIOC_MSM_CPP_QUEUE_BUF32:
	{
		struct msm_pproc_queue_buf_info32_t *u32_queue_buf =
		  (struct msm_pproc_queue_buf_info32_t *)kp_ioctl.ioctl_ptr;

		k_queue_buf.is_buf_dirty = u32_queue_buf->is_buf_dirty;
		k_queue_buf.buff_mgr_info.session_id =
			u32_queue_buf->buff_mgr_info.session_id;
		k_queue_buf.buff_mgr_info.stream_id =
			u32_queue_buf->buff_mgr_info.stream_id;
		k_queue_buf.buff_mgr_info.frame_id =
			u32_queue_buf->buff_mgr_info.frame_id;
		k_queue_buf.buff_mgr_info.index =
			u32_queue_buf->buff_mgr_info.index;
		k_queue_buf.buff_mgr_info.timestamp.tv_sec =
			u32_queue_buf->buff_mgr_info.timestamp.tv_sec;
		k_queue_buf.buff_mgr_info.timestamp.tv_usec =
			u32_queue_buf->buff_mgr_info.timestamp.tv_usec;
		/*
		 * Update the reserved field (cds information) to buffer
		 * manager structure so that it is propogated back to HAL
		 */
		k_queue_buf.buff_mgr_info.reserved =
			u32_queue_buf->buff_mgr_info.reserved;

		kp_ioctl.ioctl_ptr = (void *)&k_queue_buf;
		kp_ioctl.len = sizeof(struct msm_pproc_queue_buf_info);
		is_copytouser_req = false;
		cmd = VIDIOC_MSM_CPP_QUEUE_BUF;
		break;
	}
	case VIDIOC_MSM_CPP_POP_STREAM_BUFFER32:
	{
		if (kp_ioctl.len != sizeof(struct msm_cpp_frame_info32_t))
			return -EINVAL;
		else
			kp_ioctl.len = sizeof(struct msm_cpp_frame_info_t);

		if (copy_from_user(&k32_frame_info,
			(void __user *)kp_ioctl.ioctl_ptr,
			sizeof(k32_frame_info)))
			return -EFAULT;

		memset(&k64_frame_info, 0, sizeof(k64_frame_info));
		k64_frame_info.identity = k32_frame_info.identity;
		k64_frame_info.frame_id = k32_frame_info.frame_id;

		kp_ioctl.ioctl_ptr = (void *)&k64_frame_info;
		is_copytouser_req = false;
		cmd = VIDIOC_MSM_CPP_POP_STREAM_BUFFER;
		break;
	}
	case VIDIOC_MSM_CPP_IOMMU_ATTACH32:
		cmd = VIDIOC_MSM_CPP_IOMMU_ATTACH;
		break;
	case VIDIOC_MSM_CPP_IOMMU_DETACH32:
		cmd = VIDIOC_MSM_CPP_IOMMU_DETACH;
		break;
	case MSM_SD_SHUTDOWN:
		cmd = MSM_SD_SHUTDOWN;
		break;
	default:
		pr_err_ratelimited("%s: unsupported compat type :%x LOAD %lu\n",
				__func__, cmd, VIDIOC_MSM_CPP_LOAD_FIRMWARE);
		return -EINVAL;
	}

	switch (cmd) {
	case VIDIOC_MSM_CPP_LOAD_FIRMWARE:
	case VIDIOC_MSM_CPP_FLUSH_QUEUE:
	case VIDIOC_MSM_CPP_APPEND_STREAM_BUFF_INFO:
	case VIDIOC_MSM_CPP_ENQUEUE_STREAM_BUFF_INFO:
	case VIDIOC_MSM_CPP_DELETE_STREAM_BUFF:
	case VIDIOC_MSM_CPP_DEQUEUE_STREAM_BUFF_INFO:
	case VIDIOC_MSM_CPP_SET_CLOCK:
	case VIDIOC_MSM_CPP_QUEUE_BUF:
	case VIDIOC_MSM_CPP_POP_STREAM_BUFFER:
	case VIDIOC_MSM_CPP_IOMMU_ATTACH:
	case VIDIOC_MSM_CPP_IOMMU_DETACH:
	case MSM_SD_SHUTDOWN:
		rc = v4l2_subdev_call(sd, core, ioctl, cmd, &kp_ioctl);
		break;
	case VIDIOC_MSM_CPP_GET_HW_INFO:
	case VIDIOC_MSM_CPP_CFG:
	case VIDIOC_MSM_CPP_GET_EVENTPAYLOAD:
	case VIDIOC_MSM_CPP_GET_INST_INFO:
		break;
	default:
		pr_err_ratelimited("%s: unsupported compat type :%d\n",
				__func__, cmd);
		return -EINVAL;
	}

	if (is_copytouser_req) {
		up32_ioctl.id = kp_ioctl.id;
		up32_ioctl.len = kp_ioctl.len;
		up32_ioctl.trans_code = kp_ioctl.trans_code;
		up32_ioctl.ioctl_ptr = ptr_to_compat(kp_ioctl.ioctl_ptr);

		if (copy_to_user((void __user *)up, &up32_ioctl,
			sizeof(up32_ioctl)))
			return -EFAULT;
	}
	return rc;
}
#endif

static int cpp_register_domain(void)
{
	struct msm_iova_partition cpp_fw_partition = {
		.start = SZ_128K,
		.size = SZ_2G - SZ_128K,
	};
	struct msm_iova_layout cpp_fw_layout = {
		.partitions = &cpp_fw_partition,
		.npartitions = 1,
		.client_name = "camera_cpp",
		.domain_flags = 0,
	};

	return msm_register_domain(&cpp_fw_layout);
}

static int msm_cpp_get_clk_info(struct cpp_device *cpp_dev,
	struct platform_device *pdev)
{
	uint32_t count;
	int i, rc;
	uint32_t rates[CPP_CLK_INFO_MAX];

	struct device_node *of_node;
	of_node = pdev->dev.of_node;

	count = of_property_count_strings(of_node, "clock-names");

	CPP_DBG("count = %d\n", count);
	if (count == 0) {
		pr_err("no clocks found in device tree, count=%d", count);
		return 0;
	}

	if (count > CPP_CLK_INFO_MAX) {
		pr_err("invalid count=%d, max is %d\n", count,
			CPP_CLK_INFO_MAX);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		rc = of_property_read_string_index(of_node, "clock-names",
				i, &(cpp_clk_info[i].clk_name));
		CPP_DBG("clock-names[%d] = %s\n", i, cpp_clk_info[i].clk_name);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			return rc;
		}
	}
	rc = of_property_read_u32_array(of_node, "qcom,clock-rates",
		rates, count);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		return rc;
	}
	for (i = 0; i < count; i++) {
		cpp_clk_info[i].clk_rate = (rates[i] == 0) ?
				(long)-1 : rates[i];
		pr_info("clk_rate[%d] = %ld\n", i, cpp_clk_info[i].clk_rate);
	}
	cpp_dev->num_clk = count;
	rc = of_property_read_u32(of_node, "qcom,min-clock-rate",
				  &cpp_dev->min_clk_rate);
	if (rc < 0) {
		pr_err("min-clk-rate not defined, setting it to 0\n");
		cpp_dev->min_clk_rate = 0;
	}
	return 0;
}

struct v4l2_file_operations msm_cpp_v4l2_subdev_fops = {
	.unlocked_ioctl = msm_cpp_subdev_fops_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = msm_cpp_subdev_fops_compat_ioctl,
#endif
};

static int cpp_probe(struct platform_device *pdev)
{
	struct cpp_device *cpp_dev;
	int rc = 0;
	CPP_DBG("E");

	cpp_dev = kzalloc(sizeof(struct cpp_device), GFP_KERNEL);
	if (!cpp_dev) {
		pr_err("no enough memory\n");
		return -ENOMEM;
	}

	cpp_dev->cpp_clk = kzalloc(sizeof(struct clk *) *
		ARRAY_SIZE(cpp_clk_info), GFP_KERNEL);
	if (!cpp_dev->cpp_clk) {
		pr_err("no enough memory\n");
		rc = -ENOMEM;
		goto clk_err;
	}

	v4l2_subdev_init(&cpp_dev->msm_sd.sd, &msm_cpp_subdev_ops);
	cpp_dev->msm_sd.sd.internal_ops = &msm_cpp_internal_ops;
	snprintf(cpp_dev->msm_sd.sd.name, ARRAY_SIZE(cpp_dev->msm_sd.sd.name),
		 "cpp");
	cpp_dev->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	cpp_dev->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_EVENTS;
	v4l2_set_subdevdata(&cpp_dev->msm_sd.sd, cpp_dev);
	platform_set_drvdata(pdev, &cpp_dev->msm_sd.sd);
	mutex_init(&cpp_dev->mutex);
	spin_lock_init(&cpp_dev->tasklet_lock);
	spin_lock_init(&cpp_timer.data.processed_frame_lock);

	if (pdev->dev.of_node)
		of_property_read_u32((&pdev->dev)->of_node,
					"cell-index", &pdev->id);

	cpp_dev->pdev = pdev;

	cpp_dev->mem = platform_get_resource_byname(pdev,
					IORESOURCE_MEM, "cpp");
	if (!cpp_dev->mem) {
		pr_err("no mem resource?\n");
		rc = -ENODEV;
		goto mem_err;
	}

	cpp_dev->vbif_mem = platform_get_resource_byname(pdev,
					IORESOURCE_MEM, "cpp_vbif");
	if (!cpp_dev->vbif_mem) {
		pr_err("no mem resource?\n");
		rc = -ENODEV;
		goto mem_err;
	}

	cpp_dev->cpp_hw_mem = platform_get_resource_byname(pdev,
					IORESOURCE_MEM, "cpp_hw");
	if (!cpp_dev->cpp_hw_mem) {
		pr_err("no mem resource?\n");
		rc = -ENODEV;
		goto mem_err;
	}

	cpp_dev->irq = platform_get_resource_byname(pdev,
					IORESOURCE_IRQ, "cpp");
	if (!cpp_dev->irq) {
		pr_err("%s: no irq resource?\n", __func__);
		rc = -ENODEV;
		goto mem_err;
	}

	cpp_dev->io = request_mem_region(cpp_dev->mem->start,
		resource_size(cpp_dev->mem), pdev->name);
	if (!cpp_dev->io) {
		pr_err("%s: no valid mem region\n", __func__);
		rc = -EBUSY;
		goto mem_err;
	}

	cpp_dev->domain_num = cpp_register_domain();
	if (cpp_dev->domain_num < 0) {
		pr_err("%s: could not register domain\n", __func__);
		rc = -ENODEV;
		goto iommu_err;
	}

	cpp_dev->domain =
		msm_get_iommu_domain(cpp_dev->domain_num);
	if (!cpp_dev->domain) {
		pr_err("%s: cannot find domain\n", __func__);
		rc = -ENODEV;
		goto iommu_err;
	}

	if (msm_cpp_get_clk_info(cpp_dev, pdev) < 0) {
		pr_err("msm_cpp_get_clk_info() failed\n");
		goto iommu_err;
	}

	if (pdev->dev.of_node)
		rc = of_property_read_u32(pdev->dev.of_node, "bus_master",
			&cpp_dev->bus_master_flag);
	if (rc)
		cpp_dev->bus_master_flag = 0;
	pr_err("Bus master %d\n", cpp_dev->bus_master_flag);

	rc = cpp_init_hardware(cpp_dev);
	if (rc < 0)
		goto cpp_probe_init_error;

	if (cpp_dev->hw_info.cpp_hw_version == CPP_HW_VERSION_5_0_0 ||
		cpp_dev->hw_info.cpp_hw_version == CPP_HW_VERSION_5_1_0)
		cpp_dev->iommu_ctx = msm_iommu_get_ctx("cpp_0");
	else
		cpp_dev->iommu_ctx = msm_iommu_get_ctx("cpp");

	if (IS_ERR(cpp_dev->iommu_ctx)) {
		pr_err("%s: cannot get iommu_ctx\n", __func__);
		rc = -EPROBE_DEFER;
		goto iommu_err;
	}

	media_entity_init(&cpp_dev->msm_sd.sd.entity, 0, NULL, 0);
	cpp_dev->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	cpp_dev->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_CPP;
	cpp_dev->msm_sd.sd.entity.name = pdev->name;
	cpp_dev->msm_sd.close_seq = MSM_SD_CLOSE_3RD_CATEGORY;
	msm_sd_register(&cpp_dev->msm_sd);
	msm_cpp_v4l2_subdev_fops.owner = v4l2_subdev_fops.owner;
	msm_cpp_v4l2_subdev_fops.open = v4l2_subdev_fops.open;
	msm_cpp_v4l2_subdev_fops.release = v4l2_subdev_fops.release;
	msm_cpp_v4l2_subdev_fops.poll = v4l2_subdev_fops.poll;

	cpp_dev->msm_sd.sd.devnode->fops = &msm_cpp_v4l2_subdev_fops;
	cpp_dev->msm_sd.sd.entity.revision = cpp_dev->msm_sd.sd.devnode->num;
	cpp_dev->state = CPP_STATE_BOOT;

	msm_camera_io_w(0x0, cpp_dev->base +
					   MSM_CPP_MICRO_IRQGEN_MASK);
	msm_camera_io_w(0xFFFF, cpp_dev->base +
					   MSM_CPP_MICRO_IRQGEN_CLR);
	msm_camera_io_w(0x80000000, cpp_dev->base + 0xF0);
	cpp_release_hardware(cpp_dev);
	cpp_dev->state = CPP_STATE_OFF;
	msm_cpp_enable_debugfs(cpp_dev);

	msm_queue_init(&cpp_dev->eventData_q, "eventdata");
	msm_queue_init(&cpp_dev->processing_q, "frame");
	INIT_LIST_HEAD(&cpp_dev->tasklet_q);
	tasklet_init(&cpp_dev->cpp_tasklet, msm_cpp_do_tasklet,
		(unsigned long)cpp_dev);
	cpp_dev->timer_wq = create_workqueue("msm_cpp_workqueue");
	cpp_dev->work = kmalloc(sizeof(struct msm_cpp_work_t),
		GFP_KERNEL);

	if (!cpp_dev->work) {
		pr_err("no enough memory\n");
		rc = -ENOMEM;
		goto cpp_probe_init_error;
	}

	INIT_WORK((struct work_struct *)cpp_dev->work, msm_cpp_do_timeout_work);
	cpp_dev->cpp_open_cnt = 0;
	cpp_dev->is_firmware_loaded = 0;
	cpp_dev->iommu_state = CPP_IOMMU_STATE_DETACHED;
	cpp_timer.data.cpp_dev = cpp_dev;
	atomic_set(&cpp_timer.used, 0);
	/* install timer for cpp timeout */
	CPP_DBG("Installing cpp_timer\n");
	setup_timer(&cpp_timer.cpp_timer,
		cpp_timer_callback, (unsigned long)&cpp_timer);
	cpp_dev->fw_name_bin = NULL;
	cpp_dev->max_timeout_trial_cnt = MSM_CPP_MAX_TIMEOUT_TRIAL;
	if (rc == 0)
		CPP_DBG("SUCCESS.");
	else
		CPP_DBG("FAILED.");
	return rc;
cpp_probe_init_error:
	media_entity_cleanup(&cpp_dev->msm_sd.sd.entity);
	msm_sd_unregister(&cpp_dev->msm_sd);
iommu_err:
	release_mem_region(cpp_dev->mem->start, resource_size(cpp_dev->mem));
mem_err:
	kfree(cpp_dev->cpp_clk);
clk_err:
	kfree(cpp_dev);
	return rc;
}

static const struct of_device_id msm_cpp_dt_match[] = {
	{.compatible = "qcom,cpp"},
	{}
};

static int cpp_device_remove(struct platform_device *dev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(dev);
	struct cpp_device  *cpp_dev;
	if (!sd) {
		pr_err("%s: Subdevice is NULL\n", __func__);
		return 0;
	}

	cpp_dev = (struct cpp_device *)v4l2_get_subdevdata(sd);
	if (!cpp_dev) {
		pr_err("%s: cpp device is NULL\n", __func__);
		return 0;
	}

	msm_sd_unregister(&cpp_dev->msm_sd);
	release_mem_region(cpp_dev->mem->start, resource_size(cpp_dev->mem));
	release_mem_region(cpp_dev->vbif_mem->start,
		resource_size(cpp_dev->vbif_mem));
	release_mem_region(cpp_dev->cpp_hw_mem->start,
		resource_size(cpp_dev->cpp_hw_mem));
	mutex_destroy(&cpp_dev->mutex);
	kfree(cpp_dev->work);
	destroy_workqueue(cpp_dev->timer_wq);
	kfree(cpp_dev->cpp_clk);
	kfree(cpp_dev);
	return 0;
}

static struct platform_driver cpp_driver = {
	.probe = cpp_probe,
	.remove = cpp_device_remove,
	.driver = {
		.name = MSM_CPP_DRV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = msm_cpp_dt_match,
	},
};

static int __init msm_cpp_init_module(void)
{
	return platform_driver_register(&cpp_driver);
}

static void __exit msm_cpp_exit_module(void)
{
	platform_driver_unregister(&cpp_driver);
}

static int msm_cpp_debugfs_error_s(void *data, u64 val)
{
	pr_err("setting error inducement");
	induce_error = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(cpp_debugfs_error, NULL,
	msm_cpp_debugfs_error_s, "%llu\n");

static int msm_cpp_enable_debugfs(struct cpp_device *cpp_dev)
{
	struct dentry *debugfs_base;
	debugfs_base = debugfs_create_dir("msm_cpp", NULL);
	if (!debugfs_base)
		return -ENOMEM;

	if (!debugfs_create_file("error", S_IRUGO | S_IWUSR, debugfs_base,
		(void *)cpp_dev, &cpp_debugfs_error))
		return -ENOMEM;

	return 0;
}

module_init(msm_cpp_init_module);
module_exit(msm_cpp_exit_module);
MODULE_DESCRIPTION("MSM CPP driver");
MODULE_LICENSE("GPL v2");
