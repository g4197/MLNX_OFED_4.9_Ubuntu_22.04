// SPDX-License-Identifier: GPL-2.0
/*
 * NVM Express device driver
 * Copyright (c) 2011-2014, Intel Corporation.
 */

#include <linux/aer.h>
#include <linux/async.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blk-mq-pci.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#ifdef HAVE_ONCE_H
#include <linux/once.h>
#endif
#include <linux/pci.h>
#include <linux/nvme-peer.h>
#include <linux/t10-pi.h>
#include <linux/types.h>
#ifdef HAVE_IO_64_NONATOMIC_LO_HI_H
#include <linux/io-64-nonatomic-lo-hi.h>
#else
#include <asm-generic/io-64-nonatomic-lo-hi.h>
#endif
#ifdef HAVE_LINUX_SED_OPAL_H
#include <linux/sed-opal.h>
#endif
#include <linux/sizes.h>
#include <linux/pci-p2pdma.h>

#include "trace.h"
#include "nvme.h"

#define SQ_SIZE(depth)		(depth * sizeof(struct nvme_command))
#define CQ_SIZE(depth)		(depth * sizeof(struct nvme_completion))

#define SGES_PER_PAGE	(PAGE_SIZE / sizeof(struct nvme_sgl_desc))

/*
 * These can be higher, but we need to ensure that any command doesn't
 * require an sg allocation that needs more than a page of data.
 */
#define NVME_MAX_KB_SZ	4096
#define NVME_MAX_SEGS	127

static int use_threaded_interrupts;
module_param(use_threaded_interrupts, int, 0);

static bool use_cmb_sqes = true;
module_param(use_cmb_sqes, bool, 0444);
MODULE_PARM_DESC(use_cmb_sqes, "use controller's memory buffer for I/O SQes");

static unsigned int max_host_mem_size_mb = 128;
module_param(max_host_mem_size_mb, uint, 0444);
MODULE_PARM_DESC(max_host_mem_size_mb,
	"Maximum Host Memory Buffer (HMB) size per controller (in MiB)");

static unsigned int sgl_threshold = SZ_32K;
module_param(sgl_threshold, uint, 0644);
MODULE_PARM_DESC(sgl_threshold,
		"Use SGLs when average request segment size is larger or equal to "
		"this size. Use 0 to disable SGLs.");

static int io_queue_depth_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops io_queue_depth_ops = {
	.set = io_queue_depth_set,
	.get = param_get_int,
};

static int io_queue_depth = 1024;
module_param_cb(io_queue_depth, &io_queue_depth_ops, &io_queue_depth, 0644);
MODULE_PARM_DESC(io_queue_depth, "set io queue depth, should >= 2");

#ifdef HAVE_BLK_MQ_HCTX_TYPE
static int queue_count_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops queue_count_ops = {
	.set = queue_count_set,
	.get = param_get_int,
};

static int write_queues;
module_param_cb(write_queues, &queue_count_ops, &write_queues, 0644);
MODULE_PARM_DESC(write_queues,
	"Number of queues to use for writes. If not set, reads and writes "
	"will share a queue set.");

static int poll_queues = 0;
module_param_cb(poll_queues, &queue_count_ops, &poll_queues, 0644);
MODULE_PARM_DESC(poll_queues, "Number of queues to use for polled IO.");
#else
static int write_queues = 0;
MODULE_PARM_DESC(write_queues,
	"Number of queues to use for writes [deprecated]");

static int poll_queues = 0;
MODULE_PARM_DESC(poll_queues, "Number of queues to use for polled IO [deprecated]");
#endif

static int num_p2p_queues_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops num_p2p_queues_ops = {
	.set = num_p2p_queues_set,
	.get = param_get_int,
};

static unsigned int num_p2p_queues = 0;
module_param_cb(num_p2p_queues, &num_p2p_queues_ops, &num_p2p_queues, S_IRUGO);
MODULE_PARM_DESC(num_p2p_queues,
		 "number of I/O queues to create for peer-to-peer data transfer per pci function (Default: 0)");

struct nvme_dev;
struct nvme_queue;

static void nvme_dev_disable(struct nvme_dev *dev, bool shutdown);
static int nvme_create_queue(struct nvme_queue *nvmeq, int qid, bool polled);
static int nvme_suspend_queue(struct nvme_queue *nvmeq);
static int adapter_delete_sq(struct nvme_dev *dev, u16 sqid);
static int adapter_delete_cq(struct nvme_dev *dev, u16 cqid);
static bool __nvme_disable_io_queues(struct nvme_dev *dev, u8 opcode);

/*
 * Represents an NVM Express device.  Each nvme_dev is a PCI function.
 */
struct nvme_dev {
	struct nvme_queue *queues;
	struct blk_mq_tag_set tagset;
	struct blk_mq_tag_set admin_tagset;
	u32 __iomem *dbs;
	struct device *dev;
	struct dma_pool *prp_page_pool;
	struct dma_pool *prp_small_pool;
	unsigned online_queues;
	unsigned max_qid;
#ifdef HAVE_BLK_MQ_HCTX_TYPE
	unsigned io_queues[HCTX_MAX_TYPES];
#endif
#if defined(HAVE_PCI_IRQ_API) && defined(HAVE_IRQ_CALC_AFFINITY_VECTORS_3_ARGS)
	unsigned int num_vecs;
#endif
	int q_depth;
	u32 db_stride;
#ifndef HAVE_PCI_IRQ_API
	struct msix_entry *entry;
#endif
	void __iomem *bar;
	unsigned long bar_mapped_size;
	struct work_struct remove_work;
	struct mutex shutdown_lock;
	bool subsystem;
	u64 cmb_size;
	bool cmb_use_sqes;
	u32 cmbsz;
	u32 cmbloc;
	struct nvme_ctrl ctrl;
	unsigned num_p2p_queues;

	mempool_t *iod_mempool;

	/* shadow doorbell buffer support: */
	u32 *dbbuf_dbs;
	dma_addr_t dbbuf_dbs_dma_addr;
	u32 *dbbuf_eis;
	dma_addr_t dbbuf_eis_dma_addr;

	/* host memory buffer support: */
	u64 host_mem_size;
	u32 nr_host_mem_descs;
	dma_addr_t host_mem_descs_dma;
	struct nvme_host_mem_buf_desc *host_mem_descs;
	void **host_mem_desc_bufs;
};

static int io_queue_depth_set(const char *val, const struct kernel_param *kp)
{
	int n = 0, ret;

	ret = kstrtoint(val, 10, &n);
	if (ret != 0 || n < 2)
		return -EINVAL;

	return param_set_int(val, kp);
}

#ifdef HAVE_BLK_MQ_HCTX_TYPE
static int queue_count_set(const char *val, const struct kernel_param *kp)
{
	int n, ret;

	ret = kstrtoint(val, 10, &n);
	if (ret)
		return ret;
	if (n > num_possible_cpus())
		n = num_possible_cpus();

	return param_set_int(val, kp);
}
#endif

static int num_p2p_queues_set(const char *val, const struct kernel_param *kp)
{
	unsigned n = 0;
	int ret;

	ret = kstrtouint(val, 0, &n);
	if (ret != 0 || n > 65534)
		return -EINVAL;

	return param_set_uint(val, kp);
}

static inline unsigned int sq_idx(unsigned int qid, u32 stride)
{
	return qid * 2 * stride;
}

static inline unsigned int cq_idx(unsigned int qid, u32 stride)
{
	return (qid * 2 + 1) * stride;
}

static inline struct nvme_dev *to_nvme_dev(struct nvme_ctrl *ctrl)
{
	return container_of(ctrl, struct nvme_dev, ctrl);
}

/*
 * An NVM Express queue.  Each device has at least two (one for admin
 * commands and one for I/O commands).
 */
struct nvme_queue {
	struct nvme_dev *dev;
#ifndef HAVE_PCI_FREE_IRQ
	char irqname[24];	/* nvme4294967295-65535\0 */
#endif
	spinlock_t sq_lock;
	struct nvme_command *sq_cmds;
	 /* only used for poll queues: */
	spinlock_t cq_poll_lock ____cacheline_aligned_in_smp;
	volatile struct nvme_completion *cqes;
	struct blk_mq_tags **tags;
	dma_addr_t sq_dma_addr;
	dma_addr_t cq_dma_addr;
	u32 __iomem *q_db;
	u16 q_depth;
	u16 cq_vector;
	u16 sq_tail;
#ifdef HAVE_BLK_MQ_OPS_COMMIT_RQS
	u16 last_sq_tail;
#endif
	u16 cq_head;
	u16 last_cq_head;
	u16 qid;
	u8 cq_phase;
	unsigned long flags;
#define NVMEQ_ENABLED		0
#define NVMEQ_SQ_CMB		1
#define NVMEQ_DELETE_ERROR	2
#define NVMEQ_POLLED		3
	u32 *dbbuf_sq_db;
	u32 *dbbuf_cq_db;
	u32 *dbbuf_sq_ei;
	u32 *dbbuf_cq_ei;
	struct completion delete_done;

	/* p2p */
	bool p2p;
	struct nvme_peer_resource resource;
};

/*
 * The nvme_iod describes the data in an I/O.
 *
 * The sg pointer contains the list of PRP/SGL chunk allocations in addition
 * to the actual struct scatterlist.
 */
struct nvme_iod {
	struct nvme_request req;
	struct nvme_queue *nvmeq;
	bool use_sgl;
	int aborted;
	int npages;		/* In the PRP list. 0 means small pool in use */
	int nents;		/* Used in scatterlist */
	dma_addr_t first_dma;
#if defined(HAVE_BLKDEV_DMA_MAP_BVEC) && defined(HAVE_BLKDEV_REQ_BVEC)
	unsigned int dma_len;	/* length of single DMA segment mapping */
	dma_addr_t meta_dma;
#else
	struct scatterlist meta_sg;
#endif
	struct scatterlist *sg;
};

static int nvme_peer_init_resource(struct nvme_queue *nvmeq,
				   enum nvme_peer_resource_mask mask,
				   void (* stop_master_peer)(void *priv), void *dd_data)
{
	struct nvme_dev *dev = nvmeq->dev;
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	int qid = nvmeq->qid;
	int ret = 0;

	if (mask & NVME_PEER_SQT_DBR)
		/* Calculation from NVMe 1.2.1 SPEC */
#ifndef CONFIG_PPC
		nvmeq->resource.sqt_dbr_addr = pci_bus_address(pdev, 0) + (0x1000 + ((2 * (qid)) * (4 << NVME_CAP_STRIDE(dev->ctrl.cap))));
#else
		nvmeq->resource.sqt_dbr_addr = 0x800000000000000 | (pci_resource_start(pdev, 0) + (0x1000 + ((2 * (qid)) * (4 << NVME_CAP_STRIDE(dev->ctrl.cap)))));
#endif

	if (mask & NVME_PEER_CQH_DBR)
		/* Calculation from NVMe 1.2.1 SPEC */
#ifndef CONFIG_PPC
		nvmeq->resource.cqh_dbr_addr = pci_bus_address(pdev, 0) + (0x1000 + ((2 * (qid) + 1) * (4 << NVME_CAP_STRIDE(dev->ctrl.cap))));
#else
		nvmeq->resource.cqh_dbr_addr = 0x800000000000000 | (pci_resource_start(pdev, 0) + (0x1000 + ((2 * (qid) + 1) * (4 << NVME_CAP_STRIDE(dev->ctrl.cap)))));
#endif

	if (mask & NVME_PEER_SQ_PAS)
		nvmeq->resource.sq_dma_addr = nvmeq->sq_dma_addr;

	if (mask & NVME_PEER_CQ_PAS)
		nvmeq->resource.cq_dma_addr = nvmeq->cq_dma_addr;

	if (mask & NVME_PEER_SQ_SZ)
		nvmeq->resource.nvme_sq_size = SQ_SIZE(dev->q_depth);

	if (mask & NVME_PEER_CQ_SZ)
		nvmeq->resource.nvme_cq_size = CQ_SIZE(dev->q_depth);

	if (mask & NVME_PEER_MEM_LOG_PG_SZ)
		nvmeq->resource.memory_log_page_size = __ffs(dev->ctrl.page_size >> 12); /* memory_log_page_size is in 4K granularity */

	nvmeq->resource.flags = NVME_QUEUE_PHYS_CONTIG;
	nvmeq->resource.stop_master_peer = stop_master_peer;
	nvmeq->resource.dd_data = dd_data;

	return ret;
}

void nvme_peer_put_resource(struct nvme_peer_resource *resource, bool restart)
{
	struct nvme_queue *nvmeq = container_of(resource, struct nvme_queue,
						resource);
	mutex_lock(&resource->lock);
	resource->in_use = false;
	resource->stop_master_peer = NULL;
	resource->dd_data = NULL;
	mutex_unlock(&resource->lock);

	// TODO: create/destroy on demand

	/* Restart the queue for future usage */
	if (restart) {
		nvme_suspend_queue(nvmeq);
		adapter_delete_sq(nvmeq->dev, nvmeq->qid);
		adapter_delete_cq(nvmeq->dev, nvmeq->qid);
		nvme_create_queue(nvmeq, nvmeq->qid, false);
	}
}
EXPORT_SYMBOL_GPL(nvme_peer_put_resource);

