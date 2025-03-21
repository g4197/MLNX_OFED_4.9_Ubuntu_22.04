/*
 * Copyright (c) 2017, Mellanox Technologies. All rights reserved.
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

#include <rdma/ib_verbs.h>
#include <linux/mlx5/fs.h>
#include "en.h"
#include "ipoib.h"

#define IB_DEFAULT_Q_KEY   0xb1b
#define MLX5I_PARAMS_DEFAULT_LOG_RQ_SIZE 9

#define MLX5I_MAX_NUM_CHANNELS           16

static int mlx5i_open(struct net_device *netdev);
static int mlx5i_close(struct net_device *netdev);
static int mlx5i_change_mtu(struct net_device *netdev, int new_mtu);

static const struct net_device_ops mlx5i_netdev_ops = {
	.ndo_open                = mlx5i_open,
	.ndo_stop                = mlx5i_close,
	.ndo_tx_timeout          = mlx5i_tx_timeout,
#if defined(HAVE_NDO_GET_STATS64) || defined(HAVE_NDO_GET_STATS64_RET_VOID)
	.ndo_get_stats64         = mlx5i_get_stats,
#else
	.ndo_get_stats           = mlx5i_get_stats,
#endif
	.ndo_init                = mlx5i_dev_init,
	.ndo_uninit              = mlx5i_dev_cleanup,
#ifndef HAVE_NDO_CHANGE_MTU_EXTENDED
	.ndo_change_mtu          = mlx5i_change_mtu,
#else
	.extended.ndo_change_mtu          = mlx5i_change_mtu,
#endif
	.ndo_do_ioctl            = mlx5i_ioctl,
#ifdef HAVE_NET_DEVICE_OPS_EXTENDED
	.ndo_size = sizeof(struct net_device_ops),
#endif
};

/* IPoIB mlx5 netdev profile */
static void mlx5i_build_nic_params(struct mlx5_core_dev *mdev,
				   struct mlx5e_params *params)
{
	/* Override RQ params as IPoIB supports only LINKED LIST RQ for now */
	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_RX_STRIDING_RQ, false);
	mlx5e_set_rq_type(mdev, params);
	mlx5e_init_rq_type_params(mdev, params);

	/* RQ size in ipoib by default is 512 */
	params->log_rq_mtu_frames = is_kdump_kernel() ?
		MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE :
		MLX5I_PARAMS_DEFAULT_LOG_RQ_SIZE;

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	params->lro_en = true;
	MLX5E_SET_PFLAG(params, MLX5E_PFLAG_HWLRO, false);
#else
       params->lro_en = false;
#endif
	params->hard_mtu = MLX5_IB_GRH_BYTES + MLX5_IPOIB_HARD_LEN;
	params->tunneled_offload_en = false;
}

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
void set_lro_features_bit(struct mlx5e_priv *priv)
{
	u64 hw_support_lro = 0;
#ifdef HAVE_NETDEV_HW_FEATURES
	hw_support_lro = priv->netdev->hw_features & NETIF_F_RXCSUM;
#else
	hw_support_lro = priv->netdev->features & NETIF_F_RXCSUM;
#endif
	if (hw_support_lro) {
		priv->netdev->features |= NETIF_F_LRO;
#ifdef HAVE_NETDEV_HW_FEATURES
		priv->netdev->hw_features |= NETIF_F_LRO;
		priv->netdev->wanted_features |= NETIF_F_LRO;
#endif
	}
}
#endif

static void mlx5i_tx_timeout_work(struct work_struct *work)
{
	struct mlx5e_priv *priv = container_of(work, struct mlx5e_priv,
					       tx_timeout_work);
	int err;

	rtnl_lock();
	if (!test_bit(MLX5E_STATE_OPENED, &priv->state))
		goto unlock;
	priv->netdev->netdev_ops->ndo_stop(priv->netdev);
	err = priv->netdev->netdev_ops->ndo_open(priv->netdev);
	if (err)
		netdev_err(priv->netdev, "mlx5i_open failed recovering from a tx_timeout, err(%d).\n",
			   err);
unlock:
	rtnl_unlock();
}

