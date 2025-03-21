/*
 * Copyright (c) 2015-2016, Mellanox Technologies. All rights reserved.
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
#ifndef __MLX5_EN_H__
#define __MLX5_EN_H__

#ifdef HAVE_XDP_BUFF
#include <linux/bpf.h>
#endif
#include <linux/if_vlan.h>
#include <linux/etherdevice.h>
#include <linux/timecounter.h>
#include <linux/clocksource.h>
#include <linux/net_tstamp.h>
#if defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED)
#include <linux/hashtable.h>
#endif
#if defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
#include <linux/ptp_clock_kernel.h>
#endif
#include <linux/crash_dump.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/qp.h>
#include <linux/mlx5/cq.h>
#include <linux/mlx5/port.h>
#include <linux/mlx5/vport.h>
#include <linux/mlx5/transobj.h>
#include <linux/mlx5/fs.h>
#include <linux/mlx5/fs.h>
#ifdef HAVE_TC_FLOWER_OFFLOAD
#include <linux/rhashtable.h>
#endif
#include <net/switchdev.h>
#include <net/xdp.h>
#include <linux/net_dim.h>
#ifdef HAVE_BITS_H
#include <linux/bits.h>
#endif
#include <linux/mlx5/accel.h>
#include "wq.h"
#include "mlx5_core.h"
#include "en_stats.h"
#include "en/fs.h"

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
#include <linux/inet_lro.h>
#else
#include <net/ip.h>
#endif

#ifndef HAVE_DEVLINK_HEALTH_REPORT_SUPPORT
/* The intention is to pass NULL for backports of old kernels */
struct devlink_health_reporter {};
#endif /* HAVE_DEVLINK_HEALTH_REPORT_SUPPORT */

extern const struct net_device_ops mlx5e_netdev_ops;
#ifdef HAVE_NET_PAGE_POOL_H
struct page_pool;
#endif

#define MLX5E_METADATA_ETHER_TYPE (0x8CE4)
#define MLX5E_METADATA_ETHER_LEN 8

#define MLX5_SET_CFG(p, f, v) MLX5_SET(create_flow_group_in, p, f, v)

#define MLX5E_ETH_HARD_MTU (ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN)

#define MLX5E_HW2SW_MTU(params, hwmtu) ((hwmtu) - ((params)->hard_mtu))
#define MLX5E_SW2HW_MTU(params, swmtu) ((swmtu) + ((params)->hard_mtu))

#define MLX5E_MAX_PRIORITY      8
#define MLX5E_MAX_DSCP          64
#define MLX5E_MAX_NUM_TC	8
#define MLX5E_MIN_NUM_TC	0

#define MLX5_RX_HEADROOM NET_SKB_PAD
#define MLX5_SKB_FRAG_SZ(len)	(SKB_DATA_ALIGN(len) +	\
				 SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))

#define MLX5E_RX_MAX_HEAD (256)

#define MLX5_MPWRQ_MIN_LOG_STRIDE_SZ(mdev) \
	(6 + MLX5_CAP_GEN(mdev, cache_line_128byte)) /* HW restriction */
#define MLX5_MPWRQ_LOG_STRIDE_SZ(mdev, req) \
	max_t(u32, MLX5_MPWRQ_MIN_LOG_STRIDE_SZ(mdev), req)
#define MLX5_MPWRQ_DEF_LOG_STRIDE_SZ(mdev) \
	MLX5_MPWRQ_LOG_STRIDE_SZ(mdev, order_base_2(MLX5E_RX_MAX_HEAD))

#ifndef CONFIG_ENABLE_CX4LX_OPTIMIZATIONS
#define MLX5_MPWRQ_LOG_WQE_SZ			18
#else
#define MLX5_MPWRQ_LOG_WQE_SZ			20
#endif
#define MLX5_MPWRQ_WQE_PAGE_ORDER  (MLX5_MPWRQ_LOG_WQE_SZ - PAGE_SHIFT > 0 ? \
				    MLX5_MPWRQ_LOG_WQE_SZ - PAGE_SHIFT : 0)
#define MLX5_MPWRQ_PAGES_PER_WQE		BIT(MLX5_MPWRQ_WQE_PAGE_ORDER)

#define MLX5_MTT_OCTW(npages) (ALIGN(npages, 8) / 2)
#define MLX5E_REQUIRED_WQE_MTTS		(ALIGN(MLX5_MPWRQ_PAGES_PER_WQE, 8))
#define MLX5E_LOG_ALIGNED_MPWQE_PPW	(ilog2(MLX5E_REQUIRED_WQE_MTTS))
#define MLX5E_REQUIRED_MTTS(wqes)	(wqes * MLX5E_REQUIRED_WQE_MTTS)
#define MLX5E_MAX_RQ_NUM_MTTS	\
	((1 << 16) * 2) /* So that MLX5_MTT_OCTW(num_mtts) fits into u16 */
#define MLX5E_ORDER2_MAX_PACKET_MTU (order_base_2(10 * 1024))
#define MLX5E_PARAMS_MAXIMUM_LOG_RQ_SIZE_MPW	\
		(ilog2(MLX5E_MAX_RQ_NUM_MTTS / MLX5E_REQUIRED_WQE_MTTS))
#define MLX5E_LOG_MAX_RQ_NUM_PACKETS_MPW \
	(MLX5E_PARAMS_MAXIMUM_LOG_RQ_SIZE_MPW + \
	 (MLX5_MPWRQ_LOG_WQE_SZ - MLX5E_ORDER2_MAX_PACKET_MTU))

#define MLX5E_MIN_SKB_FRAG_SZ		(MLX5_SKB_FRAG_SZ(MLX5_RX_HEADROOM))
#define MLX5E_LOG_MAX_RX_WQE_BULK	\
	(ilog2(PAGE_SIZE / roundup_pow_of_two(MLX5E_MIN_SKB_FRAG_SZ)))

#define MLX5E_PARAMS_MINIMUM_LOG_SQ_SIZE                0x6
#define MLX5E_PARAMS_DEFAULT_LOG_SQ_SIZE                0xa
#define MLX5E_PARAMS_MAXIMUM_LOG_SQ_SIZE                0xd

#define MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE (1 + MLX5E_LOG_MAX_RX_WQE_BULK)
#define MLX5E_PARAMS_DEFAULT_LOG_RQ_SIZE                0xa
#define MLX5E_PARAMS_MAXIMUM_LOG_RQ_SIZE min_t(u8, 0xd,	\
					       MLX5E_LOG_MAX_RQ_NUM_PACKETS_MPW)

#define MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE_MPW            0x2

#define MLX5E_PARAMS_DEFAULT_LRO_WQE_SZ                 (64 * 1024)

#ifdef CONFIG_PPC
#define MLX5E_DEFAULT_LRO_TIMEOUT                       1024
#else
#define MLX5E_DEFAULT_LRO_TIMEOUT                       32
#endif
#define MLX5E_LRO_TIMEOUT_ARR_SIZE                      4

#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC      0x10
#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC_FROM_CQE 0x3
#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_PKTS      0x20
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC      0x10
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC_FROM_CQE 0x10
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_PKTS      0x20
#define MLX5E_PARAMS_DEFAULT_MIN_RX_WQES                0x80
#define MLX5E_PARAMS_DEFAULT_MIN_RX_WQES_MPW            0x2

#define MLX5E_LOG_INDIR_RQT_SIZE       0x7
#define MLX5E_INDIR_RQT_SIZE           BIT(MLX5E_LOG_INDIR_RQT_SIZE)
#define MLX5E_MIN_NUM_CHANNELS         0x1
#define MLX5E_MAX_NUM_CHANNELS         MLX5E_INDIR_RQT_SIZE
#define MLX5E_MAX_NUM_SQS              (MLX5E_MAX_NUM_CHANNELS * MLX5E_MAX_NUM_TC)

#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
#define MLX5E_MAX_RL_QUEUES            512
#else
#define MLX5E_MAX_RL_QUEUES            0
#endif

#define MLX5E_TX_CQ_POLL_BUDGET        128
#define MLX5E_SQ_RECOVER_MIN_INTERVAL  500 /* msecs */

#ifndef CONFIG_ENABLE_CX4LX_OPTIMIZATIONS
#define MLX5E_UMR_WQE_INLINE_SZ \
	(sizeof(struct mlx5e_umr_wqe) + \
	 ALIGN(MLX5_MPWRQ_PAGES_PER_WQE * sizeof(struct mlx5_mtt), \
	       MLX5_UMR_MTT_ALIGNMENT))