struct nvme_peer_resource *nvme_peer_get_resource(struct pci_dev *pdev,
	enum nvme_peer_resource_mask mask,
	void (* stop_master_peer)(void *priv), void *dd_data)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);
	struct nvme_queue *nvmeq;
	unsigned i;
	int ret;

	if (!dev)
		return NULL;


	for (i = 0; i < dev->online_queues; i++) {
		nvmeq = &dev->queues[i];
		if (nvmeq->p2p) {
			mutex_lock(&nvmeq->resource.lock);
			if (!nvmeq->resource.in_use) {
				ret = nvme_peer_init_resource(nvmeq, mask,
							      stop_master_peer,
							      dd_data);
				if (!ret) {
					nvmeq->resource.in_use = true;
					mutex_unlock(&nvmeq->resource.lock);
					return &nvmeq->resource;
				}
			}
			mutex_unlock(&nvmeq->resource.lock);
		}
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(nvme_peer_get_resource);

static unsigned int max_io_queues(void)
{
	return num_possible_cpus() + write_queues + poll_queues;
}

static unsigned int max_queue_count(void)
{
	/* IO queues + admin queue */
	return 1 + max_io_queues();
}

static inline unsigned int nvme_dbbuf_size(u32 stride)
{
	return (max_queue_count() * 8 * stride);
}

static int nvme_dbbuf_dma_alloc(struct nvme_dev *dev)
{
	unsigned int mem_size = nvme_dbbuf_size(dev->db_stride);

	if (dev->dbbuf_dbs)
		return 0;

	dev->dbbuf_dbs = dma_alloc_coherent(dev->dev, mem_size,
					    &dev->dbbuf_dbs_dma_addr,
					    GFP_KERNEL);
	if (!dev->dbbuf_dbs)
		return -ENOMEM;
	dev->dbbuf_eis = dma_alloc_coherent(dev->dev, mem_size,
					    &dev->dbbuf_eis_dma_addr,
					    GFP_KERNEL);
	if (!dev->dbbuf_eis) {
		dma_free_coherent(dev->dev, mem_size,
				  dev->dbbuf_dbs, dev->dbbuf_dbs_dma_addr);
		dev->dbbuf_dbs = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void nvme_dbbuf_dma_free(struct nvme_dev *dev)
{
	unsigned int mem_size = nvme_dbbuf_size(dev->db_stride);

	if (dev->dbbuf_dbs) {
		dma_free_coherent(dev->dev, mem_size,
				  dev->dbbuf_dbs, dev->dbbuf_dbs_dma_addr);
		dev->dbbuf_dbs = NULL;
	}
	if (dev->dbbuf_eis) {
		dma_free_coherent(dev->dev, mem_size,
				  dev->dbbuf_eis, dev->dbbuf_eis_dma_addr);
		dev->dbbuf_eis = NULL;
	}
}

static void nvme_dbbuf_init(struct nvme_dev *dev,
			    struct nvme_queue *nvmeq, int qid)
{
	if (!dev->dbbuf_dbs || !qid)
		return;

	nvmeq->dbbuf_sq_db = &dev->dbbuf_dbs[sq_idx(qid, dev->db_stride)];
	nvmeq->dbbuf_cq_db = &dev->dbbuf_dbs[cq_idx(qid, dev->db_stride)];
	nvmeq->dbbuf_sq_ei = &dev->dbbuf_eis[sq_idx(qid, dev->db_stride)];
	nvmeq->dbbuf_cq_ei = &dev->dbbuf_eis[cq_idx(qid, dev->db_stride)];
}

static void nvme_dbbuf_set(struct nvme_dev *dev)
{
	struct nvme_command c;

	if (!dev->dbbuf_dbs)
		return;

	memset(&c, 0, sizeof(c));
	c.dbbuf.opcode = nvme_admin_dbbuf;
	c.dbbuf.prp1 = cpu_to_le64(dev->dbbuf_dbs_dma_addr);
	c.dbbuf.prp2 = cpu_to_le64(dev->dbbuf_eis_dma_addr);

	if (nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0)) {
		dev_warn(dev->ctrl.device, "unable to set dbbuf\n");
		/* Free memory and continue on */
		nvme_dbbuf_dma_free(dev);
	}
}

static inline int nvme_dbbuf_need_event(u16 event_idx, u16 new_idx, u16 old)
{
	return (u16)(new_idx - event_idx - 1) < (u16)(new_idx - old);
}

/* Update dbbuf and return true if an MMIO is required */
static bool nvme_dbbuf_update_and_check_event(u16 value, u32 *dbbuf_db,
					      volatile u32 *dbbuf_ei)
{
	if (dbbuf_db) {
		u16 old_value;

		/*
		 * Ensure that the queue is written before updating
		 * the doorbell in memory
		 */
		wmb();

		old_value = *dbbuf_db;
		*dbbuf_db = value;

		/*
		 * Ensure that the doorbell is updated before reading the event
		 * index from memory.  The controller needs to provide similar
		 * ordering to ensure the envent index is updated before reading
		 * the doorbell.
		 */
		mb();

		if (!nvme_dbbuf_need_event(*dbbuf_ei, value, old_value))
			return false;
	}

	return true;
}

/*
 * Will slightly overestimate the number of pages needed.  This is OK
 * as it only leads to a small amount of wasted memory for the lifetime of
 * the I/O.
 */
static int nvme_npages(unsigned size, struct nvme_dev *dev)
{
	unsigned nprps = DIV_ROUND_UP(size + dev->ctrl.page_size,
				      dev->ctrl.page_size);
	return DIV_ROUND_UP(8 * nprps, PAGE_SIZE - 8);
}

/*
 * Calculates the number of pages needed for the SGL segments. For example a 4k
 * page can accommodate 256 SGL descriptors.
 */
static int nvme_pci_npages_sgl(unsigned int num_seg)
{
	return DIV_ROUND_UP(num_seg * sizeof(struct nvme_sgl_desc), PAGE_SIZE);
}

static unsigned int nvme_pci_iod_alloc_size(struct nvme_dev *dev,
		unsigned int size, unsigned int nseg, bool use_sgl)
{
	size_t alloc_size;

	if (use_sgl)
		alloc_size = sizeof(__le64 *) * nvme_pci_npages_sgl(nseg);
	else
		alloc_size = sizeof(__le64 *) * nvme_npages(size, dev);

	return alloc_size + sizeof(struct scatterlist) * nseg;
}

#ifndef HAVE_PCI_FREE_IRQ
static int nvmeq_irq(struct nvme_queue *nvmeq)
{
#ifdef HAVE_PCI_IRQ_API
	return pci_irq_vector(to_pci_dev(nvmeq->dev->dev), nvmeq->cq_vector);
#else
	return nvmeq->dev->entry[nvmeq->cq_vector].vector;
#endif
}
#endif

static int nvme_admin_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
				unsigned int hctx_idx)
{
	struct nvme_dev *dev = data;
	struct nvme_queue *nvmeq = &dev->queues[0];

	WARN_ON(hctx_idx != 0);
	WARN_ON(dev->admin_tagset.tags[0] != hctx->tags);
	WARN_ON(nvmeq->tags);

	hctx->driver_data = nvmeq;
	nvmeq->tags = &dev->admin_tagset.tags[0];
	return 0;
}

static void nvme_admin_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct nvme_queue *nvmeq = hctx->driver_data;

	nvmeq->tags = NULL;
}

static int nvme_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			  unsigned int hctx_idx)
{
	struct nvme_dev *dev = data;
	struct nvme_queue *nvmeq = &dev->queues[hctx_idx + 1];

	if (!nvmeq->tags)
		nvmeq->tags = &dev->tagset.tags[hctx_idx];

	WARN_ON(dev->tagset.tags[hctx_idx] != hctx->tags);
	hctx->driver_data = nvmeq;
	return 0;
}

#ifdef HAVE_BLK_MQ_OPS_INIT_REQUEST_HAS_4_PARAMS
static int nvme_init_request(struct blk_mq_tag_set *set, struct request *req,
		unsigned int hctx_idx, unsigned int numa_node)
{
	struct nvme_dev *dev = set->driver_data;
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	int queue_idx = (set == &dev->tagset) ? hctx_idx + 1 : 0;
	struct nvme_queue *nvmeq = &dev->queues[queue_idx];

	BUG_ON(!nvmeq);
	iod->nvmeq = nvmeq;

	nvme_req(req)->ctrl = &dev->ctrl;
	return 0;
}
#else
static int nvme_init_request(void *data, struct request *req,
		unsigned int hctx_idx, unsigned int rq_idx,
		unsigned int numa_node)
{
	struct nvme_dev *dev = data;
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = &dev->queues[hctx_idx + 1];

	BUG_ON(!nvmeq);
	iod->nvmeq = nvmeq;

	nvme_req(req)->ctrl = &dev->ctrl;
	return 0;
}

static int nvme_admin_init_request(void *data, struct request *req,
		unsigned int hctx_idx, unsigned int rq_idx,
		unsigned int numa_node)
{
	struct nvme_dev *dev = data;
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = &dev->queues[0];

	BUG_ON(!nvmeq);
	iod->nvmeq = nvmeq;

	nvme_req(req)->ctrl = &dev->ctrl;
	return 0;
}
#endif

#if defined(HAVE_BLK_MQ_OPS_MAP_QUEUES) && \
	(defined(HAVE_PCI_IRQ_GET_AFFINITY) || \
	defined(HAVE_BLK_MQ_PCI_MAP_QUEUES_3_ARGS))
static int queue_irq_offset(struct nvme_dev *dev)
{
#if defined(HAVE_PCI_IRQ_API) && defined(HAVE_IRQ_CALC_AFFINITY_VECTORS_3_ARGS)
	/* if we have more than 1 vec, admin queue offsets us by 1 */
	if (dev->num_vecs > 1)
		return 1;
#endif

	return 0;
}

#ifdef HAVE_BLK_MQ_HCTX_TYPE
static int nvme_pci_map_queues(struct blk_mq_tag_set *set)
{
	struct nvme_dev *dev = set->driver_data;
	int i, qoff, offset;

	offset = queue_irq_offset(dev);
	for (i = 0, qoff = 0; i < set->nr_maps; i++) {
		struct blk_mq_queue_map *map = &set->map[i];

		map->nr_queues = dev->io_queues[i];
		if (!map->nr_queues) {
			BUG_ON(i == HCTX_TYPE_DEFAULT);
			continue;
		}

		/*
		 * The poll queue(s) doesn't have an IRQ (and hence IRQ
		 * affinity), so use the regular blk-mq cpu mapping
		 */
		map->queue_offset = qoff;
		if (i != HCTX_TYPE_POLL && offset)
			blk_mq_pci_map_queues(map, to_pci_dev(dev->dev), offset);
		else
			blk_mq_map_queues(map);
		qoff += map->nr_queues;
		offset += map->nr_queues;
	}

	return 0;
}
#else
static int nvme_pci_map_queues(struct blk_mq_tag_set *set)
{
	struct nvme_dev *dev = set->driver_data;
	int offset = queue_irq_offset(dev);

#ifdef HAVE_BLK_MQ_PCI_MAP_QUEUES_3_ARGS
#ifdef HAVE_BLK_MQ_QUEUE_MAP
	return blk_mq_pci_map_queues(&set->map[0], to_pci_dev(dev->dev), offset);
#else
	return blk_mq_pci_map_queues(set, to_pci_dev(dev->dev), offset);
#endif
#else
	return __blk_mq_pci_map_queues(set, to_pci_dev(dev->dev), offset);
#endif /* HAVE_BLK_MQ_PCI_MAP_QUEUES_3_ARGS */
}
#endif /* HAVE_BLK_MQ_TAG_SET_NR_MAPS */
#endif

#ifdef HAVE_BLK_MQ_OPS_COMMIT_RQS
/*
 * Write sq tail if we are asked to, or if the next command would wrap.
 */
static inline void nvme_write_sq_db(struct nvme_queue *nvmeq, bool write_sq)
{
	if (!write_sq) {
		u16 next_tail = nvmeq->sq_tail + 1;

		if (next_tail == nvmeq->q_depth)
			next_tail = 0;
		if (next_tail != nvmeq->last_sq_tail)
			return;
	}

	if (nvme_dbbuf_update_and_check_event(nvmeq->sq_tail,
			nvmeq->dbbuf_sq_db, nvmeq->dbbuf_sq_ei))
		writel(nvmeq->sq_tail, nvmeq->q_db);
	nvmeq->last_sq_tail = nvmeq->sq_tail;
}
#endif

/**
 * nvme_submit_cmd() - Copy a command into a queue and ring the doorbell
 * @nvmeq: The queue to use
 * @cmd: The command to send
 * @write_sq: whether to write to the SQ doorbell
 */
static void nvme_submit_cmd(struct nvme_queue *nvmeq, struct nvme_command *cmd,
			    bool write_sq)
{
	spin_lock(&nvmeq->sq_lock);
	memcpy(&nvmeq->sq_cmds[nvmeq->sq_tail], cmd, sizeof(*cmd));
	if (++nvmeq->sq_tail == nvmeq->q_depth)
		nvmeq->sq_tail = 0;
#ifdef HAVE_BLK_MQ_OPS_COMMIT_RQS
	nvme_write_sq_db(nvmeq, write_sq);
#else
	if (nvme_dbbuf_update_and_check_event(nvmeq->sq_tail,
			nvmeq->dbbuf_sq_db, nvmeq->dbbuf_sq_ei))
		writel(nvmeq->sq_tail, nvmeq->q_db);
#endif
	spin_unlock(&nvmeq->sq_lock);
}

#ifdef HAVE_BLK_MQ_OPS_COMMIT_RQS
static void nvme_commit_rqs(struct blk_mq_hw_ctx *hctx)
{
	struct nvme_queue *nvmeq = hctx->driver_data;

	spin_lock(&nvmeq->sq_lock);
	if (nvmeq->sq_tail != nvmeq->last_sq_tail)
		nvme_write_sq_db(nvmeq, true);
	spin_unlock(&nvmeq->sq_lock);
}
#endif

static void **nvme_pci_iod_list(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	return (void **)(iod->sg + blk_rq_nr_phys_segments(req));
}

static inline bool nvme_pci_use_sgls(struct nvme_dev *dev, struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	int nseg = blk_rq_nr_phys_segments(req);
	unsigned int avg_seg_size;

	if (nseg == 0)
		return false;

#ifdef HAVE_BLK_RQ_NR_PAYLOAD_BYTES
	avg_seg_size = DIV_ROUND_UP(blk_rq_payload_bytes(req), nseg);
#else
	avg_seg_size = DIV_ROUND_UP(nvme_map_len(req), nseg);
#endif

	if (!(dev->ctrl.sgls & ((1 << 0) | (1 << 1))))
		return false;
	if (!iod->nvmeq->qid)
		return false;
	if (!sgl_threshold || avg_seg_size < sgl_threshold)
		return false;
	return true;
}

static void nvme_unmap_data(struct nvme_dev *dev, struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	enum dma_data_direction dma_dir = rq_data_dir(req) ?
			DMA_TO_DEVICE : DMA_FROM_DEVICE;
	const int last_prp = dev->ctrl.page_size / sizeof(__le64) - 1;
	dma_addr_t dma_addr = iod->first_dma, next_dma_addr;
	int i;

#if defined(HAVE_BLKDEV_DMA_MAP_BVEC) && defined(HAVE_BLKDEV_REQ_BVEC)
	if (iod->dma_len) {
		dma_unmap_page(dev->dev, dma_addr, iod->dma_len, dma_dir);
		return;
	}

	WARN_ON_ONCE(!iod->nents);

	/* P2PDMA requests do not need to be unmapped */
	if (!is_pci_p2pdma_page(sg_page(iod->sg)))
		dma_unmap_sg(dev->dev, iod->sg, iod->nents, rq_dma_dir(req));

#else
	if (iod->nents) {
		/* P2PDMA requests do not need to be unmapped */
		if (!is_pci_p2pdma_page(sg_page(iod->sg)))
			dma_unmap_sg(dev->dev, iod->sg, iod->nents, dma_dir);
	}
#endif

	if (iod->npages == 0)
		dma_pool_free(dev->prp_small_pool, nvme_pci_iod_list(req)[0],
			dma_addr);

	for (i = 0; i < iod->npages; i++) {
		void *addr = nvme_pci_iod_list(req)[i];

		if (iod->use_sgl) {
			struct nvme_sgl_desc *sg_list = addr;

			next_dma_addr =
			    le64_to_cpu((sg_list[SGES_PER_PAGE - 1]).addr);
		} else {
			__le64 *prp_list = addr;

			next_dma_addr = le64_to_cpu(prp_list[last_prp]);
		}

		dma_pool_free(dev->prp_page_pool, addr, dma_addr);
		dma_addr = next_dma_addr;
	}

	mempool_free(iod->sg, dev->iod_mempool);
}

#ifdef HAVE_ONCE_H
static void nvme_print_sgl(struct scatterlist *sgl, int nents)
{
	int i;
	struct scatterlist *sg;

	for_each_sg(sgl, sg, nents, i) {
		dma_addr_t phys = sg_phys(sg);
		pr_warn("sg[%d] phys_addr:%pad offset:%d length:%d "
			"dma_address:%pad dma_length:%d\n",
			i, &phys, sg->offset, sg->length, &sg_dma_address(sg),
			sg_dma_len(sg));
	}
}
#endif

static blk_status_t nvme_pci_setup_prps(struct nvme_dev *dev,
		struct request *req, struct nvme_rw_command *cmnd)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct dma_pool *pool;
#ifdef HAVE_BLK_RQ_NR_PAYLOAD_BYTES
	int length = blk_rq_payload_bytes(req);
#else
	int length = nvme_map_len(req);
#endif
	struct scatterlist *sg = iod->sg;
	int dma_len = sg_dma_len(sg);
	u64 dma_addr = sg_dma_address(sg);
	u32 page_size = dev->ctrl.page_size;
	int offset = dma_addr & (page_size - 1);
	__le64 *prp_list;
	void **list = nvme_pci_iod_list(req);
	dma_addr_t prp_dma;
	int nprps, i;

	length -= (page_size - offset);
	if (length <= 0) {
		iod->first_dma = 0;
		goto done;
	}

	dma_len -= (page_size - offset);
	if (dma_len) {
		dma_addr += (page_size - offset);
	} else {
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}

	if (length <= page_size) {
		iod->first_dma = dma_addr;
		goto done;
	}

	nprps = DIV_ROUND_UP(length, page_size);
	if (nprps <= (256 / 8)) {
		pool = dev->prp_small_pool;
		iod->npages = 0;
	} else {
		pool = dev->prp_page_pool;
		iod->npages = 1;
	}

	prp_list = dma_pool_alloc(pool, GFP_ATOMIC, &prp_dma);
	if (!prp_list) {
		iod->first_dma = dma_addr;
		iod->npages = -1;
		return BLK_STS_RESOURCE;
	}
	list[0] = prp_list;
	iod->first_dma = prp_dma;
	i = 0;
	for (;;) {
		if (i == page_size >> 3) {
			__le64 *old_prp_list = prp_list;
			prp_list = dma_pool_alloc(pool, GFP_ATOMIC, &prp_dma);
			if (!prp_list)
				return BLK_STS_RESOURCE;
			list[iod->npages++] = prp_list;
			prp_list[0] = old_prp_list[i - 1];
			old_prp_list[i - 1] = cpu_to_le64(prp_dma);
			i = 1;
		}
		prp_list[i++] = cpu_to_le64(dma_addr);
		dma_len -= page_size;
		dma_addr += page_size;
		length -= page_size;
		if (length <= 0)
			break;
		if (dma_len > 0)
			continue;
		if (unlikely(dma_len < 0))
			goto bad_sgl;
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}

