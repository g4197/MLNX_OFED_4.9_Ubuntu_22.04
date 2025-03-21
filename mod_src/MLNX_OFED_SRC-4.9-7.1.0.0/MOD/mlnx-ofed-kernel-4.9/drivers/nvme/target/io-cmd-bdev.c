// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe I/O command implementation.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */
#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/blkdev.h>
#include <linux/module.h>
#include "nvmet.h"

int nvmet_bdev_ns_enable(struct nvmet_ns *ns)
{
	int ret;

	ns->bdev = blkdev_get_by_path(ns->device_path,
			FMODE_READ | FMODE_WRITE, NULL);
	if (IS_ERR(ns->bdev)) {
		ret = PTR_ERR(ns->bdev);
		if (ret != -ENOTBLK) {
			pr_err("failed to open block device %s: (%ld)\n",
					ns->device_path, PTR_ERR(ns->bdev));
		}
		ns->bdev = NULL;
		return ret;
	}
	ns->size = i_size_read(ns->bdev->bd_inode);
	ns->blksize_shift = blksize_bits(bdev_logical_block_size(ns->bdev));
	return 0;
}

void nvmet_bdev_ns_disable(struct nvmet_ns *ns)
{
	if (ns->bdev) {
		blkdev_put(ns->bdev, FMODE_WRITE | FMODE_READ);
		ns->bdev = NULL;
	}
}

#ifdef HAVE_BLK_STATUS_T
static u16 blk_to_nvme_status(struct nvmet_req *req, blk_status_t blk_sts)
{
	u16 status = NVME_SC_SUCCESS;

	if (likely(blk_sts == BLK_STS_OK))
		return status;
	/*
	 * Right now there exists M : 1 mapping between block layer error
	 * to the NVMe status code (see nvme_error_status()). For consistency,
	 * when we reverse map we use most appropriate NVMe Status code from
	 * the group of the NVMe staus codes used in the nvme_error_status().
	 */
	switch (blk_sts) {
	case BLK_STS_NOSPC:
		status = NVME_SC_CAP_EXCEEDED | NVME_SC_DNR;
		req->error_loc = offsetof(struct nvme_rw_command, length);
		break;
	case BLK_STS_TARGET:
		status = NVME_SC_LBA_RANGE | NVME_SC_DNR;
		req->error_loc = offsetof(struct nvme_rw_command, slba);
		break;
	case BLK_STS_NOTSUPP:
		req->error_loc = offsetof(struct nvme_common_command, opcode);
		switch (req->cmd->common.opcode) {
		case nvme_cmd_dsm:
		case nvme_cmd_write_zeroes:
			status = NVME_SC_ONCS_NOT_SUPPORTED | NVME_SC_DNR;
			break;
		default:
			status = NVME_SC_INVALID_OPCODE | NVME_SC_DNR;
		}
		break;
	case BLK_STS_MEDIUM:
		status = NVME_SC_ACCESS_DENIED;
		req->error_loc = offsetof(struct nvme_rw_command, nsid);
		break;
	case BLK_STS_IOERR:
		/* fallthru */
	default:
		status = NVME_SC_INTERNAL | NVME_SC_DNR;
		req->error_loc = offsetof(struct nvme_common_command, opcode);
	}

	switch (req->cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
		req->error_slba = le64_to_cpu(req->cmd->rw.slba);
		break;
	case nvme_cmd_write_zeroes:
		req->error_slba =
			le64_to_cpu(req->cmd->write_zeroes.slba);
		break;
	default:
		req->error_slba = 0;
	}
	return status;
}
#endif

#ifdef HAVE_BIO_ENDIO_1_PARAM
static void nvmet_bio_done(struct bio *bio)
#else
static void nvmet_bio_done(struct bio *bio, int error)
#endif
{
	struct nvmet_req *req = bio->bi_private;

#ifdef HAVE_BLK_STATUS_T
	nvmet_req_complete(req, blk_to_nvme_status(req, bio->bi_status));
#elif defined(HAVE_STRUCT_BIO_BI_ERROR)
	nvmet_req_complete(req, bio->bi_error ? NVME_SC_INTERNAL | NVME_SC_DNR : 0);
#else
	nvmet_req_complete(req, error ? NVME_SC_INTERNAL | NVME_SC_DNR : 0);
#endif
	if (bio != &req->b.inline_bio)
		bio_put(bio);
}