#define MLX5E_UMR_WQEBBS \
	(DIV_ROUND_UP(MLX5E_UMR_WQE_INLINE_SZ, MLX5_SEND_WQE_BB))
#else
#define MLX5E_UMR_WQE_MTT_SZ \
	(ALIGN(MLX5_MPWRQ_PAGES_PER_WQE * sizeof(struct mlx5_mtt), \
	       MLX5_UMR_MTT_ALIGNMENT))
#define MLX5E_UMR_WQEBBS \
	(DIV_ROUND_UP(sizeof(struct mlx5e_umr_wqe), MLX5_SEND_WQE_BB))
#endif

#define MLX5E_MSG_LEVEL			NETIF_MSG_LINK

#define mlx5e_dbg(mlevel, priv, format, ...)                    \
do {                                                            \
	if (NETIF_MSG_##mlevel & (priv)->msglevel)              \
		netdev_warn(priv->netdev, format,               \
			    ##__VA_ARGS__);                     \
} while (0)


static inline u8 mlx5e_get_num_lag_ports(struct mlx5_core_dev *mdev)
{
	u8 ret = min_t(u8, MLX5_MAX_PORTS, MLX5_CAP_GEN(mdev, num_lag_ports));

	return ret ? : 1;
}

static inline u16 mlx5_min_rx_wqes(int wq_type, u32 wq_size)
{
	switch (wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		return min_t(u16, MLX5E_PARAMS_DEFAULT_MIN_RX_WQES_MPW,
			     wq_size / 2);
	default:
		return min_t(u16, MLX5E_PARAMS_DEFAULT_MIN_RX_WQES,
			     wq_size / 2);
	}
}

/* Use this function to get max num channels (rxqs/txqs) only to create netdev */
static inline int mlx5e_get_max_num_channels(struct mlx5_core_dev *mdev)
{
	return is_kdump_kernel() ?
		MLX5E_MIN_NUM_CHANNELS :
		min_t(int, mlx5_comp_vectors_count(mdev), MLX5E_MAX_NUM_CHANNELS);
}

enum {
	MLX5E_CON_PROTOCOL_802_1_RP,
	MLX5E_CON_PROTOCOL_R_ROCE_RP,
	MLX5E_CON_PROTOCOL_R_ROCE_NP,
	MLX5E_CONG_PROTOCOL_NUM,
};

struct mlx5e_tx_wqe {
	struct mlx5_wqe_ctrl_seg ctrl;
	union {
		struct {
			struct mlx5_wqe_eth_seg  eth;
			struct mlx5_wqe_data_seg data[0];
		};
		u8 tls_progress_params_ctx[0];
	};
};

struct mlx5e_rx_wqe_ll {
	struct mlx5_wqe_srq_next_seg  next;
	struct mlx5_wqe_data_seg      data[0];
};

struct mlx5e_rx_wqe_cyc {
	struct mlx5_wqe_data_seg      data[0];
};

struct mlx5e_umr_wqe {
	struct mlx5_wqe_ctrl_seg       ctrl;
	struct mlx5_wqe_umr_ctrl_seg   uctrl;
	struct mlx5_mkey_seg           mkc;
	union {
#ifndef CONFIG_ENABLE_CX4LX_OPTIMIZATIONS
		struct mlx5_mtt        inline_mtts[0];
#else
		struct mlx5_wqe_data_seg data;
#endif
		u8                     tls_static_params_ctx[0];
	};
};

extern const char mlx5e_self_tests[][ETH_GSTRING_LEN];

enum mlx5e_priv_flag {
	MLX5E_PFLAG_RX_CQE_BASED_MODER,
	MLX5E_PFLAG_TX_CQE_BASED_MODER,
	MLX5E_PFLAG_RX_CQE_COMPRESS,
	MLX5E_PFLAG_TX_CQE_COMPRESS,
	MLX5E_PFLAG_RX_STRIDING_RQ,
	MLX5E_PFLAG_RX_NO_CSUM_COMPLETE,
#ifdef HAVE_XDP_BUFF
	MLX5E_PFLAG_XDP_TX_MPWQE,
#endif
	MLX5E_PFLAG_SNIFFER,
	MLX5E_PFLAG_DROPLESS_RQ,
	MLX5E_PFLAG_PER_CH_STATS ,
#ifdef HAVE_XDP_BUFF
	MLX5E_PFLAG_TX_XDP_CSUM ,
#endif
	/* OFED-specific private flags */
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	MLX5E_PFLAG_HWLRO,
#endif
	MLX5E_NUM_PFLAGS, /* Keep last */
};

#define MLX5E_SET_PFLAG(params, pflag, enable)			\
	do {							\
		if (enable)					\
			(params)->pflags |= BIT(pflag);		\
		else						\
			(params)->pflags &= ~(BIT(pflag));	\
	} while (0)

#define MLX5E_GET_PFLAG(params, pflag) (!!((params)->pflags & (BIT(pflag))))

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
#define MLX5E_MAX_BW_ALLOC 100 /* Max percentage of BW allocation */
#endif
#endif

struct mlx5e_params {
	u8  log_sq_size;
	u8  rq_wq_type;
	u8  log_rq_mtu_frames;
	u8  log_rx_page_cache_mult;
	u16 num_channels;
	u8  num_tc;
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	u16 num_rl_txqs;
#endif
	bool rx_cqe_compress_def;
	struct net_dim_cq_moder rx_cq_moderation;
	struct net_dim_cq_moder tx_cq_moderation;
	bool tunneled_offload_en;
	bool lro_en;
	u8  tx_min_inline_mode;
	bool vlan_strip_disable;
	bool scatter_fcs_en;
	bool rx_dim_enabled;
	bool tx_dim_enabled;
	u32 lro_timeout;
	u32 pflags;
#if defined(HAVE_VLAN_GRO_RECEIVE) || defined(HAVE_VLAN_HWACCEL_RX)
	struct vlan_group          *vlan_grp;
#endif
#ifdef HAVE_XDP_BUFF
	struct bpf_prog *xdp_prog;
#endif
	unsigned int sw_mtu;
	int hard_mtu;
#ifdef HAVE_GET_SET_DUMP
	struct {
		__u32 flag;
		u32 mst_size;
	}                          dump;
#endif
};

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
struct mlx5e_cee_config {
	/* bw pct for priority group */
	u8                         pg_bw_pct[CEE_DCBX_MAX_PGS];
	u8                         prio_to_pg_map[CEE_DCBX_MAX_PRIO];
	bool                       pfc_setting[CEE_DCBX_MAX_PRIO];
	bool                       pfc_enable;
};

enum {
	MLX5_DCB_CHG_RESET,
	MLX5_DCB_NO_CHG,
	MLX5_DCB_CHG_NO_RESET,
};

struct mlx5e_dcbx {
	enum mlx5_dcbx_oper_mode   mode;
	struct mlx5e_cee_config    cee_cfg; /* pending configuration */
	u8                         dscp_app_cnt;

	/* The only setting that cannot be read from FW */
	u8                         tc_tsa[IEEE_8021QAZ_MAX_TCS];
	u8                         cap;

	/* Buffer configuration */
	bool                       manual_buffer;
	u32                        cable_len;
	u32                        xoff;
	u16                        port_buff_cell_sz;
};

struct mlx5e_dcbx_dp {
	u8                         dscp2prio[MLX5E_MAX_DSCP];
	u8                         trust_state;
};
#endif
#endif

enum {
	MLX5E_RQ_STATE_ENABLED,
	MLX5E_RQ_STATE_RECOVERING,
	MLX5E_RQ_STATE_AM,
	MLX5E_RQ_STATE_NO_CSUM_COMPLETE,
	MLX5E_RQ_STATE_CSUM_FULL, /* cqe_csum_full hw bit is set */
	MLX5E_RQ_STATE_CACHE_REDUCE_PENDING,
};

struct mlx5e_cq {
	/* data path - accessed per cqe */
	struct mlx5_cqwq           wq;

	/* data path - accessed per napi poll */
	u16                        event_ctr;
	struct napi_struct        *napi;
	struct mlx5_core_cq        mcq;
	struct mlx5e_channel      *channel;

	/* control */
	struct mlx5_core_dev      *mdev;
	struct mlx5_wq_ctrl        wq_ctrl;
} ____cacheline_aligned_in_smp;

struct mlx5e_cq_decomp {
	/* cqe decompression */
	struct mlx5_cqe64          title;
	struct mlx5_mini_cqe8      mini_arr[MLX5_MINI_CQE_ARRAY_SIZE];
	u8                         mini_arr_idx;
	u16                        left;
	u16                        wqe_counter;
} ____cacheline_aligned_in_smp;

struct mlx5e_tx_wqe_info {
	struct sk_buff *skb;
	u32 num_bytes;
	u8  num_wqebbs;
	u8  num_dma;
#ifdef CONFIG_MLX5_EN_TLS
	struct page *resync_dump_frag_page;
#endif
};

enum mlx5e_dma_map_type {
	MLX5E_DMA_MAP_SINGLE,
	MLX5E_DMA_MAP_PAGE
};

struct mlx5e_sq_dma {
	dma_addr_t              addr;
	u32                     size;
	enum mlx5e_dma_map_type type;
};

enum {
	MLX5E_SQ_STATE_ENABLED,
	MLX5E_SQ_STATE_RECOVERING,
	MLX5E_SQ_STATE_IPSEC,
	MLX5E_SQ_STATE_AM,
	MLX5E_SQ_STATE_TLS,
	MLX5E_SQ_STATE_VLAN_NEED_L2_INLINE,
#ifdef HAVE_XDP_REDIRECT
	MLX5E_SQ_STATE_REDIRECT,
#endif

};

struct mlx5e_sq_wqe_info {
	u8  opcode;
	u8 num_wqebbs;
};

#if defined(CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
struct mlx5e_sq_flow_map {
	struct hlist_node hlist;
	u32               dst_ip;
	u16               dst_port;
	u16               queue_index;
};
#endif

struct mlx5e_dim {
	struct net_dim dim;
	struct net_dim_sample sample;
};

struct mlx5e_txqsq {
	/* dirtied @completion */
	u16                        cc;
	u32                        dma_fifo_cc;
	struct mlx5e_dim           dim_obj; /* Adaptive Moderation */

	/* dirtied @xmit */
	u16                        pc ____cacheline_aligned_in_smp;
	u32                        dma_fifo_pc;
#ifdef CONFIG_MLX5_IPSEC
	struct mlx5_accel_trailer  trailer;
#endif
	struct mlx5e_cq            cq;
	struct mlx5e_cq_decomp     cqd;

	/* read only */
	struct mlx5_wq_cyc         wq;
	u32                        dma_fifo_mask;
	struct mlx5e_sq_stats     *stats;
	struct {
		struct mlx5e_sq_dma       *dma_fifo;
		struct mlx5e_tx_wqe_info  *wqe_info;
	} db;
	void __iomem              *uar_map;
	struct netdev_queue       *txq;
	u32                        sqn;
	u16                        stop_room;
	u8                         min_inline_mode;
	struct device             *pdev;
	__be32                     mkey_be;
	unsigned long              state;
	unsigned int               hw_mtu;
	struct hwtstamp_config    *tstamp;
	struct mlx5_clock         *clock;

	/* control path */
	struct mlx5_wq_ctrl        wq_ctrl;
	struct mlx5e_channel      *channel;
	int                        ch_ix;
	int                        txq_ix;
	u32                        rate_limit;
	struct work_struct         recover_work;
#if defined(CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	struct mlx5e_sq_flow_map   flow_map;
#endif
} ____cacheline_aligned_in_smp;

struct mlx5e_dma_info {
	struct page     *page;
	dma_addr_t      addr;
	u32 refcnt_bias;
};

#ifdef HAVE_XDP_BUFF
struct mlx5e_xdp_info {
	struct xdp_frame      *xdpf;
	dma_addr_t            dma_addr;
	struct mlx5e_dma_info di;
};

struct mlx5e_xdp_info_fifo {
	struct mlx5e_xdp_info *xi;
	u32 *cc;
	u32 *pc;
	u32 mask;
};

struct mlx5e_xdp_wqe_info {
	u8 num_wqebbs;
	u8 num_pkts;
};

struct mlx5e_xdp_mpwqe {
	/* Current MPWQE session */
	struct mlx5e_tx_wqe *wqe;
	u8                   ds_count;
	u8                   pkt_count;
	u8                   max_ds_count;
	u8                   complete;
	u8                   inline_on;
};

struct mlx5e_xdpsq;
typedef bool (*mlx5e_fp_xmit_xdp_frame)(struct mlx5e_xdpsq*,
					struct mlx5e_xdp_info*);
struct mlx5e_xdpsq {
	/* data path */

	/* dirtied @completion */
	u32                        xdpi_fifo_cc;
	u16                        cc;

	/* dirtied @xmit */
	u32                        xdpi_fifo_pc ____cacheline_aligned_in_smp;
	u16                        pc;
	struct mlx5_wqe_ctrl_seg   *doorbell_cseg;
	struct mlx5e_xdp_mpwqe     mpwqe;

	struct mlx5e_cq            cq;

	/* read only */
	struct mlx5_wq_cyc         wq;
	struct mlx5e_xdpsq_stats  *stats;
	mlx5e_fp_xmit_xdp_frame    xmit_xdp_frame;
	struct {
		struct mlx5e_xdp_wqe_info *wqe_info;
		struct mlx5e_xdp_info_fifo xdpi_fifo;
	} db;
	void __iomem              *uar_map;
	u32                        sqn;
	struct device             *pdev;
	__be32                     mkey_be;
	u8                         min_inline_mode;
	unsigned long              state;
	unsigned int               hw_mtu;

	/* control path */
	struct mlx5_wq_ctrl        wq_ctrl;
	struct mlx5e_channel      *channel;
} ____cacheline_aligned_in_smp;
#endif

struct mlx5e_icosq {
	/* data path */
	u16                        cc;
	u16                        pc;

	struct mlx5_wqe_ctrl_seg  *doorbell_cseg;
	struct mlx5e_cq            cq;

	/* write@xmit, read@completion */
	struct {
		struct mlx5e_sq_wqe_info *ico_wqe;
	} db;

	/* read only */
	struct mlx5_wq_cyc         wq;
	void __iomem              *uar_map;
	u32                        sqn;
	unsigned long              state;

	/* control path */
	struct mlx5_wq_ctrl        wq_ctrl;
	struct mlx5e_channel      *channel;

	struct work_struct         recover_work;
} ____cacheline_aligned_in_smp;

struct mlx5e_wqe_frag_info {
	struct mlx5e_dma_info *di;
	u32 offset;
	bool last_in_page;
};

struct mlx5e_umr_dma_info {
	struct mlx5e_dma_info  dma_info[MLX5_MPWRQ_PAGES_PER_WQE];
#ifdef CONFIG_ENABLE_CX4LX_OPTIMIZATIONS
	struct mlx5_mtt       *mtt;
	dma_addr_t             mtt_addr;
#endif
};

struct mlx5e_mpw_info {
	struct mlx5e_umr_dma_info umr;
	u16 consumed_strides;
#ifdef HAVE_XDP_BUFF
	DECLARE_BITMAP(xdp_xmit_bitmap, MLX5_MPWRQ_PAGES_PER_WQE);
#endif
};

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
#define IS_HW_LRO(params) \
	((params)->lro_en && MLX5E_GET_PFLAG(params, MLX5E_PFLAG_HWLRO))
#define IS_SW_LRO(params) \
	((params)->lro_en && !MLX5E_GET_PFLAG(params, MLX5E_PFLAG_HWLRO))

/* SW LRO defines for MLX5 */
#define MLX5E_LRO_MAX_DESC	32
struct mlx5e_sw_lro {
	struct net_lro_mgr	lro_mgr;
	struct net_lro_desc	lro_desc[MLX5E_LRO_MAX_DESC];
};
#endif

#define MLX5E_MAX_RX_FRAGS 4

#define MLX5E_PAGE_CACHE_LOG_MAX_RQ_MULT	4
#define MLX5E_PAGE_CACHE_REDUCE_WORK_INTERVAL	200 /* msecs */
#define MLX5E_PAGE_CACHE_REDUCE_GRACE_PERIOD	1000 /* msecs */
#define MLX5E_PAGE_CACHE_REDUCE_SUCCESSIVE_CNT	5

struct mlx5e_page_cache_reduce {
	struct delayed_work reduce_work;
	u32 successive;
	unsigned long next_ts;
	unsigned long graceful_period;
	unsigned long delay;

	struct mlx5e_dma_info *pending;
	u32 npages;
};

struct mlx5e_page_cache {
	struct mlx5e_dma_info *page_cache;
	int head;
	u32 sz;
	u32 lrs; /* least recently sampled */
	u8 log_min_sz;
	u8 log_max_sz;
	struct mlx5e_page_cache_reduce reduce;
};

static inline void mlx5e_put_page(struct mlx5e_dma_info *dma_info)
{
	page_ref_sub(dma_info->page, dma_info->refcnt_bias);
	put_page(dma_info->page);
}

struct mlx5e_rq;
typedef void (*mlx5e_fp_handle_rx_cqe)(struct mlx5e_rq*, struct mlx5_cqe64*);
typedef struct sk_buff *
(*mlx5e_fp_skb_from_cqe_mpwrq)(struct mlx5e_rq *rq, struct mlx5e_mpw_info *wi,
			       u16 cqe_bcnt, u32 head_offset, u32 page_idx);
typedef struct sk_buff *
(*mlx5e_fp_skb_from_cqe)(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe,
			 struct mlx5e_wqe_frag_info *wi, u32 cqe_bcnt);
typedef bool (*mlx5e_fp_post_rx_wqes)(struct mlx5e_rq *rq);
typedef void (*mlx5e_fp_dealloc_wqe)(struct mlx5e_rq*, u16);

enum mlx5e_rq_flag {
	MLX5E_RQ_FLAG_XDP_XMIT,
	MLX5E_RQ_FLAG_XDP_REDIRECT,
};

struct mlx5e_rq_frag_info {
	int frag_size;
	int frag_stride;
};

struct mlx5e_rq_frags_info {
	struct mlx5e_rq_frag_info arr[MLX5E_MAX_RX_FRAGS];
	u8 num_frags;
	u8 log_num_frags;
	u8 wqe_bulk;
};

struct mlx5e_rq {
	/* data path */
	union {
		struct {
			struct mlx5_wq_cyc          wq;
			struct mlx5e_wqe_frag_info *frags;
			struct mlx5e_dma_info      *di;
			struct mlx5e_rq_frags_info  info;
			mlx5e_fp_skb_from_cqe       skb_from_cqe;
		} wqe;
		struct {
			struct mlx5_wq_ll      wq;
			struct mlx5e_umr_wqe   umr_wqe;
#ifdef CONFIG_ENABLE_CX4LX_OPTIMIZATIONS
			void                  *mtt_no_align;
#endif
			struct mlx5e_mpw_info *info;
			mlx5e_fp_skb_from_cqe_mpwrq skb_from_cqe_mpwrq;
			u16                    num_strides;
			u16                    actual_wq_head;
			u8                     log_stride_sz;
			u8                     umr_in_progress;
			u8                     umr_last_bulk;
		} mpwqe;
	};
	struct {
		u16            headroom;
		u8             map_dir;   /* dma map direction */
	} buff;

	struct mlx5e_channel  *channel;
	struct device         *pdev;
	struct net_device     *netdev;
	struct mlx5e_rq_stats *stats;
	struct mlx5e_cq        cq;
	struct mlx5e_cq_decomp cqd;
	struct mlx5e_page_cache page_cache;
	struct hwtstamp_config *tstamp;
	struct mlx5_clock      *clock;

	mlx5e_fp_handle_rx_cqe handle_rx_cqe;
	mlx5e_fp_post_rx_wqes  post_wqes;
	mlx5e_fp_dealloc_wqe   dealloc_wqe;

	unsigned long          state;
	int                    ix;
	unsigned int           hw_mtu;

	struct mlx5e_dim       dim_obj; /* Adaptive Moderation */

	/* XDP */
#ifdef HAVE_XDP_BUFF
	struct bpf_prog       *xdp_prog;
	struct mlx5e_xdpsq     xdpsq;
#endif
	DECLARE_BITMAP(flags, 8);
#ifdef HAVE_NET_PAGE_POOL_H
	struct page_pool      *page_pool;
#endif

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_sw_lro   *sw_lro;
#endif

	struct work_struct     recover_work;

	/* control */
	struct mlx5_wq_ctrl    wq_ctrl;
	__be32                 mkey_be;
	u8                     wq_type;
	u32                    rqn;
	struct mlx5_core_dev  *mdev;
	struct mlx5_core_mkey  umr_mkey;

	/* XDP read-mostly */
#ifdef HAVE_NET_XDP_H
	struct xdp_rxq_info    xdp_rxq;
#endif
} ____cacheline_aligned_in_smp;

#ifndef HAVE_NAPI_STATE_MISSED
enum channel_flags {
	MLX5E_CHANNEL_NAPI_SCHED = 1,
};
#endif
struct mlx5e_channel {
	/* data path */
	struct mlx5e_rq            rq;
	struct mlx5e_txqsq         sq[MLX5E_MAX_NUM_TC];
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	struct mlx5e_txqsq         *special_sq;
	u16			   num_special_sq;
#endif
	struct mlx5e_icosq         icosq;   /* internal control operations */
#ifdef HAVE_XDP_BUFF
	bool                       xdp;
#endif
	struct napi_struct         napi;
	struct device             *pdev;
	struct net_device         *netdev;
	__be32                     mkey_be;
	u8                         num_tc;
	u8                         lag_port;
#ifndef HAVE_NAPI_STATE_MISSED
	unsigned long              flags;
#endif

#ifdef HAVE_XDP_REDIRECT
	/* XDP_REDIRECT */
	struct mlx5e_xdpsq         xdpsq;
#endif

#if defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED)
	/* data path - accessed per napi poll */
	struct irq_desc *irq_desc;
#endif
	struct mlx5e_ch_stats     *stats;

	/* control */
	struct mlx5e_priv         *priv;
	struct mlx5_core_dev      *mdev;
	struct hwtstamp_config    *tstamp;
	int                        ix;
	int                        cpu;
	cpumask_var_t              xps_cpumask;

	struct dentry             *dfs_root;
};

struct mlx5e_channels {
	struct mlx5e_channel **c;
	unsigned int           num;
	struct mlx5e_params    params;
};

struct mlx5e_channel_stats {
	struct mlx5e_ch_stats ch;
	struct mlx5e_sq_stats sq[MLX5E_MAX_NUM_TC];
	struct mlx5e_rq_stats rq;
#ifdef HAVE_XDP_BUFF
	struct mlx5e_xdpsq_stats rq_xdpsq;
#ifdef HAVE_XDP_REDIRECT
	struct mlx5e_xdpsq_stats xdpsq;
#endif
#endif
} ____cacheline_aligned_in_smp;

enum {
	MLX5E_STATE_OPENED,
	MLX5E_STATE_DESTROYING,
	MLX5E_STATE_XDP_TX_ENABLED,
};

struct mlx5e_rqt {
	u32              rqtn;
	bool		 enabled;
};

struct mlx5e_tir {
	u32		  tirn;
	struct mlx5e_rqt  rqt;
	struct list_head  list;
};

enum {
	MLX5E_TC_PRIO = 0,
	MLX5E_NIC_PRIO
};

struct mlx5e_ecn_rp_attributes {
	struct mlx5_core_dev	*mdev;
	/* ATTRIBUTES */
	struct kobj_attribute	enable;
	struct kobj_attribute	clamp_tgt_rate;
	struct kobj_attribute	clamp_tgt_rate_ati;
	struct kobj_attribute	rpg_time_reset;
	struct kobj_attribute	rpg_byte_reset;
	struct kobj_attribute	rpg_threshold;
	struct kobj_attribute	rpg_max_rate;
	struct kobj_attribute	rpg_ai_rate;
	struct kobj_attribute	rpg_hai_rate;
	struct kobj_attribute	rpg_gd;
	struct kobj_attribute	rpg_min_dec_fac;
	struct kobj_attribute	rpg_min_rate;
	struct kobj_attribute	rate2set_fcnp;
	struct kobj_attribute	dce_tcp_g;
	struct kobj_attribute	dce_tcp_rtt;
	struct kobj_attribute	rreduce_mperiod;
	struct kobj_attribute	initial_alpha_value;
};

struct mlx5e_ecn_np_attributes {
	struct mlx5_core_dev	*mdev;
	/* ATTRIBUTES */
	struct kobj_attribute	enable;
	struct kobj_attribute	min_time_between_cnps;
	struct kobj_attribute	cnp_dscp;
	struct kobj_attribute	cnp_802p_prio;
};

union mlx5e_ecn_attributes {
	struct mlx5e_ecn_rp_attributes rp_attr;
	struct mlx5e_ecn_np_attributes np_attr;
};

struct mlx5e_ecn_ctx {
	union mlx5e_ecn_attributes ecn_attr;
	struct kobject *ecn_proto_kobj;
	struct kobject *ecn_enable_kobj;
};

struct mlx5e_ecn_enable_ctx {
	int cong_protocol;
	int priority;
	struct mlx5_core_dev	*mdev;

	struct kobj_attribute	enable;
};

struct mlx5e_rss_params {
	u32	indirection_rqt[MLX5E_INDIR_RQT_SIZE];
	u32	rx_hash_fields[MLX5E_NUM_INDIR_TIRS];
	u8	toeplitz_hash_key[40];
	u8	hfunc;
};

struct mlx5e_modify_sq_param {
	int curr_state;
	int next_state;
	int rl_update;
	int rl_index;
};

struct mlx5e_delay_drop {
	struct work_struct	work;
	/* serialize setting of delay drop */
	struct mutex		lock;
	u32			usec_timeout;
	bool			activate;
};

struct mlx5e_priv {
	/* priv data path fields - start */
	struct mlx5e_txqsq *txq2sq[MLX5E_MAX_NUM_CHANNELS * MLX5E_MAX_NUM_TC +
				   MLX5E_MAX_RL_QUEUES];
	int channel_tc2txq[MLX5E_MAX_NUM_CHANNELS][MLX5E_MAX_NUM_TC];
	int channel_tc2realtxq[MLX5E_MAX_NUM_CHANNELS][MLX5E_MAX_NUM_TC];
#if defined(CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
	DECLARE_HASHTABLE(flow_map_hash, ilog2(MLX5E_MAX_RL_QUEUES));
#endif
#if defined(CONFIG_MLX5_CORE_EN_DCB) && defined(HAVE_IEEE_DCBNL_ETS)
	struct mlx5e_dcbx_dp       dcbx_dp;
#endif
	/* priv data path fields - end */

	u32                        msglevel;
	unsigned long              state;
	struct mutex               state_lock; /* Protects Interface state */
	struct mlx5e_rq            drop_rq;

	struct mlx5e_channels      channels;
	u32                        tisn[MLX5_MAX_PORTS][MLX5E_MAX_NUM_TC];
	struct mlx5e_rqt           indir_rqt;
	struct mlx5e_tir           indir_tir[MLX5E_NUM_INDIR_TIRS];
	struct mlx5e_tir           inner_indir_tir[MLX5E_NUM_INDIR_TIRS];
	struct mlx5e_tir           direct_tir[MLX5E_MAX_NUM_CHANNELS];
	struct mlx5e_rss_params    rss_params;
	u32                        tx_rates[MLX5E_MAX_NUM_SQS +
					    MLX5E_MAX_RL_QUEUES];

	struct mlx5e_flow_steering fs;

	struct workqueue_struct    *wq;
	struct work_struct         update_carrier_work;
	struct work_struct         set_rx_mode_work;
	struct work_struct         tx_timeout_work;
	struct work_struct         update_stats_work;
	struct work_struct         monitor_counters_work;
	struct mlx5_nb             monitor_counters_nb;

	struct mlx5_core_dev      *mdev;
	struct net_device         *netdev;
	struct mlx5e_stats         stats;
	struct mlx5e_channel_stats channel_stats[MLX5E_MAX_NUM_CHANNELS];
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_sw_lro        sw_lro[MLX5E_MAX_NUM_CHANNELS];
#endif
	u8                         max_opened_tc;
#if !defined(HAVE_NDO_GET_STATS64) && !defined(HAVE_NDO_GET_STATS64_RET_VOID)
	struct net_device_stats    netdev_stats;
#endif
#ifdef CONFIG_MLX5_EN_SPECIAL_SQ
	struct mlx5e_sq_stats      special_sq_stats[MLX5E_MAX_RL_QUEUES];
	int                        max_opened_special_sq;
#endif
	struct hwtstamp_config     tstamp;
	u16                        q_counter;
	u16                        drop_rq_q_counter;
	struct notifier_block      events_nb;

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	struct mlx5e_dcbx          dcbx;
#endif
#endif

	const struct mlx5e_profile *profile;
	void                      *ppriv;
#ifdef CONFIG_MLX5_EN_IPSEC
	struct mlx5e_ipsec        *ipsec;
#endif
#ifdef CONFIG_MLX5_EN_TLS
	struct mlx5e_tls          *tls;
#endif

	struct devlink_health_reporter *tx_reporter;
	struct devlink_health_reporter *rx_reporter;

	struct dentry *dfs_root;

	struct kobject *ecn_root_kobj;
#ifdef CONFIG_MLX5_ESWITCH
	struct kobject *compat_kobj;
	struct kobject *devlink_kobj;
	void *devlink_attributes;
#endif

	struct mlx5e_ecn_ctx ecn_ctx[MLX5E_CONG_PROTOCOL_NUM];
	struct mlx5e_ecn_enable_ctx ecn_enable_ctx[MLX5E_CONG_PROTOCOL_NUM][8];
	struct mlx5e_delay_drop delay_drop;
};

struct mlx5e_profile {
	int	(*init)(struct mlx5_core_dev *mdev,
			struct net_device *netdev,
			const struct mlx5e_profile *profile, void *ppriv);
	void	(*cleanup)(struct mlx5e_priv *priv);
	int	(*init_rx)(struct mlx5e_priv *priv);
	void	(*cleanup_rx)(struct mlx5e_priv *priv);
	int	(*init_tx)(struct mlx5e_priv *priv);
	void	(*cleanup_tx)(struct mlx5e_priv *priv);
	void	(*enable)(struct mlx5e_priv *priv);
	void	(*disable)(struct mlx5e_priv *priv);
	int     (*update_rx)(struct mlx5e_priv *priv);
	void	(*update_stats)(struct mlx5e_priv *priv);
	void	(*update_carrier)(struct mlx5e_priv *priv);
	struct {
		mlx5e_fp_handle_rx_cqe handle_rx_cqe;
		mlx5e_fp_handle_rx_cqe handle_rx_cqe_mpwqe;
	} rx_handlers;
	int	max_tc;
};

/* Use this function to get max num channels after netdev was created */
static inline int mlx5e_get_netdev_max_channels(struct mlx5e_priv *priv)
{
	struct net_device *netdev = priv->netdev;

#ifdef HAVE_NET_DEVICE_NUM_RX_QUEUES
	return min_t(unsigned int, netdev->num_rx_queues,
#else
	struct mlx5_core_dev *mdev = priv->mdev;

	return min_t(unsigned int, mlx5e_get_max_num_channels(mdev),
#endif
		     netdev->num_tx_queues);
}

int mlx5e_priv_flags_num(void);
const char *mlx5e_priv_flags_name(int flag);
#ifdef __ETHTOOL_DECLARE_LINK_MODE_MASK
void mlx5e_build_ptys2ethtool_map(void);
#endif


#ifdef HAVE_NDO_SELECT_QUEUE_HAS_3_PARMS_NO_FALLBACK
u16 mlx5e_select_queue(struct net_device *dev, struct sk_buff *skb,
		       struct net_device *sb_dev);

#elif defined(HAVE_NDO_SELECT_QUEUE_HAS_ACCEL_PRIV) || defined(HAVE_SELECT_QUEUE_FALLBACK_T)

u16 mlx5e_select_queue(struct net_device *dev, struct sk_buff *skb,
#ifdef HAVE_SELECT_QUEUE_FALLBACK_T
#ifdef HAVE_SELECT_QUEUE_NET_DEVICE
		       struct net_device *sb_dev,
#else
		       void *accel_priv,
#endif /* HAVE_SELECT_QUEUE_NET_DEVICE */
		       select_queue_fallback_t fallback);
#else
		       void *accel_priv);
#endif
#else /* HAVE_NDO_SELECT_QUEUE_HAS_ACCEL_PRIV || HAVE_SELECT_QUEUE_FALLBACK_T */
u16 mlx5e_select_queue(struct net_device *dev, struct sk_buff *skb);
#endif

netdev_tx_t mlx5e_xmit(struct sk_buff *skb, struct net_device *dev);
netdev_tx_t mlx5e_sq_xmit(struct mlx5e_txqsq *sq, struct sk_buff *skb,
#if defined(HAVE_SK_BUFF_XMIT_MORE) || defined(HAVE_NETDEV_XMIT_MORE)
			  struct mlx5e_tx_wqe *wqe, u16 pi, bool xmit_more);
#else
			  struct mlx5e_tx_wqe *wqe, u16 pi);
#endif

void mlx5e_trigger_irq(struct mlx5e_icosq *sq);
void mlx5e_completion_event(struct mlx5_core_cq *mcq, struct mlx5_eqe *eqe);
void mlx5e_cq_error_event(struct mlx5_core_cq *mcq, enum mlx5_event event);
int mlx5e_napi_poll(struct napi_struct *napi, int budget);
void mlx5e_do_tx_timeout(struct mlx5e_priv *priv);
bool mlx5e_poll_tx_cq(struct mlx5e_cq *cq, int napi_budget);
int mlx5e_poll_rx_cq(struct mlx5e_cq *cq, int budget);
void mlx5e_free_txqsq_descs(struct mlx5e_txqsq *sq);

static inline u32 mlx5e_rqwq_get_size(struct mlx5e_rq *rq)
{
	switch (rq->wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		return mlx5_wq_ll_get_size(&rq->mpwqe.wq);
	default:
		return mlx5_wq_cyc_get_size(&rq->wqe.wq);
	}
}

static inline u32 mlx5e_rqwq_get_cur_sz(struct mlx5e_rq *rq)
{
	switch (rq->wq_type) {
	case MLX5_WQ_TYPE_LINKED_LIST_STRIDING_RQ:
		return rq->mpwqe.wq.cur_sz;
	default:
		return rq->wqe.wq.cur_sz;
	}
}

bool mlx5e_check_fragmented_striding_rq_cap(struct mlx5_core_dev *mdev);
bool mlx5e_striding_rq_possible(struct mlx5_core_dev *mdev,
				struct mlx5e_params *params);

void mlx5e_page_dma_unmap(struct mlx5e_rq *rq, struct mlx5e_dma_info *dma_info);
void mlx5e_page_release(struct mlx5e_rq *rq, struct mlx5e_dma_info *dma_info,
			bool recycle);
void mlx5e_handle_rx_cqe(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe);
void mlx5e_handle_rx_cqe_mpwrq(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe);
bool mlx5e_post_rx_wqes(struct mlx5e_rq *rq);
bool mlx5e_post_rx_mpwqes(struct mlx5e_rq *rq);
void mlx5e_dealloc_rx_wqe(struct mlx5e_rq *rq, u16 ix);
void mlx5e_dealloc_rx_mpwqe(struct mlx5e_rq *rq, u16 ix);
struct sk_buff *
mlx5e_skb_from_cqe_mpwrq_linear(struct mlx5e_rq *rq, struct mlx5e_mpw_info *wi,
				u16 cqe_bcnt, u32 head_offset, u32 page_idx);
struct sk_buff *
mlx5e_skb_from_cqe_mpwrq_nonlinear(struct mlx5e_rq *rq, struct mlx5e_mpw_info *wi,
				   u16 cqe_bcnt, u32 head_offset, u32 page_idx);
struct sk_buff *
mlx5e_skb_from_cqe_linear(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe,
			  struct mlx5e_wqe_frag_info *wi, u32 cqe_bcnt);
struct sk_buff *
mlx5e_skb_from_cqe_nonlinear(struct mlx5e_rq *rq, struct mlx5_cqe64 *cqe,
			     struct mlx5e_wqe_frag_info *wi, u32 cqe_bcnt);

void mlx5e_update_stats(struct mlx5e_priv *priv);
int mlx5e_sysfs_create(struct net_device *dev);
void mlx5e_sysfs_remove(struct net_device *dev);

#if defined(CONFIG_MLX5_EN_SPECIAL_SQ) && (defined(HAVE_NDO_SET_TX_MAXRATE) || defined(HAVE_NDO_SET_TX_MAXRATE_EXTENDED))
int mlx5e_rl_init_sysfs(struct net_device *netdev, struct mlx5e_params params);
void mlx5e_rl_remove_sysfs(struct mlx5e_priv *priv);
#endif

#if defined(HAVE_NDO_SETUP_TC_TAKES_TC_SETUP_TYPE) || defined(HAVE_NDO_SETUP_TC_RH_EXTENDED)
int mlx5e_setup_tc_mqprio(struct net_device *netdev,
			  struct tc_mqprio_qopt *mqprio);
#else
int mlx5e_setup_tc(struct net_device *netdev, u8 tc);
#endif

#ifdef HAVE_NDO_GET_STATS64_RET_VOID
void mlx5e_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats);
#elif defined(HAVE_NDO_GET_STATS64)
struct rtnl_link_stats64 * mlx5e_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats);
#else
struct net_device_stats * mlx5e_get_stats(struct net_device *dev);
#endif