done:
	cmnd->dptr.prp1 = cpu_to_le64(sg_dma_address(iod->sg));
	cmnd->dptr.prp2 = cpu_to_le64(iod->first_dma);

	return BLK_STS_OK;

 bad_sgl:
#ifdef HAVE_ONCE_H
	WARN(DO_ONCE(nvme_print_sgl, iod->sg, iod->nents),
			"Invalid SGL for payload:%d nents:%d\n",
#ifdef HAVE_BLK_RQ_NR_PAYLOAD_BYTES
			blk_rq_payload_bytes(req), iod->nents);
#else
			nvme_map_len(req), iod->nents);
#endif
#else
	if (WARN_ONCE(1, "Invalid SGL for payload:%d nents:%d\n",
#ifdef HAVE_BLK_RQ_NR_PAYLOAD_BYTES
		      blk_rq_payload_bytes(req), iod->nents)) {
#else
		      nvme_map_len(req), iod->nents)) {
#endif
		for_each_sg(iod->sg, sg, iod->nents, i) {
			dma_addr_t phys = sg_phys(sg);
			pr_warn("sg[%d] phys_addr:%pad offset:%d length:%d "
				"dma_address:%pad dma_length:%d\n", i, &phys,
				sg->offset, sg->length,
				&sg_dma_address(sg),
				sg_dma_len(sg));
		}
	}
#endif
	return BLK_STS_IOERR;
}

static void nvme_pci_sgl_set_data(struct nvme_sgl_desc *sge,
		struct scatterlist *sg)
{
	sge->addr = cpu_to_le64(sg_dma_address(sg));
	sge->length = cpu_to_le32(sg_dma_len(sg));
	sge->type = NVME_SGL_FMT_DATA_DESC << 4;
}

static void nvme_pci_sgl_set_seg(struct nvme_sgl_desc *sge,
		dma_addr_t dma_addr, int entries)
{
	sge->addr = cpu_to_le64(dma_addr);
	if (entries < SGES_PER_PAGE) {
		sge->length = cpu_to_le32(entries * sizeof(*sge));
		sge->type = NVME_SGL_FMT_LAST_SEG_DESC << 4;
	} else {
		sge->length = cpu_to_le32(PAGE_SIZE);
		sge->type = NVME_SGL_FMT_SEG_DESC << 4;
	}
}

static blk_status_t nvme_pci_setup_sgls(struct nvme_dev *dev,
		struct request *req, struct nvme_rw_command *cmd, int entries)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct dma_pool *pool;
	struct nvme_sgl_desc *sg_list;
	struct scatterlist *sg = iod->sg;
	dma_addr_t sgl_dma;
	int i = 0;

	/* setting the transfer type as SGL */
	cmd->flags = NVME_CMD_SGL_METABUF;

	if (entries == 1) {
		nvme_pci_sgl_set_data(&cmd->dptr.sgl, sg);
		return BLK_STS_OK;
	}

	if (entries <= (256 / sizeof(struct nvme_sgl_desc))) {
		pool = dev->prp_small_pool;
		iod->npages = 0;
	} else {
		pool = dev->prp_page_pool;
		iod->npages = 1;
	}

	sg_list = dma_pool_alloc(pool, GFP_ATOMIC, &sgl_dma);
	if (!sg_list) {
		iod->npages = -1;
		return BLK_STS_RESOURCE;
	}

	nvme_pci_iod_list(req)[0] = sg_list;
	iod->first_dma = sgl_dma;

	nvme_pci_sgl_set_seg(&cmd->dptr.sgl, sgl_dma, entries);

	do {
		if (i == SGES_PER_PAGE) {
			struct nvme_sgl_desc *old_sg_desc = sg_list;
			struct nvme_sgl_desc *link = &old_sg_desc[i - 1];

			sg_list = dma_pool_alloc(pool, GFP_ATOMIC, &sgl_dma);
			if (!sg_list)
				return BLK_STS_RESOURCE;

			i = 0;
			nvme_pci_iod_list(req)[iod->npages++] = sg_list;
			sg_list[i++] = *link;
			nvme_pci_sgl_set_seg(link, sgl_dma, entries);
		}

		nvme_pci_sgl_set_data(&sg_list[i++], sg);
		sg = sg_next(sg);
	} while (--entries > 0);

	return BLK_STS_OK;
}

#if defined(HAVE_BLKDEV_DMA_MAP_BVEC) && defined(HAVE_BLKDEV_REQ_BVEC)
static blk_status_t nvme_setup_prp_simple(struct nvme_dev *dev,
		struct request *req, struct nvme_rw_command *cmnd,
		struct bio_vec *bv)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	unsigned int first_prp_len = dev->ctrl.page_size - bv->bv_offset;

	iod->first_dma = dma_map_bvec(dev->dev, bv, rq_dma_dir(req), 0);
	if (dma_mapping_error(dev->dev, iod->first_dma))
		return BLK_STS_RESOURCE;
	iod->dma_len = bv->bv_len;

	cmnd->dptr.prp1 = cpu_to_le64(iod->first_dma);
	if (bv->bv_len > first_prp_len)
		cmnd->dptr.prp2 = cpu_to_le64(iod->first_dma + first_prp_len);
	return 0;
}

static blk_status_t nvme_setup_sgl_simple(struct nvme_dev *dev,
		struct request *req, struct nvme_rw_command *cmnd,
		struct bio_vec *bv)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);

	iod->first_dma = dma_map_bvec(dev->dev, bv, rq_dma_dir(req), 0);
	if (dma_mapping_error(dev->dev, iod->first_dma))
		return BLK_STS_RESOURCE;
	iod->dma_len = bv->bv_len;

	cmnd->flags = NVME_CMD_SGL_METABUF;
	cmnd->dptr.sgl.addr = cpu_to_le64(iod->first_dma);
	cmnd->dptr.sgl.length = cpu_to_le32(iod->dma_len);
	cmnd->dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;
	return 0;
}
#endif

static blk_status_t nvme_map_data(struct nvme_dev *dev, struct request *req,
		struct nvme_command *cmnd)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	blk_status_t ret = BLK_STS_RESOURCE;
	int nr_mapped;

#if defined(HAVE_BLKDEV_DMA_MAP_BVEC) && defined(HAVE_BLKDEV_REQ_BVEC)
	if (blk_rq_nr_phys_segments(req) == 1) {
		struct bio_vec bv = req_bvec(req);

		if (!is_pci_p2pdma_page(bv.bv_page)) {
			if (bv.bv_offset + bv.bv_len <= dev->ctrl.page_size * 2)
				return nvme_setup_prp_simple(dev, req,
							     &cmnd->rw, &bv);

			if (iod->nvmeq->qid &&
			    dev->ctrl.sgls & ((1 << 0) | (1 << 1)))
				return nvme_setup_sgl_simple(dev, req,
							     &cmnd->rw, &bv);
		}
	}

	iod->dma_len = 0;
#endif
	iod->sg = mempool_alloc(dev->iod_mempool, GFP_ATOMIC);
	if (!iod->sg)
		return BLK_STS_RESOURCE;
	sg_init_table(iod->sg, blk_rq_nr_phys_segments(req));
	iod->nents = blk_rq_map_sg(req->q, req, iod->sg);
	if (!iod->nents)
		goto out;

	if (is_pci_p2pdma_page(sg_page(iod->sg)))
		nr_mapped = pci_p2pdma_map_sg(dev->dev, iod->sg, iod->nents,
					      rq_dma_dir(req));
	else
#if defined(HAVE_DMA_ATTR_NO_WARN) && \
	defined(HAVE_DMA_SET_ATTR_TAKES_UNSIGNED_LONG_ATTRS)
		nr_mapped = dma_map_sg_attrs(dev->dev, iod->sg, iod->nents,
					     rq_dma_dir(req), DMA_ATTR_NO_WARN);
#else
		nr_mapped = dma_map_sg(dev->dev, iod->sg, iod->nents,
				       rq_dma_dir(req));
#endif
	if (!nr_mapped)
		goto out;

	iod->use_sgl = nvme_pci_use_sgls(dev, req);
	if (iod->use_sgl)
		ret = nvme_pci_setup_sgls(dev, req, &cmnd->rw, nr_mapped);
	else
		ret = nvme_pci_setup_prps(dev, req, &cmnd->rw);
out:
	if (ret != BLK_STS_OK)
		nvme_unmap_data(dev, req);
	return ret;
}

static blk_status_t nvme_map_metadata(struct nvme_dev *dev, struct request *req,
		struct nvme_command *cmnd)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);

#if defined(HAVE_BLKDEV_DMA_MAP_BVEC) && defined(HAVE_BLKDEV_REQ_BVEC)
	iod->meta_dma = dma_map_bvec(dev->dev, rq_integrity_vec(req),
			rq_dma_dir(req), 0);
	if (dma_mapping_error(dev->dev, iod->meta_dma))
		return BLK_STS_IOERR;
	cmnd->rw.metadata = cpu_to_le64(iod->meta_dma);
#else
	if (blk_rq_count_integrity_sg(req->q, req->bio) != 1)
		return BLK_STS_IOERR;

	sg_init_table(&iod->meta_sg, 1);
	if (blk_rq_map_integrity_sg(req->q, req->bio, &iod->meta_sg) != 1)
		return BLK_STS_IOERR;

	if (!dma_map_sg(dev->dev, &iod->meta_sg, 1, rq_dma_dir(req)))
		return BLK_STS_IOERR;
	cmnd->rw.metadata = cpu_to_le64(sg_dma_address(&iod->meta_sg));
#endif
	return 0;
}

/*
 * NOTE: ns is NULL when called on the admin queue.
 */
static blk_status_t nvme_queue_rq(struct blk_mq_hw_ctx *hctx,
			 const struct blk_mq_queue_data *bd)
{
	struct nvme_ns *ns = hctx->queue->queuedata;
	struct nvme_queue *nvmeq = hctx->driver_data;
	struct nvme_dev *dev = nvmeq->dev;
	struct request *req = bd->rq;
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct nvme_command cmnd;
	blk_status_t ret;

	iod->aborted = 0;
	iod->npages = -1;
	iod->nents = 0;

	/*
	 * We should not need to do this, but we're still using this to
	 * ensure we can drain requests on a dying queue.
	 */
	if (unlikely(!test_bit(NVMEQ_ENABLED, &nvmeq->flags)))
		return BLK_STS_IOERR;

	ret = nvme_setup_cmd(ns, req, &cmnd);
	if (ret)
		return ret;

	if (blk_rq_nr_phys_segments(req)) {
		ret = nvme_map_data(dev, req, &cmnd);
		if (ret)
			goto out_free_cmd;
	}

	if (blk_integrity_rq(req)) {
		ret = nvme_map_metadata(dev, req, &cmnd);
		if (ret)
			goto out_unmap_data;
	}

	blk_mq_start_request(req);
	nvme_submit_cmd(nvmeq, &cmnd, bd->last);
	return BLK_STS_OK;
out_unmap_data:
	nvme_unmap_data(dev, req);
out_free_cmd:
	nvme_cleanup_cmd(req);
	return ret;
}

static void nvme_pci_complete_rq(struct request *req)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct nvme_dev *dev = iod->nvmeq->dev;

	nvme_cleanup_cmd(req);
	if (blk_integrity_rq(req))
#if defined(HAVE_BLKDEV_DMA_MAP_BVEC) && defined(HAVE_BLKDEV_REQ_BVEC)
		dma_unmap_page(dev->dev, iod->meta_dma,
			       rq_integrity_vec(req)->bv_len, rq_data_dir(req));
#else
		dma_unmap_sg(dev->dev, &iod->meta_sg, 1, rq_data_dir(req));
#endif
	if (blk_rq_nr_phys_segments(req))
		nvme_unmap_data(dev, req);
	nvme_complete_rq(req);
}

/* We read the CQE phase first to check if the rest of the entry is valid */
static inline bool nvme_cqe_pending(struct nvme_queue *nvmeq)
{
	return (le16_to_cpu(nvmeq->cqes[nvmeq->cq_head].status) & 1) ==
			nvmeq->cq_phase;
}

static inline void nvme_ring_cq_doorbell(struct nvme_queue *nvmeq)
{
	u16 head = nvmeq->cq_head;

	if (nvme_dbbuf_update_and_check_event(head, nvmeq->dbbuf_cq_db,
					      nvmeq->dbbuf_cq_ei))
		writel(head, nvmeq->q_db + nvmeq->dev->db_stride);
}

static inline void nvme_handle_cqe(struct nvme_queue *nvmeq, u16 idx)
{
	volatile struct nvme_completion *cqe = &nvmeq->cqes[idx];
	struct request *req;

	if (unlikely(cqe->command_id >= nvmeq->q_depth)) {
		dev_warn(nvmeq->dev->ctrl.device,
			"invalid id %d completed on queue %d\n",
			cqe->command_id, le16_to_cpu(cqe->sq_id));
		return;
	}

	/*
	 * AEN requests are special as they don't time out and can
	 * survive any kind of queue freeze and often don't respond to
	 * aborts.  We don't even bother to allocate a struct request
	 * for them but rather special case them here.
	 */
	if (unlikely(nvme_is_aen_req(nvmeq->qid, cqe->command_id))) {
		nvme_complete_async_event(&nvmeq->dev->ctrl,
				cqe->status, &cqe->result);
		return;
	}

	req = blk_mq_tag_to_rq(*nvmeq->tags, cqe->command_id);
	trace_nvme_sq(req, cqe->sq_head, nvmeq->sq_tail);
	nvme_end_request(req, cqe->status, cqe->result);
}

static void nvme_complete_cqes(struct nvme_queue *nvmeq, u16 start, u16 end)
{
	while (start != end) {
		nvme_handle_cqe(nvmeq, start);
		if (++start == nvmeq->q_depth)
			start = 0;
	}
}

static inline void nvme_update_cq_head(struct nvme_queue *nvmeq)
{
	if (nvmeq->cq_head == nvmeq->q_depth - 1) {
		nvmeq->cq_head = 0;
		nvmeq->cq_phase = !nvmeq->cq_phase;
	} else {
		nvmeq->cq_head++;
	}
}

static inline int nvme_process_cq(struct nvme_queue *nvmeq, u16 *start,
				  u16 *end, unsigned int tag)
{
	int found = 0;

	*start = nvmeq->cq_head;
	while (nvme_cqe_pending(nvmeq)) {
		if (tag == -1U || nvmeq->cqes[nvmeq->cq_head].command_id == tag)
			found++;
		nvme_update_cq_head(nvmeq);
	}
	*end = nvmeq->cq_head;

	if (*start != *end)
		nvme_ring_cq_doorbell(nvmeq);
	return found;
}

static irqreturn_t nvme_irq(int irq, void *data)
{
	struct nvme_queue *nvmeq = data;
	irqreturn_t ret = IRQ_NONE;
	u16 start, end;

	/*
	 * The rmb/wmb pair ensures we see all updates from a previous run of
	 * the irq handler, even if that was on another CPU.
	 */
	rmb();
	if (nvmeq->cq_head != nvmeq->last_cq_head)
		ret = IRQ_HANDLED;
	nvme_process_cq(nvmeq, &start, &end, -1);
	nvmeq->last_cq_head = nvmeq->cq_head;
	wmb();

	if (start != end) {
		nvme_complete_cqes(nvmeq, start, end);
		return IRQ_HANDLED;
	}

	return ret;
}