static void nvmet_bdev_execute_rw(struct nvmet_req *req)
{
	int sg_cnt = req->sg_cnt;
	struct bio *bio;
	struct scatterlist *sg;
	sector_t sector;
	int op, op_flags = 0, i;

	if (!req->sg_cnt) {
		nvmet_req_complete(req, 0);
		return;
	}

	if (req->cmd->rw.opcode == nvme_cmd_write) {
		op = REQ_OP_WRITE;
#ifdef HAVE_REQ_IDLE
		op_flags = REQ_SYNC | REQ_IDLE;
#else
		op_flags = WRITE_ODIRECT;
#endif
		if (req->cmd->rw.control & cpu_to_le16(NVME_RW_FUA))
			op_flags |= REQ_FUA;
	} else {
		op = REQ_OP_READ;
	}

	if (is_pci_p2pdma_page(sg_page(req->sg)))
		op_flags |= REQ_NOMERGE;

	sector = le64_to_cpu(req->cmd->rw.slba);
	sector <<= (req->ns->blksize_shift - 9);

	if (req->data_len <= NVMET_MAX_INLINE_DATA_LEN) {
		bio = &req->b.inline_bio;
#ifdef HAVE_BIO_INIT_3_PARAMS
		bio_init(bio, req->inline_bvec, ARRAY_SIZE(req->inline_bvec));
#else
		bio_init(bio);
		bio->bi_io_vec = req->inline_bvec;
		bio->bi_max_vecs = ARRAY_SIZE(req->inline_bvec);
#endif
	} else {
		bio = bio_alloc(GFP_KERNEL, min(sg_cnt, BIO_MAX_PAGES));
	}
#ifdef HAVE_BIO_BI_DISK
	bio_set_dev(bio, req->ns->bdev);
#else
	bio->bi_bdev = req->ns->bdev;
#endif
#ifdef HAVE_STRUCT_BIO_BI_ITER
	bio->bi_iter.bi_sector = sector;
#else
	bio->bi_sector = sector;
#endif
	bio->bi_private = req;
	bio->bi_end_io = nvmet_bio_done;
	bio_set_op_attrs(bio, op, op_flags);

#ifdef HAVE_RH7_STRUCT_BIO_AUX
	bio_init_aux(bio, &req->bio_aux);
#endif

	for_each_sg(req->sg, sg, req->sg_cnt, i) {
		while (bio_add_page(bio, sg_page(sg), sg->length, sg->offset)
				!= sg->length) {
			struct bio *prev = bio;

			bio = bio_alloc(GFP_KERNEL, min(sg_cnt, BIO_MAX_PAGES));
#ifdef HAVE_BIO_BI_DISK
			bio_set_dev(bio, req->ns->bdev);
#else
			bio->bi_bdev = req->ns->bdev;
#endif
#ifdef HAVE_STRUCT_BIO_BI_ITER
			bio->bi_iter.bi_sector = sector;
#else
			bio->bi_sector = sector;
#endif
			bio_set_op_attrs(bio, op, op_flags);

			bio_chain(bio, prev);
#ifdef HAVE_SUBMIT_BIO_1_PARAM
			submit_bio(prev);
#else
			submit_bio(bio_data_dir(prev), prev);
#endif
		}

		sector += sg->length >> 9;
		sg_cnt--;
	}

#ifdef HAVE_SUBMIT_BIO_1_PARAM
	submit_bio(bio);
#else
	submit_bio(bio_data_dir(bio), bio);
#endif
}

static void nvmet_bdev_execute_flush(struct nvmet_req *req)
{
	struct bio *bio = &req->b.inline_bio;

#ifdef HAVE_BIO_INIT_3_PARAMS
	bio_init(bio, req->inline_bvec, ARRAY_SIZE(req->inline_bvec));
#else
	bio_init(bio);
	bio->bi_io_vec = req->inline_bvec;
	bio->bi_max_vecs = ARRAY_SIZE(req->inline_bvec);
#endif
#ifdef HAVE_BIO_BI_DISK
	bio_set_dev(bio, req->ns->bdev);
#else
	bio->bi_bdev = req->ns->bdev;
#endif
	bio->bi_private = req;
	bio->bi_end_io = nvmet_bio_done;
#ifdef HAVE_STRUCT_BIO_BI_OPF
	bio->bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
#else
	bio_set_op_attrs(bio, REQ_OP_WRITE, WRITE_FLUSH);
#endif

#ifdef HAVE_SUBMIT_BIO_1_PARAM
	submit_bio(bio);
#else
	submit_bio(bio_data_dir(bio), bio);
#endif
}

u16 nvmet_bdev_flush(struct nvmet_req *req)
{
#ifdef HAVE_BLKDEV_ISSUE_FLUSH_1_PARAM
	if (blkdev_issue_flush(req->ns->bdev))
#else
#ifdef HAVE_BLKDEV_ISSUE_FLUSH_2_PARAM
	if (blkdev_issue_flush(req->ns->bdev, GFP_KERNEL))
#else
	if (blkdev_issue_flush(req->ns->bdev, GFP_KERNEL, NULL))
#endif
#endif
		return NVME_SC_INTERNAL | NVME_SC_DNR;
	return 0;
}