#if defined(HAVE_NDO_GET_OFFLOAD_STATS) || defined(HAVE_NDO_GET_OFFLOAD_STATS_EXTENDED)
void mlx5e_fold_sw_stats64(struct mlx5e_priv *priv, struct rtnl_link_stats64 *s);
#endif

void mlx5e_init_l2_addr(struct mlx5e_priv *priv);
int mlx5e_self_test_num(struct mlx5e_priv *priv);
void mlx5e_self_test(struct net_device *ndev, struct ethtool_test *etest,
		     u64 *buf);
void mlx5e_set_rx_mode_work(struct work_struct *work);

#ifdef HAVE_SIOCGHWTSTAMP
int mlx5e_hwstamp_set(struct mlx5e_priv *priv, struct ifreq *ifr);
int mlx5e_hwstamp_get(struct mlx5e_priv *priv, struct ifreq *ifr);
#else
int mlx5e_hwstamp_ioctl(struct mlx5e_priv *priv, struct ifreq *ifr);
#endif
int mlx5e_modify_rx_cqe_compression_locked(struct mlx5e_priv *priv, bool val);
int mlx5e_modify_tx_cqe_compression_locked(struct mlx5e_priv *priv, bool val);

#if defined(HAVE_NDO_RX_ADD_VID_HAS_3_PARAMS)
int mlx5e_vlan_rx_add_vid(struct net_device *dev, __always_unused __be16 proto,
			  u16 vid);
