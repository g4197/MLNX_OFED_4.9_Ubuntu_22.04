/*
 * Copyright (c) 2007 Mellanox Technologies. All rights reserved.
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
 *
 */

#ifdef HAVE_XDP_BUFF
#include <linux/bpf.h>
#endif
#include <linux/bpf_trace.h>
#include <linux/mlx4/cq.h>
#include <linux/slab.h>
#include <linux/mlx4/qp.h>
#include <linux/skbuff.h>
#include <linux/rculist.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/vmalloc.h>
#include <linux/irq.h>
#include <linux/prefetch.h>

#include <net/ip.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ip6_checksum.h>
#endif

#include "mlx4_en.h"

static int mlx4_alloc_page(struct mlx4_en_priv *priv,
			   struct mlx4_en_rx_alloc *frag,
			   gfp_t gfp)
{
	struct page *page;
	dma_addr_t dma;

	page = alloc_page(gfp);
	if (unlikely(!page))
		return -ENOMEM;
	dma = dma_map_page(priv->ddev, page, 0, PAGE_SIZE, priv->dma_dir);
	if (unlikely(dma_mapping_error(priv->ddev, dma))) {
		__free_page(page);
		return -ENOMEM;
	}
	frag->page = page;
	frag->dma = dma;
	frag->page_offset = priv->rx_headroom;
	return 0;
}

static int mlx4_en_alloc_frags(struct mlx4_en_priv *priv,
			       struct mlx4_en_rx_ring *ring,
			       struct mlx4_en_rx_desc *rx_desc,
			       struct mlx4_en_rx_alloc *frags,
			       gfp_t gfp)
{
	int i;

	for (i = 0; i < priv->num_frags; i++, frags++) {
		if (!frags->page) {
			if (mlx4_alloc_page(priv, frags, gfp))
				return -ENOMEM;
			ring->rx_alloc_pages++;
		}
		rx_desc->data[i].addr = cpu_to_be64(frags->dma +
						    frags->page_offset);
	}
	return 0;
}

static void mlx4_en_free_frag(const struct mlx4_en_priv *priv,
			      struct mlx4_en_rx_alloc *frag)
{
	if (frag->page) {
		dma_unmap_page(priv->ddev, frag->dma,
			       PAGE_SIZE, priv->dma_dir);
		__free_page(frag->page);
	}
	/* We need to clear all fields, otherwise a change of priv->log_rx_info
	 * could lead to see garbage later in frag->page.
	 */
	memset(frag, 0, sizeof(*frag));
}

static void mlx4_en_init_rx_desc(const struct mlx4_en_priv *priv,
				 struct mlx4_en_rx_ring *ring, int index)
{
	struct mlx4_en_rx_desc *rx_desc = ring->buf + ring->stride * index;
	int possible_frags;
	int i;

	/* Set size and memtype fields */
	for (i = 0; i < priv->num_frags; i++) {
		rx_desc->data[i].byte_count =
			cpu_to_be32(priv->frag_info[i].frag_size);
		rx_desc->data[i].lkey = cpu_to_be32(priv->mdev->mr.key);
	}

	/* If the number of used fragments does not fill up the ring stride,
	 * remaining (unused) fragments must be padded with null address/size
	 * and a special memory key */
	possible_frags = (ring->stride - sizeof(struct mlx4_en_rx_desc)) / DS_SIZE;
	for (i = priv->num_frags; i < possible_frags; i++) {
		rx_desc->data[i].byte_count = 0;
		rx_desc->data[i].lkey = cpu_to_be32(MLX4_EN_MEMTYPE_PAD);
		rx_desc->data[i].addr = 0;
	}
}

static int mlx4_en_prepare_rx_desc(struct mlx4_en_priv *priv,
				   struct mlx4_en_rx_ring *ring, int index,
				   gfp_t gfp)
{
	struct mlx4_en_rx_desc *rx_desc = ring->buf +
		(index << ring->log_stride);
	struct mlx4_en_rx_alloc *frags = ring->rx_info +
					(index << priv->log_rx_info);
#ifdef HAVE_XDP_BUFF
	if (likely(ring->page_cache.index > 0)) {
		/* XDP uses a single page per frame */
		if (!frags->page) {
			ring->page_cache.index--;
			frags->page = ring->page_cache.buf[ring->page_cache.index].page;
			frags->dma  = ring->page_cache.buf[ring->page_cache.index].dma;
		}
		frags->page_offset = XDP_PACKET_HEADROOM;
		rx_desc->data[0].addr = cpu_to_be64(frags->dma +
						    XDP_PACKET_HEADROOM);
		return 0;
	}
#endif

	return mlx4_en_alloc_frags(priv, ring, rx_desc, frags, gfp);
}

static bool mlx4_en_is_ring_empty(const struct mlx4_en_rx_ring *ring)
{
	return ring->prod == ring->cons;
}

static inline void mlx4_en_update_rx_prod_db(struct mlx4_en_rx_ring *ring)
{
	/* ensure rx_desc updating reaches HW before prod db updating */
	wmb();
	*ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff);
}

/* slow path */
static void mlx4_en_free_rx_desc(const struct mlx4_en_priv *priv,
				 struct mlx4_en_rx_ring *ring,
				 int index)
{
	struct mlx4_en_rx_alloc *frags;
	int nr;

	frags = ring->rx_info + (index << priv->log_rx_info);
	for (nr = 0; nr < priv->num_frags; nr++) {
		en_dbg(DRV, priv, "Freeing fragment:%d\n", nr);
		mlx4_en_free_frag(priv, frags + nr);
	}
}

#ifdef CONFIG_COMPAT_LRO_ENABLED
static inline int mlx4_en_can_lro(__be16 status)
{
	static __be16 status_ipv4_ipok_tcp;
	static __be16 status_all;

	status_all		= cpu_to_be16(
					MLX4_CQE_STATUS_IPV4    |
					MLX4_CQE_STATUS_IPV4F   |
					MLX4_CQE_STATUS_IPV6    |
					MLX4_CQE_STATUS_IPV4OPT |
					MLX4_CQE_STATUS_TCP     |
					MLX4_CQE_STATUS_UDP     |
					MLX4_CQE_STATUS_IPOK);

	status_ipv4_ipok_tcp	= cpu_to_be16(
					MLX4_CQE_STATUS_IPV4    |
					MLX4_CQE_STATUS_IPOK    |
					MLX4_CQE_STATUS_TCP);

	status &= status_all;
	return status == status_ipv4_ipok_tcp;
}
#endif

/* Function not in fast-path */
static int mlx4_en_fill_rx_buffers(struct mlx4_en_priv *priv)
{
	struct mlx4_en_rx_ring *ring;
	int ring_ind;
	int buf_ind;
	int new_size;

	for (buf_ind = 0; buf_ind < priv->prof->rx_ring_size; buf_ind++) {
		for (ring_ind = 0; ring_ind < priv->rx_ring_num; ring_ind++) {
			ring = priv->rx_ring[ring_ind];

			if (mlx4_en_prepare_rx_desc(priv, ring,
						    ring->actual_size,
						    GFP_KERNEL)) {
				if (ring->actual_size < MLX4_EN_MIN_RX_SIZE) {
					en_err(priv, "Failed to allocate enough rx buffers\n");
					return -ENOMEM;
				} else {
					new_size = rounddown_pow_of_two(ring->actual_size);
					en_warn(priv, "Only %d buffers allocated reducing ring size to %d\n",
						ring->actual_size, new_size);
					goto reduce_rings;
				}
			}
			ring->actual_size++;
			ring->prod++;
		}
	}
	return 0;

reduce_rings:
	for (ring_ind = 0; ring_ind < priv->rx_ring_num; ring_ind++) {
		ring = priv->rx_ring[ring_ind];
		while (ring->actual_size > new_size) {
			ring->actual_size--;
			ring->prod--;
			mlx4_en_free_rx_desc(priv, ring, ring->actual_size);
		}
	}

	return 0;
}

