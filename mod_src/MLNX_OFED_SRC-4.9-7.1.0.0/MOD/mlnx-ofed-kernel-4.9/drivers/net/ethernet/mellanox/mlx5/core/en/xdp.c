/*
 * Copyright (c) 2018, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef HAVE_XDP_BUFF
#include <linux/bpf_trace.h>
#ifdef HAVE_NET_PAGE_POOL_H
#include <net/page_pool.h>
#endif
#include "en/xdp.h"

int mlx5e_xdp_max_mtu(struct mlx5e_params *params)
{
	int hr = NET_IP_ALIGN + XDP_PACKET_HEADROOM;

	/* Let S := SKB_DATA_ALIGN(sizeof(struct skb_shared_info)).
	 * The condition checked in mlx5e_rx_is_linear_skb is:
	 *   SKB_DATA_ALIGN(sw_mtu + hard_mtu + hr) + S <= PAGE_SIZE         (1)
	 *   (Note that hw_mtu == sw_mtu + hard_mtu.)
	 * What is returned from this function is:
	 *   max_mtu = PAGE_SIZE - S - hr - hard_mtu                         (2)
	 * After assigning sw_mtu := max_mtu, the left side of (1) turns to
	 * SKB_DATA_ALIGN(PAGE_SIZE - S) + S, which is equal to PAGE_SIZE,
	 * because both PAGE_SIZE and S are already aligned. Any number greater
	 * than max_mtu would make the left side of (1) greater than PAGE_SIZE,
	 * so max_mtu is the maximum MTU allowed.
	 */

	return MLX5E_HW2SW_MTU(params, SKB_MAX_HEAD(hr));
}

static inline bool
mlx5e_xmit_xdp_buff(struct mlx5e_xdpsq *sq, struct mlx5e_dma_info *di,
		    struct xdp_buff *xdp)
{
	struct mlx5e_xdp_info xdpi;

#ifdef HAVE_XDP_CONVERT_BUFF_TO_FRAME
	xdpi.xdpf = xdp_convert_buff_to_frame(xdp);
#else
	xdpi.xdpf = convert_to_xdp_frame(xdp);
#endif

	if (unlikely(!xdpi.xdpf))
		return false;
	xdpi.dma_addr = di->addr + (xdpi.xdpf->data - (void *)xdpi.xdpf);
	dma_sync_single_for_device(sq->pdev, xdpi.dma_addr,
				   xdpi.xdpf->len, PCI_DMA_TODEVICE);
	xdpi.di = *di;

	return sq->xmit_xdp_frame(sq, &xdpi);
}

/* returns true if packet was consumed by xdp */
bool mlx5e_xdp_handle(struct mlx5e_rq *rq, struct mlx5e_dma_info *di,
		      void *va, u16 *rx_headroom, u32 *len)
{
	struct bpf_prog *prog = READ_ONCE(rq->xdp_prog);
	struct xdp_buff xdp;
	u32 act;
#ifdef HAVE_XDP_REDIRECT
	int err;
#endif

	if (!prog)
		return false;

	xdp.data = va + *rx_headroom;
#ifdef HAVE_XDP_SET_DATA_META_INVALID
	xdp_set_data_meta_invalid(&xdp);
#endif
	xdp.data_end = xdp.data + *len;
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
	xdp.data_hard_start = va;
#endif
#ifdef HAVE_NET_XDP_H
	xdp.rxq = &rq->xdp_rxq;
#endif

	act = bpf_prog_run_xdp(prog, &xdp);
	switch (act) {
	case XDP_PASS:
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
		*rx_headroom = xdp.data - xdp.data_hard_start;
#endif
		*len = xdp.data_end - xdp.data;
		return false;
	case XDP_TX:
		if (unlikely(!mlx5e_xmit_xdp_buff(&rq->xdpsq, di, &xdp)))
			goto xdp_abort;
		__set_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags); /* non-atomic */
		return true;
#ifdef HAVE_XDP_REDIRECT
	case XDP_REDIRECT:
		page_ref_sub(di->page, di->refcnt_bias);
		di->refcnt_bias = 0;
		/* When XDP enabled then page-refcnt==1 here */
		err = xdp_do_redirect(rq->netdev, &xdp, prog);
		if (unlikely(err))
			goto xdp_abort;
		__set_bit(MLX5E_RQ_FLAG_XDP_XMIT, rq->flags);
		__set_bit(MLX5E_RQ_FLAG_XDP_REDIRECT, rq->flags);
		mlx5e_page_dma_unmap(rq, di);
		rq->stats->xdp_redirect++;
		return true;
#endif
	default:
#ifdef HAVE_BPF_WARN_IVALID_XDP_ACTION_GET_3_PARAMS
		bpf_warn_invalid_xdp_action(rq->netdev, prog, act);
#else
		bpf_warn_invalid_xdp_action(act);
#endif
		/* fall through */
	case XDP_ABORTED:
xdp_abort:
#if defined(HAVE_TRACE_XDP_EXCEPTION) && !defined(MLX_DISABLE_TRACEPOINTS)
		trace_xdp_exception(rq->netdev, prog, act);
#endif
		/* fall through */
	case XDP_DROP:
		rq->stats->xdp_drop++;
		return true;
	}
}