#elif defined(HAVE_NDO_RX_ADD_VID_HAS_2_PARAMS_RET_INT)
int mlx5e_vlan_rx_add_vid(struct net_device *dev, u16 vid);
#else
void mlx5e_vlan_rx_add_vid(struct net_device *dev, u16 vid);
#endif
#if defined(HAVE_NDO_RX_ADD_VID_HAS_3_PARAMS)
int mlx5e_vlan_rx_kill_vid(struct net_device *dev, __always_unused __be16 proto,
			   u16 vid);
#elif defined(HAVE_NDO_RX_ADD_VID_HAS_2_PARAMS_RET_INT)
int mlx5e_vlan_rx_kill_vid(struct net_device *dev, u16 vid);
#else
void mlx5e_vlan_rx_kill_vid(struct net_device *dev, u16 vid);
#endif
void mlx5e_timestamp_init(struct mlx5e_priv *priv);

#if defined(LEGACY_ETHTOOL_OPS) && defined(HAVE_GET_SET_FLAGS)
int mlx5e_modify_channels_vsd(struct mlx5e_channels *chs, bool vsd);
#endif

struct mlx5e_redirect_rqt_param {
	bool is_rss;
	union {
		u32 rqn; /* Direct RQN (Non-RSS) */
		struct {
			u8 hfunc;
			struct mlx5e_channels *channels;
		} rss; /* RSS data */
	};
};