/* Called directly after IPoIB netdevice was created to initialize SW structs */
int mlx5i_init(struct mlx5_core_dev *mdev,
	       struct net_device *netdev,
	       const struct mlx5e_profile *profile,
	       void *ppriv)
{
	struct mlx5e_priv *priv  = mlx5i_epriv(netdev);
	int err;
#ifndef HAVE_NET_DEVICE_MIN_MAX_MTU
	u16 max_mtu;
#endif

	err = mlx5e_netdev_init(netdev, priv, mdev, profile, ppriv);
	if (err)
		return err;

	INIT_WORK(&priv->tx_timeout_work, mlx5i_tx_timeout_work);
	mlx5e_set_netdev_mtu_boundaries(priv);
#ifdef HAVE_NET_DEVICE_MIN_MAX_MTU
	netdev->mtu = netdev->max_mtu;
#else
	mlx5_query_port_max_mtu(mdev, &max_mtu, 1);
	netdev->mtu = max_mtu;
#endif

	mlx5e_build_nic_params(priv, &priv->rss_params, &priv->channels.params,
			       netdev->mtu);
	mlx5i_build_nic_params(mdev, &priv->channels.params);

	mlx5e_timestamp_init(priv);

	/* netdev init */
#ifdef HAVE_NETDEV_HW_FEATURES
	netdev->hw_features    |= NETIF_F_SG;
	netdev->hw_features    |= NETIF_F_IP_CSUM;
	netdev->hw_features    |= NETIF_F_IPV6_CSUM;
	netdev->hw_features    |= NETIF_F_GRO;
	netdev->hw_features    |= NETIF_F_TSO;
	netdev->hw_features    |= NETIF_F_TSO6;
	netdev->hw_features    |= NETIF_F_RXCSUM;
#ifdef HAVE_NETIF_F_RXHASH
	netdev->hw_features    |= NETIF_F_RXHASH;
#endif
#else /* HAVE_NETDEV_HW_FEATURES */
	netdev->features    |= NETIF_F_SG;
	netdev->features    |= NETIF_F_IP_CSUM;
	netdev->features    |= NETIF_F_IPV6_CSUM;
	netdev->features    |= NETIF_F_GRO;
	netdev->features    |= NETIF_F_TSO;
	netdev->features    |= NETIF_F_TSO6;
	netdev->features    |= NETIF_F_RXCSUM;
#ifdef HAVE_NETIF_F_RXHASH
	netdev->features    |= NETIF_F_RXHASH;
#endif
#endif /* HAVE_NETDEV_HW_FEATURES */
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	set_lro_features_bit(priv);
#endif

	netdev->netdev_ops = &mlx5i_netdev_ops;
#ifndef HAVE_ETHTOOL_OPS_EXT
	netdev->ethtool_ops = &mlx5i_ethtool_ops;
#else
	SET_ETHTOOL_OPS(netdev, &mlx5i_ethtool_ops);
	set_ethtool_ops_ext(netdev, &mlx5i_ethtool_ops_ext);
#endif

	return 0;
}

/* Called directly before IPoIB netdevice is destroyed to cleanup SW structs */
void mlx5i_cleanup(struct mlx5e_priv *priv)
{
	mlx5e_netdev_cleanup(priv->netdev, priv);
}

static void mlx5i_grp_sw_update_stats(struct mlx5e_priv *priv)
{
	int max_nch = mlx5e_get_netdev_max_channels(priv);
	struct mlx5e_sw_stats s = { 0 };
	int i, j;

	for (i = 0; i < max_nch; i++) {
		struct mlx5e_channel_stats *channel_stats;
		struct mlx5e_rq_stats *rq_stats;

		channel_stats = &priv->channel_stats[i];
		rq_stats = &channel_stats->rq;

		s.rx_packets += rq_stats->packets;
		s.rx_bytes   += rq_stats->bytes;

		for (j = 0; j < priv->max_opened_tc; j++) {
			struct mlx5e_sq_stats *sq_stats = &channel_stats->sq[j];

			s.tx_packets           += sq_stats->packets;
			s.tx_bytes             += sq_stats->bytes;
			s.tx_queue_dropped     += sq_stats->dropped;
		}
	}

	memcpy(&priv->stats.sw, &s, sizeof(s));
}