static void mlx4_en_free_rx_buf(struct mlx4_en_priv *priv,
				struct mlx4_en_rx_ring *ring)
{
	int index;

	en_dbg(DRV, priv, "Freeing Rx buf - cons:%d prod:%d\n",
	       ring->cons, ring->prod);

	/* Unmap and free Rx buffers */
	for (index = 0; index < ring->size; index++) {
		en_dbg(DRV, priv, "Processing descriptor:%d\n", index);
		mlx4_en_free_rx_desc(priv, ring, index);
	}
	ring->cons = 0;
	ring->prod = 0;
}

void mlx4_en_set_num_rx_rings(struct mlx4_en_dev *mdev)
{
	int i;
	int num_of_eqs;
	int num_rx_rings;
	struct mlx4_dev *dev = mdev->dev;

	mlx4_foreach_port(i, dev, MLX4_PORT_TYPE_ETH) {
		num_of_eqs = max_t(int, MIN_RX_RINGS,
				   min_t(int,
					 mlx4_get_eqs_per_port(mdev->dev, i),
					 DEF_RX_RINGS));

		num_rx_rings = mlx4_low_memory_profile() ? MIN_RX_RINGS :
			min_t(int, num_of_eqs, num_online_cpus());
		mdev->profile.prof[i].rx_ring_num =
			rounddown_pow_of_two(num_rx_rings);
	}
}

#ifdef CONFIG_COMPAT_LRO_ENABLED
static int mlx4_en_get_frag_hdr(struct skb_frag_struct *frags, void **mac_hdr,
				void **ip_hdr, void **tcpudp_hdr,
				u64 *hdr_flags, void *priv)
{
	*mac_hdr = page_address(skb_frag_page(frags)) + frags->page_offset;
	*ip_hdr = *mac_hdr + ETH_HLEN;
	*tcpudp_hdr = (struct tcphdr *)(*ip_hdr + sizeof(struct iphdr));
	*hdr_flags = LRO_IPV4 | LRO_TCP;

	return 0;
}

static void mlx4_en_lro_init(struct mlx4_en_rx_ring *ring,
			     struct mlx4_en_priv *priv)
{
	/* The lro_receive_frags routine aggregates priv->num_frags to this
	 * array and only then checks that total number of frags is greater
	 * or equal to max_aggr, to issue an lro_flush().
	 * We need to make sure that by the addition of priv->num_frags
	 * to an open LRO session we never exceed MAX_SKB_FRAGS,
	 * to avoid overflow.
	 */
	ring->lro.lro_mgr.max_aggr = MAX_SKB_FRAGS - priv->num_frags + 1;

	ring->lro.lro_mgr.max_desc		= MLX4_EN_LRO_MAX_DESC;
	ring->lro.lro_mgr.lro_arr		= ring->lro.lro_desc;
	ring->lro.lro_mgr.get_frag_header	= mlx4_en_get_frag_hdr;
	ring->lro.lro_mgr.features		= LRO_F_NAPI;
	ring->lro.lro_mgr.frag_align_pad	= NET_IP_ALIGN;
	ring->lro.lro_mgr.dev			= priv->dev;
	ring->lro.lro_mgr.ip_summed		= CHECKSUM_UNNECESSARY;
	ring->lro.lro_mgr.ip_summed_aggr	= CHECKSUM_UNNECESSARY;
}
#endif

int mlx4_en_create_rx_ring(struct mlx4_en_priv *priv,
			   struct mlx4_en_rx_ring **pring,
			   u32 size, u16 stride, int node, int queue_index)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_rx_ring *ring;
	int err = -ENOMEM;
	int tmp;

	ring = kzalloc_node(sizeof(*ring), GFP_KERNEL, node);
	if (!ring) {
		en_err(priv, "Failed to allocate RX ring structure\n");
		return -ENOMEM;
	}

	ring->prod = 0;
	ring->cons = 0;
	ring->size = size;
	ring->size_mask = size - 1;
	ring->stride = stride;
	ring->log_stride = ffs(ring->stride) - 1;
	ring->buf_size = ring->size * ring->stride + TXBB_SIZE;

#ifdef HAVE_NET_XDP_H
#ifdef HAVE_XDP_RXQ_INFO_REG_GET_4_PARAMS
	if (xdp_rxq_info_reg(&ring->xdp_rxq, priv->dev, queue_index, 0) < 0)
#else
	if (xdp_rxq_info_reg(&ring->xdp_rxq, priv->dev, queue_index) < 0)
#endif
		goto err_ring;
#endif

	tmp = size * roundup_pow_of_two(MLX4_EN_MAX_RX_FRAGS *
					sizeof(struct mlx4_en_rx_alloc));
	ring->rx_info = kvzalloc_node(tmp, GFP_KERNEL, node);
	if (!ring->rx_info) {
		err = -ENOMEM;
		goto err_xdp_info;
	}

	en_dbg(DRV, priv, "Allocated rx_info ring at addr:%p size:%d\n",
		 ring->rx_info, tmp);

	/* Allocate HW buffers on provided NUMA node */
	set_dev_node(&mdev->dev->persist->pdev->dev, node);
	err = mlx4_alloc_hwq_res(mdev->dev, &ring->wqres, ring->buf_size);
	set_dev_node(&mdev->dev->persist->pdev->dev, mdev->dev->numa_node);
	if (err)
		goto err_info;

	ring->buf = ring->wqres.buf.direct.buf;

	ring->hwtstamp_rx_filter = priv->hwtstamp_config.rx_filter;

	*pring = ring;
	return 0;

err_info:
	kvfree(ring->rx_info);
	ring->rx_info = NULL;
err_xdp_info:
#ifdef HAVE_NET_XDP_H
	xdp_rxq_info_unreg(&ring->xdp_rxq);
err_ring:
#endif
	kfree(ring);
	*pring = NULL;

	return err;
}

int mlx4_en_activate_rx_rings(struct mlx4_en_priv *priv)
{
	struct mlx4_en_rx_ring *ring;
	int i;
	int ring_ind;
	int err;
	int stride = priv->prof->inline_scatter_thold ? priv->stride :
		roundup_pow_of_two(sizeof(struct mlx4_en_rx_desc) +
				   DS_SIZE * priv->num_frags);

	for (ring_ind = 0; ring_ind < priv->rx_ring_num; ring_ind++) {
		ring = priv->rx_ring[ring_ind];

		ring->prod = 0;
		ring->cons = 0;
		ring->actual_size = 0;
		ring->cqn = priv->rx_cq[ring_ind]->mcq.cqn;

		ring->stride = stride;
		if (ring->stride <= TXBB_SIZE) {
			/* Stamp first unused send wqe */
			__be32 *ptr = (__be32 *)ring->buf;
			__be32 stamp = cpu_to_be32(1 << STAMP_SHIFT);
			*ptr = stamp;
			/* Move pointer to start of rx section */
			ring->buf += TXBB_SIZE;
		}

		ring->log_stride = ffs(ring->stride) - 1;
		ring->buf_size = ring->size * ring->stride;

		memset(ring->buf, 0, ring->buf_size);
		mlx4_en_update_rx_prod_db(ring);

		/* Initialize all descriptors */
		for (i = 0; i < ring->size; i++)
			mlx4_en_init_rx_desc(priv, ring, i);
#ifdef CONFIG_COMPAT_LRO_ENABLED
		mlx4_en_lro_init(ring, priv);
#endif
	}
	err = mlx4_en_fill_rx_buffers(priv);
	if (err)
		goto err_buffers;

	for (ring_ind = 0; ring_ind < priv->rx_ring_num; ring_ind++) {
		ring = priv->rx_ring[ring_ind];

		ring->size_mask = ring->actual_size - 1;
		mlx4_en_update_rx_prod_db(ring);
	}

	return 0;

err_buffers:
	for (ring_ind = 0; ring_ind < priv->rx_ring_num; ring_ind++)
		mlx4_en_free_rx_buf(priv, priv->rx_ring[ring_ind]);

	ring_ind = priv->rx_ring_num - 1;
	while (ring_ind >= 0) {
		if (priv->rx_ring[ring_ind]->stride <= TXBB_SIZE)
			priv->rx_ring[ring_ind]->buf -= TXBB_SIZE;
		ring_ind--;
	}
	return err;
}