int mlx5e_redirect_rqt(struct mlx5e_priv *priv, u32 rqtn, int sz,
		       struct mlx5e_redirect_rqt_param rrp);
void mlx5e_build_indir_tir_ctx_hash(struct mlx5e_rss_params *rss_params,
				    const struct mlx5e_tirc_config *ttconfig,
				    void *tirc, bool inner);
void mlx5e_modify_tirs_hash(struct mlx5e_priv *priv, void *in, int inlen);
void mlx5e_sysfs_modify_tirs_hash(struct mlx5e_priv *priv, void *in, int inlen);
struct mlx5e_tirc_config mlx5e_tirc_get_default_config(enum mlx5e_traffic_types tt);

int mlx5e_open_locked(struct net_device *netdev);
int mlx5e_close_locked(struct net_device *netdev);

int mlx5e_open_channels(struct mlx5e_priv *priv,
			struct mlx5e_channels *chs);
void mlx5e_close_channels(struct mlx5e_channels *chs);

/* Function pointer to be used to modify WH settings while
 * switching channels
 */
typedef int (*mlx5e_fp_hw_modify)(struct mlx5e_priv *priv);
int mlx5e_safe_reopen_channels(struct mlx5e_priv *priv);
int mlx5e_safe_switch_channels(struct mlx5e_priv *priv,
			       struct mlx5e_channels *new_chs,
			       mlx5e_fp_hw_modify hw_modify);