static irqreturn_t nvme_irq_check(int irq, void *data)
{
	struct nvme_queue *nvmeq = data;
	if (nvme_cqe_pending(nvmeq))
		return IRQ_WAKE_THREAD;
	return IRQ_NONE;
}

/*
 * Poll for completions any queue, including those not dedicated to polling.
 * Can be called from any context.
 */
static int nvme_poll_irqdisable(struct nvme_queue *nvmeq, unsigned int tag)
{
#ifdef HAVE_PCI_IRQ_API
	struct pci_dev *pdev = to_pci_dev(nvmeq->dev->dev);
#endif
	u16 start, end;
	int found;

	if (nvmeq->p2p)
		return 0;

	/*
	 * For a poll queue we need to protect against the polling thread
	 * using the CQ lock.  For normal interrupt driven threads we have
	 * to disable the interrupt to avoid racing with it.
	 */
	if (test_bit(NVMEQ_POLLED, &nvmeq->flags)) {
		spin_lock(&nvmeq->cq_poll_lock);
		found = nvme_process_cq(nvmeq, &start, &end, tag);
		spin_unlock(&nvmeq->cq_poll_lock);
	} else {
#ifdef HAVE_PCI_IRQ_API
		disable_irq(pci_irq_vector(pdev, nvmeq->cq_vector));
#else
		disable_irq(nvmeq->dev->entry[nvmeq->cq_vector].vector);
#endif
		found = nvme_process_cq(nvmeq, &start, &end, tag);
#ifdef HAVE_PCI_IRQ_API
		enable_irq(pci_irq_vector(pdev, nvmeq->cq_vector));
#else
		enable_irq(nvmeq->dev->entry[nvmeq->cq_vector].vector);
#endif
	}

	nvme_complete_cqes(nvmeq, start, end);
	return found;
}

#ifdef HAVE_BLK_MQ_OPS_POLL
// #ifdef HAVE_BLK_MQ_POLL_FN_1_ARG
#if 1
static int nvme_poll(struct blk_mq_hw_ctx *hctx)
#else
static int nvme_poll(struct blk_mq_hw_ctx *hctx, unsigned int tag)
#endif
{
	struct nvme_queue *nvmeq = hctx->driver_data;
	u16 start, end;
	bool found;

	if (!nvme_cqe_pending(nvmeq))
		return 0;

	spin_lock(&nvmeq->cq_poll_lock);
// #ifdef HAVE_BLK_MQ_POLL_FN_1_ARG
#if 1
	found = nvme_process_cq(nvmeq, &start, &end, -1);
#else
	found = nvme_process_cq(nvmeq, &start, &end, tag);
#endif
	spin_unlock(&nvmeq->cq_poll_lock);

	nvme_complete_cqes(nvmeq, start, end);
	return found;
}
#endif /* HAVE_BLK_MQ_OPS_POLL */

static void nvme_pci_submit_async_event(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);
	struct nvme_queue *nvmeq = &dev->queues[0];
	struct nvme_command c;

	memset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_async_event;
	c.common.command_id = NVME_AQ_BLK_MQ_DEPTH;
	nvme_submit_cmd(nvmeq, &c, true);
}

static int adapter_delete_queue(struct nvme_dev *dev, u8 opcode, u16 id)
{
	struct nvme_command c;

	memset(&c, 0, sizeof(c));
	c.delete_queue.opcode = opcode;
	c.delete_queue.qid = cpu_to_le16(id);

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_alloc_cq(struct nvme_dev *dev, u16 qid,
		struct nvme_queue *nvmeq, s16 vector)
{
	struct nvme_command c;
	int flags = NVME_QUEUE_PHYS_CONTIG;

	if (!test_bit(NVMEQ_POLLED, &nvmeq->flags) && !nvmeq->p2p)
		flags |= NVME_CQ_IRQ_ENABLED;

	/*
	 * Note: we (ab)use the fact that the prp fields survive if no data
	 * is attached to the request.
	 */
	memset(&c, 0, sizeof(c));
	c.create_cq.opcode = nvme_admin_create_cq;
	c.create_cq.prp1 = cpu_to_le64(nvmeq->cq_dma_addr);
	c.create_cq.cqid = cpu_to_le16(qid);
	c.create_cq.qsize = cpu_to_le16(nvmeq->q_depth - 1);
	c.create_cq.cq_flags = cpu_to_le16(flags);
	c.create_cq.irq_vector = cpu_to_le16(vector);

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_alloc_sq(struct nvme_dev *dev, u16 qid,
						struct nvme_queue *nvmeq)
{
	struct nvme_ctrl *ctrl = &dev->ctrl;
	struct nvme_command c;
	int flags = NVME_QUEUE_PHYS_CONTIG;

	/*
	 * Some drives have a bug that auto-enables WRRU if MEDIUM isn't
	 * set. Since URGENT priority is zeroes, it makes all queues
	 * URGENT.
	 */
	if (ctrl->quirks & NVME_QUIRK_MEDIUM_PRIO_SQ)
		flags |= NVME_SQ_PRIO_MEDIUM;

	/*
	 * Note: we (ab)use the fact that the prp fields survive if no data
	 * is attached to the request.
	 */
	memset(&c, 0, sizeof(c));
	c.create_sq.opcode = nvme_admin_create_sq;
	c.create_sq.prp1 = cpu_to_le64(nvmeq->sq_dma_addr);
	c.create_sq.sqid = cpu_to_le16(qid);
	c.create_sq.qsize = cpu_to_le16(nvmeq->q_depth - 1);
	c.create_sq.sq_flags = cpu_to_le16(flags);
	c.create_sq.cqid = cpu_to_le16(qid);

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_delete_cq(struct nvme_dev *dev, u16 cqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_cq, cqid);
}

static int adapter_delete_sq(struct nvme_dev *dev, u16 sqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_sq, sqid);
}

static void abort_endio(struct request *req, blk_status_t error)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = iod->nvmeq;

	dev_warn(nvmeq->dev->ctrl.device,
		 "Abort status: 0x%x", nvme_req(req)->status);
	atomic_inc(&nvmeq->dev->ctrl.abort_limit);
	blk_mq_free_request(req);
}

static bool nvme_should_reset(struct nvme_dev *dev, u32 csts)
{

	/* If true, indicates loss of adapter communication, possibly by a
	 * NVMe Subsystem reset.
	 */
	bool nssro = dev->subsystem && (csts & NVME_CSTS_NSSRO);

	/* If there is a reset/reinit ongoing, we shouldn't reset again. */
	switch (dev->ctrl.state) {
	case NVME_CTRL_RESETTING:
	case NVME_CTRL_CONNECTING:
		return false;
	default:
		break;
	}

	/* We shouldn't reset unless the controller is on fatal error state
	 * _or_ if we lost the communication with it.
	 */
	if (!(csts & NVME_CSTS_CFS) && !nssro)
		return false;

	return true;
}

static void nvme_warn_reset(struct nvme_dev *dev, u32 csts)
{
	/* Read a config register to help see what died. */
	u16 pci_status;
	int result;

	result = pci_read_config_word(to_pci_dev(dev->dev), PCI_STATUS,
				      &pci_status);
	if (result == PCIBIOS_SUCCESSFUL)
		dev_warn(dev->ctrl.device,
			 "controller is down; will reset: CSTS=0x%x, PCI_STATUS=0x%hx\n",
			 csts, pci_status);
	else
		dev_warn(dev->ctrl.device,
			 "controller is down; will reset: CSTS=0x%x, PCI_STATUS read failed (%d)\n",
			 csts, result);
}

static enum blk_eh_timer_return nvme_timeout(struct request *req, bool reserved)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = iod->nvmeq;
	struct nvme_dev *dev = nvmeq->dev;
	struct request *abort_req;
	struct nvme_command cmd;
	u32 csts = readl(dev->bar + NVME_REG_CSTS);

	/* If PCI error recovery process is happening, we cannot reset or
	 * the recovery mechanism will surely fail.
	 */
	mb();
	if (pci_channel_offline(to_pci_dev(dev->dev)))
		return BLK_EH_RESET_TIMER;

	/*
	 * Reset immediately if the controller is failed
	 */
	if (nvme_should_reset(dev, csts)) {
		nvme_warn_reset(dev, csts);
		nvme_dev_disable(dev, false);
		nvme_reset_ctrl(&dev->ctrl);
#ifdef HAVE_BLK_EH_DONE
		return BLK_EH_DONE;
#else
		return BLK_EH_HANDLED;
#endif
	}

	/*
	 * Did we miss an interrupt?
	 */
	if (nvme_poll_irqdisable(nvmeq, req->tag)) {
		dev_warn(dev->ctrl.device,
			 "I/O %d QID %d timeout, completion polled\n",
			 req->tag, nvmeq->qid);
#ifdef HAVE_BLK_EH_DONE
		return BLK_EH_DONE;
#else
		return BLK_EH_HANDLED;
#endif
	}

	/*
	 * Shutdown immediately if controller times out while starting. The
	 * reset work will see the pci device disabled when it gets the forced
	 * cancellation error. All outstanding requests are completed on
	 * shutdown, so we return BLK_EH_DONE.
	 */
	switch (dev->ctrl.state) {
	case NVME_CTRL_CONNECTING:
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);
		/* fall through */
	case NVME_CTRL_DELETING:
		dev_warn_ratelimited(dev->ctrl.device,
			 "I/O %d QID %d timeout, disable controller\n",
			 req->tag, nvmeq->qid);
		nvme_dev_disable(dev, true);
		nvme_req(req)->flags |= NVME_REQ_CANCELLED;
#ifdef HAVE_BLK_EH_DONE
		return BLK_EH_DONE;
#else
		return BLK_EH_HANDLED;
#endif
	case NVME_CTRL_RESETTING:
		return BLK_EH_RESET_TIMER;
	default:
		break;
	}

	/*
 	 * Shutdown the controller immediately and schedule a reset if the
 	 * command was already aborted once before and still hasn't been
 	 * returned to the driver, or if this is the admin queue.
	 */
	if (!nvmeq->qid || iod->aborted) {
		dev_warn(dev->ctrl.device,
			 "I/O %d QID %d timeout, reset controller\n",
			 req->tag, nvmeq->qid);
		nvme_dev_disable(dev, false);
		nvme_reset_ctrl(&dev->ctrl);

		nvme_req(req)->flags |= NVME_REQ_CANCELLED;
#ifdef HAVE_BLK_EH_DONE
		return BLK_EH_DONE;
#else
		return BLK_EH_HANDLED;
#endif
	}

	if (atomic_dec_return(&dev->ctrl.abort_limit) < 0) {
		atomic_inc(&dev->ctrl.abort_limit);
		return BLK_EH_RESET_TIMER;
	}
	iod->aborted = 1;

	memset(&cmd, 0, sizeof(cmd));
	cmd.abort.opcode = nvme_admin_abort_cmd;
	cmd.abort.cid = req->tag;
	cmd.abort.sqid = cpu_to_le16(nvmeq->qid);

	dev_warn(nvmeq->dev->ctrl.device,
		"I/O %d QID %d timeout, aborting\n",
		 req->tag, nvmeq->qid);

#ifdef HAVE_BLK_MQ_ALLOC_REQUEST_HAS_3_PARAMS
	abort_req = nvme_alloc_request(dev->ctrl.admin_q, &cmd,
			BLK_MQ_REQ_NOWAIT, NVME_QID_ANY);
#else
	abort_req = nvme_alloc_request(dev->ctrl.admin_q, &cmd,
			GFP_KERNEL, reserved, NVME_QID_ANY);
#endif
	if (IS_ERR(abort_req)) {
		atomic_inc(&dev->ctrl.abort_limit);
		return BLK_EH_RESET_TIMER;
	}

	abort_req->timeout = ADMIN_TIMEOUT;
	abort_req->end_io_data = NULL;
	blk_execute_rq_nowait(NULL, abort_req, 0, abort_endio);

	/*
	 * The aborted req will be completed on receiving the abort req.
	 * We enable the timer again. If hit twice, it'll cause a device reset,
	 * as the device then is in a faulty state.
	 */
	return BLK_EH_RESET_TIMER;
}

static void nvme_free_queue(struct nvme_queue *nvmeq)
{
	dma_free_coherent(nvmeq->dev->dev, CQ_SIZE(nvmeq->q_depth),
				(void *)nvmeq->cqes, nvmeq->cq_dma_addr);
	if (!nvmeq->sq_cmds)
		return;

	if (test_and_clear_bit(NVMEQ_SQ_CMB, &nvmeq->flags)) {
		pci_free_p2pmem(to_pci_dev(nvmeq->dev->dev),
				nvmeq->sq_cmds, SQ_SIZE(nvmeq->q_depth));
	} else {
		dma_free_coherent(nvmeq->dev->dev, SQ_SIZE(nvmeq->q_depth),
				nvmeq->sq_cmds, nvmeq->sq_dma_addr);
	}
}

static void nvme_free_queues(struct nvme_dev *dev, int lowest)
{
	int i;

	for (i = dev->ctrl.queue_count - 1; i >= lowest; i--) {
		dev->ctrl.queue_count--;
		nvme_free_queue(&dev->queues[i]);
	}
}

/**
 * nvme_suspend_queue - put queue into suspended state
 * @nvmeq: queue to suspend
 */
static int nvme_suspend_queue(struct nvme_queue *nvmeq)
{
	if (!test_and_clear_bit(NVMEQ_ENABLED, &nvmeq->flags))
		return 1;

	/* ensure that nvme_queue_rq() sees NVMEQ_ENABLED cleared */
	mb();

	if(nvmeq->p2p) {
		mutex_lock(&nvmeq->resource.lock);
		if (nvmeq->resource.in_use && nvmeq->resource.stop_master_peer) {
			mutex_unlock(&nvmeq->resource.lock);
			nvmeq->resource.stop_master_peer(nvmeq->resource.dd_data);
		} else
			mutex_unlock(&nvmeq->resource.lock);
	}

	nvmeq->dev->online_queues--;
	if (!nvmeq->qid && nvmeq->dev->ctrl.admin_q)
#ifdef HAVE_BLK_MQ_UNQUIESCE_QUEUE
		blk_mq_quiesce_queue(nvmeq->dev->ctrl.admin_q);
#else
		blk_mq_stop_hw_queues(nvmeq->dev->ctrl.admin_q);
#endif
	if (!nvmeq->p2p && !test_and_clear_bit(NVMEQ_POLLED, &nvmeq->flags))
#ifdef HAVE_PCI_FREE_IRQ
		pci_free_irq(to_pci_dev(nvmeq->dev->dev), nvmeq->cq_vector, nvmeq);
#else
		free_irq(nvmeq_irq(nvmeq), nvmeq);
#endif
	return 0;
}

static void nvme_suspend_io_queues(struct nvme_dev *dev)
{
	int i;

	for (i = dev->ctrl.queue_count - 1; i > 0; i--)
		nvme_suspend_queue(&dev->queues[i]);
}

static void nvme_disable_admin_queue(struct nvme_dev *dev, bool shutdown)
{
	struct nvme_queue *nvmeq = &dev->queues[0];

	if (shutdown)
		nvme_shutdown_ctrl(&dev->ctrl);
	else
		nvme_disable_ctrl(&dev->ctrl, dev->ctrl.cap);

	nvme_poll_irqdisable(nvmeq, -1);
}

static int nvme_cmb_qdepth(struct nvme_dev *dev, int nr_io_queues,
				int entry_size)
{
	int q_depth = dev->q_depth;
	unsigned q_size_aligned = roundup(q_depth * entry_size,
					  dev->ctrl.page_size);

	if (q_size_aligned * nr_io_queues > dev->cmb_size) {
		u64 mem_per_q = div_u64(dev->cmb_size, nr_io_queues);
		mem_per_q = round_down(mem_per_q, dev->ctrl.page_size);
		q_depth = div_u64(mem_per_q, entry_size);

		/*
		 * Ensure the reduced q_depth is above some threshold where it
		 * would be better to map queues in system memory with the
		 * original depth
		 */
		if (q_depth < 64)
			return -ENOMEM;
	}

	return q_depth;
}