static u16 nvmet_bdev_discard_range(struct nvmet_req *req,
		struct nvme_dsm_range *range, struct bio **bio)
{
	struct nvmet_ns *ns = req->ns;
	int ret;

#ifdef HAVE___BLKDEV_ISSUE_DISCARD
	ret = __blkdev_issue_discard(ns->bdev,
			le64_to_cpu(range->slba) << (ns->blksize_shift - 9),
			le32_to_cpu(range->nlb) << (ns->blksize_shift - 9),
			GFP_KERNEL, 0, bio);
#else
	ret = blkdev_issue_discard(ns->bdev,
			le64_to_cpu(range->slba) << (ns->blksize_shift - 9),
			le32_to_cpu(range->nlb) << (ns->blksize_shift - 9),
			GFP_KERNEL, 0);
#endif
	if (ret && ret != -EOPNOTSUPP) {
		req->error_slba = le64_to_cpu(range->slba);
		return errno_to_nvme_status(req, ret);
	}
	return NVME_SC_SUCCESS;
}

static void nvmet_bdev_execute_discard(struct nvmet_req *req)
{
	struct nvme_dsm_range range;
	struct bio *bio = NULL;
	int i;
	u16 status;

	for (i = 0; i <= le32_to_cpu(req->cmd->dsm.nr); i++) {
		status = nvmet_copy_from_sgl(req, i * sizeof(range), &range,
				sizeof(range));
		if (status)
			break;

		status = nvmet_bdev_discard_range(req, &range, &bio);
		if (status)
			break;
	}

	if (bio) {
		bio->bi_private = req;
		bio->bi_end_io = nvmet_bio_done;
		if (status) {
#ifdef HAVE_BLK_STATUS_T
			bio->bi_status = BLK_STS_IOERR;
#elif defined(HAVE_STRUCT_BIO_BI_ERROR)
			bio->bi_error = -EIO;
#endif
#ifdef HAVE_BIO_ENDIO_1_PARAM
			bio_endio(bio);
#else
			bio_endio(bio, -EIO);
#endif
		} else {
#ifdef HAVE_SUBMIT_BIO_1_PARAM
			submit_bio(bio);
#else
			submit_bio(bio_data_dir(bio), bio);
#endif
		}
	} else {
		nvmet_req_complete(req, status);
	}
}

static void nvmet_bdev_execute_dsm(struct nvmet_req *req)
{
	switch (le32_to_cpu(req->cmd->dsm.attributes)) {
	case NVME_DSMGMT_AD:
		nvmet_bdev_execute_discard(req);
		return;
	case NVME_DSMGMT_IDR:
	case NVME_DSMGMT_IDW:
	default:
		/* Not supported yet */
		nvmet_req_complete(req, 0);
		return;
	}
}

#ifdef HAVE_BLKDEV_ISSUE_ZEROOUT
static void nvmet_bdev_execute_write_zeroes(struct nvmet_req *req)
{
	struct nvme_write_zeroes_cmd *write_zeroes = &req->cmd->write_zeroes;
	struct bio *bio = NULL;
	sector_t sector;
	sector_t nr_sector;
	int ret;

	sector = le64_to_cpu(write_zeroes->slba) <<
		(req->ns->blksize_shift - 9);
	nr_sector = (((sector_t)le16_to_cpu(write_zeroes->length) + 1) <<
		(req->ns->blksize_shift - 9));

#ifdef CONFIG_COMPAT_IS_BLKDEV_ISSUE_ZEROOUT_HAS_FLAGS
	ret = __blkdev_issue_zeroout(req->ns->bdev, sector, nr_sector,
			GFP_KERNEL, &bio, 0);
#else
	if (__blkdev_issue_zeroout(req->ns->bdev, sector, nr_sector,
			GFP_KERNEL, &bio, true))
		ret = -EIO;
	else
		ret = 0;
#endif
	if (bio) {
		bio->bi_private = req;
		bio->bi_end_io = nvmet_bio_done;
#ifdef HAVE_SUBMIT_BIO_1_PARAM
		submit_bio(bio);
#else
		submit_bio(bio_data_dir(bio), bio);
#endif
	} else {
		nvmet_req_complete(req, errno_to_nvme_status(req, ret));
	}
}
#endif

u16 nvmet_bdev_parse_io_cmd(struct nvmet_req *req)
{
	struct nvme_command *cmd = req->cmd;

	switch (cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
		req->execute = nvmet_bdev_execute_rw;
		req->data_len = nvmet_rw_len(req);
		return 0;
	case nvme_cmd_flush:
		req->execute = nvmet_bdev_execute_flush;
		req->data_len = 0;
		return 0;
	case nvme_cmd_dsm:
		req->execute = nvmet_bdev_execute_dsm;
		req->data_len = (le32_to_cpu(cmd->dsm.nr) + 1) *
			sizeof(struct nvme_dsm_range);
		return 0;
#ifdef HAVE_BLKDEV_ISSUE_ZEROOUT
	case nvme_cmd_write_zeroes:
		req->execute = nvmet_bdev_execute_write_zeroes;
		req->data_len = 0;
		return 0;
#endif
	default:
		pr_err("unhandled cmd %d on qid %d\n", cmd->common.opcode,
		       req->sq->qid);
		req->error_loc = offsetof(struct nvme_common_command, opcode);
		return NVME_SC_INVALID_OPCODE | NVME_SC_DNR;
	}
}
