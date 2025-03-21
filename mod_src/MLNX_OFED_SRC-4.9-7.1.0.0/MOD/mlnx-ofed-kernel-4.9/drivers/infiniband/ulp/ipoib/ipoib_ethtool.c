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
 */

#include <linux/kernel.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>

#include "ipoib.h"

struct ipoib_stats {
	char stat_string[ETH_GSTRING_LEN];
	int stat_offset;
};

#ifdef HAVE_RTNL_LINK_STATS64
#define IPOIB_NETDEV_STAT(m) { \
		.stat_string = #m, \
		.stat_offset = offsetof(struct rtnl_link_stats64, m) }
#else
#define IPOIB_NETDEV_STAT(m) { \
		.stat_string = #m, \
		.stat_offset = offsetof(struct rtnl_link_stats, m) }
#endif

static const struct ipoib_stats ipoib_gstrings_stats[] = {
	IPOIB_NETDEV_STAT(rx_packets),
	IPOIB_NETDEV_STAT(tx_packets),
	IPOIB_NETDEV_STAT(rx_bytes),
	IPOIB_NETDEV_STAT(tx_bytes),
	IPOIB_NETDEV_STAT(tx_errors),
	IPOIB_NETDEV_STAT(rx_dropped),
	IPOIB_NETDEV_STAT(tx_dropped),
	IPOIB_NETDEV_STAT(multicast),
};

#define IPOIB_GLOBAL_STATS_LEN	ARRAY_SIZE(ipoib_gstrings_stats)

static int ipoib_set_ring_param(struct net_device *dev,
#ifdef HAVE_GET_RINGPARAM_GET_4_PARAMS
				struct ethtool_ringparam *ringparam,
				struct kernel_ethtool_ringparam *kernel_param,
				struct netlink_ext_ack *extack)
#else
				struct ethtool_ringparam *ringparam)
#endif
{
	struct ipoib_dev_priv *priv = ipoib_priv(dev);
	unsigned int new_recvq_size, new_sendq_size;
	unsigned long priv_current_flags;
	unsigned int dev_current_flags;
	bool init = false, init_fail = false;
	bool is_changed_rx = false, is_changed_tx = false;

	if (ringparam->rx_pending <= IPOIB_MAX_QUEUE_SIZE &&
	    ringparam->rx_pending >= IPOIB_MIN_QUEUE_SIZE) {
		new_recvq_size = roundup_pow_of_two(ringparam->rx_pending);
		is_changed_rx = (new_recvq_size != priv->recvq_size);
		if (ringparam->rx_pending != new_recvq_size)
			pr_warn("%s: %s: rx_pending should be power of two. rx_pending is %d\n",
				dev->name, __func__, new_recvq_size);
	} else {
		pr_err("rx_pending (%d) is out of bounds [%d-%d]\n",
		       ringparam->rx_pending,
		       IPOIB_MIN_QUEUE_SIZE, IPOIB_MAX_QUEUE_SIZE);
		return -EINVAL;
	}

	if (ringparam->tx_pending <= IPOIB_MAX_QUEUE_SIZE &&
	    ringparam->tx_pending >= IPOIB_MIN_QUEUE_SIZE) {
		new_sendq_size = roundup_pow_of_two(ringparam->tx_pending);
		is_changed_tx = (new_sendq_size != priv->sendq_size);
		if (ringparam->tx_pending != new_sendq_size)
			pr_warn("%s: %s: tx_pending should be power of two. tx_pending is %d\n",
				dev->name, __func__, new_sendq_size);
	} else {
		pr_err("tx_pending (%d) is out of bounds [%d-%d]\n",
		       ringparam->tx_pending,
		       IPOIB_MIN_QUEUE_SIZE, IPOIB_MAX_QUEUE_SIZE);
		return -EINVAL;
	}

	if (is_changed_rx || is_changed_tx) {
		priv_current_flags = priv->flags;
		dev_current_flags = dev->flags;

#ifdef HAVE_DEV_CHANGE_FLAGS_HAS_3_PARAMS
		dev_change_flags(dev, dev->flags & ~IFF_UP, NULL);
#else
		dev_change_flags(dev, dev->flags & ~IFF_UP);
#endif
		priv->rn_ops->ndo_uninit(dev);

		do {
			priv->recvq_size = new_recvq_size;
			priv->sendq_size = new_sendq_size;
			if (priv->rn_ops->ndo_init(dev)) {
				new_recvq_size >>= is_changed_rx;
				new_sendq_size >>= is_changed_tx;
				/* keep the values always legal */
				new_recvq_size = max_t(unsigned int,
						       new_recvq_size,
						       IPOIB_MIN_QUEUE_SIZE);
				new_sendq_size = max_t(unsigned int,
						       new_sendq_size,
						       IPOIB_MIN_QUEUE_SIZE);
				init_fail = true;
			} else {
				init = true;
			}
		} while (!init &&
			 !(priv->recvq_size == IPOIB_MIN_QUEUE_SIZE &&
			   priv->sendq_size == IPOIB_MIN_QUEUE_SIZE));

		if (!init) {
			pr_err("%s: Failed to init interface %s, removing it\n",
			       __func__, dev->name);
			return -ENOMEM;
		}

		if (init_fail)
			pr_warn("%s: Unable to set the requested ring size values, "
				"new values are rx = %d, tx = %d\n",
				dev->name, new_recvq_size, new_sendq_size);

		if (dev_current_flags & IFF_UP)
#ifdef HAVE_DEV_CHANGE_FLAGS_HAS_3_PARAMS
			dev_change_flags(dev, dev_current_flags, NULL);
#else
			dev_change_flags(dev, dev_current_flags);
#endif
	}

	return 0;
}