#ifdef HAVE_NDO_GET_STATS64_RET_VOID
void mlx5i_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
#elif defined(HAVE_NDO_GET_STATS64)
struct rtnl_link_stats64 * mlx5i_get_stats(struct net_device *dev,
					   struct rtnl_link_stats64 *stats)
#else
struct net_device_stats * mlx5i_get_stats(struct net_device *dev)
#endif
{
	struct mlx5e_priv     *priv   = mlx5i_epriv(dev);
	struct mlx5e_sw_stats *sstats = &priv->stats.sw;
#if !defined(HAVE_NDO_GET_STATS64) && !defined(HAVE_NDO_GET_STATS64_RET_VOID)
	struct net_device_stats *stats = &priv->netdev_stats;
#endif

	mlx5i_grp_sw_update_stats(priv);

	stats->rx_packets = sstats->rx_packets;
	stats->rx_bytes   = sstats->rx_bytes;
	stats->tx_packets = sstats->tx_packets;
	stats->tx_bytes   = sstats->tx_bytes;
	stats->tx_dropped = sstats->tx_queue_dropped;

#ifndef HAVE_NDO_GET_STATS64_RET_VOID
	return stats;
#endif
}

int mlx5i_init_underlay_qp(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5i_priv *ipriv = priv->ppriv;
	struct mlx5_core_qp *qp = &ipriv->qp;
	struct mlx5_qp_context *context;
	int ret;

	/* QP states */
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	context->flags = cpu_to_be32(MLX5_QP_PM_MIGRATED << 11);
	context->pri_path.port = 1;
	context->pri_path.pkey_index = cpu_to_be16(ipriv->pkey_index);
	context->qkey = cpu_to_be32(IB_DEFAULT_Q_KEY);

	ret = mlx5_core_qp_modify(mdev, MLX5_CMD_OP_RST2INIT_QP, 0, context, qp);
	if (ret) {
		mlx5_core_err(mdev, "Failed to modify qp RST2INIT, err: %d\n", ret);
		goto err_qp_modify_to_err;
	}
	memset(context, 0, sizeof(*context));

	ret = mlx5_core_qp_modify(mdev, MLX5_CMD_OP_INIT2RTR_QP, 0, context, qp);
	if (ret) {
		mlx5_core_err(mdev, "Failed to modify qp INIT2RTR, err: %d\n", ret);
		goto err_qp_modify_to_err;
	}

	ret = mlx5_core_qp_modify(mdev, MLX5_CMD_OP_RTR2RTS_QP, 0, context, qp);
	if (ret) {
		mlx5_core_err(mdev, "Failed to modify qp RTR2RTS, err: %d\n", ret);
		goto err_qp_modify_to_err;
	}

	kfree(context);
	return 0;

err_qp_modify_to_err:
	mlx5_core_qp_modify(mdev, MLX5_CMD_OP_2ERR_QP, 0, &context, qp);
	kfree(context);
	return ret;
}

void mlx5i_uninit_underlay_qp(struct mlx5e_priv *priv)
{
	struct mlx5i_priv *ipriv = priv->ppriv;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5_qp_context context;
	int err;

	err = mlx5_core_qp_modify(mdev, MLX5_CMD_OP_2RST_QP, 0, &context,
				  &ipriv->qp);
	if (err)
		mlx5_core_err(mdev, "Failed to modify qp 2RST, err: %d\n", err);
}

#define MLX5_QP_ENHANCED_ULP_STATELESS_MODE 2

int mlx5i_create_underlay_qp(struct mlx5_core_dev *mdev, struct mlx5_core_qp *qp)
{
	u32 *in = NULL;
	void *addr_path;
	int ret = 0;
	int inlen;
	void *qpc;

	inlen = MLX5_ST_SZ_BYTES(create_qp_in);
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	qpc = MLX5_ADDR_OF(create_qp_in, in, qpc);
	MLX5_SET(qpc, qpc, st, MLX5_QP_ST_UD);
	MLX5_SET(qpc, qpc, pm_state, MLX5_QP_PM_MIGRATED);
	MLX5_SET(qpc, qpc, ulp_stateless_offload_mode,
		 MLX5_QP_ENHANCED_ULP_STATELESS_MODE);

	addr_path = MLX5_ADDR_OF(qpc, qpc, primary_address_path);
	MLX5_SET(ads, addr_path, vhca_port_num, 1);
	MLX5_SET(ads, addr_path, grh, 1);

	ret = mlx5_core_create_qp(mdev, qp, in, inlen);
	if (ret) {
		mlx5_core_err(mdev, "Failed creating IPoIB QP err : %d\n", ret);
		goto out;
	}

out:
	kvfree(in);
	return ret;
}