/* We recover from out of memory by scheduling our napi poll
 * function (mlx4_en_process_cq), which tries to allocate
 * all missing RX buffers (call to mlx4_en_refill_rx_buffers).
 */
void mlx4_en_recover_from_oom(struct mlx4_en_priv *priv)
{
	int ring;

	if (!priv->port_up)
		return;

	for (ring = 0; ring < priv->rx_ring_num; ring++) {
		if (mlx4_en_is_ring_empty(priv->rx_ring[ring])) {
			local_bh_disable();
			napi_reschedule(&priv->rx_cq[ring]->napi);
			local_bh_enable();
		}
	}
}

/* When the rx ring is running in page-per-packet mode, a released frame can go
 * directly into a small cache, to avoid unmapping or touching the page
 * allocator. In bpf prog performance scenarios, buffers are either forwarded
 * or dropped, never converted to skbs, so every page can come directly from
 * this cache when it is sized to be a multiple of the napi budget.
 */
bool mlx4_en_rx_recycle(struct mlx4_en_rx_ring *ring,
			struct mlx4_en_rx_alloc *frame)
{
	struct mlx4_en_page_cache *cache = &ring->page_cache;

	if (cache->index >= MLX4_EN_CACHE_SIZE)
		return false;

	cache->buf[cache->index].page = frame->page;
	cache->buf[cache->index].dma = frame->dma;
	cache->index++;
	return true;
}

void mlx4_en_destroy_rx_ring(struct mlx4_en_priv *priv,
			     struct mlx4_en_rx_ring **pring,
			     u32 size, u16 stride)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_rx_ring *ring = *pring;
#ifdef HAVE_XDP_BUFF
	struct bpf_prog *old_prog;

	old_prog = rcu_dereference_protected(
					ring->xdp_prog,
					lockdep_is_held(&mdev->state_lock));
	if (old_prog)
		bpf_prog_put(old_prog);
#ifdef HAVE_NET_XDP_H
	xdp_rxq_info_unreg(&ring->xdp_rxq);
#endif
#endif
	mlx4_free_hwq_res(mdev->dev, &ring->wqres, size * stride + TXBB_SIZE);
	kvfree(ring->rx_info);
	ring->rx_info = NULL;
	kfree(ring);
	*pring = NULL;
}

void mlx4_en_deactivate_rx_ring(struct mlx4_en_priv *priv,
				struct mlx4_en_rx_ring *ring)
{
	int i;

	for (i = 0; i < ring->page_cache.index; i++) {
		dma_unmap_page(priv->ddev, ring->page_cache.buf[i].dma,
			       PAGE_SIZE, priv->dma_dir);
		put_page(ring->page_cache.buf[i].page);
	}
	ring->page_cache.index = 0;
	mlx4_en_free_rx_buf(priv, ring);
	if (ring->stride <= TXBB_SIZE)
		ring->buf -= TXBB_SIZE;
}


static int mlx4_en_complete_rx_desc(struct mlx4_en_priv *priv,
				    struct mlx4_en_rx_alloc *frags,
#ifndef CONFIG_COMPAT_LRO_ENABLED
				    struct sk_buff *skb,
				    int length)
#else
				    struct skb_frag_struct *skb_frags_rx,
				    int length,
				    int *truesize)
#endif
{
	const struct mlx4_en_frag_info *frag_info = priv->frag_info;
#ifndef CONFIG_COMPAT_LRO_ENABLED
	int *truesize = &skb->truesize;
#endif
	bool release = true;
	int nr, frag_size;
	struct page *page;
	dma_addr_t dma;

	/* Collect used fragments while replacing them in the HW descriptors */
	for (nr = 0;; frags++) {
		frag_size = min_t(int, length, frag_info->frag_size);

		page = frags->page;
		if (unlikely(!page))
			goto fail;

		dma = frags->dma;
		dma_sync_single_range_for_cpu(priv->ddev, dma, frags->page_offset,
					      frag_size, priv->dma_dir);

#ifndef CONFIG_COMPAT_LRO_ENABLED
		__skb_fill_page_desc(skb, nr, page, frags->page_offset,
				     frag_size);
#else
		/* Save page reference in skb */
		__skb_frag_set_page(&skb_frags_rx[nr], frags->page);
		skb_frag_size_set(&skb_frags_rx[nr], frag_size);
		skb_frags_rx[nr].page_offset = frags->page_offset;
#endif
		*truesize += frag_info->frag_stride;

		if (frag_info->frag_stride == PAGE_SIZE / 2) {
			frags->page_offset ^= PAGE_SIZE / 2;
			release = page_count(page) != 1 ||
#ifdef HAVE_PAGE_IS_PFMEMALLOC
				  page_is_pfmemalloc(page) ||
#endif
#ifdef HAVE_NUMA_MEM_ID
				  page_to_nid(page) != numa_mem_id();
#else
				  page_to_nid(page) != numa_node_id();
#endif
		} else if (!priv->rx_headroom) {
			/* rx_headroom for non XDP setup is always 0.
			 * When XDP is set, the above condition will
			 * guarantee page is always released.
			 */
			u32 sz_align = ALIGN(frag_size, SMP_CACHE_BYTES);

			frags->page_offset += sz_align;
			release = frags->page_offset + frag_info->frag_size > PAGE_SIZE;
		}
		if (release) {
			dma_unmap_page(priv->ddev, dma, PAGE_SIZE, priv->dma_dir);
			frags->page = NULL;
		} else {
			page_ref_inc(page);
		}

		nr++;
		length -= frag_size;
		if (!length)
			break;
		frag_info++;
	}
	return nr;

fail:
	while (nr > 0) {
		nr--;
#ifndef CONFIG_COMPAT_LRO_ENABLED
		__skb_frag_unref(skb_shinfo(skb)->frags + nr);
#else
		__skb_frag_unref(skb_frags_rx + nr);
#endif
	}
	return 0;
}

static void validate_loopback(struct mlx4_en_priv *priv, void *va)
{
	const unsigned char *data = va + ETH_HLEN;
	int i;

	for (i = 0; i < MLX4_LOOPBACK_TEST_PAYLOAD; i++) {
		if (data[i] != (unsigned char)i)
			return;
	}
	/* Loopback found */
	priv->loopback_ok = 1;
}