void mlx5e_activate_priv_channels(struct mlx5e_priv *priv);
void mlx5e_deactivate_priv_channels(struct mlx5e_priv *priv);

void mlx5e_build_default_indir_rqt(u32 *indirection_rqt, int len,
				   int num_channels);
void mlx5e_build_direct_tir_ctx(struct mlx5e_priv *priv, u32 rqtn, u32 *tirc);
void mlx5e_set_tx_cq_mode_params(struct mlx5e_params *params,
				 u8 cq_period_mode);
void mlx5e_set_rx_cq_mode_params(struct mlx5e_params *params,
				 u8 cq_period_mode);
void mlx5e_set_rq_type(struct mlx5_core_dev *mdev, struct mlx5e_params *params);
void mlx5e_init_rq_type_params(struct mlx5_core_dev *mdev,
			       struct mlx5e_params *params);
int mlx5e_modify_rq_state(struct mlx5e_rq *rq, int curr_state, int next_state);
void mlx5e_activate_rq(struct mlx5e_rq *rq);
void mlx5e_deactivate_rq(struct mlx5e_rq *rq);
void mlx5e_free_rx_descs(struct mlx5e_rq *rq);
void mlx5e_free_rx_in_progress_descs(struct mlx5e_rq *rq);
void mlx5e_activate_icosq(struct mlx5e_icosq *icosq);
void mlx5e_deactivate_icosq(struct mlx5e_icosq *icosq);