void mlx5i_destroy_underlay_qp(struct mlx5_core_dev *mdev, struct mlx5_core_qp *qp)
{
	mlx5_core_destroy_qp(mdev, qp);
}

int mlx5i_create_tis(struct mlx5_core_dev *mdev, u32 underlay_qpn, u32 *tisn)
{
	u32 in[MLX5_ST_SZ_DW(create_tis_in)] = {};
	void *tisc;

	tisc = MLX5_ADDR_OF(create_tis_in, in, ctx);

	MLX5_SET(tisc, tisc, underlay_qpn, underlay_qpn);

	return mlx5e_create_tis(mdev, in, 0, tisn);
}

static int mlx5i_init_tx(struct mlx5e_priv *priv)
{
	struct mlx5i_priv *ipriv = priv->ppriv;
	int err;

	err = mlx5i_create_underlay_qp(priv->mdev, &ipriv->qp);
	if (err) {
		mlx5_core_warn(priv->mdev, "create underlay QP failed, %d\n", err);
		return err;
	}

	err = mlx5i_create_tis(priv->mdev, ipriv->qp.qpn, &priv->tisn[0][0]);
	if (err) {
		mlx5_core_warn(priv->mdev, "create tis failed, %d\n", err);
		goto err_destroy_underlay_qp;
	}

	return 0;

err_destroy_underlay_qp:
	mlx5i_destroy_underlay_qp(priv->mdev, &ipriv->qp);
	return err;
}

static void mlx5i_cleanup_tx(struct mlx5e_priv *priv)
{
	struct mlx5i_priv *ipriv = priv->ppriv;

	mlx5e_destroy_tis(priv->mdev, priv->tisn[0][0]);
	mlx5i_destroy_underlay_qp(priv->mdev, &ipriv->qp);
}

static int mlx5i_create_flow_steering(struct mlx5e_priv *priv)
{
	struct ttc_params ttc_params = {};
	int tt, err;

	priv->fs.ns = mlx5_get_flow_namespace(priv->mdev,
					       MLX5_FLOW_NAMESPACE_KERNEL);

	if (!priv->fs.ns)
		return -EINVAL;

	err = mlx5e_arfs_create_tables(priv);
	if (err) {
		netdev_err(priv->netdev, "Failed to create arfs tables, err=%d\n",
			   err);
#ifdef HAVE_NETDEV_HW_FEATURES
#ifdef CONFIG_RFS_ACCEL
		priv->netdev->hw_features &= ~NETIF_F_NTUPLE;
#endif
#endif
	}

	mlx5e_set_ttc_basic_params(priv, &ttc_params);
	mlx5e_set_inner_ttc_ft_params(&ttc_params);
	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++)
		ttc_params.indir_tirn[tt] = priv->inner_indir_tir[tt].tirn;

	err = mlx5e_create_inner_ttc_table(priv, &ttc_params, &priv->fs.inner_ttc);
	if (err) {
		netdev_err(priv->netdev, "Failed to create inner ttc table, err=%d\n",
			   err);
		goto err_destroy_arfs_tables;
	}

	mlx5e_set_ttc_ft_params(&ttc_params);
	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++)
		ttc_params.indir_tirn[tt] = priv->indir_tir[tt].tirn;

	err = mlx5e_create_ttc_table(priv, &ttc_params, &priv->fs.ttc);
	if (err) {
		netdev_err(priv->netdev, "Failed to create ttc table, err=%d\n",
			   err);
		goto err_destroy_inner_ttc_table;
	}

	return 0;