static void mlx4_en_refill_rx_buffers(struct mlx4_en_priv *priv,
				      struct mlx4_en_rx_ring *ring)
{
	u32 missing = ring->actual_size - (ring->prod - ring->cons);

	/* Try to batch allocations, but not too much. */
	if (missing < 8)
		return;
	do {
		if (mlx4_en_prepare_rx_desc(priv, ring,
					    ring->prod & ring->size_mask,
					    GFP_ATOMIC | __GFP_MEMALLOC))
			break;
		ring->prod++;
	} while (likely(--missing));

	mlx4_en_update_rx_prod_db(ring);
}

/* When hardware doesn't strip the vlan, we need to calculate the checksum
 * over it and add it to the hardware's checksum calculation
 */
static inline __wsum get_fixed_vlan_csum(__wsum hw_checksum,
					 struct vlan_hdr *vlanh)
{
	return csum_add(hw_checksum, *(__wsum *)vlanh);
}

/* Although the stack expects checksum which doesn't include the pseudo
 * header, the HW adds it. To address that, we are subtracting the pseudo
 * header checksum from the checksum value provided by the HW.
 */
static int get_fixed_ipv4_csum(__wsum hw_checksum, struct sk_buff *skb,
			       struct iphdr *iph)
{
	__u16 length_for_csum = 0;
	__wsum csum_pseudo_header = 0;
	__u8 ipproto = iph->protocol;

	if (unlikely(ipproto == IPPROTO_SCTP))
		return -1;

	length_for_csum = (be16_to_cpu(iph->tot_len) - (iph->ihl << 2));
	csum_pseudo_header = csum_tcpudp_nofold(iph->saddr, iph->daddr,
						length_for_csum, ipproto, 0);
	skb->csum = csum_sub(hw_checksum, csum_pseudo_header);
	return 0;
}

#if IS_ENABLED(CONFIG_IPV6)
/* In IPv6 packets, hw_checksum lacks 6 bytes from IPv6 header:
 * 4 first bytes : priority, version, flow_lbl
 * and 2 additional bytes : nexthdr, hop_limit.
 */
static int get_fixed_ipv6_csum(__wsum hw_checksum, struct sk_buff *skb,
			       struct ipv6hdr *ipv6h)
{
	__u8 nexthdr = ipv6h->nexthdr;
	__wsum temp;

	if (unlikely(nexthdr == IPPROTO_FRAGMENT ||
		     nexthdr == IPPROTO_HOPOPTS ||
		     nexthdr == IPPROTO_SCTP))
		return -1;

	/* priority, version, flow_lbl */
	temp = csum_add(hw_checksum, *(__wsum *)ipv6h);
	/* nexthdr and hop_limit */
	skb->csum = csum_add(temp, (__force __wsum)*(__be16 *)&ipv6h->nexthdr);
	return 0;
}
#endif

static void mlx4_en_inline_scatter(struct mlx4_en_rx_ring *ring,
				   struct mlx4_en_rx_alloc *frags,
				   struct mlx4_en_priv *priv,
				   unsigned int length,
				   int index, void *va)
{
	struct mlx4_en_rx_desc *rx_desc = ring->buf + (index << ring->log_stride);
	dma_addr_t dma;
	int frag_size;

	/* fill frag */
	frag_size = priv->frag_info[0].frag_size;
	dma = frags[0].dma + frags[0].page_offset;
	dma_sync_single_for_cpu(priv->ddev, dma, frag_size,
				DMA_FROM_DEVICE);
	memcpy(va, rx_desc, length);

	/* prepare a valid rx_desc */
	memset(rx_desc, 0, ring->stride);
	rx_desc->data[0].byte_count = cpu_to_be32(frag_size);
	rx_desc->data[0].lkey = cpu_to_be32(priv->mdev->mr.key);
	rx_desc->data[0].addr = cpu_to_be64(dma);

	rx_desc->data[1].lkey = cpu_to_be32(MLX4_EN_MEMTYPE_PAD);

	ring->inline_scatter++;
}

#define short_frame(size) ((size) <= ETH_ZLEN + ETH_FCS_LEN)

/* We reach this function only after checking that any of
 * the (IPv4 | IPv6) bits are set in cqe->status.
 */
static int check_csum(struct mlx4_cqe *cqe, struct sk_buff *skb, void *va,
		      netdev_features_t dev_features)
{
	__wsum hw_checksum = 0;
	void *hdr;

	/* CQE csum doesn't cover padding octets in short ethernet
	 * frames. And the pad field is appended prior to calculating
	 * and appending the FCS field.
	 *
	 * Detecting these padded frames requires to verify and parse
	 * IP headers, so we simply force all those small frames to skip
	 * checksum complete.
	 */
	if (short_frame(skb->len))
		return -EINVAL;

	hdr = (u8 *)va + sizeof(struct ethhdr);
	hw_checksum = csum_unfold((__force __sum16)cqe->checksum);

	if (cqe->vlan_my_qpn & cpu_to_be32(MLX4_CQE_CVLAN_PRESENT_MASK) &&
	    !(dev_features & NETIF_F_HW_VLAN_CTAG_RX)) {
		hw_checksum = get_fixed_vlan_csum(hw_checksum, hdr);
		hdr += sizeof(struct vlan_hdr);
	}

#if IS_ENABLED(CONFIG_IPV6)
	if (cqe->status & cpu_to_be16(MLX4_CQE_STATUS_IPV6))
		return get_fixed_ipv6_csum(hw_checksum, skb, hdr);
#endif
	return get_fixed_ipv4_csum(hw_checksum, skb, hdr);
}

#if IS_ENABLED(CONFIG_IPV6)
#define MLX4_CQE_STATUS_IP_ANY (MLX4_CQE_STATUS_IPV4 | MLX4_CQE_STATUS_IPV6)
#else
#define MLX4_CQE_STATUS_IP_ANY (MLX4_CQE_STATUS_IPV4)
#endif

int mlx4_en_process_rx_cq(struct net_device *dev, struct mlx4_en_cq *cq, int budget)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	int factor = priv->cqe_factor;
	struct mlx4_en_rx_ring *ring;
#ifdef HAVE_XDP_BUFF
	struct bpf_prog *xdp_prog;
#endif
	int cq_ring = cq->ring;
#ifdef HAVE_XDP_BUFF
	bool doorbell_pending;
#endif
	struct mlx4_cqe *cqe;
#ifdef HAVE_XDP_BUFF
	struct xdp_buff xdp;
#endif
	int polled = 0;
	int index;

	if (unlikely(!priv->port_up || budget <= 0))
		return 0;

	ring = priv->rx_ring[cq_ring];

#ifdef HAVE_XDP_BUFF
	/* Protect accesses to: ring->xdp_prog, priv->mac_hash list */
	rcu_read_lock();
	xdp_prog = rcu_dereference(ring->xdp_prog);
#ifdef HAVE_NET_XDP_H
	xdp.rxq = &ring->xdp_rxq;
#endif
	doorbell_pending = 0;