int mlx5e_modify_sq(struct mlx5_core_dev *mdev, u32 sqn,
		    struct mlx5e_modify_sq_param *p);
void mlx5e_activate_txqsq(struct mlx5e_txqsq *sq);
void mlx5e_tx_disable_queue(struct netdev_queue *txq);

static inline bool mlx5_tx_swp_supported(struct mlx5_core_dev *mdev)
{
	return MLX5_CAP_ETH(mdev, swp) &&
		MLX5_CAP_ETH(mdev, swp_csum) && MLX5_CAP_ETH(mdev, swp_lso);
}

void mlx5e_create_debugfs(struct mlx5e_priv *priv);
void mlx5e_destroy_debugfs(struct mlx5e_priv *priv);

extern const struct ethtool_ops mlx5e_ethtool_ops;
#ifdef HAVE_ETHTOOL_OPS_EXT
extern const struct ethtool_ops_ext mlx5e_ethtool_ops_ext;
#endif

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
#ifdef CONFIG_COMPAT_IS_DCBNL_OPS_CONST
extern const struct dcbnl_rtnl_ops mlx5e_dcbnl_ops;
#else
extern struct dcbnl_rtnl_ops mlx5e_dcbnl_ops;
#endif
int mlx5e_dcbnl_ieee_setets_core(struct mlx5e_priv *priv, struct ieee_ets *ets);
void mlx5e_dcbnl_initialize(struct mlx5e_priv *priv);
void mlx5e_dcbnl_init_app(struct mlx5e_priv *priv);
void mlx5e_dcbnl_delete_app(struct mlx5e_priv *priv);
#endif
#endif

int mlx5e_create_tir(struct mlx5_core_dev *mdev,
		     struct mlx5e_tir *tir, u32 *in, int inlen);
void mlx5e_destroy_tir(struct mlx5_core_dev *mdev,
		       struct mlx5e_tir *tir);
int mlx5e_create_mdev_resources(struct mlx5_core_dev *mdev);
void mlx5e_destroy_mdev_resources(struct mlx5_core_dev *mdev);
int mlx5e_refresh_tirs(struct mlx5e_priv *priv, bool enable_uc_lb);
int mlx5e_modify_tirs_lro(struct mlx5e_priv *priv);
#if (!defined(HAVE_NDO_SET_FEATURES) && !defined(HAVE_NET_DEVICE_OPS_EXT))
int mlx5e_update_lro(struct net_device *netdev, bool enable);
#endif

/* common netdev helpers */
void mlx5e_create_q_counters(struct mlx5e_priv *priv);
void mlx5e_destroy_q_counters(struct mlx5e_priv *priv);
int mlx5e_open_drop_rq(struct mlx5e_priv *priv,
		       struct mlx5e_rq *drop_rq);
void mlx5e_close_drop_rq(struct mlx5e_rq *drop_rq);

int mlx5e_create_indirect_rqt(struct mlx5e_priv *priv);

int mlx5e_create_indirect_tirs(struct mlx5e_priv *priv, bool inner_ttc);
void mlx5e_destroy_indirect_tirs(struct mlx5e_priv *priv, bool inner_ttc);

int mlx5e_create_direct_rqts(struct mlx5e_priv *priv);
void mlx5e_destroy_direct_rqts(struct mlx5e_priv *priv);
int mlx5e_create_direct_tirs(struct mlx5e_priv *priv);
void mlx5e_destroy_direct_tirs(struct mlx5e_priv *priv);
void mlx5e_destroy_rqt(struct mlx5e_priv *priv, struct mlx5e_rqt *rqt);

int mlx5e_create_tis(struct mlx5_core_dev *mdev, void *in, u8 tx_affinity, u32 *tisn);
void mlx5e_destroy_tis(struct mlx5_core_dev *mdev, u32 tisn);

int mlx5e_create_tises(struct mlx5e_priv *priv);
void mlx5e_destroy_tises(struct mlx5e_priv *priv);
int mlx5e_update_nic_rx(struct mlx5e_priv *priv);
void mlx5e_update_carrier(struct mlx5e_priv *priv);
int mlx5e_close(struct net_device *netdev);
int mlx5e_open(struct net_device *netdev);
u32 mlx5e_choose_lro_timeout(struct mlx5_core_dev *mdev, u32 wanted_timeout);
void mlx5e_update_ndo_stats(struct mlx5e_priv *priv);

void mlx5e_queue_update_stats(struct mlx5e_priv *priv);
u32 mlx5e_ptys_to_speed(u32 eth_proto_oper);
int mlx5e_get_port_speed(struct mlx5e_priv *priv, u32 *speed);

int mlx5e_bits_invert(unsigned long a, int size);

typedef int (*change_hw_mtu_cb)(struct mlx5e_priv *priv);
int mlx5e_set_dev_port_mtu(struct mlx5e_priv *priv);
int mlx5e_change_mtu(struct net_device *netdev, int new_mtu,
		     change_hw_mtu_cb set_mtu_cb);

/* ethtool helpers */
void mlx5e_ethtool_get_drvinfo(struct mlx5e_priv *priv,
			       struct ethtool_drvinfo *drvinfo);
void mlx5e_ethtool_get_strings(struct mlx5e_priv *priv,
			       uint32_t stringset, uint8_t *data);
int mlx5e_ethtool_get_sset_count(struct mlx5e_priv *priv, int sset);
void mlx5e_ethtool_get_ethtool_stats(struct mlx5e_priv *priv,
				     struct ethtool_stats *stats, u64 *data);
void mlx5e_ethtool_get_ringparam(struct mlx5e_priv *priv,
				 struct ethtool_ringparam *param);
int mlx5e_ethtool_set_ringparam(struct mlx5e_priv *priv,
				struct ethtool_ringparam *param);