err_destroy_inner_ttc_table:
	mlx5e_destroy_inner_ttc_table(priv, &priv->fs.inner_ttc);
err_destroy_arfs_tables:
	mlx5e_arfs_destroy_tables(priv);

	return err;
}

static void mlx5i_destroy_flow_steering(struct mlx5e_priv *priv)
{
	mlx5e_destroy_ttc_table(priv, &priv->fs.ttc);
	mlx5e_destroy_inner_ttc_table(priv, &priv->fs.inner_ttc);
	mlx5e_arfs_destroy_tables(priv);
}

static int mlx5i_init_rx(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	int err;

	mlx5e_create_q_counters(priv);

	err = mlx5e_open_drop_rq(priv, &priv->drop_rq);
	if (err) {
		mlx5_core_err(mdev, "open drop rq failed, %d\n", err);
		goto err_destroy_q_counters;
	}

	err = mlx5e_create_indirect_rqt(priv);
	if (err)
		goto err_close_drop_rq;

	err = mlx5e_create_direct_rqts(priv);
	if (err)
		goto err_destroy_indirect_rqts;

	err = mlx5e_create_indirect_tirs(priv, true);
	if (err)
		goto err_destroy_direct_rqts;

	err = mlx5e_create_direct_tirs(priv);
	if (err)
		goto err_destroy_indirect_tirs;

	err = mlx5i_create_flow_steering(priv);
	if (err)
		goto err_destroy_direct_tirs;

	return 0;

err_destroy_direct_tirs:
	mlx5e_destroy_direct_tirs(priv);
err_destroy_indirect_tirs:
	mlx5e_destroy_indirect_tirs(priv, true);
err_destroy_direct_rqts:
	mlx5e_destroy_direct_rqts(priv);
err_destroy_indirect_rqts:
	mlx5e_destroy_rqt(priv, &priv->indir_rqt);
err_close_drop_rq:
	mlx5e_close_drop_rq(&priv->drop_rq);
err_destroy_q_counters:
	mlx5e_destroy_q_counters(priv);
	return err;
}

static void mlx5i_cleanup_rx(struct mlx5e_priv *priv)
{
	mlx5i_destroy_flow_steering(priv);
	mlx5e_destroy_direct_tirs(priv);
	mlx5e_destroy_indirect_tirs(priv, true);
	mlx5e_destroy_direct_rqts(priv);
	mlx5e_destroy_rqt(priv, &priv->indir_rqt);
	mlx5e_close_drop_rq(&priv->drop_rq);
	mlx5e_destroy_q_counters(priv);
}

static void mlx5i_update_stats(struct mlx5e_priv *priv)
{
	mlx5e_grp_sw_update_stats(priv);
}

static inline int mlx5i_get_max_num_channels(struct mlx5_core_dev *mdev)
{
	return is_kdump_kernel() ?
		MLX5E_MIN_NUM_CHANNELS :
		min_t(int, mlx5_comp_vectors_count(mdev), MLX5E_MAX_NUM_CHANNELS);
}

static const struct mlx5e_profile mlx5i_nic_profile = {
	.init		   = mlx5i_init,
	.cleanup	   = mlx5i_cleanup,
	.init_tx	   = mlx5i_init_tx,
	.cleanup_tx	   = mlx5i_cleanup_tx,
	.init_rx	   = mlx5i_init_rx,
	.cleanup_rx	   = mlx5i_cleanup_rx,
	.enable		   = NULL, /* mlx5i_enable */
	.disable	   = NULL, /* mlx5i_disable */
	.update_rx         = mlx5e_update_nic_rx,
	.update_stats	   = mlx5i_update_stats,
	.update_carrier    = NULL, /* no HW update in IB link */
	.rx_handlers.handle_rx_cqe       = mlx5i_handle_rx_cqe,
	.rx_handlers.handle_rx_cqe_mpwqe = NULL, /* Not supported */
	.max_tc		   = MLX5I_MAX_NUM_TC,
};

/* mlx5i netdev NDos */