#endif

	/* We assume a 1:1 mapping between CQEs and Rx descriptors, so Rx
	 * descriptor offset can be deduced from the CQE index instead of
	 * reading 'cqe->index' */
	index = cq->mcq.cons_index & ring->size_mask;
	cqe = mlx4_en_get_cqe(cq->buf, index, priv->cqe_size) + factor;

	/* Process all completed CQEs */
	while (XNOR(cqe->owner_sr_opcode & MLX4_CQE_OWNER_MASK,
		    cq->mcq.cons_index & cq->size)) {
		struct mlx4_en_rx_alloc *frags;
#ifdef HAVE_SKB_SET_HASH
		enum pkt_hash_types hash_type;
#endif
		struct sk_buff *skb;
		unsigned int length;
		int ip_summed;
		void *va;
		int nr;

		frags = ring->rx_info + (index << priv->log_rx_info);
		va = page_address(frags[0].page) + frags[0].page_offset;
		prefetchw(va);
		/*
		 * make sure we read the CQE after we read the ownership bit
		 */
#ifdef dma_rmb
		dma_rmb();
#else
		rmb();
#endif

		/* Drop packet on bad receive or bad checksum */
		if (unlikely((cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) ==
						MLX4_CQE_OPCODE_ERROR)) {
			en_err(priv, "CQE completed in error - vendor syndrom:%d syndrom:%d\n",
			       ((struct mlx4_err_cqe *)cqe)->vendor_err_syndrome,
			       ((struct mlx4_err_cqe *)cqe)->syndrome);
			goto next;
		}
		if (unlikely(cqe->badfcs_enc & MLX4_CQE_BAD_FCS)) {
			en_dbg(RX_ERR, priv, "Accepted frame with bad FCS\n");
			goto next;
		}

		length = be32_to_cpu(cqe->byte_cnt);
		length -= ring->fcs_del;

		if (cqe->owner_sr_opcode & MLX4_CQE_IS_RECV_MASK)
			mlx4_en_inline_scatter(ring, frags, priv, length, index, va);

		/* Check if we need to drop the packet if SRIOV is not enabled
		 * and not performing the selftest or flb disabled
		 */
		if (priv->flags & MLX4_EN_FLAG_RX_FILTER_NEEDED) {
			const struct ethhdr *ethh = va;
			dma_addr_t dma;
			COMPAT_HL_NODE
			/* Get pointer to first fragment since we haven't
			 * skb yet and cast it to ethhdr struct
			 */
			dma = frags[0].dma + frags[0].page_offset;
			dma_sync_single_for_cpu(priv->ddev, dma, sizeof(*ethh),
						DMA_FROM_DEVICE);

			if (is_multicast_ether_addr(ethh->h_dest)) {
				struct mlx4_mac_entry *entry;
				struct hlist_head *bucket;
				unsigned int mac_hash;

				/* Drop the packet, since HW loopback-ed it */
				mac_hash = ethh->h_source[MLX4_EN_MAC_HASH_IDX];
				bucket = &priv->mac_hash[mac_hash];
				compat_hlist_for_each_entry_rcu(entry, bucket, hlist) {
					if (ether_addr_equal_64bits(entry->mac,
								    ethh->h_source))
						goto next;
				}
			}
		}

		if (unlikely(priv->validate_loopback)) {
			validate_loopback(priv, va);
			goto next;
		}

		/*
		 * Packet is OK - process it.
		 */
		/* A bpf program gets first chance to drop the packet. It may
		 * read bytes but not past the end of the frag.
		 */
#ifdef HAVE_XDP_BUFF
		if (xdp_prog) {
			dma_addr_t dma;
#ifdef HAVE_XDP_BUFF_DATA_HARD_START
			void *orig_data;
#endif
			u32 act;

			dma = frags[0].dma + frags[0].page_offset;
			dma_sync_single_for_cpu(priv->ddev, dma,
						priv->frag_info[0].frag_size,
						DMA_FROM_DEVICE);

#ifdef HAVE_XDP_BUFF_DATA_HARD_START
			xdp.data_hard_start = va - frags[0].page_offset;
			xdp.data = va;
#ifdef HAVE_XDP_SET_DATA_META_INVALID
			xdp_set_data_meta_invalid(&xdp);
#endif
			xdp.data_end = xdp.data + length;
			orig_data = xdp.data;
#else
			xdp.data = va;
			xdp.data_end = xdp.data + length;
#endif

			act = bpf_prog_run_xdp(xdp_prog, &xdp);

#ifdef HAVE_XDP_BUFF_DATA_HARD_START
			length = xdp.data_end - xdp.data;
			if (xdp.data != orig_data) {
				frags[0].page_offset = xdp.data -
					xdp.data_hard_start;
				va = xdp.data;
			}
#endif

			switch (act) {
			case XDP_PASS:
				break;
			case XDP_TX:
				if (likely(!mlx4_en_xmit_frame(ring, frags, priv,
							length, cq_ring,
							&doorbell_pending))) {
					frags[0].page = NULL;
					goto next;
				}
#if defined(HAVE_TRACE_XDP_EXCEPTION) && !defined(MLX_DISABLE_TRACEPOINTS)
				trace_xdp_exception(dev, xdp_prog, act);
#endif
				goto xdp_drop_no_cnt; /* Drop on xmit failure */
			default:
#ifdef HAVE_BPF_WARN_IVALID_XDP_ACTION_GET_3_PARAMS
		bpf_warn_invalid_xdp_action(dev, xdp_prog, act);
#else
		bpf_warn_invalid_xdp_action(act);
#endif
				/* fall through */
			case XDP_ABORTED:
#if defined(HAVE_TRACE_XDP_EXCEPTION) && !defined(MLX_DISABLE_TRACEPOINTS)
				trace_xdp_exception(dev, xdp_prog, act);
#endif
				/* fall through */
			case XDP_DROP:
				ring->xdp_drop++;
xdp_drop_no_cnt:
				goto next;
			}
		}
#endif

		ring->bytes += length;
		ring->packets++;

		skb = napi_get_frags(&cq->napi);
		if (unlikely(!skb))
			goto next;

		if (unlikely(ring->hwtstamp_rx_filter == HWTSTAMP_FILTER_ALL)) {
			u64 timestamp = mlx4_en_get_cqe_ts(cqe);

			mlx4_en_fill_hwtstamps(priv->mdev, skb_hwtstamps(skb),
					       timestamp);
		}
		skb_record_rx_queue(skb, cq_ring);

		if (likely(dev->features & NETIF_F_RXCSUM)) {
			/* TODO: For IP non TCP/UDP packets when csum complete is
			 * not an option (not supported or any other reason) we can
			 * actually check cqe IPOK status bit and report
			 * CHECKSUM_UNNECESSARY rather than CHECKSUM_NONE
			 */
			if ((cqe->status & cpu_to_be16(MLX4_CQE_STATUS_TCP |
						       MLX4_CQE_STATUS_UDP)) &&
			    (cqe->status & cpu_to_be16(MLX4_CQE_STATUS_IPOK)) &&
			    cqe->checksum == cpu_to_be16(0xffff)) {
#ifdef HAVE_NETDEV_HW_ENC_FEATURES
				bool l2_tunnel;

				l2_tunnel = (dev->hw_enc_features & NETIF_F_RXCSUM) &&
					(cqe->vlan_my_qpn & cpu_to_be32(MLX4_CQE_L2_TUNNEL));
#endif
				ip_summed = CHECKSUM_UNNECESSARY;
#ifdef HAVE_SKB_SET_HASH
				hash_type = PKT_HASH_TYPE_L4;
#endif
#ifdef HAVE_NETDEV_HW_ENC_FEATURES
				if (l2_tunnel)
#ifdef HAVE_SK_BUFF_CSUM_LEVEL
					skb->csum_level = 1;
#else
						skb->encapsulation = 1;
#endif
#endif
				ring->csum_ok++;
#ifdef CONFIG_COMPAT_LRO_ENABLED
					/* traffic eligible for LRO */
					if ((dev->features & NETIF_F_LRO) &&
					    mlx4_en_can_lro(cqe->status) &&
					    (ring->hwtstamp_rx_filter ==
					     HWTSTAMP_FILTER_NONE) &&
#ifdef HAVE_NETDEV_HW_ENC_FEATURES
					    !l2_tunnel &&
#endif
					    !(be32_to_cpu(cqe->vlan_my_qpn) &
					      (MLX4_CQE_CVLAN_PRESENT_MASK |
					       MLX4_CQE_SVLAN_PRESENT_MASK))) {
						int truesize = 0;
						struct skb_frag_struct lro_frag[MLX4_EN_MAX_RX_FRAGS];

						nr = mlx4_en_complete_rx_desc(priv, frags,
									      lro_frag,
									      length, &truesize);

						if (unlikely(nr < 0))
							goto next;

						/* Push it up the stack (LRO) */
						lro_receive_frags(&ring->lro.lro_mgr, lro_frag,
								  length, truesize, NULL, 0);
						goto next;
					}
#endif
			} else {
				if (!(priv->flags & MLX4_EN_FLAG_RX_CSUM_NON_TCP_UDP &&
				      (cqe->status & cpu_to_be16(MLX4_CQE_STATUS_IP_ANY))))
					goto csum_none;
				if (check_csum(cqe, skb, va, dev->features))
					goto csum_none;
				ip_summed = CHECKSUM_COMPLETE;
#ifdef HAVE_SKB_SET_HASH
				hash_type = PKT_HASH_TYPE_L3;
#endif
				ring->csum_complete++;
			}
		} else {
csum_none:
			ip_summed = CHECKSUM_NONE;
#ifdef HAVE_SKB_SET_HASH
			hash_type = PKT_HASH_TYPE_L3;
#endif
			ring->csum_none++;
		}
		skb->ip_summed = ip_summed;
#ifdef HAVE_NETIF_F_RXHASH
		if (dev->features & NETIF_F_RXHASH)
#ifdef HAVE_SKB_SET_HASH
			skb_set_hash(skb,
				     be32_to_cpu(cqe->immed_rss_invalid),
				     hash_type);
#else
			skb->rxhash = be32_to_cpu(cqe->immed_rss_invalid);
#endif
#endif

		if ((cqe->vlan_my_qpn &
		     cpu_to_be32(MLX4_CQE_CVLAN_PRESENT_MASK)) &&
		    (dev->features & NETIF_F_HW_VLAN_CTAG_RX))
#ifdef MLX4_EN_VLGRP
		{
			if (priv->vlgrp) {
#ifndef CONFIG_COMPAT_LRO_ENABLED
				nr = mlx4_en_complete_rx_desc(priv, frags, skb, length);
#else
				nr = mlx4_en_complete_rx_desc(priv, frags,
							      skb_shinfo(skb)->frags,
							      length, &skb->truesize);
#endif
				if (likely(nr >= 0)) {
#ifdef CONFIG_COMPAT_LRO_ENABLED
					skb_shinfo(skb)->nr_frags = nr;
#endif
					skb->len = length;
					skb->data_len = length;
					vlan_gro_frags(&cq->napi, priv->vlgrp,
						       be16_to_cpu(cqe->sl_vid));
				} else {
					skb->vlan_tci = 0;
					skb_clear_hash(skb);
				}
				goto next;
			}
#endif
#ifdef HAVE_3_PARAMS_FOR_VLAN_HWACCEL_PUT_TAG
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       be16_to_cpu(cqe->sl_vid));
#else
			__vlan_hwaccel_put_tag(skb, be16_to_cpu(cqe->sl_vid));
#endif
#ifdef MLX4_EN_VLGRP
		}