static void mlx5e_xdp_mpwqe_session_start(struct mlx5e_xdpsq *sq)
{
	struct mlx5e_xdp_mpwqe *session = &sq->mpwqe;
	struct mlx5e_xdpsq_stats *stats = sq->stats;
	struct mlx5_wq_cyc *wq = &sq->wq;
	u8  wqebbs;
	u16 pi;

	session->wqe = mlx5e_xdpsq_fetch_wqe(sq, &pi);

	prefetchw(session->wqe->data);
	session->ds_count  = MLX5E_XDP_TX_EMPTY_DS_COUNT;
	session->pkt_count = 0;
	session->complete  = 0;

/* The mult of MLX5_SEND_WQE_MAX_WQEBBS * MLX5_SEND_WQEBB_NUM_DS
 * (16 * 4 == 64) does not fit in the 6-bit DS field of Ctrl Segment.
 * We use a bound lower that MLX5_SEND_WQE_MAX_WQEBBS to let a
 * full-session WQE be cache-aligned.
 */
#if L1_CACHE_BYTES < 128
#define MLX5E_XDP_MPW_MAX_WQEBBS (MLX5_SEND_WQE_MAX_WQEBBS - 1)
#else
#define MLX5E_XDP_MPW_MAX_WQEBBS (MLX5_SEND_WQE_MAX_WQEBBS - 2)
#endif

	wqebbs = min_t(u16, mlx5_wq_cyc_get_contig_wqebbs(wq, pi),
		       MLX5E_XDP_MPW_MAX_WQEBBS);

	session->max_ds_count = MLX5_SEND_WQEBB_NUM_DS * wqebbs;

	mlx5e_xdp_update_inline_state(sq);

	stats->mpwqe++;
}

static void mlx5e_xdp_mpwqe_complete(struct mlx5e_xdpsq *sq)
{
	struct mlx5_wq_cyc       *wq    = &sq->wq;
	struct mlx5e_xdp_mpwqe *session = &sq->mpwqe;
	struct mlx5_wqe_ctrl_seg *cseg = &session->wqe->ctrl;
	u16 ds_count = session->ds_count;
	u16 pi = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	struct mlx5e_xdp_wqe_info *wi = &sq->db.wqe_info[pi];

	cseg->opmod_idx_opcode =
		cpu_to_be32((sq->pc << 8) | MLX5_OPCODE_ENHANCED_MPSW);
	cseg->qpn_ds = cpu_to_be32((sq->sqn << 8) | ds_count);

	wi->num_wqebbs = DIV_ROUND_UP(ds_count, MLX5_SEND_WQEBB_NUM_DS);
	wi->num_pkts   = session->pkt_count;

	sq->pc += wi->num_wqebbs;

	sq->doorbell_cseg = cseg;

	session->wqe = NULL; /* Close session */
}