static int nvme_alloc_sq_cmds(struct nvme_dev *dev, struct nvme_queue *nvmeq,
				int qid, int depth)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (qid && dev->cmb_use_sqes && (dev->cmbsz & NVME_CMBSZ_SQS)) {
		nvmeq->sq_cmds = pci_alloc_p2pmem(pdev, SQ_SIZE(depth));
		nvmeq->sq_dma_addr = pci_p2pmem_virt_to_bus(pdev,
						nvmeq->sq_cmds);
		if (nvmeq->sq_dma_addr) {
			set_bit(NVMEQ_SQ_CMB, &nvmeq->flags);
			return 0; 
		}
	}

	nvmeq->sq_cmds = dma_alloc_coherent(dev->dev, SQ_SIZE(depth),
				&nvmeq->sq_dma_addr, GFP_KERNEL);
	if (!nvmeq->sq_cmds)
		return -ENOMEM;
	return 0;
}

static int nvme_alloc_queue(struct nvme_dev *dev, int qid, int depth)
{
	struct nvme_queue *nvmeq = &dev->queues[qid];

	if (dev->ctrl.queue_count > qid)
		return 0;

	nvmeq->cqes = dma_alloc_coherent(dev->dev, CQ_SIZE(depth),
					 &nvmeq->cq_dma_addr, GFP_KERNEL);
	if (!nvmeq->cqes)
		goto free_nvmeq;

	if (nvme_alloc_sq_cmds(dev, nvmeq, qid, depth))
		goto free_cqdma;

	nvmeq->dev = dev;
#ifndef HAVE_PCI_FREE_IRQ
	snprintf(nvmeq->irqname, sizeof(nvmeq->irqname), "nvme%dq%d",
		 dev->ctrl.instance, qid);
#endif
	spin_lock_init(&nvmeq->sq_lock);
	spin_lock_init(&nvmeq->cq_poll_lock);
	nvmeq->cq_head = 0;
	nvmeq->cq_phase = 1;
	nvmeq->q_db = &dev->dbs[qid * 2 * dev->db_stride];
	nvmeq->q_depth = depth;
	nvmeq->qid = qid;
	nvmeq->p2p = qid > (dev->max_qid - dev->num_p2p_queues);
	if (nvmeq->p2p)
		mutex_init(&nvmeq->resource.lock);
	dev->ctrl.queue_count++;

	return 0;

 free_cqdma:
	dma_free_coherent(dev->dev, CQ_SIZE(depth), (void *)nvmeq->cqes,
							nvmeq->cq_dma_addr);
 free_nvmeq:
	return -ENOMEM;
}

static int queue_request_irq(struct nvme_queue *nvmeq)
{
#ifdef HAVE_PCI_FREE_IRQ
	struct pci_dev *pdev = to_pci_dev(nvmeq->dev->dev);
	int nr = nvmeq->dev->ctrl.instance;

	if (use_threaded_interrupts) {
		return pci_request_irq(pdev, nvmeq->cq_vector, nvme_irq_check,
				nvme_irq, nvmeq, "nvme%dq%d", nr, nvmeq->qid);
	} else {
		return pci_request_irq(pdev, nvmeq->cq_vector, nvme_irq,
				NULL, nvmeq, "nvme%dq%d", nr, nvmeq->qid);
	}
#else
	if (use_threaded_interrupts)
		return request_threaded_irq(nvmeq_irq(nvmeq), nvme_irq_check,
				nvme_irq, IRQF_SHARED, nvmeq->irqname, nvmeq);
	else
		return request_irq(nvmeq_irq(nvmeq), nvme_irq, IRQF_SHARED,
				nvmeq->irqname, nvmeq);
#endif
}

static void nvme_init_queue(struct nvme_queue *nvmeq, u16 qid)
{
	struct nvme_dev *dev = nvmeq->dev;

	nvmeq->sq_tail = 0;
#ifdef HAVE_BLK_MQ_OPS_COMMIT_RQS
	nvmeq->last_sq_tail = 0;
#endif
	nvmeq->cq_head = 0;
	nvmeq->cq_phase = 1;
	nvmeq->q_db = &dev->dbs[qid * 2 * dev->db_stride];
	memset((void *)nvmeq->cqes, 0, CQ_SIZE(nvmeq->q_depth));
	nvme_dbbuf_init(dev, nvmeq, qid);
	dev->online_queues++;
	wmb(); /* ensure the first interrupt sees the initialization */
}

static int nvme_create_queue(struct nvme_queue *nvmeq, int qid, bool polled)
{
	struct nvme_dev *dev = nvmeq->dev;
	int result;
	u16 vector = 0;

	clear_bit(NVMEQ_DELETE_ERROR, &nvmeq->flags);

	/*
	 * A queue's vector matches the queue identifier unless the controller
	 * has only one vector available.
	 */
	if (!polled && !nvmeq->p2p)
#if defined(HAVE_PCI_IRQ_API) && defined(HAVE_IRQ_CALC_AFFINITY_VECTORS_3_ARGS)
		vector = dev->num_vecs == 1 ? 0 : qid;
#else
		vector = qid - 1;
#endif
	else if (polled)
		set_bit(NVMEQ_POLLED, &nvmeq->flags);

	result = adapter_alloc_cq(dev, qid, nvmeq, vector);
	if (result)
		return result;

	result = adapter_alloc_sq(dev, qid, nvmeq);
	if (result < 0)
		return result;
	else if (result)
		goto release_cq;

	nvmeq->cq_vector = vector;
	nvme_init_queue(nvmeq, qid);

	if (!nvmeq->p2p && !polled) {
		result = queue_request_irq(nvmeq);
		if (result < 0)
			goto release_sq;
	}

	set_bit(NVMEQ_ENABLED, &nvmeq->flags);
	return result;

release_sq:
	dev->online_queues--;
	adapter_delete_sq(dev, qid);
release_cq:
	adapter_delete_cq(dev, qid);
	return result;
}

#ifdef HAVE_BLK_MQ_TAG_SET_HAS_CONST_OPS
static const struct blk_mq_ops nvme_mq_admin_ops = {
#else
static struct blk_mq_ops nvme_mq_admin_ops = {
#endif
	.queue_rq	= nvme_queue_rq,
	.complete	= nvme_pci_complete_rq,
#ifdef HAVE_BLK_MQ_OPS_MAP_QUEUE
	.map_queue	= blk_mq_map_queue,
#endif
	.init_hctx	= nvme_admin_init_hctx,
	.exit_hctx      = nvme_admin_exit_hctx,
#ifdef HAVE_BLK_MQ_OPS_INIT_REQUEST_HAS_4_PARAMS
	.init_request	= nvme_init_request,
#else
	.init_request	= nvme_admin_init_request,
#endif
	.timeout	= nvme_timeout,
};

#ifdef HAVE_BLK_MQ_TAG_SET_HAS_CONST_OPS
static const struct blk_mq_ops nvme_mq_ops = {
#else
static struct blk_mq_ops nvme_mq_ops = {
#endif
	.queue_rq	= nvme_queue_rq,
	.complete	= nvme_pci_complete_rq,
#ifdef HAVE_BLK_MQ_OPS_MAP_QUEUE
	.map_queue	= blk_mq_map_queue,
#endif
#ifdef HAVE_BLK_MQ_OPS_COMMIT_RQS
	.commit_rqs	= nvme_commit_rqs,
#endif
	.init_hctx	= nvme_init_hctx,
	.init_request	= nvme_init_request,
#if defined(HAVE_BLK_MQ_OPS_MAP_QUEUES) && \
	(defined(HAVE_PCI_IRQ_GET_AFFINITY) || \
	defined(HAVE_BLK_MQ_PCI_MAP_QUEUES_3_ARGS))
	.map_queues	= nvme_pci_map_queues,
#endif
	.timeout	= nvme_timeout,
#ifdef HAVE_BLK_MQ_OPS_POLL
	.poll		= nvme_poll,
#endif
};

static void nvme_dev_remove_admin(struct nvme_dev *dev)
{
	if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q)) {
		/*
		 * If the controller was reset during removal, it's possible
		 * user requests may be waiting on a stopped queue. Start the
		 * queue to flush these to completion.
		 */
#ifdef HAVE_BLK_MQ_UNQUIESCE_QUEUE
		blk_mq_unquiesce_queue(dev->ctrl.admin_q);
#else
		blk_mq_start_stopped_hw_queues(dev->ctrl.admin_q, true);
#endif
		blk_cleanup_queue(dev->ctrl.admin_q);
		blk_mq_free_tag_set(&dev->admin_tagset);
	}
}

static int nvme_alloc_admin_tags(struct nvme_dev *dev)
{
	if (!dev->ctrl.admin_q) {
		dev->admin_tagset.ops = &nvme_mq_admin_ops;
		dev->admin_tagset.nr_hw_queues = 1;

		dev->admin_tagset.queue_depth = NVME_AQ_MQ_TAG_DEPTH;
		dev->admin_tagset.timeout = ADMIN_TIMEOUT;
		dev->admin_tagset.numa_node = dev_to_node(dev->dev);
		dev->admin_tagset.cmd_size = sizeof(struct nvme_iod);
#ifdef HAVE_BLK_MQ_F_NO_SCHED
		dev->admin_tagset.flags = BLK_MQ_F_NO_SCHED;
#endif
		dev->admin_tagset.driver_data = dev;

		if (blk_mq_alloc_tag_set(&dev->admin_tagset))
			return -ENOMEM;
		dev->ctrl.admin_tagset = &dev->admin_tagset;

		dev->ctrl.admin_q = blk_mq_init_queue(&dev->admin_tagset);
		if (IS_ERR(dev->ctrl.admin_q)) {
			blk_mq_free_tag_set(&dev->admin_tagset);
			return -ENOMEM;
		}
		if (!blk_get_queue(dev->ctrl.admin_q)) {
			nvme_dev_remove_admin(dev);
			dev->ctrl.admin_q = NULL;
			return -ENODEV;
		}
	} else
#ifdef HAVE_BLK_MQ_UNQUIESCE_QUEUE
		blk_mq_unquiesce_queue(dev->ctrl.admin_q);
#else
		blk_mq_start_stopped_hw_queues(dev->ctrl.admin_q, true);
#endif

	return 0;
}

static unsigned long db_bar_size(struct nvme_dev *dev, unsigned nr_io_queues)
{
	return NVME_REG_DBS + ((nr_io_queues + 1) * 8 * dev->db_stride);
}

static int nvme_remap_bar(struct nvme_dev *dev, unsigned long size)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (size <= dev->bar_mapped_size)
		return 0;
	if (size > pci_resource_len(pdev, 0))
		return -ENOMEM;
	if (dev->bar)
		iounmap(dev->bar);
	dev->bar = ioremap(pci_resource_start(pdev, 0), size);
	if (!dev->bar) {
		dev->bar_mapped_size = 0;
		return -ENOMEM;
	}
	dev->bar_mapped_size = size;
	dev->dbs = dev->bar + NVME_REG_DBS;

	return 0;
}

static int nvme_pci_configure_admin_queue(struct nvme_dev *dev)
{
	int result;
	u32 aqa;
	struct nvme_queue *nvmeq;

	result = nvme_remap_bar(dev, db_bar_size(dev, 0));
	if (result < 0)
		return result;

	dev->subsystem = readl(dev->bar + NVME_REG_VS) >= NVME_VS(1, 1, 0) ?
				NVME_CAP_NSSRC(dev->ctrl.cap) : 0;

	if (dev->subsystem &&
	    (readl(dev->bar + NVME_REG_CSTS) & NVME_CSTS_NSSRO))
		writel(NVME_CSTS_NSSRO, dev->bar + NVME_REG_CSTS);

	result = nvme_disable_ctrl(&dev->ctrl, dev->ctrl.cap);
	if (result < 0)
		return result;

	result = nvme_alloc_queue(dev, 0, NVME_AQ_DEPTH);
	if (result)
		return result;

	nvmeq = &dev->queues[0];
	aqa = nvmeq->q_depth - 1;
	aqa |= aqa << 16;

	writel(aqa, dev->bar + NVME_REG_AQA);
	lo_hi_writeq(nvmeq->sq_dma_addr, dev->bar + NVME_REG_ASQ);
	lo_hi_writeq(nvmeq->cq_dma_addr, dev->bar + NVME_REG_ACQ);

	result = nvme_enable_ctrl(&dev->ctrl, dev->ctrl.cap);
	if (result)
		return result;

	nvmeq->cq_vector = 0;
	nvme_init_queue(nvmeq, 0);
	result = queue_request_irq(nvmeq);
	if (result) {
		dev->online_queues--;
		return result;
	}

	set_bit(NVMEQ_ENABLED, &nvmeq->flags);
	return result;
}

static int nvme_create_io_queues(struct nvme_dev *dev)
{
	unsigned i, max, rw_queues;
	int ret = 0;

	for (i = dev->ctrl.queue_count; i <= dev->max_qid; i++) {
		if (nvme_alloc_queue(dev, i, dev->q_depth)) {
			ret = -ENOMEM;
			break;
		}
	}

	max = min(dev->max_qid, dev->ctrl.queue_count - 1);
#ifdef HAVE_BLK_MQ_HCTX_TYPE
	if (max != 1 && dev->io_queues[HCTX_TYPE_POLL]) {
		rw_queues = dev->io_queues[HCTX_TYPE_DEFAULT] +
				dev->io_queues[HCTX_TYPE_READ];
	} else {
		rw_queues = max;
	}
#else
	rw_queues = max;
#endif

	for (i = dev->online_queues; i <= max; i++) {
		bool polled = i > rw_queues && !dev->queues[i].p2p;

		ret = nvme_create_queue(&dev->queues[i], i, polled);
		if (ret)
			break;
	}

	/*
	 * Ignore failing Create SQ/CQ commands, we can continue with less
	 * than the desired amount of queues, and even a controller without
	 * I/O queues can still be used to issue admin commands.  This might
	 * be useful to upgrade a buggy firmware for example.
	 */
	return ret >= 0 ? 0 : ret;
}

static ssize_t nvme_num_p2p_queues_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct nvme_dev *ndev = to_nvme_dev(dev_get_drvdata(dev));
	unsigned num_p2p_queues = ndev->online_queues > 1 ?
			ndev->num_p2p_queues : 0;

	return scnprintf(buf, PAGE_SIZE, "%u\n", num_p2p_queues);
}
static DEVICE_ATTR(num_p2p_queues, S_IRUGO, nvme_num_p2p_queues_show, NULL);

static ssize_t nvme_cmb_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct nvme_dev *ndev = to_nvme_dev(dev_get_drvdata(dev));

	return scnprintf(buf, PAGE_SIZE, "cmbloc : x%08x\ncmbsz  : x%08x\n",
		       ndev->cmbloc, ndev->cmbsz);
}
static DEVICE_ATTR(cmb, S_IRUGO, nvme_cmb_show, NULL);

static u64 nvme_cmb_size_unit(struct nvme_dev *dev)
{
	u8 szu = (dev->cmbsz >> NVME_CMBSZ_SZU_SHIFT) & NVME_CMBSZ_SZU_MASK;

	return 1ULL << (12 + 4 * szu);
}

static u32 nvme_cmb_size(struct nvme_dev *dev)
{
	return (dev->cmbsz >> NVME_CMBSZ_SZ_SHIFT) & NVME_CMBSZ_SZ_MASK;
}

static void nvme_map_cmb(struct nvme_dev *dev)
{
	u64 size, offset;
	resource_size_t bar_size;
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	int bar;

	if (dev->cmb_size)
		return;

	dev->cmbsz = readl(dev->bar + NVME_REG_CMBSZ);
	if (!dev->cmbsz)
		return;
	dev->cmbloc = readl(dev->bar + NVME_REG_CMBLOC);

	size = nvme_cmb_size_unit(dev) * nvme_cmb_size(dev);
	offset = nvme_cmb_size_unit(dev) * NVME_CMB_OFST(dev->cmbloc);
	bar = NVME_CMB_BIR(dev->cmbloc);
	bar_size = pci_resource_len(pdev, bar);

	if (offset > bar_size)
		return;

	/*
	 * Controllers may support a CMB size larger than their BAR,
	 * for example, due to being behind a bridge. Reduce the CMB to
	 * the reported size of the BAR
	 */
	if (size > bar_size - offset)
		size = bar_size - offset;

	if (pci_p2pdma_add_resource(pdev, bar, size, offset)) {
		dev_warn(dev->ctrl.device,
			 "failed to register the CMB\n");
		return;
	}

	dev->cmb_size = size;
	dev->cmb_use_sqes = use_cmb_sqes && (dev->cmbsz & NVME_CMBSZ_SQS);

	if ((dev->cmbsz & (NVME_CMBSZ_WDS | NVME_CMBSZ_RDS)) ==
			(NVME_CMBSZ_WDS | NVME_CMBSZ_RDS))
		pci_p2pmem_publish(pdev, true);

	if (sysfs_add_file_to_group(&dev->ctrl.device->kobj,
				    &dev_attr_cmb.attr, NULL))
		dev_warn(dev->ctrl.device,
			 "failed to add sysfs attribute for CMB\n");
}