#endif
#ifdef HAVE_NETIF_F_HW_VLAN_STAG_RX
		else if ((cqe->vlan_my_qpn &
			  cpu_to_be32(MLX4_CQE_SVLAN_PRESENT_MASK)) &&
			 (dev->features & NETIF_F_HW_VLAN_STAG_RX))
#ifdef HAVE_3_PARAMS_FOR_VLAN_HWACCEL_PUT_TAG
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021AD),
					       be16_to_cpu(cqe->sl_vid));
#else
			__vlan_hwaccel_put_tag(skb, be16_to_cpu(cqe->sl_vid));
#endif
#endif

#ifndef CONFIG_COMPAT_LRO_ENABLED
		nr = mlx4_en_complete_rx_desc(priv, frags, skb, length);
#else
		nr = mlx4_en_complete_rx_desc(priv, frags,
					      skb_shinfo(skb)->frags,
					      length, &skb->truesize);
#endif
		if (likely(nr)) {
			skb_shinfo(skb)->nr_frags = nr;
			skb->len = length;
			skb->data_len = length;
			napi_gro_frags(&cq->napi);
		} else {
#ifdef HAVE__VLAN_HWACCEL_CLEAR_TAG
			__vlan_hwaccel_clear_tag(skb);
#else
			skb->vlan_tci = 0;
#endif
			skb_clear_hash(skb);
		}
next:
		++cq->mcq.cons_index;
		index = (cq->mcq.cons_index) & ring->size_mask;
		cqe = mlx4_en_get_cqe(cq->buf, index, priv->cqe_size) + factor;
		if (unlikely(++polled == budget))
			break;
	}

#ifdef HAVE_XDP_BUFF
	rcu_read_unlock();
#endif

	if (likely(polled)) {
#ifdef HAVE_XDP_BUFF
		if (doorbell_pending) {
			priv->tx_cq[TX_XDP][cq_ring]->xdp_busy = true;
			mlx4_en_xmit_doorbell(priv->tx_ring[TX_XDP][cq_ring]);
		}
#endif

		mlx4_cq_set_ci(&cq->mcq);
		wmb(); /* ensure HW sees CQ consumer before we post new buffers */
		ring->cons = cq->mcq.cons_index;
	}
	AVG_PERF_COUNTER(priv->pstats.rx_coal_avg, polled);
#ifdef CONFIG_COMPAT_LRO_ENABLED
	if (dev->features & NETIF_F_LRO)
		lro_flush_all(&priv->rx_ring[cq->ring]->lro.lro_mgr);
#endif

	mlx4_en_refill_rx_buffers(priv, ring);

	return polled;
}


void mlx4_en_rx_irq(struct mlx4_cq *mcq)
{
	struct mlx4_en_cq *cq = container_of(mcq, struct mlx4_en_cq, mcq);
	struct mlx4_en_priv *priv = netdev_priv(cq->dev);

	if (likely(priv->port_up))
		napi_schedule_irqoff(&cq->napi);
	else
		mlx4_en_arm_cq(priv, cq);
}