static bool mlx5e_xmit_xdp_frame_mpwqe(struct mlx5e_xdpsq *sq,
				       struct mlx5e_xdp_info *xdpi)
{
	struct mlx5e_xdp_mpwqe *session = &sq->mpwqe;
	struct mlx5e_xdpsq_stats *stats = sq->stats;

	struct xdp_frame *xdpf = xdpi->xdpf;

	if (unlikely(sq->hw_mtu < xdpf->len)) {
		stats->err++;
		return false;
	}

	if (unlikely(!session->wqe)) {
		if (unlikely(!mlx5e_wqc_has_room_for(&sq->wq, sq->cc, sq->pc,
						     MLX5_SEND_WQE_MAX_WQEBBS))) {
			/* SQ is full, ring doorbell */
			mlx5e_xmit_xdp_doorbell(sq);
			stats->full++;
			return false;
		}

		mlx5e_xdp_mpwqe_session_start(sq);
	}

	mlx5e_xdp_mpwqe_add_dseg(sq, xdpi, stats);

	if (unlikely(session->complete ||
		     session->ds_count == session->max_ds_count))
		mlx5e_xdp_mpwqe_complete(sq);

	mlx5e_xdpi_fifo_push(&sq->db.xdpi_fifo, xdpi);
	stats->xmit++;
	return true;
}

static bool mlx5e_xmit_xdp_frame(struct mlx5e_xdpsq *sq, struct mlx5e_xdp_info *xdpi)
{
	struct mlx5_wq_cyc       *wq   = &sq->wq;
	u16                       pi   = mlx5_wq_cyc_ctr2ix(wq, sq->pc);
	struct mlx5e_tx_wqe      *wqe  = mlx5_wq_cyc_get_wqe(wq, pi);

	struct mlx5_wqe_ctrl_seg *cseg = &wqe->ctrl;
	struct mlx5_wqe_eth_seg  *eseg = &wqe->eth;
	struct mlx5_wqe_data_seg *dseg = wqe->data;
	struct mlx5e_priv *priv = netdev_priv(sq->channel->netdev);

	struct xdp_frame *xdpf = xdpi->xdpf;
	dma_addr_t dma_addr  = xdpi->dma_addr;
	unsigned int dma_len = xdpf->len;

	struct mlx5e_xdpsq_stats *stats = sq->stats;

	prefetchw(wqe);

	if (unlikely(dma_len < MLX5E_XDP_MIN_INLINE || sq->hw_mtu < dma_len)) {
		stats->err++;
		return false;
	}

	if (unlikely(!mlx5e_wqc_has_room_for(wq, sq->cc, sq->pc, 1))) {
		/* SQ is full, ring doorbell */
		mlx5e_xmit_xdp_doorbell(sq);
		stats->full++;
		return false;
	}

	cseg->fm_ce_se = 0;

	/* copy the inline part if required */
	if (sq->min_inline_mode != MLX5_INLINE_MODE_NONE) {
		memcpy(eseg->inline_hdr.start, xdpf->data, MLX5E_XDP_MIN_INLINE);
		eseg->inline_hdr.sz = cpu_to_be16(MLX5E_XDP_MIN_INLINE);
		dma_len  -= MLX5E_XDP_MIN_INLINE;
		dma_addr += MLX5E_XDP_MIN_INLINE;
		dseg++;
	}

	if (MLX5E_GET_PFLAG(&priv->channels.params, MLX5E_PFLAG_TX_XDP_CSUM))
		eseg->cs_flags = MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;

	/* write the dma part */
	dseg->addr       = cpu_to_be64(dma_addr);
	dseg->byte_count = cpu_to_be32(dma_len);

	cseg->opmod_idx_opcode = cpu_to_be32((sq->pc << 8) | MLX5_OPCODE_SEND);

	sq->pc++;

	sq->doorbell_cseg = cseg;

	mlx5e_xdpi_fifo_push(&sq->db.xdpi_fifo, xdpi);
	stats->xmit++;
	return true;
}