#if defined(HAVE_GET_SET_CHANNELS) || defined(HAVE_GET_SET_CHANNELS_EXT)
void mlx5e_ethtool_get_channels(struct mlx5e_priv *priv,
				struct ethtool_channels *ch);
int mlx5e_ethtool_set_channels(struct mlx5e_priv *priv,
			       struct ethtool_channels *ch);
#endif
int mlx5e_ethtool_get_coalesce(struct mlx5e_priv *priv,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS //forwardport
			       struct ethtool_coalesce *coal,
			       struct kernel_ethtool_coalesce *kernel_coal);
#else
			       struct ethtool_coalesce *coal);
#endif
int mlx5e_ethtool_set_coalesce(struct mlx5e_priv *priv,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS //forwardport
			       struct ethtool_coalesce *coal,
			       struct kernel_ethtool_coalesce *kernel_coal,
			       struct netlink_ext_ack *extack);
#else
			       struct ethtool_coalesce *coal);
#endif
#ifdef HAVE_GET_SET_LINK_KSETTINGS
int mlx5e_ethtool_get_link_ksettings(struct mlx5e_priv *priv,
				     struct ethtool_link_ksettings *link_ksettings);
int mlx5e_ethtool_set_link_ksettings(struct mlx5e_priv *priv,
				     const struct ethtool_link_ksettings *link_ksettings);
#endif
int mlx5e_get_settings(struct net_device *netdev, struct ethtool_cmd *cmd);
int mlx5e_set_settings(struct net_device *netdev, struct ethtool_cmd *cmd);
#if defined(HAVE_RXFH_INDIR_SIZE) || defined(HAVE_RXFH_INDIR_SIZE_EXT)
u32 mlx5e_ethtool_get_rxfh_indir_size(struct mlx5e_priv *priv);
#endif
#if defined(HAVE_GET_SET_RXFH) && !defined(HAVE_GET_SET_RXFH_INDIR_EXT)
u32 mlx5e_ethtool_get_rxfh_key_size(struct mlx5e_priv *priv);
#ifdef HAVE_ETH_SS_RSS_HASH_FUNCS
int mlx5e_get_rxfh(struct net_device *netdev, u32 *indir, u8 *key, u8 *hfunc);
int mlx5e_set_rxfh(struct net_device *dev, const u32 *indir, const u8 *key,
		   const u8 hfunc);
#else
int mlx5e_get_rxfh(struct net_device *netdev, u32 *indir, u8 *key);
int mlx5e_set_rxfh(struct net_device *dev, const u32 *indir, const u8 *key);
#endif
#elif defined(HAVE_GET_SET_RXFH_INDIR) || defined(HAVE_GET_SET_RXFH_INDIR_EXT)
int mlx5e_get_rxfh_indir(struct net_device *netdev, u32 *indir);
int mlx5e_set_rxfh_indir(struct net_device *dev, const u32 *indir);
#endif
#if defined(HAVE_GET_TS_INFO) || defined(HAVE_GET_TS_INFO_EXT)
int mlx5e_ethtool_get_ts_info(struct mlx5e_priv *priv,
			      struct ethtool_ts_info *info);
#endif
int mlx5e_ethtool_flash_device(struct mlx5e_priv *priv,
			       struct ethtool_flash *flash);
#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
#ifndef HAVE_TC_BLOCK_OFFLOAD
int mlx5e_setup_tc(struct net_device *dev, enum tc_setup_type type,
		   void *type_data);
#endif
#endif
void mlx5e_ethtool_get_pauseparam(struct mlx5e_priv *priv,
				  struct ethtool_pauseparam *pauseparam);
int mlx5e_ethtool_set_pauseparam(struct mlx5e_priv *priv,
				 struct ethtool_pauseparam *pauseparam);
u32 mlx5e_get_priv_flags(struct net_device *netdev);
int mlx5e_set_priv_flags(struct net_device *netdev, u32 pflags);

/* mlx5e generic netdev management API */
int mlx5e_netdev_init(struct net_device *netdev,
		      struct mlx5e_priv *priv,
		      struct mlx5_core_dev *mdev,
		      const struct mlx5e_profile *profile,
		      void *ppriv);
void mlx5e_netdev_cleanup(struct net_device *netdev, struct mlx5e_priv *priv);
struct net_device*
mlx5e_create_netdev(struct mlx5_core_dev *mdev, const struct mlx5e_profile *profile,
		    int nch, void *ppriv);
int mlx5e_attach_netdev(struct mlx5e_priv *priv);
void mlx5e_detach_netdev(struct mlx5e_priv *priv);
void mlx5e_destroy_netdev(struct mlx5e_priv *priv);
void mlx5e_set_netdev_mtu_boundaries(struct mlx5e_priv *priv);
void mlx5e_build_nic_params(struct mlx5e_priv *priv,
			    struct mlx5e_rss_params *rss_params,
			    struct mlx5e_params *params,
			    u16 mtu);
void mlx5e_build_tc2txq_maps(struct mlx5e_priv *priv);
void mlx5e_build_rq_params(struct mlx5_core_dev *mdev,
			   struct mlx5e_params *params);
void mlx5e_build_rss_params(struct mlx5e_rss_params *rss_params,
			    u16 num_channels);

#ifdef HAVE_GET_SET_DUMP
int mlx5e_get_dump_flag(struct net_device *netdev, struct ethtool_dump *dump);
int mlx5e_get_dump_data(struct net_device *netdev, struct ethtool_dump *dump,
			void *buffer);
int mlx5e_set_dump(struct net_device *dev, struct ethtool_dump *dump);
#endif

static inline bool mlx5e_dropless_rq_supported(struct mlx5_core_dev *mdev)
{
	return (MLX5_CAP_GEN(mdev, rq_delay_drop) &&
		MLX5_CAP_GEN(mdev, general_notification_event));
}

void mlx5e_rx_dim_work(struct work_struct *work);
void mlx5e_tx_dim_work(struct work_struct *work);
#ifdef HAVE_GET_SET_LINK_KSETTINGS
int mlx5e_get_link_ksettings(struct net_device *netdev,
			     struct ethtool_link_ksettings *link_ksettings);
int mlx5e_set_link_ksettings(struct net_device *netdev,
			     const struct ethtool_link_ksettings *link_ksettings);
#endif
#if defined(HAVE_NDO_UDP_TUNNEL_ADD) || defined(HAVE_NDO_UDP_TUNNEL_ADD_EXTENDED)
void mlx5e_add_vxlan_port(struct net_device *netdev, struct udp_tunnel_info *ti);
void mlx5e_del_vxlan_port(struct net_device *netdev, struct udp_tunnel_info *ti);
#elif defined(HAVE_NDO_ADD_VXLAN_PORT)
void mlx5e_add_vxlan_port(struct net_device *netdev, sa_family_t sa_family, __be16 port);
void mlx5e_del_vxlan_port(struct net_device *netdev, sa_family_t sa_family, __be16 port);
#endif

#ifdef HAVE_NETDEV_FEATURES_T
netdev_features_t mlx5e_features_check(struct sk_buff *skb, struct net_device *netdev,
				       netdev_features_t features);
#elif defined(HAVE_KERNEL_WITH_VXLAN_SUPPORT_ON) && defined(HAVE_VXLAN_GSO_CHECK)
bool mlx5e_gso_check(struct sk_buff *skb, struct net_device *netdev);
#endif

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
int mlx5e_set_features(struct net_device *netdev,
#ifdef HAVE_NET_DEVICE_OPS_EXT
			      u32 features);
#else
			      netdev_features_t features);
#endif
#endif /*(defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))*/

#ifdef CONFIG_MLX5_ESWITCH
#ifdef HAVE_NDO_SET_VF_MAC
int mlx5e_set_vf_mac(struct net_device *dev, int vf, u8 *mac);
#ifdef HAVE_VF_TX_RATE_LIMITS
int mlx5e_set_vf_rate(struct net_device *dev, int vf, int min_tx_rate, int max_tx_rate);
#else
int mlx5e_set_vf_rate(struct net_device *dev, int vf, int max_tx_rate);
#endif
#endif
#ifdef HAVE_NDO_GET_VF_STATS
int mlx5e_get_vf_config(struct net_device *dev, int vf, struct ifla_vf_info *ivi);
int mlx5e_get_vf_stats(struct net_device *dev, int vf, struct ifla_vf_stats *vf_stats);
#endif
#endif
#endif /* __MLX5_EN_H__ */