static inline void nvme_release_cmb(struct nvme_dev *dev)
{
	if (dev->cmb_size) {
		sysfs_remove_file_from_group(&dev->ctrl.device->kobj,
					     &dev_attr_cmb.attr, NULL);
		dev->cmb_size = 0;
	}
}

static int nvme_set_host_mem(struct nvme_dev *dev, u32 bits)
{
	u64 dma_addr = dev->host_mem_descs_dma;
	struct nvme_command c;
	int ret;

	memset(&c, 0, sizeof(c));
	c.features.opcode	= nvme_admin_set_features;
	c.features.fid		= cpu_to_le32(NVME_FEAT_HOST_MEM_BUF);
	c.features.dword11	= cpu_to_le32(bits);
	c.features.dword12	= cpu_to_le32(dev->host_mem_size >>
					      ilog2(dev->ctrl.page_size));
	c.features.dword13	= cpu_to_le32(lower_32_bits(dma_addr));
	c.features.dword14	= cpu_to_le32(upper_32_bits(dma_addr));
	c.features.dword15	= cpu_to_le32(dev->nr_host_mem_descs);

	ret = nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
	if (ret) {
		dev_warn(dev->ctrl.device,
			 "failed to set host mem (err %d, flags %#x).\n",
			 ret, bits);
	}
	return ret;
}

static void nvme_free_host_mem(struct nvme_dev *dev)
{
	int i;

	for (i = 0; i < dev->nr_host_mem_descs; i++) {
		struct nvme_host_mem_buf_desc *desc = &dev->host_mem_descs[i];
		size_t size = le32_to_cpu(desc->size) * dev->ctrl.page_size;

#ifdef HAVE_DMA_ATTRS
		DEFINE_DMA_ATTRS(attrs);
		dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
		dma_free_attrs(dev->dev, size, dev->host_mem_desc_bufs[i],
			       le64_to_cpu(desc->addr), &attrs);
#else
		dma_free_attrs(dev->dev, size, dev->host_mem_desc_bufs[i],
			       le64_to_cpu(desc->addr),
#ifdef HAVE_DMA_ATTR_NO_WARN
			       DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_NO_WARN);
#else
			       DMA_ATTR_NO_KERNEL_MAPPING);
#endif
#endif
	}

	kfree(dev->host_mem_desc_bufs);
	dev->host_mem_desc_bufs = NULL;
	dma_free_coherent(dev->dev,
			dev->nr_host_mem_descs * sizeof(*dev->host_mem_descs),
			dev->host_mem_descs, dev->host_mem_descs_dma);
	dev->host_mem_descs = NULL;
	dev->nr_host_mem_descs = 0;
}

static int __nvme_alloc_host_mem(struct nvme_dev *dev, u64 preferred,
		u32 chunk_size)
{
	struct nvme_host_mem_buf_desc *descs;
	u32 max_entries, len;
	dma_addr_t descs_dma;
	int i = 0;
	void **bufs;
	u64 size, tmp;

	tmp = (preferred + chunk_size - 1);
	do_div(tmp, chunk_size);
	max_entries = tmp;

	if (dev->ctrl.hmmaxd && dev->ctrl.hmmaxd < max_entries)
		max_entries = dev->ctrl.hmmaxd;

	descs = dma_alloc_coherent(dev->dev, max_entries * sizeof(*descs),
				   &descs_dma, GFP_KERNEL);
	if (!descs)
		goto out;

	bufs = kcalloc(max_entries, sizeof(*bufs), GFP_KERNEL);
	if (!bufs)
		goto out_free_descs;

	for (size = 0; size < preferred && i < max_entries; size += len) {
		dma_addr_t dma_addr;
#ifndef HAVE_DMA_SET_ATTR_TAKES_UNSIGNED_LONG_ATTRS
		DEFINE_DMA_ATTRS(attrs);
#ifdef HAVE_DMA_ATTR_NO_WARN
		dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_NO_WARN, &attrs);
#else
		dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
#endif
#endif

		len = min_t(u64, chunk_size, preferred - size);
		bufs[i] = dma_alloc_attrs(dev->dev, len, &dma_addr, GFP_KERNEL,
#ifdef HAVE_DMA_SET_ATTR_TAKES_UNSIGNED_LONG_ATTRS
#ifdef HAVE_DMA_ATTR_NO_WARN
				DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_NO_WARN);
#else
				DMA_ATTR_NO_KERNEL_MAPPING);
#endif
#else
				&attrs);
#endif
		if (!bufs[i])
			break;

		descs[i].addr = cpu_to_le64(dma_addr);
		descs[i].size = cpu_to_le32(len / dev->ctrl.page_size);
		i++;
	}

	if (!size)
		goto out_free_bufs;

	dev->nr_host_mem_descs = i;
	dev->host_mem_size = size;
	dev->host_mem_descs = descs;
	dev->host_mem_descs_dma = descs_dma;
	dev->host_mem_desc_bufs = bufs;
	return 0;

out_free_bufs:
	while (--i >= 0) {
		size_t size = le32_to_cpu(descs[i].size) * dev->ctrl.page_size;

#ifdef HAVE_DMA_ATTRS
		DEFINE_DMA_ATTRS(attrs);
		dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &attrs);
		dma_free_attrs(dev->dev, size, bufs[i],
			       le64_to_cpu(descs[i].addr), &attrs);
#else
		dma_free_attrs(dev->dev, size, bufs[i],
			       le64_to_cpu(descs[i].addr),
#ifdef HAVE_DMA_ATTR_NO_WARN
			       DMA_ATTR_NO_KERNEL_MAPPING | DMA_ATTR_NO_WARN);
#else
			       DMA_ATTR_NO_KERNEL_MAPPING);
#endif
#endif
	}

	kfree(bufs);
out_free_descs:
	dma_free_coherent(dev->dev, max_entries * sizeof(*descs), descs,
			descs_dma);
out:
	dev->host_mem_descs = NULL;
	return -ENOMEM;
}

static int nvme_alloc_host_mem(struct nvme_dev *dev, u64 min, u64 preferred)
{
	u32 chunk_size;

	/* start big and work our way down */
	for (chunk_size = min_t(u64, preferred, PAGE_SIZE * MAX_ORDER_NR_PAGES);
	     chunk_size >= max_t(u32, dev->ctrl.hmminds * 4096, PAGE_SIZE * 2);
	     chunk_size /= 2) {
		if (!__nvme_alloc_host_mem(dev, preferred, chunk_size)) {
			if (!min || dev->host_mem_size >= min)
				return 0;
			nvme_free_host_mem(dev);
		}
	}

	return -ENOMEM;
}

static int nvme_setup_host_mem(struct nvme_dev *dev)
{
	u64 max = (u64)max_host_mem_size_mb * SZ_1M;
	u64 preferred = (u64)dev->ctrl.hmpre * 4096;
	u64 min = (u64)dev->ctrl.hmmin * 4096;
	u32 enable_bits = NVME_HOST_MEM_ENABLE;
	int ret;

	preferred = min(preferred, max);
	if (min > max) {
		dev_warn(dev->ctrl.device,
			"min host memory (%lld MiB) above limit (%d MiB).\n",
			min >> ilog2(SZ_1M), max_host_mem_size_mb);
		nvme_free_host_mem(dev);
		return 0;
	}

	/*
	 * If we already have a buffer allocated check if we can reuse it.
	 */
	if (dev->host_mem_descs) {
		if (dev->host_mem_size >= min)
			enable_bits |= NVME_HOST_MEM_RETURN;
		else
			nvme_free_host_mem(dev);
	}

	if (!dev->host_mem_descs) {
		if (nvme_alloc_host_mem(dev, min, preferred)) {
			dev_warn(dev->ctrl.device,
				"failed to allocate host memory buffer.\n");
			return 0; /* controller must work without HMB */
		}

		dev_info(dev->ctrl.device,
			"allocated %lld MiB host memory buffer.\n",
			dev->host_mem_size >> ilog2(SZ_1M));
	}

	ret = nvme_set_host_mem(dev, enable_bits);
	if (ret)
		nvme_free_host_mem(dev);
	return ret;
}

#ifdef HAVE_IRQ_AFFINITY_PRIV
/*
 * nirqs is the number of interrupts available for write and read
 * queues. The core already reserved an interrupt for the admin queue.
 */
static void nvme_calc_irq_sets(struct irq_affinity *affd, unsigned int nrirqs)
{
	struct nvme_dev *dev = affd->priv;
	unsigned int nr_read_queues;

	/*
	 * If there is no interupt available for queues, ensure that
	 * the default queue is set to 1. The affinity set size is
	 * also set to one, but the irq core ignores it for this case.
	 *
	 * If only one interrupt is available or 'write_queue' == 0, combine
	 * write and read queues.
	 *
	 * If 'write_queues' > 0, ensure it leaves room for at least one read
	 * queue.
	 */
	if (!nrirqs) {
		nrirqs = 1;
		nr_read_queues = 0;
	} else if (nrirqs == 1 || !write_queues) {
		nr_read_queues = 0;
	} else if (write_queues >= nrirqs) {
		nr_read_queues = 1;
	} else {
		nr_read_queues = nrirqs - write_queues;
	}

	dev->io_queues[HCTX_TYPE_DEFAULT] = nrirqs - nr_read_queues;
	affd->set_size[HCTX_TYPE_DEFAULT] = nrirqs - nr_read_queues;
	dev->io_queues[HCTX_TYPE_READ] = nr_read_queues;
	affd->set_size[HCTX_TYPE_READ] = nr_read_queues;
	affd->nr_sets = nr_read_queues ? 2 : 1;
}
#endif

#if defined(HAVE_PCI_IRQ_API) && defined(HAVE_IRQ_CALC_AFFINITY_VECTORS_3_ARGS)
static int nvme_setup_irqs(struct nvme_dev *dev, unsigned int nr_io_queues)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	struct irq_affinity affd = {
		.pre_vectors	= 1,
#ifdef HAVE_IRQ_AFFINITY_PRIV
		.calc_sets	= nvme_calc_irq_sets,
		.priv		= dev,
#endif
	};
	unsigned int irq_queues, this_p_queues;

	/*
	 * Poll queues don't need interrupts, but we need at least one IO
	 * queue left over for non-polled IO.
	 */
	this_p_queues = poll_queues;
	if (this_p_queues >= nr_io_queues) {
		this_p_queues = nr_io_queues - 1;
		irq_queues = 1;
	} else {
		irq_queues = nr_io_queues - this_p_queues + 1 - dev->num_p2p_queues;
	}
#ifdef HAVE_BLK_MQ_HCTX_TYPE
	dev->io_queues[HCTX_TYPE_POLL] = this_p_queues;

	/* Initialize for the single interrupt case */
	dev->io_queues[HCTX_TYPE_DEFAULT] = 1;
	dev->io_queues[HCTX_TYPE_READ] = 0;
#endif

	return pci_alloc_irq_vectors_affinity(pdev, 1, irq_queues,
			      PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY, &affd);
}
#endif

static void nvme_disable_io_queues(struct nvme_dev *dev)
{
	if (__nvme_disable_io_queues(dev, nvme_admin_delete_sq))
		__nvme_disable_io_queues(dev, nvme_admin_delete_cq);
}

static int nvme_setup_io_queues(struct nvme_dev *dev)
{
	struct nvme_queue *adminq = &dev->queues[0];
	struct pci_dev *pdev = to_pci_dev(dev->dev);
#ifdef HAVE_PCI_IRQ_API
	int result, nr_io_queues;
#else
	int result, i, vecs, nr_io_queues;
#endif
	unsigned long size;

	nr_io_queues = max_io_queues() + dev->num_p2p_queues;
	result = nvme_set_queue_count(&dev->ctrl, &nr_io_queues);
	if (result < 0)
		return result;

	if (nr_io_queues == 0)
		return 0;
	
	clear_bit(NVMEQ_ENABLED, &adminq->flags);

	if (dev->cmb_use_sqes) {
		result = nvme_cmb_qdepth(dev, nr_io_queues,
				sizeof(struct nvme_command));
		if (result > 0)
			dev->q_depth = result;
		else
			dev->cmb_use_sqes = false;
	}

	if (dev->num_p2p_queues) {
		if (nr_io_queues <= dev->num_p2p_queues) {
			if (nr_io_queues > 1)
				dev->num_p2p_queues = nr_io_queues - 1;//dedicate only 1 io queue for non p2p
			else
				dev->num_p2p_queues = 0;//dedicate the only io queue for non p2p
		}
	}

	do {
		size = db_bar_size(dev, nr_io_queues);
		result = nvme_remap_bar(dev, size);
		if (!result)
			break;
		if (dev->num_p2p_queues)
			dev->num_p2p_queues--;
		if (!--nr_io_queues)
			return -ENOMEM;
	} while (1);
	adminq->q_db = dev->dbs;

 retry:
	/* Deregister the admin queue's interrupt */
#ifdef HAVE_PCI_FREE_IRQ
	pci_free_irq(pdev, 0, adminq);
#elif defined(HAVE_PCI_IRQ_API)
	free_irq(pci_irq_vector(pdev, 0), adminq);
#else
	free_irq(dev->entry[0].vector, adminq);
#endif

	/*
	 * If we enable msix early due to not intx, disable it again before
	 * setting up the full range we need.
	 */
#ifdef HAVE_PCI_IRQ_API
	pci_free_irq_vectors(pdev);
#ifdef HAVE_IRQ_CALC_AFFINITY_VECTORS_3_ARGS
	result = nvme_setup_irqs(dev, nr_io_queues);
	if (result <= 0)
		return -EIO;

	dev->num_vecs = result;
	result = max(result - 1 + dev->num_p2p_queues, 1u);
#ifdef HAVE_BLK_MQ_HCTX_TYPE
	dev->max_qid = result + dev->io_queues[HCTX_TYPE_POLL];
#else
	dev->max_qid = result;
#endif
#else
	nr_io_queues = pci_alloc_irq_vectors(pdev, 1, nr_io_queues - dev->num_p2p_queues,
				PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY);
	if (nr_io_queues <= 0)
		return -EIO;
	dev->max_qid = nr_io_queues + dev->num_p2p_queues;
#endif
#else
	if (pdev->msi_enabled)
		pci_disable_msi(pdev);
	else if (pdev->msix_enabled)
		pci_disable_msix(pdev);

	for (i = 0; i < nr_io_queues - dev->num_p2p_queues; i++)
		dev->entry[i].entry = i;
	vecs = pci_enable_msix_range(pdev, dev->entry, 1, nr_io_queues - dev->num_p2p_queues);
	if (vecs < 0) {
		vecs = pci_enable_msi_range(pdev, 1, min((nr_io_queues - dev->num_p2p_queues), 32u));
		if (vecs < 0) {
			vecs = 1;
		} else {
			for (i = 0; i < vecs; i++)
				dev->entry[i].vector = i + pdev->irq;
		}
	}
	nr_io_queues = vecs;
	dev->max_qid = nr_io_queues + dev->num_p2p_queues;
#endif /* HAVE_PCI_IRQ_API */

	/*
	 * Should investigate if there's a performance win from allocating
	 * more queues than interrupt vectors; it might allow the submission
	 * path to scale better, even if the receive path is limited by the
	 * number of interrupts.
	 */
	result = queue_request_irq(adminq);
	if (result)
		return result;
	set_bit(NVMEQ_ENABLED, &adminq->flags);

	result = nvme_create_io_queues(dev);
	if (result || dev->online_queues < 2)
		return result;

	if (dev->online_queues - 1 < dev->max_qid) {
		nr_io_queues = dev->online_queues - 1;
		nvme_disable_io_queues(dev);
		nvme_suspend_io_queues(dev);
		goto retry;
	}
#ifdef HAVE_BLK_MQ_HCTX_TYPE
	dev_info(dev->ctrl.device, "%d/%d/%d default/read/poll queues\n",
					dev->io_queues[HCTX_TYPE_DEFAULT],
					dev->io_queues[HCTX_TYPE_READ],
					dev->io_queues[HCTX_TYPE_POLL]);
#endif
	return 0;
}