static int mlx5i_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct mlx5e_priv *priv = mlx5i_epriv(netdev);
	struct mlx5e_channels new_channels = {};
	struct mlx5e_params *params;
	int err = 0;

	mutex_lock(&priv->state_lock);

	params = &priv->channels.params;

	if (!test_bit(MLX5E_STATE_OPENED, &priv->state)) {
		params->sw_mtu = new_mtu;
		netdev->mtu = params->sw_mtu;
		goto out;
	}

	new_channels.params = *params;
	new_channels.params.sw_mtu = new_mtu;

	err = mlx5e_safe_switch_channels(priv, &new_channels, NULL);
	if (err)
		goto out;

	netdev->mtu = new_channels.params.sw_mtu;

out:
	mutex_unlock(&priv->state_lock);
	return err;
}

#ifdef HAVE_NDO_TX_TIMEOUT_GET_2_PARAMS
void mlx5i_tx_timeout(struct net_device *netdev, unsigned int txqueue)
#else
void mlx5i_tx_timeout(struct net_device *netdev)
#endif
{
	struct mlx5e_priv *priv  = mlx5i_epriv(netdev);

	queue_work(priv->wq, &priv->tx_timeout_work);
}

int mlx5i_dev_init(struct net_device *dev)
{
	struct mlx5e_priv    *priv   = mlx5i_epriv(dev);
	struct mlx5i_priv    *ipriv  = priv->ppriv;

	/* Set dev address using underlay QP */
	dev->dev_addr[0] |= MLX5_IPOIB_FLAGS_TSS;
	dev->dev_addr[1] = (ipriv->qp.qpn >> 16) & 0xff;
	dev->dev_addr[2] = (ipriv->qp.qpn >>  8) & 0xff;
	dev->dev_addr[3] = (ipriv->qp.qpn) & 0xff;

	/* Add QPN to net-device mapping to HT */
	mlx5i_pkey_add_qpn(dev ,ipriv->qp.qpn);

	return 0;
}

int mlx5i_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	switch (cmd) {
	case SIOCSHWTSTAMP:
#ifdef HAVE_SIOCGHWTSTAMP
		return mlx5e_hwstamp_set(priv, ifr);
	case SIOCGHWTSTAMP:
		return mlx5e_hwstamp_get(priv, ifr);
#else
		return mlx5e_hwstamp_ioctl(priv, ifr);
#endif
	default:
		return -EOPNOTSUPP;
	}
}

void mlx5i_dev_cleanup(struct net_device *dev)
{
	struct mlx5e_priv    *priv   = mlx5i_epriv(dev);
	struct mlx5i_priv    *ipriv = priv->ppriv;

	mlx5i_uninit_underlay_qp(priv);

	/* Delete QPN to net-device mapping from HT */
	mlx5i_pkey_del_qpn(dev, ipriv->qp.qpn);
}

static int mlx5i_open(struct net_device *netdev)
{
	struct mlx5e_priv *epriv = mlx5i_epriv(netdev);
	struct mlx5i_priv *ipriv = epriv->ppriv;
	struct mlx5_core_dev *mdev = epriv->mdev;
	int err;

	mutex_lock(&epriv->state_lock);

	set_bit(MLX5E_STATE_OPENED, &epriv->state);

	err = mlx5i_init_underlay_qp(epriv);
	if (err) {
		mlx5_core_warn(mdev, "prepare underlay qp state failed, %d\n", err);
		goto err_clear_state_opened_flag;
	}

	err = mlx5_fs_add_rx_underlay_qpn(mdev, ipriv->qp.qpn);
	if (err) {
		mlx5_core_warn(mdev, "attach underlay qp to ft failed, %d\n", err);
		goto err_reset_qp;
	}

	err = mlx5e_open_channels(epriv, &epriv->channels);
	if (err)
		goto err_remove_fs_underlay_qp;

	epriv->profile->update_rx(epriv);
	mlx5e_activate_priv_channels(epriv);

	mutex_unlock(&epriv->state_lock);
	return 0;

err_remove_fs_underlay_qp:
	mlx5_fs_remove_rx_underlay_qpn(mdev, ipriv->qp.qpn);
err_reset_qp:
	mlx5i_uninit_underlay_qp(epriv);
err_clear_state_opened_flag:
	clear_bit(MLX5E_STATE_OPENED, &epriv->state);
	mutex_unlock(&epriv->state_lock);
	return err;
}