static void ipoib_get_ring_param(struct net_device *dev,
#ifdef HAVE_GET_RINGPARAM_GET_4_PARAMS
				 struct ethtool_ringparam *ringparam,
				 struct kernel_ethtool_ringparam *kernel_param,
				 struct netlink_ext_ack *extack)
#else
				struct ethtool_ringparam *ringparam)
#endif
{
	struct ipoib_dev_priv *priv = ipoib_priv(dev);

	ringparam->rx_max_pending = IPOIB_MAX_QUEUE_SIZE;
	ringparam->tx_max_pending = IPOIB_MAX_QUEUE_SIZE;
	ringparam->rx_mini_max_pending = 0;
	ringparam->rx_jumbo_max_pending = 0;
	ringparam->rx_pending = priv->recvq_size;
	ringparam->tx_pending = priv->sendq_size;
	ringparam->rx_mini_pending = 0;
	ringparam->rx_jumbo_pending = 0;
}

static void ipoib_get_drvinfo(struct net_device *netdev,
			      struct ethtool_drvinfo *drvinfo)
{
	struct ipoib_dev_priv *priv = ipoib_priv(netdev);

	ib_get_device_fw_str(priv->ca, drvinfo->fw_version);

	strlcpy(drvinfo->bus_info, dev_name(priv->ca->dev.parent),
		sizeof(drvinfo->bus_info));

	strlcpy(drvinfo->version, ipoib_driver_version,
		sizeof(drvinfo->version));

	strlcpy(drvinfo->driver, "ib_ipoib", sizeof(drvinfo->driver));
}

static int ipoib_get_coalesce(struct net_device *dev,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS
			      struct ethtool_coalesce *coal,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack)
#else
			      struct ethtool_coalesce *coal)
#endif
{
	struct ipoib_dev_priv *priv = ipoib_priv(dev);

	coal->rx_coalesce_usecs = priv->ethtool.coalesce_usecs;
	coal->rx_max_coalesced_frames = priv->ethtool.max_coalesced_frames;

	return 0;
}

static int ipoib_set_coalesce(struct net_device *dev,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS
			      struct ethtool_coalesce *coal,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack)
#else
			      struct ethtool_coalesce *coal)