static void nvme_del_queue_end(struct request *req, blk_status_t error)
{
	struct nvme_queue *nvmeq = req->end_io_data;

	blk_mq_free_request(req);
	complete(&nvmeq->delete_done);
}

static void nvme_del_cq_end(struct request *req, blk_status_t error)
{
	struct nvme_queue *nvmeq = req->end_io_data;

	if (error)
		set_bit(NVMEQ_DELETE_ERROR, &nvmeq->flags);

	nvme_del_queue_end(req, error);
}

static int nvme_delete_queue(struct nvme_queue *nvmeq, u8 opcode)
{
	struct request_queue *q = nvmeq->dev->ctrl.admin_q;
	struct request *req;
	struct nvme_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.delete_queue.opcode = opcode;
	cmd.delete_queue.qid = cpu_to_le16(nvmeq->qid);

#ifdef HAVE_BLK_MQ_ALLOC_REQUEST_HAS_3_PARAMS
	req = nvme_alloc_request(q, &cmd, BLK_MQ_REQ_NOWAIT, NVME_QID_ANY);
#else
	req = nvme_alloc_request(q, &cmd, GFP_KERNEL, false, NVME_QID_ANY);
#endif
	if (IS_ERR(req))
		return PTR_ERR(req);

	req->timeout = ADMIN_TIMEOUT;
	req->end_io_data = nvmeq;

	init_completion(&nvmeq->delete_done);
	blk_execute_rq_nowait(NULL, req, false,
			opcode == nvme_admin_delete_cq ?
				nvme_del_cq_end : nvme_del_queue_end);
	return 0;
}

static bool __nvme_disable_io_queues(struct nvme_dev *dev, u8 opcode)
{
	int nr_queues = dev->online_queues - 1, sent = 0;
	unsigned long timeout;

 retry:
	timeout = ADMIN_TIMEOUT;
	while (nr_queues > 0) {
		if (nvme_delete_queue(&dev->queues[nr_queues], opcode))
			break;
		nr_queues--;
		sent++;
	}
	while (sent) {
		struct nvme_queue *nvmeq = &dev->queues[nr_queues + sent];

		timeout = wait_for_completion_io_timeout(&nvmeq->delete_done,
				timeout);
		if (timeout == 0)
			return false;

		/* handle any remaining CQEs */
		if (opcode == nvme_admin_delete_cq &&
		    !test_bit(NVMEQ_DELETE_ERROR, &nvmeq->flags))
			nvme_poll_irqdisable(nvmeq, -1);

		sent--;
		if (nr_queues)
			goto retry;
	}
	return true;
}

/*
 * return error value only when tagset allocation failed
 */
static int nvme_dev_add(struct nvme_dev *dev)
{
	int ret;
	unsigned nr_hw_queues = dev->online_queues - 1 - dev->num_p2p_queues;

	if (!dev->ctrl.tagset) {
		dev->tagset.ops = &nvme_mq_ops;
		dev->tagset.nr_hw_queues = nr_hw_queues;
#ifdef HAVE_BLK_MQ_HCTX_TYPE
		dev->tagset.nr_maps = 2; /* default + read */
		if (dev->io_queues[HCTX_TYPE_POLL])
			dev->tagset.nr_maps++;
#endif
		dev->tagset.timeout = NVME_IO_TIMEOUT;
		dev->tagset.numa_node = dev_to_node(dev->dev);
		dev->tagset.queue_depth =
				min_t(int, dev->q_depth, BLK_MQ_MAX_DEPTH) - 1;
		dev->tagset.cmd_size = sizeof(struct nvme_iod);
		dev->tagset.flags = BLK_MQ_F_SHOULD_MERGE;
		dev->tagset.driver_data = dev;

		ret = blk_mq_alloc_tag_set(&dev->tagset);
		if (ret) {
			dev_warn(dev->ctrl.device,
				"IO queues tagset allocation failed %d\n", ret);
			return ret;
		}
		dev->ctrl.tagset = &dev->tagset;
	} else {
#ifdef HAVE_BLK_MQ_UPDATE_NR_HW_QUEUES
		blk_mq_update_nr_hw_queues(&dev->tagset, nr_hw_queues);
#endif

		/* Free previously allocated queues that are no longer usable */
		nvme_free_queues(dev, dev->online_queues);
	}

	nvme_dbbuf_set(dev);
	return 0;
}

static int nvme_pci_enable(struct nvme_dev *dev)
{
	int result = -ENOMEM;
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (pci_enable_device_mem(pdev))
		return result;

	pci_set_master(pdev);

	if (dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(64)) &&
	    dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(32)))
		goto disable;

	if (readl(dev->bar + NVME_REG_CSTS) == -1) {
		result = -ENODEV;
		goto disable;
	}

	/*
	 * Some devices and/or platforms don't advertise or work with INTx
	 * interrupts. Pre-enable a single MSIX or MSI vec for setup. We'll
	 * adjust this later.
	 */
#ifdef HAVE_PCI_IRQ_API
	result = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
	if (result < 0)
		return result;
#else
	if (pci_enable_msix(pdev, dev->entry, 1)) {
		pci_enable_msi(pdev);
		dev->entry[0].vector = pdev->irq;
	}

	if (!dev->entry[0].vector) {
		result = -ENODEV;
		goto disable;
	}
#endif

	dev->ctrl.cap = lo_hi_readq(dev->bar + NVME_REG_CAP);

	dev->q_depth = min_t(int, NVME_CAP_MQES(dev->ctrl.cap) + 1,
				io_queue_depth);
	dev->db_stride = 1 << NVME_CAP_STRIDE(dev->ctrl.cap);
	dev->dbs = dev->bar + 4096;

	/*
	 * Temporary fix for the Apple controller found in the MacBook8,1 and
	 * some MacBook7,1 to avoid controller resets and data loss.
	 */
	if (pdev->vendor == PCI_VENDOR_ID_APPLE && pdev->device == 0x2001) {
		dev->q_depth = 2;
		dev_warn(dev->ctrl.device, "detected Apple NVMe controller, "
			"set queue depth=%u to work around controller resets\n",
			dev->q_depth);
	} else if (pdev->vendor == PCI_VENDOR_ID_SAMSUNG &&
		   (pdev->device == 0xa821 || pdev->device == 0xa822) &&
		   NVME_CAP_MQES(dev->ctrl.cap) == 0) {
		dev->q_depth = 64;
		dev_err(dev->ctrl.device, "detected PM1725 NVMe controller, "
                        "set queue depth=%u\n", dev->q_depth);
	}

	nvme_map_cmb(dev);

	pci_enable_pcie_error_reporting(pdev);
	pci_save_state(pdev);
	return 0;

 disable:
	pci_disable_device(pdev);
	return result;
}

static void nvme_dev_unmap(struct nvme_dev *dev)
{
	if (dev->bar)
		iounmap(dev->bar);
	pci_release_mem_regions(to_pci_dev(dev->dev));
}

static void nvme_pci_disable(struct nvme_dev *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);

#ifdef HAVE_PCI_IRQ_API
	pci_free_irq_vectors(pdev);
#else
	if (pdev->msi_enabled)
		pci_disable_msi(pdev);
	else if (pdev->msix_enabled)
		pci_disable_msix(pdev);
#endif

	if (pci_is_enabled(pdev)) {
		pci_disable_pcie_error_reporting(pdev);
		pci_disable_device(pdev);
	}
}

static void nvme_dev_disable(struct nvme_dev *dev, bool shutdown)
{
	bool dead = true, freeze = false;
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	mutex_lock(&dev->shutdown_lock);
	if (pci_is_enabled(pdev)) {
		u32 csts = readl(dev->bar + NVME_REG_CSTS);

		if (dev->ctrl.state == NVME_CTRL_LIVE ||
		    dev->ctrl.state == NVME_CTRL_RESETTING) {
			freeze = true;
			nvme_start_freeze(&dev->ctrl);
		}
		dead = !!((csts & NVME_CSTS_CFS) || !(csts & NVME_CSTS_RDY) ||
			pdev->error_state  != pci_channel_io_normal);
	}

	/*
	 * Give the controller a chance to complete all entered requests if
	 * doing a safe shutdown.
	 */
	if (!dead && shutdown && freeze)
		nvme_wait_freeze_timeout(&dev->ctrl, NVME_IO_TIMEOUT);

	nvme_stop_queues(&dev->ctrl);

	if (!dead && dev->ctrl.queue_count > 0) {
		nvme_disable_io_queues(dev);
		nvme_disable_admin_queue(dev, shutdown);
	}
	nvme_suspend_io_queues(dev);
	nvme_suspend_queue(&dev->queues[0]);
	nvme_pci_disable(dev);

	blk_mq_tagset_busy_iter(&dev->tagset, nvme_cancel_request, &dev->ctrl);
	blk_mq_tagset_busy_iter(&dev->admin_tagset, nvme_cancel_request, &dev->ctrl);
	blk_mq_tagset_wait_completed_request(&dev->tagset);
	blk_mq_tagset_wait_completed_request(&dev->admin_tagset);

	/*
	 * The driver will not be starting up queues again if shutting down so
	 * must flush all entered requests to their failed completion to avoid
	 * deadlocking blk-mq hot-cpu notifier.
	 */
	if (shutdown) {
		nvme_start_queues(&dev->ctrl);
		if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q))
#ifdef HAVE_BLK_MQ_UNQUIESCE_QUEUE
			blk_mq_unquiesce_queue(dev->ctrl.admin_q);
#else
			blk_mq_start_stopped_hw_queues(dev->ctrl.admin_q, true);
#endif
	}
	mutex_unlock(&dev->shutdown_lock);
}

static int nvme_setup_prp_pools(struct nvme_dev *dev)
{
	dev->prp_page_pool = dma_pool_create("prp list page", dev->dev,
						PAGE_SIZE, PAGE_SIZE, 0);
	if (!dev->prp_page_pool)
		return -ENOMEM;

	/* Optimisation for I/Os between 4k and 128k */
	dev->prp_small_pool = dma_pool_create("prp list 256", dev->dev,
						256, 256, 0);
	if (!dev->prp_small_pool) {
		dma_pool_destroy(dev->prp_page_pool);
		return -ENOMEM;
	}
	return 0;
}

static void nvme_release_prp_pools(struct nvme_dev *dev)
{
	dma_pool_destroy(dev->prp_page_pool);
	dma_pool_destroy(dev->prp_small_pool);
}

static void nvme_pci_free_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);

	nvme_dbbuf_dma_free(dev);
	put_device(dev->dev);
	if (dev->tagset.tags)
		blk_mq_free_tag_set(&dev->tagset);
	if (dev->ctrl.admin_q)
		blk_put_queue(dev->ctrl.admin_q);
	kfree(dev->queues);
#ifdef HAVE_LINUX_SED_OPAL_H
	free_opal_dev(dev->ctrl.opal_dev);
#endif
#ifndef HAVE_PCI_IRQ_API
	kfree(dev->entry);
#endif
	mempool_destroy(dev->iod_mempool);
	kfree(dev);
}

static void nvme_remove_dead_ctrl(struct nvme_dev *dev, int status)
{
	dev_warn(dev->ctrl.device, "Removing after probe failure status: %d\n", status);

	nvme_get_ctrl(&dev->ctrl);
	nvme_dev_disable(dev, false);
	nvme_kill_queues(&dev->ctrl);
	if (!queue_work(nvme_wq, &dev->remove_work))
		nvme_put_ctrl(&dev->ctrl);
}

static void nvme_reset_work(struct work_struct *work)
{
	struct nvme_dev *dev =
		container_of(work, struct nvme_dev, ctrl.reset_work);
#ifdef HAVE_LINUX_SED_OPAL_H
	bool was_suspend = !!(dev->ctrl.ctrl_config & NVME_CC_SHN_NORMAL);
#endif
	int result = -ENODEV;
	enum nvme_ctrl_state new_state = NVME_CTRL_LIVE;

	if (WARN_ON(dev->ctrl.state != NVME_CTRL_RESETTING))
		goto out;

	/*
	 * If we're called to reset a live controller first shut it down before
	 * moving on.
	 */
	if (dev->ctrl.ctrl_config & NVME_CC_ENABLE)
		nvme_dev_disable(dev, false);
	nvme_sync_queues(&dev->ctrl);

	mutex_lock(&dev->shutdown_lock);
	result = nvme_pci_enable(dev);
	if (result)
		goto out_unlock;

	result = nvme_pci_configure_admin_queue(dev);
	if (result)
		goto out_unlock;

	result = nvme_alloc_admin_tags(dev);
	if (result)
		goto out_unlock;

	/*
	 * Limit the max command size to prevent iod->sg allocations going
	 * over a single page.
	 */
	dev->ctrl.max_hw_sectors = NVME_MAX_KB_SZ << 1;
	dev->ctrl.max_segments = NVME_MAX_SEGS;

	/*
	 * Don't limit the IOMMU merged segment size.
	 */
	dma_set_max_seg_size(dev->dev, 0xffffffff);

	mutex_unlock(&dev->shutdown_lock);

	/*
	 * Introduce CONNECTING state from nvme-fc/rdma transports to mark the
	 * initializing procedure here.
	 */
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_CONNECTING)) {
		dev_warn(dev->ctrl.device,
			"failed to mark controller CONNECTING\n");
		result = -EBUSY;
		goto out;
	}

	result = nvme_init_identify(&dev->ctrl);
	if (result)
		goto out;

#ifdef HAVE_LINUX_SED_OPAL_H
	if (dev->ctrl.oacs & NVME_CTRL_OACS_SEC_SUPP) {
		if (!dev->ctrl.opal_dev)
			dev->ctrl.opal_dev =
				init_opal_dev(&dev->ctrl, &nvme_sec_submit);
		else if (was_suspend)
			opal_unlock_from_suspend(dev->ctrl.opal_dev);
	} else {
		free_opal_dev(dev->ctrl.opal_dev);
		dev->ctrl.opal_dev = NULL;
	}
#endif

	if (dev->ctrl.oacs & NVME_CTRL_OACS_DBBUF_SUPP) {
		result = nvme_dbbuf_dma_alloc(dev);
		if (result)
			dev_warn(dev->dev,
				 "unable to allocate dma for dbbuf\n");
	}

	if (dev->ctrl.hmpre) {
		result = nvme_setup_host_mem(dev);
		if (result < 0)
			goto out;
	}

	result = nvme_setup_io_queues(dev);
	if (result)
		goto out;

	/*
	 * Keep the controller around but remove all namespaces if we don't have
	 * any working I/O queue.
	 */
	if (dev->online_queues < 2) {
		dev_warn(dev->ctrl.device, "IO queues not created\n");
		nvme_kill_queues(&dev->ctrl);
		nvme_remove_namespaces(&dev->ctrl);
		new_state = NVME_CTRL_ADMIN_ONLY;
	} else {
		nvme_start_queues(&dev->ctrl);
		nvme_wait_freeze(&dev->ctrl);
		/* hit this only when allocate tagset fails */
		if (nvme_dev_add(dev))
			new_state = NVME_CTRL_ADMIN_ONLY;
		nvme_unfreeze(&dev->ctrl);
	}

	/*
	 * If only admin queue live, keep it to do further investigation or
	 * recovery.
	 */
	if (!nvme_change_ctrl_state(&dev->ctrl, new_state)) {
		dev_warn(dev->ctrl.device,
			"failed to mark controller state %d\n", new_state);
		goto out;
	}

	nvme_start_ctrl(&dev->ctrl);
	return;

 out_unlock:
	mutex_unlock(&dev->shutdown_lock);
 out:
	nvme_remove_dead_ctrl(dev, result);
}