static void mlx5e_free_xdpsq_desc(struct mlx5e_xdpsq *sq,
				  struct mlx5e_xdp_wqe_info *wi,
				  struct mlx5e_rq *rq,
				  bool recycle)
{
	struct mlx5e_xdp_info_fifo *xdpi_fifo = &sq->db.xdpi_fifo;
	u16 i;

	for (i = 0; i < wi->num_pkts; i++) {
		struct mlx5e_xdp_info xdpi = mlx5e_xdpi_fifo_pop(xdpi_fifo);

		if (rq) {
			/* XDP_TX */
			mlx5e_page_release(rq, &xdpi.di, recycle);
		} else {
			/* XDP_REDIRECT */
			dma_unmap_single(sq->pdev, xdpi.dma_addr,
					 xdpi.xdpf->len, DMA_TO_DEVICE);
#ifdef HAVE_XDP_FRAME
			xdp_return_frame(xdpi.xdpf);
#else
			/* Assumes order0 page*/
			put_page(virt_to_page(xdpi.xdpf->data));
#endif
		}
	}
}

bool mlx5e_poll_xdpsq_cq(struct mlx5e_cq *cq, struct mlx5e_rq *rq)
{
	struct mlx5e_xdpsq *sq;
	struct mlx5_cqe64 *cqe;
	u16 sqcc;
	int i;

	sq = container_of(cq, struct mlx5e_xdpsq, cq);

	if (unlikely(!test_bit(MLX5E_SQ_STATE_ENABLED, &sq->state)))
		return false;

	cqe = mlx5_cqwq_get_cqe(&cq->wq);
	if (!cqe)
		return false;

	/* sq->cc must be updated only after mlx5_cqwq_update_db_record(),
	 * otherwise a cq overrun may occur
	 */
	sqcc = sq->cc;

	i = 0;
	do {
		u16 wqe_counter;
		bool last_wqe;

		mlx5_cqwq_pop(&cq->wq);

		wqe_counter = be16_to_cpu(cqe->wqe_counter);

		if (unlikely(get_cqe_opcode(cqe) != MLX5_CQE_REQ))
			netdev_WARN_ONCE(sq->channel->netdev,
					 "Bad OP in XDPSQ CQE: 0x%x\n",
					 get_cqe_opcode(cqe));

		do {
			struct mlx5e_xdp_wqe_info *wi;
			u16 ci;

			last_wqe = (sqcc == wqe_counter);
			ci = mlx5_wq_cyc_ctr2ix(&sq->wq, sqcc);
			wi = &sq->db.wqe_info[ci];

			sqcc += wi->num_wqebbs;

			mlx5e_free_xdpsq_desc(sq, wi, rq, true);
		} while (!last_wqe);
	} while ((++i < MLX5E_TX_CQ_POLL_BUDGET) && (cqe = mlx5_cqwq_get_cqe(&cq->wq)));

	sq->stats->cqes += i;

	mlx5_cqwq_update_db_record(&cq->wq);

	/* ensure cq space is freed before enabling more cqes */
	wmb();

	sq->cc = sqcc;
	return (i == MLX5E_TX_CQ_POLL_BUDGET);
}

void mlx5e_free_xdpsq_descs(struct mlx5e_xdpsq *sq, struct mlx5e_rq *rq)
{
	while (sq->cc != sq->pc) {
		struct mlx5e_xdp_wqe_info *wi;
		u16 ci;

		ci = mlx5_wq_cyc_ctr2ix(&sq->wq, sq->cc);
		wi = &sq->db.wqe_info[ci];

		sq->cc += wi->num_wqebbs;

		mlx5e_free_xdpsq_desc(sq, wi, rq, false);
	}
}

void mlx5e_xdp_rx_poll_complete(struct mlx5e_rq *rq)
{
	struct mlx5e_xdpsq *xdpsq = &rq->xdpsq;

	if (xdpsq->mpwqe.wqe)
		mlx5e_xdp_mpwqe_complete(xdpsq);

	mlx5e_xmit_xdp_doorbell(xdpsq);
#ifdef HAVE_XDP_REDIRECT
	if (test_bit(MLX5E_RQ_FLAG_XDP_REDIRECT, rq->flags)) {
		xdp_do_flush_map();
		__clear_bit(MLX5E_RQ_FLAG_XDP_REDIRECT, rq->flags);
	}
#endif
}