#endif
{
	struct ipoib_dev_priv *priv = ipoib_priv(dev);
	int ret;

	/*
	 * These values are saved in the private data and returned
	 * when ipoib_get_coalesce() is called
	 */
	if (coal->rx_coalesce_usecs       > 0xffff ||
	    coal->rx_max_coalesced_frames > 0xffff)
		return -EINVAL;

	ret = rdma_set_cq_moderation(priv->recv_cq,
				     coal->rx_max_coalesced_frames,
				     coal->rx_coalesce_usecs);
	if (ret && ret != -EOPNOTSUPP) {
		ipoib_warn(priv, "failed modifying CQ (%d)\n", ret);
		return ret;
	}

	priv->ethtool.coalesce_usecs       = coal->rx_coalesce_usecs;
	priv->ethtool.max_coalesced_frames = coal->rx_max_coalesced_frames;

	return 0;
}

#ifdef HAVE_ETHTOOL_GET_SET_SETTINGS
static int ipoib_get_settings(struct net_device *dev, struct ethtool_cmd *ecmd)
{
	struct ipoib_dev_priv *priv = ipoib_priv(dev);
	struct ib_port_attr attr;
	char *speed = "";
	int rate;/* in deci-Gb/sec */
	int ret;

	ret = ib_query_port(priv->ca, priv->port, &attr);
	if (ret)
		return ret;

	ecmd->duplex = DUPLEX_FULL;
	ecmd->autoneg = AUTONEG_DISABLE;
	ecmd->phy_address = 255;
	ecmd->port = PORT_OTHER;/* till define IB port type */

	ib_active_speed_enum_to_rate(attr.active_speed,
			&rate,
			&speed);

	rate *= ib_width_enum_to_int(attr.active_width);
	if (rate < 0)
		rate = -1;

	ethtool_cmd_speed_set(ecmd, rate * 100);

	return 0;
}
#endif

static void ipoib_get_ethtool_stats(struct net_device *dev,
				    struct ethtool_stats __always_unused *stats,
				    u64 *data)
{
	int i;
	struct net_device_stats *net_stats = &dev->stats;
	u8 *p = (u8 *)net_stats;

	for (i = 0; i < IPOIB_GLOBAL_STATS_LEN; i++)
		data[i] = *(u64 *)(p + ipoib_gstrings_stats[i].stat_offset);

}
static void ipoib_get_strings(struct net_device __always_unused *dev,
			      u32 stringset, u8 *data)
{
	u8 *p = data;
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < IPOIB_GLOBAL_STATS_LEN; i++) {
			memcpy(p, ipoib_gstrings_stats[i].stat_string,
				ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		break;
	case ETH_SS_TEST:
	default:
		break;
	}
}
static int ipoib_get_sset_count(struct net_device __always_unused *dev,
				 int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return IPOIB_GLOBAL_STATS_LEN;
	case ETH_SS_TEST:
	default:
		break;
	}
	return -EOPNOTSUPP;
}

/* Return lane speed in unit of 1e6 bit/sec */
static inline int ib_speed_enum_to_int(int speed)
{
	switch (speed) {
	case IB_SPEED_SDR:
		return SPEED_2500;
	case IB_SPEED_DDR:
		return SPEED_5000;
	case IB_SPEED_QDR:
	case IB_SPEED_FDR10:
		return SPEED_10000;
	case IB_SPEED_FDR:
		return SPEED_14000;
	case IB_SPEED_EDR:
		return SPEED_25000;
	}

	return SPEED_UNKNOWN;
}

#ifdef HAVE_GET_SET_LINK_KSETTINGS
static int ipoib_get_link_ksettings(struct net_device *netdev,
				    struct ethtool_link_ksettings *cmd)
{
	struct ipoib_dev_priv *priv = ipoib_priv(netdev);
	struct ib_port_attr attr;
	int ret, speed, width;

	if (!netif_carrier_ok(netdev)) {
		cmd->base.speed = SPEED_UNKNOWN;
		cmd->base.duplex = DUPLEX_UNKNOWN;
		return 0;
	}