static void nvme_remove_dead_ctrl_work(struct work_struct *work)
{
	struct nvme_dev *dev = container_of(work, struct nvme_dev, remove_work);
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (pci_get_drvdata(pdev))
		device_release_driver(&pdev->dev);
	nvme_put_ctrl(&dev->ctrl);
}

static int nvme_pci_reg_read32(struct nvme_ctrl *ctrl, u32 off, u32 *val)
{
	*val = readl(to_nvme_dev(ctrl)->bar + off);
	return 0;
}

static int nvme_pci_reg_write32(struct nvme_ctrl *ctrl, u32 off, u32 val)
{
	writel(val, to_nvme_dev(ctrl)->bar + off);
	return 0;
}

static int nvme_pci_reg_read64(struct nvme_ctrl *ctrl, u32 off, u64 *val)
{
	*val = readq(to_nvme_dev(ctrl)->bar + off);
	return 0;
}

static int nvme_pci_get_address(struct nvme_ctrl *ctrl, char *buf, int size)
{
	struct pci_dev *pdev = to_pci_dev(to_nvme_dev(ctrl)->dev);

	return snprintf(buf, size, "%s", dev_name(&pdev->dev));
}

static const struct nvme_ctrl_ops nvme_pci_ctrl_ops = {
	.name			= "pcie",
	.module			= THIS_MODULE,
	.flags			= NVME_F_METADATA_SUPPORTED |
				  NVME_F_PCI_P2PDMA,
	.reg_read32		= nvme_pci_reg_read32,
	.reg_write32		= nvme_pci_reg_write32,
	.reg_read64		= nvme_pci_reg_read64,
	.free_ctrl		= nvme_pci_free_ctrl,
	.submit_async_event	= nvme_pci_submit_async_event,
	.get_address		= nvme_pci_get_address,
};

struct pci_dev *nvme_find_pdev_from_bdev(struct block_device *bdev)
{
	struct nvme_ns *ns = disk_to_nvme_ns(bdev->bd_disk);

	if (!ns || ns->ctrl->ops != &nvme_pci_ctrl_ops)
		return NULL;

	return to_pci_dev(to_nvme_dev(ns->ctrl)->dev);
}
EXPORT_SYMBOL_GPL(nvme_find_pdev_from_bdev);

unsigned nvme_find_ns_id_from_bdev(struct block_device *bdev)
{
	struct nvme_ns *ns = disk_to_nvme_ns(bdev->bd_disk);

	if (!ns)
		return 0;

	return ns->head->ns_id;
}
EXPORT_SYMBOL_GPL(nvme_find_ns_id_from_bdev);

static int nvme_dev_map(struct nvme_dev *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (pci_request_mem_regions(pdev, "nvme"))
		return -ENODEV;

	if (nvme_remap_bar(dev, NVME_REG_DBS + 4096))
		goto release;

	return 0;
  release:
	pci_release_mem_regions(pdev);
	return -ENODEV;
}

static unsigned long check_vendor_combination_bug(struct pci_dev *pdev)
{
	if (pdev->vendor == 0x144d && pdev->device == 0xa802) {
		/*
		 * Several Samsung devices seem to drop off the PCIe bus
		 * randomly when APST is on and uses the deepest sleep state.
		 * This has been observed on a Samsung "SM951 NVMe SAMSUNG
		 * 256GB", a "PM951 NVMe SAMSUNG 512GB", and a "Samsung SSD
		 * 950 PRO 256GB", but it seems to be restricted to two Dell
		 * laptops.
		 */
		if (dmi_match(DMI_SYS_VENDOR, "Dell Inc.") &&
		    (dmi_match(DMI_PRODUCT_NAME, "XPS 15 9550") ||
		     dmi_match(DMI_PRODUCT_NAME, "Precision 5510")))
			return NVME_QUIRK_NO_DEEPEST_PS;
	} else if (pdev->vendor == 0x144d && pdev->device == 0xa804) {
		/*
		 * Samsung SSD 960 EVO drops off the PCIe bus after system
		 * suspend on a Ryzen board, ASUS PRIME B350M-A, as well as
		 * within few minutes after bootup on a Coffee Lake board -
		 * ASUS PRIME Z370-A
		 */
		if (dmi_match(DMI_BOARD_VENDOR, "ASUSTeK COMPUTER INC.") &&
		    (dmi_match(DMI_BOARD_NAME, "PRIME B350M-A") ||
		     dmi_match(DMI_BOARD_NAME, "PRIME Z370-A")))
			return NVME_QUIRK_NO_APST;
	}

	return 0;
}

static void nvme_async_probe(void *data, async_cookie_t cookie)
{
	struct nvme_dev *dev = data;

	flush_work(&dev->ctrl.reset_work);
	flush_work(&dev->ctrl.scan_work);
	nvme_put_ctrl(&dev->ctrl);
}

static int nvme_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int node, result = -ENOMEM;
	struct nvme_dev *dev;
	unsigned long quirks = id->driver_data;
	size_t alloc_size;

	node = dev_to_node(&pdev->dev);
	if (node == NUMA_NO_NODE)
		set_dev_node(&pdev->dev, first_memory_node);

	dev = kzalloc_node(sizeof(*dev), GFP_KERNEL, node);
	if (!dev)
		return -ENOMEM;

#ifndef HAVE_PCI_IRQ_API
	dev->entry = kzalloc_node(num_possible_cpus() * sizeof(*dev->entry),
				  GFP_KERNEL, node);
	if (!dev->entry)
		goto free;
#endif

	dev->queues = kcalloc_node(max_queue_count() + num_p2p_queues, sizeof(struct nvme_queue),
					GFP_KERNEL, node);
	if (!dev->queues)
		goto free;

	dev->num_p2p_queues = num_p2p_queues;
	dev->dev = get_device(&pdev->dev);
	pci_set_drvdata(pdev, dev);

	result = nvme_dev_map(dev);
	if (result)
		goto put_pci;

	INIT_WORK(&dev->ctrl.reset_work, nvme_reset_work);
	INIT_WORK(&dev->remove_work, nvme_remove_dead_ctrl_work);
	mutex_init(&dev->shutdown_lock);

	result = nvme_setup_prp_pools(dev);
	if (result)
		goto unmap;

	quirks |= check_vendor_combination_bug(pdev);

	/*
	 * Double check that our mempool alloc size will cover the biggest
	 * command we support.
	 */
	alloc_size = nvme_pci_iod_alloc_size(dev, NVME_MAX_KB_SZ,
						NVME_MAX_SEGS, true);
	WARN_ON_ONCE(alloc_size > PAGE_SIZE);

	dev->iod_mempool = mempool_create_node(1, mempool_kmalloc,
						mempool_kfree,
						(void *) alloc_size,
						GFP_KERNEL, node);
	if (!dev->iod_mempool) {
		result = -ENOMEM;
		goto release_pools;
	}

	result = nvme_init_ctrl(&dev->ctrl, &pdev->dev, &nvme_pci_ctrl_ops,
			quirks);
	if (result)
		goto release_mempool;

	/*
	 * We populate sysfs to know how many p2p queues can be totaly created.
	 * Note that we add the P2P attribute to the nvme_ctrl kobj which removes
	 * the need to remove it on exit. Since nvme_dev_attrs_group has no name we can pass
	 * NULL as final argument to sysfs_add_file_to_group.
	 */
	if (sysfs_add_file_to_group(&dev->ctrl.device->kobj,
				    &dev_attr_num_p2p_queues.attr, NULL))
		dev_warn(dev->ctrl.device,
			 "failed to add sysfs attribute for num P2P queues\n");

	dev_info(dev->ctrl.device, "pci function %s\n", dev_name(&pdev->dev));

	nvme_reset_ctrl(&dev->ctrl);
	nvme_get_ctrl(&dev->ctrl);
	async_schedule(nvme_async_probe, dev);

	return 0;

 release_mempool:
	mempool_destroy(dev->iod_mempool);
 release_pools:
	nvme_release_prp_pools(dev);
 unmap:
	nvme_dev_unmap(dev);
 put_pci:
	put_device(dev->dev);
 free:
	kfree(dev->queues);
#ifndef HAVE_PCI_IRQ_API
	kfree(dev->entry);
#endif
	kfree(dev);
	return result;
}

#ifdef HAVE_PCI_ERROR_HANDLERS_RESET_NOTIFY
static void nvme_reset_notify(struct pci_dev *pdev, bool prepare)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	if (prepare)
		nvme_dev_disable(dev, false);
	else
		nvme_reset_ctrl(&dev->ctrl);
}
#elif defined(HAVE_PCI_ERROR_HANDLERS_RESET_PREPARE) && defined(HAVE_PCI_ERROR_HANDLERS_RESET_DONE)
static void nvme_reset_prepare(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);
	nvme_dev_disable(dev, false);
}

static void nvme_reset_done(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);
	nvme_reset_ctrl_sync(&dev->ctrl);
}
#endif

static void nvme_shutdown(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);
	nvme_dev_disable(dev, true);
}

/*
 * The driver's remove may be called on a device in a partially initialized
 * state. This function must not have any dependencies on the device state in
 * order to proceed.
 */
static void nvme_remove(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);
	pci_set_drvdata(pdev, NULL);

	if (!pci_device_is_present(pdev)) {
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DEAD);
		nvme_dev_disable(dev, true);
		nvme_dev_remove_admin(dev);
	}

	flush_work(&dev->ctrl.reset_work);
	nvme_stop_ctrl(&dev->ctrl);
	nvme_remove_namespaces(&dev->ctrl);
	nvme_dev_disable(dev, true);
	nvme_release_cmb(dev);
	nvme_free_host_mem(dev);
	nvme_dev_remove_admin(dev);
	nvme_free_queues(dev, 0);
	nvme_uninit_ctrl(&dev->ctrl);
	nvme_release_prp_pools(dev);
	nvme_dev_unmap(dev);
	nvme_put_ctrl(&dev->ctrl);
}

#ifdef CONFIG_PM_SLEEP
static int nvme_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct nvme_dev *ndev = pci_get_drvdata(pdev);

	nvme_dev_disable(ndev, true);
	return 0;
}

static int nvme_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct nvme_dev *ndev = pci_get_drvdata(pdev);

	nvme_reset_ctrl(&ndev->ctrl);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(nvme_dev_pm_ops, nvme_suspend, nvme_resume);

static pci_ers_result_t nvme_error_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	/*
	 * A frozen channel requires a reset. When detected, this method will
	 * shutdown the controller to quiesce. The controller will be restarted
	 * after the slot reset through driver's slot_reset callback.
	 */
	switch (state) {
	case pci_channel_io_normal:
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		dev_warn(dev->ctrl.device,
			"frozen state error detected, reset controller\n");
		nvme_dev_disable(dev, false);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		dev_warn(dev->ctrl.device,
			"failure state error detected, request disconnect\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t nvme_slot_reset(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	dev_info(dev->ctrl.device, "restart after slot reset\n");
	pci_restore_state(pdev);
	nvme_reset_ctrl(&dev->ctrl);
	return PCI_ERS_RESULT_RECOVERED;
}

static void nvme_error_resume(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	flush_work(&dev->ctrl.reset_work);
}

static const struct pci_error_handlers nvme_err_handler = {
	.error_detected	= nvme_error_detected,
	.slot_reset	= nvme_slot_reset,
	.resume		= nvme_error_resume,
#ifdef HAVE_PCI_ERROR_HANDLERS_RESET_NOTIFY
	.reset_notify   = nvme_reset_notify,
#elif defined(HAVE_PCI_ERROR_HANDLERS_RESET_PREPARE) && defined(HAVE_PCI_ERROR_HANDLERS_RESET_DONE)
	.reset_prepare  = nvme_reset_prepare,
	.reset_done     = nvme_reset_done,
#endif /* HAVE_PCI_ERROR_HANDLERS_RESET_NOTIFY */
};

#ifndef HAVE_PCI_CLASS_STORAGE_EXPRESS
#define PCI_CLASS_STORAGE_EXPRESS      0x010802
#endif

static const struct pci_device_id nvme_id_table[] = {
#ifdef HAVE_BLK_QUEUE_MAX_WRITE_ZEROES_SECTORS
	{ PCI_VDEVICE(INTEL, 0x0953),
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DEALLOCATE_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0x0a53),
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DEALLOCATE_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0x0a54),
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DEALLOCATE_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0x0a55),
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DEALLOCATE_ZEROES, },
#else
	{ PCI_VDEVICE(INTEL, 0x0953),
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DISCARD_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0x0a53),
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DISCARD_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0x0a54),
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DISCARD_ZEROES, },
#endif
	{ PCI_VDEVICE(INTEL, 0xf1a5),	/* Intel 600P/P3100 */
		.driver_data = NVME_QUIRK_NO_DEEPEST_PS |
				NVME_QUIRK_MEDIUM_PRIO_SQ },
	{ PCI_VDEVICE(INTEL, 0xf1a6),	/* Intel 760p/Pro 7600p */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN, },
	{ PCI_VDEVICE(INTEL, 0x5845),	/* Qemu emulated controller */
		.driver_data = NVME_QUIRK_IDENTIFY_CNS |
				NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x1bb1, 0x0100),   /* Seagate Nytro Flash Storage */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x1c58, 0x0003),	/* HGST adapter */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x1c58, 0x0023),	/* WDC SN200 adapter */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x1c5f, 0x0540),	/* Memblaze Pblaze4 adapter */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x144d, 0xa821),   /* Samsung PM1725 */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x144d, 0xa822),   /* Samsung PM1725a */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x1d1d, 0x1f1f),	/* LighNVM qemu device */
		.driver_data = NVME_QUIRK_LIGHTNVM, },
	{ PCI_DEVICE(0x1d1d, 0x2807),	/* CNEX WL */
		.driver_data = NVME_QUIRK_LIGHTNVM, },
	{ PCI_DEVICE(0x1d1d, 0x2601),	/* CNEX Granby */
		.driver_data = NVME_QUIRK_LIGHTNVM, },
	{ PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2001) },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2003) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, nvme_id_table);

#ifndef PCI_SRIOV_CONFIGURE_SIMPLE
static int nvme_pci_sriov_configure(struct pci_dev *pdev, int numvfs)
{
	int ret = 0;

	if (numvfs == 0) {
		if (pci_vfs_assigned(pdev)) {
			dev_warn(&pdev->dev,
				 "Cannot disable SR-IOV VFs while assigned\n");
			return -EPERM;
		}
		pci_disable_sriov(pdev);
		return 0;
	}

	ret = pci_enable_sriov(pdev, numvfs);
	return ret ? ret : numvfs;
}
#endif

static struct pci_driver nvme_driver = {
	.name		= "nvme",
	.id_table	= nvme_id_table,
	.probe		= nvme_probe,
	.remove		= nvme_remove,
	.shutdown	= nvme_shutdown,
	.driver		= {
		.pm	= &nvme_dev_pm_ops,
	},
#ifdef PCI_SRIOV_CONFIGURE_SIMPLE
	.sriov_configure = pci_sriov_configure_simple,
#else
	.sriov_configure = nvme_pci_sriov_configure,
#endif
	.err_handler	= &nvme_err_handler,
};

static int __init nvme_init(void)
{
	BUILD_BUG_ON(sizeof(struct nvme_create_cq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_create_sq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_delete_queue) != 64);
#ifdef HAVE_IRQ_AFFINITY_PRIV
	BUILD_BUG_ON(IRQ_AFFINITY_MAX_SETS < 2);
#endif
	return pci_register_driver(&nvme_driver);
}

static void __exit nvme_exit(void)
{
	pci_unregister_driver(&nvme_driver);
	flush_workqueue(nvme_wq);
}

MODULE_AUTHOR("Matthew Wilcox <willy@linux.intel.com>");
MODULE_LICENSE("GPL");
#ifdef RETPOLINE_MLNX
MODULE_INFO(retpoline, "Y");
#endif
MODULE_VERSION("1.0");
module_init(nvme_init);
module_exit(nvme_exit);