void mlx5e_set_xmit_fp(struct mlx5e_xdpsq *sq, bool is_mpw)
{
	sq->xmit_xdp_frame = is_mpw ?
		mlx5e_xmit_xdp_frame_mpwqe : mlx5e_xmit_xdp_frame;
}

#ifdef HAVE_NDO_XDP_XMIT
#ifndef HAVE_NDO_XDP_FLUSH
int mlx5e_xdp_xmit(struct net_device *dev, int n, struct xdp_frame **frames,
		   u32 flags)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_xdpsq *sq;
	int drops = 0;
	int sq_num;
	int i;

	/* this flag is sufficient, no need to test internal sq state */
	if (unlikely(!mlx5e_xdp_tx_is_enabled(priv)))
		return -ENETDOWN;

	if (unlikely(flags & ~XDP_XMIT_FLAGS_MASK))
		return -EINVAL;

	sq_num = smp_processor_id();

	if (unlikely(sq_num >= priv->channels.num))
		return -ENXIO;

	sq = &priv->channels.c[sq_num]->xdpsq;

	for (i = 0; i < n; i++) {
		struct xdp_frame *xdpf = frames[i];
		struct mlx5e_xdp_info xdpi;

		xdpi.dma_addr = dma_map_single(sq->pdev, xdpf->data, xdpf->len,
					       DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(sq->pdev, xdpi.dma_addr))) {
			xdp_return_frame_rx_napi(xdpf);
			drops++;
			continue;
		}

		xdpi.xdpf = xdpf;

		if (unlikely(!sq->xmit_xdp_frame(sq, &xdpi))) {
#ifdef HAVE_XDP_FRAME
			dma_unmap_single(sq->pdev, xdpi.dma_addr,
					 xdpf->len, DMA_TO_DEVICE);
			xdp_return_frame_rx_napi(xdpf);
#else
			/* Assumes order0 page*/
			put_page(virt_to_page(xdpf->data));
#endif
			drops++;
		}
	}

	if (flags & XDP_XMIT_FLUSH) {
		if (sq->mpwqe.wqe)
			mlx5e_xdp_mpwqe_complete(sq);
		mlx5e_xmit_xdp_doorbell(sq);
	}

	return n - drops;
}
#else
int mlx5e_xdp_xmit(struct net_device *dev, struct xdp_buff *xdp)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_xdp_info xdpi;
	struct mlx5e_xdpsq *sq;
	int sq_num;

	/* this flag is sufficient, no need to test internal sq state */
	if (unlikely(!mlx5e_xdp_tx_is_enabled(priv)))
		return -ENETDOWN;

	sq_num = smp_processor_id();

	if (unlikely(sq_num >= priv->channels.num))
		return -ENXIO;

	sq = &priv->channels.c[sq_num]->xdpsq;

	xdpi.xdpf = convert_to_xdp_frame(xdp);
	if (unlikely(!xdpi.xdpf))
		return -ENOSPC;

	xdpi.dma_addr = dma_map_single(sq->pdev, xdpi.xdpf->data,
				       xdpi.xdpf->len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(sq->pdev, xdpi.dma_addr)))
		return -ENOSPC;

	if (unlikely(!sq->xmit_xdp_frame(sq, &xdpi)))
		return -ENOSPC;

	return 0;
}

void mlx5e_xdp_flush(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_xdpsq *sq;
	int sq_num;

	/* this flag is sufficient, no need to test internal sq state */
	if (unlikely(!mlx5e_xdp_tx_is_enabled(priv)))
		return;

	sq_num = smp_processor_id();

	if (unlikely(sq_num >= priv->channels.num))
		return;

	sq = &priv->channels.c[sq_num]->xdpsq;

	if (sq->mpwqe.wqe)
		mlx5e_xdp_mpwqe_complete(sq);
	mlx5e_xmit_xdp_doorbell(sq);
}
#endif
#endif
#endif