	ret = ib_query_port(priv->ca, priv->port, &attr);
	if (ret < 0)
		return -EINVAL;

	speed = ib_speed_enum_to_int(attr.active_speed);
	width = ib_width_enum_to_int(attr.active_width);

	if (speed < 0 || width < 0)
		return -EINVAL;

	/* Except the following are set, the other members of
	 * the struct ethtool_link_settings are initialized to
	 * zero in the function __ethtool_get_link_ksettings.
	 */
	cmd->base.speed		 = speed * width;
	cmd->base.duplex	 = DUPLEX_FULL;

	cmd->base.phy_address	 = 0xFF;

	cmd->base.autoneg	 = AUTONEG_ENABLE;
	cmd->base.port		 = PORT_OTHER;

	return 0;
}
#endif

#ifndef HAVE_NETDEV_HW_FEATURES
#if defined(HAVE_GET_SET_FLAGS) && defined(CONFIG_COMPAT_LRO_ENABLED_IPOIB)
int ipoib_set_flags(struct net_device *dev, u32 data)
{
	struct ipoib_dev_priv *priv = ipoib_priv(dev);
	int hw_support_lro = 0;

#ifdef HAVE_NETDEV_HW_FEATURES
	hw_support_lro = priv->dev->hw_features & NETIF_F_RXCSUM;
#else
	hw_support_lro = priv->dev->features & NETIF_F_RXCSUM;
#endif

	if ((data & ETH_FLAG_LRO) && hw_support_lro)
		dev->features |= NETIF_F_LRO;
	else
		dev->features &= ~NETIF_F_LRO;
	return 0;
}
#endif
#ifdef HAVE_GET_SET_RX_CSUM
static u32 ipoib_get_rx_csum(struct net_device *dev)
{
	return dev->features & NETIF_F_RXCSUM;
}
#endif
#endif

static const struct ethtool_ops ipoib_ethtool_ops = {
#ifdef HAVE_SUPPORTED_COALESCE_PARAM
	.supported_coalesce_params = ETHTOOL_COALESCE_RX_USECS |
				     ETHTOOL_COALESCE_RX_MAX_FRAMES,
#endif
#ifdef HAVE_GET_SET_LINK_KSETTINGS
	.get_link_ksettings	= ipoib_get_link_ksettings,
#endif
	.get_drvinfo		= ipoib_get_drvinfo,
	.get_coalesce		= ipoib_get_coalesce,
	.set_coalesce		= ipoib_set_coalesce,
#ifdef HAVE_ETHTOOL_GET_SET_SETTINGS
	.get_settings           = ipoib_get_settings,
#endif
	.get_link		= ethtool_op_get_link,
	.get_strings		= ipoib_get_strings,
	.get_ethtool_stats	= ipoib_get_ethtool_stats,
	.get_sset_count		= ipoib_get_sset_count,
	.set_ringparam		= ipoib_set_ring_param,
	.get_ringparam		= ipoib_get_ring_param,
/* IPoIB current code supports HW_FEATURES and doesn't
 * support EXTENDED_HW_FEATURES. If support for EXTENDED_HW_FEATURES
 * is added then this code and the set function should be masked
 *  with LEGACY_ETHTOOL_OPS.
 */
#ifndef HAVE_NETDEV_HW_FEATURES
#ifdef HAVE_GET_SET_FLAGS
#if defined (CONFIG_COMPAT_LRO_ENABLED_IPOIB)
	.set_flags              = ipoib_set_flags,
#endif
	.get_flags              = ethtool_op_get_flags,
#endif
#ifdef HAVE_GET_SET_TSO
	.set_tso                = ethtool_op_set_tso,
#endif
#ifdef HAVE_GET_SET_RX_CSUM
	.get_rx_csum 		= ipoib_get_rx_csum,
#endif
#endif
};

void ipoib_set_ethtool_ops(struct net_device *dev)
{
	dev->ethtool_ops = &ipoib_ethtool_ops;
}

#include "rss_tss/ipoib_ethtool_rss.c"