/* Rx CQ polling - called by NAPI */
int mlx4_en_poll_rx_cq(struct napi_struct *napi, int budget)
{
	struct mlx4_en_cq *cq = container_of(napi, struct mlx4_en_cq, napi);
	struct net_device *dev = cq->dev;
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_cq *xdp_tx_cq = NULL;
	bool clean_complete = true;
	int done;

	if (priv->tx_ring_num[TX_XDP]) {
		xdp_tx_cq = priv->tx_cq[TX_XDP][cq->ring];
		if (xdp_tx_cq->xdp_busy) {
			clean_complete = _mlx4_en_process_tx_cq(dev, xdp_tx_cq,
							       budget,
							       priv->tx_ring[TX_XDP][cq->ring]);
			xdp_tx_cq->xdp_busy = !clean_complete;
		}
	}

	if (!mlx4_en_cq_lock_napi(cq))
	{
		done = 0;
		goto complete;
	}

	done = mlx4_en_process_rx_cq(dev, cq, budget);

	mlx4_en_cq_unlock_napi(cq);

	/* If we used up all the quota - we're probably not done yet... */
#if !(defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED))
	cq->tot_rx += done;
#endif
	if (done == budget || !clean_complete) {
#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
		const struct cpumask *aff;
#ifndef HAVE_IRQ_DATA_AFFINITY
		struct irq_data *idata;
#endif
		int cpu_curr;
#endif

		/* in case we got here because of !clean_complete */
		done = budget;

		INC_PERF_COUNTER(priv->pstats.napi_quota);

#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
		cpu_curr = smp_processor_id();
#ifndef HAVE_IRQ_DATA_AFFINITY
		idata = irq_desc_get_irq_data(cq->irq_desc);
		aff = irq_data_get_affinity_mask(idata);
#else
		aff = irq_desc_get_irq_data(cq->irq_desc)->affinity;
#endif

		if (likely(cpumask_test_cpu(cpu_curr, aff)))
			return budget;

		/* Current cpu is not according to smp_irq_affinity -
		 * probably affinity changed. Need to stop this NAPI
		 * poll, and restart it on the right CPU.
		 * Try to avoid returning a too small value (like 0),
		 * to not fool net_rx_action() and its netdev_budget
		 */
		if (done)
			done--;
#else
		if (cq->tot_rx < MLX4_EN_MIN_RX_ARM)
			return budget;

		cq->tot_rx = 0;
		done = 0;
	} else {
		cq->tot_rx = 0;
#endif

	}
complete:
	/* Done for now */
#ifdef HAVE_NAPI_COMPLETE_DONE
#ifdef NAPI_COMPLETE_DONE_RET_VALUE
	if (likely(napi_complete_done(napi, done)))
		mlx4_en_arm_cq(priv, cq);
#else
	napi_complete_done(napi, done);
	mlx4_en_arm_cq(priv, cq);
#endif
#else
	napi_complete(napi);
	mlx4_en_arm_cq(priv, cq);
#endif
	return done;
}

void mlx4_en_calc_rx_buf(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	int eff_mtu = MLX4_EN_EFF_MTU(dev->mtu);
	int i = 0;

#ifdef HAVE_XDP_BUFF
	/* bpf requires buffers to be set up as 1 packet per page.
	 * This only works when num_frags == 1.
	 */
	if (priv->tx_ring_num[TX_XDP]) {
		priv->frag_info[0].frag_size = eff_mtu;
		/* This will gain efficient xdp frame recycling at the
		 * expense of more costly truesize accounting
		 */
		priv->frag_info[0].frag_stride = PAGE_SIZE;
		priv->dma_dir = PCI_DMA_BIDIRECTIONAL;
		priv->rx_headroom = XDP_PACKET_HEADROOM;
		i = 1;
	} else
#endif
	{
		int frag_size_max = 2048, buf_size = 0;

		/* should not happen, right ? */
		if (eff_mtu > PAGE_SIZE + (MLX4_EN_MAX_RX_FRAGS - 1) * 2048)
			frag_size_max = PAGE_SIZE;

		while (buf_size < eff_mtu) {
			int frag_stride, frag_size = eff_mtu - buf_size;
			int pad, nb;

			if (i < MLX4_EN_MAX_RX_FRAGS - 1)
				frag_size = min(frag_size, frag_size_max);

			priv->frag_info[i].frag_size = frag_size;
			frag_stride = ALIGN(frag_size, SMP_CACHE_BYTES);
			/* We can only pack 2 1536-bytes frames in on 4K page
			 * Therefore, each frame would consume more bytes (truesize)
			 */
			nb = PAGE_SIZE / frag_stride;
			pad = (PAGE_SIZE - nb * frag_stride) / nb;
			pad &= ~(SMP_CACHE_BYTES - 1);
			priv->frag_info[i].frag_stride = frag_stride + pad;

			buf_size += frag_size;
			i++;
		}
		priv->dma_dir = PCI_DMA_FROMDEVICE;
		priv->rx_headroom = 0;
	}

	priv->num_frags = i;
	priv->rx_skb_size = eff_mtu;
	priv->log_rx_info = ROUNDUP_LOG2(i * sizeof(struct mlx4_en_rx_alloc));

	en_dbg(DRV, priv, "Rx buffer scatter-list (effective-mtu:%d num_frags:%d):\n",
	       eff_mtu, priv->num_frags);
	for (i = 0; i < priv->num_frags; i++) {
		en_dbg(DRV,
		       priv,
		       "  frag:%d - size:%d stride:%d\n",
		       i,
		       priv->frag_info[i].frag_size,
		       priv->frag_info[i].frag_stride);
	}
}

/* RSS related functions */

static int mlx4_en_config_rss_qp(struct mlx4_en_priv *priv, int qpn,
				 struct mlx4_en_rx_ring *ring,
				 enum mlx4_qp_state *state,
				 struct mlx4_qp *qp)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_qp_context *context;
	int err = 0;

	context = kmalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

#ifdef HAVE_MEMALLOC_NOIO_SAVE
	err = mlx4_qp_alloc(mdev->dev, qpn, qp);
#else
	err = mlx4_qp_alloc(mdev->dev, qpn, qp, GFP_KERNEL);
#endif
	if (err) {
		en_err(priv, "Failed to allocate qp #%x\n", qpn);
		goto out;
	}
	qp->event = mlx4_en_sqp_event;

	memset(context, 0, sizeof(*context));
	mlx4_en_fill_qp_context(priv, ring->actual_size, ring->stride, 0, 0,
				qpn, ring->cqn, -1, context, MLX4_EN_NO_VLAN);
	context->db_rec_addr = cpu_to_be64(ring->wqres.db.dma);

	/* Cancel FCS removal if FW allows */
	if (mdev->dev->caps.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP) {
		context->param3 |= cpu_to_be32(1 << 29);
#ifdef HAVE_NETIF_F_RXFCS
		if (priv->dev->features & NETIF_F_RXFCS)
#else
		if (priv->pflags & MLX4_EN_PRIV_FLAGS_RXFCS)
#endif
			ring->fcs_del = 0;
		else
			ring->fcs_del = ETH_FCS_LEN;
	} else
		ring->fcs_del = 0;

	err = mlx4_qp_to_ready(mdev->dev, &ring->wqres.mtt, context, qp, state);
	if (err) {
		mlx4_qp_remove(mdev->dev, qp);
		mlx4_qp_free(mdev->dev, qp);
	}
	mlx4_en_update_rx_prod_db(ring);
out:
	kfree(context);
	return err;
}

int mlx4_en_create_drop_qp(struct mlx4_en_priv *priv)
{
	int err;
	u32 qpn;

	err = mlx4_qp_reserve_range(priv->mdev->dev, 1, 1, &qpn,
				    MLX4_RESERVE_A0_QP,
				    MLX4_RES_USAGE_DRIVER);
	if (err) {
		en_err(priv, "Failed reserving drop qpn\n");
		return err;
	}
#ifdef HAVE_MEMALLOC_NOIO_SAVE
	err = mlx4_qp_alloc(priv->mdev->dev, qpn, &priv->drop_qp);
#else
	err = mlx4_qp_alloc(priv->mdev->dev, qpn, &priv->drop_qp, GFP_KERNEL);
#endif
	if (err) {
		en_err(priv, "Failed allocating drop qp\n");
		mlx4_qp_release_range(priv->mdev->dev, qpn, 1);
		return err;
	}

	return 0;
}