static int mlx5i_close(struct net_device *netdev)
{
	struct mlx5e_priv *epriv = mlx5i_epriv(netdev);
	struct mlx5i_priv *ipriv = epriv->ppriv;
	struct mlx5_core_dev *mdev = epriv->mdev;

	/* May already be CLOSED in case a previous configuration operation
	 * (e.g RX/TX queue size change) that involves close&open failed.
	 */
	mutex_lock(&epriv->state_lock);

	if (!test_bit(MLX5E_STATE_OPENED, &epriv->state))
		goto unlock;

	clear_bit(MLX5E_STATE_OPENED, &epriv->state);

	netif_carrier_off(epriv->netdev);
	mlx5_fs_remove_rx_underlay_qpn(mdev, ipriv->qp.qpn);
	mlx5e_deactivate_priv_channels(epriv);
	mlx5e_close_channels(&epriv->channels);
	mlx5i_uninit_underlay_qp(epriv);
unlock:
	mutex_unlock(&epriv->state_lock);
	return 0;
}

/* IPoIB RDMA netdev callbacks */
static int mlx5i_attach_mcast(struct net_device *netdev, struct ib_device *hca,
			      union ib_gid *gid, u16 lid, int set_qkey,
			      u32 qkey)
{
	struct mlx5e_priv    *epriv = mlx5i_epriv(netdev);
	struct mlx5_core_dev *mdev  = epriv->mdev;
	struct mlx5i_priv    *ipriv = epriv->ppriv;
	int err;

	mlx5_core_dbg(mdev, "attaching QPN 0x%x, MGID %pI6\n", ipriv->qp.qpn, gid->raw);
	err = mlx5_core_attach_mcg(mdev, gid, ipriv->qp.qpn);
	if (err)
		mlx5_core_warn(mdev, "failed attaching QPN 0x%x, MGID %pI6\n",
			       ipriv->qp.qpn, gid->raw);

	if (set_qkey) {
		mlx5_core_dbg(mdev, "%s setting qkey 0x%x\n",
			      netdev->name, qkey);
		ipriv->qkey = qkey;
	}

	return err;
}

static int mlx5i_detach_mcast(struct net_device *netdev, struct ib_device *hca,
			      union ib_gid *gid, u16 lid)
{
	struct mlx5e_priv    *epriv = mlx5i_epriv(netdev);
	struct mlx5_core_dev *mdev  = epriv->mdev;
	struct mlx5i_priv    *ipriv = epriv->ppriv;
	int err;

	mlx5_core_dbg(mdev, "detaching QPN 0x%x, MGID %pI6\n", ipriv->qp.qpn, gid->raw);

	err = mlx5_core_detach_mcg(mdev, gid, ipriv->qp.qpn);
	if (err)
		mlx5_core_dbg(mdev, "failed detaching QPN 0x%x, MGID %pI6\n",
			      ipriv->qp.qpn, gid->raw);

	return err;
}

static int mlx5i_xmit(struct net_device *dev, struct sk_buff *skb,
		      struct ib_ah *address, u32 dqpn)
{
	struct mlx5e_priv *epriv = mlx5i_epriv(dev);
	struct mlx5e_txqsq *sq   = epriv->txq2sq[skb_get_queue_mapping(skb)];
	struct mlx5_ib_ah *mah   = to_mah(address);
	struct mlx5i_priv *ipriv = epriv->ppriv;
#ifdef HAVE_NETDEV_XMIT_MORE
	return mlx5i_sq_xmit(sq, skb, &mah->av, dqpn, ipriv->qkey, netdev_xmit_more());
#elif defined(HAVE_SK_BUFF_XMIT_MORE)
	return mlx5i_sq_xmit(sq, skb, &mah->av, dqpn, ipriv->qkey, skb->xmit_more);
#else
	return mlx5i_sq_xmit(sq, skb, &mah->av, dqpn, ipriv->qkey);
#endif

}

static void mlx5i_set_pkey_index(struct net_device *netdev, int id)
{
	struct mlx5i_priv *ipriv = netdev_priv(netdev);

	ipriv->pkey_index = (u16)id;
}