void mlx4_en_destroy_drop_qp(struct mlx4_en_priv *priv)
{
	u32 qpn;

	qpn = priv->drop_qp.qpn;
	mlx4_qp_remove(priv->mdev->dev, &priv->drop_qp);
	mlx4_qp_free(priv->mdev->dev, &priv->drop_qp);
	mlx4_qp_release_range(priv->mdev->dev, qpn, 1);
}

/* Allocate rx qp's and configure them according to rss map */
int mlx4_en_config_rss_steer(struct mlx4_en_priv *priv)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_rss_map *rss_map = &priv->rss_map;
	struct mlx4_qp_context context;
	struct mlx4_rss_context *rss_context;
	int rss_rings;
	void *ptr;
	u8 rss_mask = (MLX4_RSS_IPV4 | MLX4_RSS_TCP_IPV4 | MLX4_RSS_IPV6 |
			MLX4_RSS_TCP_IPV6);
	int i, qpn;
	int err = 0;
	int good_qps = 0;
	u8 flags;

	en_dbg(DRV, priv, "Configuring rss steering\n");

	flags = priv->rx_ring_num == 1 ? MLX4_RESERVE_A0_QP : 0;
	err = mlx4_qp_reserve_range(mdev->dev, priv->rx_ring_num,
				    priv->rx_ring_num,
				    &rss_map->base_qpn, flags,
				    MLX4_RES_USAGE_DRIVER);
	if (err) {
		en_err(priv, "Failed reserving %d qps\n", priv->rx_ring_num);
		return err;
	}

	for (i = 0; i < priv->rx_ring_num; i++) {
		qpn = rss_map->base_qpn + i;
		err = mlx4_en_config_rss_qp(priv, qpn, priv->rx_ring[i],
					    &rss_map->state[i],
					    &rss_map->qps[i]);
		if (err)
			goto rss_err;

		++good_qps;
	}

	if (priv->rx_ring_num == 1) {
		rss_map->indir_qp = &rss_map->qps[0];
		priv->base_qpn = rss_map->indir_qp->qpn;
		en_info(priv, "Optimized Non-RSS steering\n");
		return 0;
	}

	rss_map->indir_qp = kzalloc(sizeof(*rss_map->indir_qp), GFP_KERNEL);
	if (!rss_map->indir_qp) {
		err = -ENOMEM;
		goto rss_err;
	}

	/* Configure RSS indirection qp */
#ifdef HAVE_MEMALLOC_NOIO_SAVE
	err = mlx4_qp_alloc(mdev->dev, priv->base_qpn, rss_map->indir_qp);
#else
	err = mlx4_qp_alloc(mdev->dev, priv->base_qpn, rss_map->indir_qp,
			    GFP_KERNEL);
#endif
	if (err) {
		en_err(priv, "Failed to allocate RSS indirection QP\n");
		goto rss_err;
	}

	rss_map->indir_qp->event = mlx4_en_sqp_event;
	mlx4_en_fill_qp_context(priv, 0, 0, 0, 1, priv->base_qpn,
				priv->rx_ring[0]->cqn, -1, &context, MLX4_EN_NO_VLAN);

	if (!priv->prof->rss_rings || priv->prof->rss_rings > priv->rx_ring_num)
		rss_rings = priv->rx_ring_num;
	else
		rss_rings = priv->prof->rss_rings;

	ptr = ((void *) &context) + offsetof(struct mlx4_qp_context, pri_path)
					+ MLX4_RSS_OFFSET_IN_QPC_PRI_PATH;
	rss_context = ptr;
	rss_context->base_qpn = cpu_to_be32(ilog2(rss_rings) << 24 |
					    (rss_map->base_qpn));
	rss_context->default_qpn = cpu_to_be32(rss_map->base_qpn);
	if (priv->mdev->profile.udp_rss) {
		rss_mask |=  MLX4_RSS_UDP_IPV4 | MLX4_RSS_UDP_IPV6;
		rss_context->base_qpn_udp = rss_context->default_qpn;
	}

	if (mdev->dev->caps.tunnel_offload_mode == MLX4_TUNNEL_OFFLOAD_MODE_VXLAN) {
		en_info(priv, "Setting RSS context tunnel type to RSS on inner headers\n");
		rss_mask |= MLX4_RSS_BY_INNER_HEADERS;
	}

	rss_context->flags = rss_mask;
	rss_context->hash_fn = MLX4_RSS_HASH_TOP;
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
	if (priv->rss_hash_fn == ETH_RSS_HASH_XOR) {
#else
	if (priv->pflags & MLX4_EN_PRIV_FLAGS_RSS_HASH_XOR) {
#endif
		rss_context->hash_fn = MLX4_RSS_HASH_XOR;
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
	} else if (priv->rss_hash_fn == ETH_RSS_HASH_TOP) {
#else
	} else if (!(priv->pflags & MLX4_EN_PRIV_FLAGS_RSS_HASH_XOR)) {
#endif
		rss_context->hash_fn = MLX4_RSS_HASH_TOP;
		memcpy(rss_context->rss_key, priv->rss_key,
		       MLX4_EN_RSS_KEY_SIZE);
	} else {
		en_err(priv, "Unknown RSS hash function requested\n");
		err = -EINVAL;
		goto indir_err;
	}

	err = mlx4_qp_to_ready(mdev->dev, &priv->res.mtt, &context,
			       rss_map->indir_qp, &rss_map->indir_state);
	if (err)
		goto indir_err;

	return 0;

indir_err:
	mlx4_qp_modify(mdev->dev, NULL, rss_map->indir_state,
		       MLX4_QP_STATE_RST, NULL, 0, 0, rss_map->indir_qp);
	mlx4_qp_remove(mdev->dev, rss_map->indir_qp);
	mlx4_qp_free(mdev->dev, rss_map->indir_qp);
	kfree(rss_map->indir_qp);
	rss_map->indir_qp = NULL;
rss_err:
	for (i = 0; i < good_qps; i++) {
		mlx4_qp_modify(mdev->dev, NULL, rss_map->state[i],
			       MLX4_QP_STATE_RST, NULL, 0, 0, &rss_map->qps[i]);
		mlx4_qp_remove(mdev->dev, &rss_map->qps[i]);
		mlx4_qp_free(mdev->dev, &rss_map->qps[i]);
	}
	mlx4_qp_release_range(mdev->dev, rss_map->base_qpn, priv->rx_ring_num);
	return err;
}

void mlx4_en_release_rss_steer(struct mlx4_en_priv *priv)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_rss_map *rss_map = &priv->rss_map;
	int i;

	if (priv->rx_ring_num > 1) {
		mlx4_qp_modify(mdev->dev, NULL, rss_map->indir_state,
			       MLX4_QP_STATE_RST, NULL, 0, 0,
			       rss_map->indir_qp);
		mlx4_qp_remove(mdev->dev, rss_map->indir_qp);
		mlx4_qp_free(mdev->dev, rss_map->indir_qp);
		kfree(rss_map->indir_qp);
		rss_map->indir_qp = NULL;
	}

	for (i = 0; i < priv->rx_ring_num; i++) {
		mlx4_qp_modify(mdev->dev, NULL, rss_map->state[i],
			       MLX4_QP_STATE_RST, NULL, 0, 0, &rss_map->qps[i]);
		mlx4_qp_remove(mdev->dev, &rss_map->qps[i]);
		mlx4_qp_free(mdev->dev, &rss_map->qps[i]);
	}
	mlx4_qp_release_range(mdev->dev, rss_map->base_qpn, priv->rx_ring_num);
}