static int mlx5i_check_required_hca_cap(struct mlx5_core_dev *mdev)
{
	if (MLX5_CAP_GEN(mdev, port_type) != MLX5_CAP_PORT_TYPE_IB)
		return -EOPNOTSUPP;

	if (!MLX5_CAP_GEN(mdev, ipoib_enhanced_offloads)) {
		mlx5_core_warn(mdev, "IPoIB enhanced offloads are not supported\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static void mlx5_rdma_netdev_free(struct net_device *netdev)
{
	struct mlx5e_priv *priv = mlx5i_epriv(netdev);
	struct mlx5i_priv *ipriv = priv->ppriv;
	const struct mlx5e_profile *profile = priv->profile;

	mlx5e_detach_netdev(priv);
	profile->cleanup(priv);

	if (!ipriv->sub_interface) {
		mlx5i_pkey_qpn_ht_cleanup(netdev);
		mlx5e_destroy_mdev_resources(priv->mdev);
	}
}

static bool mlx5_is_sub_interface(struct mlx5_core_dev *mdev)
{
	return mdev->mlx5e_res.pdn != 0;
}

static const struct mlx5e_profile *mlx5_get_profile(struct mlx5_core_dev *mdev)
{
	if (mlx5_is_sub_interface(mdev))
		return mlx5i_pkey_get_profile();
	return &mlx5i_nic_profile;
}

static int mlx5_rdma_setup_rn(struct ib_device *ibdev, u8 port_num,
			      struct net_device *netdev, void *param)
{
	struct mlx5_core_dev *mdev = (struct mlx5_core_dev *)param;
	const struct mlx5e_profile *prof = mlx5_get_profile(mdev);
	struct mlx5i_priv *ipriv;
	struct mlx5e_priv *epriv;
	struct rdma_netdev *rn;
	int err;

	ipriv = netdev_priv(netdev);
	epriv = mlx5i_epriv(netdev);

	ipriv->sub_interface = mlx5_is_sub_interface(mdev);
	if (!ipriv->sub_interface) {
		err = mlx5i_pkey_qpn_ht_init(netdev);
		if (err) {
			mlx5_core_warn(mdev, "allocate qpn_to_netdev ht failed\n");
			return err;
		}

		/* This should only be called once per mdev */
		err = mlx5e_create_mdev_resources(mdev);
		if (err)
			goto destroy_ht;
	}

	prof->init(mdev, netdev, prof, ipriv);

	mlx5e_attach_netdev(epriv);
	netif_carrier_off(netdev);

	/* set rdma_netdev func pointers */
	rn = &ipriv->rn;
	rn->hca  = ibdev;
	rn->send = mlx5i_xmit;
	rn->attach_mcast = mlx5i_attach_mcast;
	rn->detach_mcast = mlx5i_detach_mcast;
	rn->set_id = mlx5i_set_pkey_index;

#ifdef HAVE_NET_DEVICE_NEEDS_FREE_NETDEV
	netdev->priv_destructor = mlx5_rdma_netdev_free;
	netdev->needs_free_netdev = 1;
#endif
	return 0;

destroy_ht:
	mlx5i_pkey_qpn_ht_cleanup(netdev);
	return err;
}

int mlx5_rdma_rn_get_params(struct mlx5_core_dev *mdev,
			    struct ib_device *device,
			    struct rdma_netdev_alloc_params *params)
{
	int nch;
	int rc;

	rc = mlx5i_check_required_hca_cap(mdev);
	if (rc)
		return rc;

	nch = mlx5e_get_max_num_channels(mdev);

	*params = (struct rdma_netdev_alloc_params){
		.sizeof_priv = sizeof(struct mlx5i_priv) +
			       sizeof(struct mlx5e_priv),
		.txqs = nch * MLX5E_MAX_NUM_TC,
		.rxqs = nch,
		.param = mdev,
		.initialize_rdma_netdev = mlx5_rdma_setup_rn,
#ifndef HAVE_NET_DEVICE_NEEDS_FREE_NETDEV
		.uninitialize_rdma_netdev = mlx5_rdma_netdev_free,
#endif
	};

	return 0;
}
EXPORT_SYMBOL(mlx5_rdma_rn_get_params);
