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

#include "en.h"
#include "ipoib.h"

static void mlx5i_get_drvinfo(struct net_device *dev,
			      struct ethtool_drvinfo *drvinfo)
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	mlx5e_ethtool_get_drvinfo(priv, drvinfo);
	strlcpy(drvinfo->driver, DRIVER_NAME "[ib_ipoib]",
		sizeof(drvinfo->driver));
}

static void mlx5i_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	struct mlx5e_priv *priv  = mlx5i_epriv(dev);

	mlx5e_ethtool_get_strings(priv, stringset, data);
}

static int mlx5i_get_sset_count(struct net_device *dev, int sset)
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	return mlx5e_ethtool_get_sset_count(priv, sset);
}

static void mlx5i_get_ethtool_stats(struct net_device *dev,
				    struct ethtool_stats *stats,
				    u64 *data)
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	mlx5e_ethtool_get_ethtool_stats(priv, stats, data);
}

static int mlx5i_set_ringparam(struct net_device *dev,
#ifdef HAVE_GET_RINGPARAM_GET_4_PARAMS
			       struct ethtool_ringparam *param,
			       struct kernel_ethtool_ringparam *kernel_param,
			       struct netlink_ext_ack *extack)
#else
			       struct ethtool_ringparam *param)
#endif
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	return mlx5e_ethtool_set_ringparam(priv, param);
}

static void mlx5i_get_ringparam(struct net_device *dev,
#ifdef HAVE_GET_RINGPARAM_GET_4_PARAMS
				struct ethtool_ringparam *param,
				struct kernel_ethtool_ringparam *kernel_param,
				struct netlink_ext_ack *extack)
#else
				struct ethtool_ringparam *param)
#endif
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	mlx5e_ethtool_get_ringparam(priv, param);
}

#if defined(HAVE_GET_SET_CHANNELS) || defined(HAVE_GET_SET_CHANNELS_EXT)
static int mlx5i_set_channels(struct net_device *dev,
			      struct ethtool_channels *ch)
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	return mlx5e_ethtool_set_channels(priv, ch);
}

static void mlx5i_get_channels(struct net_device *dev,
			       struct ethtool_channels *ch)
{
	struct mlx5e_priv *priv = mlx5i_epriv(dev);

	mlx5e_ethtool_get_channels(priv, ch);
}
#endif

static int mlx5i_set_coalesce(struct net_device *netdev,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS
	   		      struct ethtool_coalesce *coal,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack)
#else
 			      struct ethtool_coalesce *coal)
#endif
{
	struct mlx5e_priv *priv = mlx5i_epriv(netdev);

	return mlx5e_ethtool_set_coalesce(priv,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS //forwardport
					  coal, kernel_coal, extack);
#else
					  coal);
#endif
}

static int mlx5i_get_coalesce(struct net_device *netdev,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS
	   		      struct ethtool_coalesce *coal,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack)
#else
 			      struct ethtool_coalesce *coal)
#endif
{
	struct mlx5e_priv *priv = mlx5i_epriv(netdev);

	return mlx5e_ethtool_get_coalesce(priv,
#ifdef HAVE_NDO_GET_COALESCE_GET_4_PARAMS //forwardport
					  coal, kernel_coal);
#else
					  coal);
#endif
}


#ifdef HAVE_GET_TS_INFO
static int mlx5i_get_ts_info(struct net_device *netdev,
			     struct ethtool_ts_info *info)
{
	struct mlx5e_priv *priv = mlx5i_epriv(netdev);

	return mlx5e_ethtool_get_ts_info(priv, info);
}
#endif

static int mlx5i_flash_device(struct net_device *netdev,
			      struct ethtool_flash *flash)
{
	struct mlx5e_priv *priv = mlx5i_epriv(netdev);

	return mlx5e_ethtool_flash_device(priv, flash);
}

enum mlx5_ptys_width {
	MLX5_PTYS_WIDTH_1X	= 1 << 0,
	MLX5_PTYS_WIDTH_2X	= 1 << 1,
	MLX5_PTYS_WIDTH_4X	= 1 << 2,
	MLX5_PTYS_WIDTH_8X	= 1 << 3,
	MLX5_PTYS_WIDTH_12X	= 1 << 4,
};

static inline int mlx5_ptys_width_enum_to_int(enum mlx5_ptys_width width)
{
	switch (width) {
	case MLX5_PTYS_WIDTH_1X:  return  1;
	case MLX5_PTYS_WIDTH_2X:  return  2;
	case MLX5_PTYS_WIDTH_4X:  return  4;
	case MLX5_PTYS_WIDTH_8X:  return  8;
	case MLX5_PTYS_WIDTH_12X: return 12;
	default:		  return -1;
	}
}

enum mlx5_ptys_rate {
	MLX5_PTYS_RATE_SDR	= 1 << 0,
	MLX5_PTYS_RATE_DDR	= 1 << 1,
	MLX5_PTYS_RATE_QDR	= 1 << 2,
	MLX5_PTYS_RATE_FDR10	= 1 << 3,
	MLX5_PTYS_RATE_FDR	= 1 << 4,
	MLX5_PTYS_RATE_EDR	= 1 << 5,
	MLX5_PTYS_RATE_HDR	= 1 << 6,
};

static inline int mlx5_ptys_rate_enum_to_int(enum mlx5_ptys_rate rate)
{
	switch (rate) {
	case MLX5_PTYS_RATE_SDR:   return 2500;
	case MLX5_PTYS_RATE_DDR:   return 5000;
	case MLX5_PTYS_RATE_QDR:
	case MLX5_PTYS_RATE_FDR10: return 10000;
	case MLX5_PTYS_RATE_FDR:   return 14000;
	case MLX5_PTYS_RATE_EDR:   return 25000;
	case MLX5_PTYS_RATE_HDR:   return 50000;
	default:		   return -1;
	}
}

static int mlx5i_get_port_settings(struct net_device *netdev,
				   u16 *ib_link_width_oper, u16 *ib_proto_oper)
{
	struct mlx5e_priv *priv    = mlx5i_epriv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	u32 out[MLX5_ST_SZ_DW(ptys_reg)] = {0};
	int ret;

	ret = mlx5_query_port_ptys(mdev, out, sizeof(out), MLX5_PTYS_IB, 1);
	if (ret)
		return ret;

	*ib_link_width_oper = MLX5_GET(ptys_reg, out, ib_link_width_oper);
	*ib_proto_oper      = MLX5_GET(ptys_reg, out, ib_proto_oper);

	return 0;
}

static int mlx5i_get_speed_settings(u16 ib_link_width_oper, u16 ib_proto_oper)
{
	int rate, width;

	rate = mlx5_ptys_rate_enum_to_int(ib_proto_oper);
	if (rate < 0)
		return -EINVAL;
	width = mlx5_ptys_width_enum_to_int(ib_link_width_oper);
	if (width < 0)
		return -EINVAL;

	return rate * width;
}

#ifdef HAVE_GET_SET_LINK_KSETTINGS
static int mlx5i_get_link_ksettings(struct net_device *netdev,
				    struct ethtool_link_ksettings *link_ksettings)
{
	u16 ib_link_width_oper;
	u16 ib_proto_oper;
	int speed, ret;

	ret = mlx5i_get_port_settings(netdev, &ib_link_width_oper, &ib_proto_oper);
	if (ret)
		return ret;

	ethtool_link_ksettings_zero_link_mode(link_ksettings, supported);
	ethtool_link_ksettings_zero_link_mode(link_ksettings, advertising);

	speed = mlx5i_get_speed_settings(ib_link_width_oper, ib_proto_oper);
	if (speed < 0)
		return -EINVAL;

	link_ksettings->base.duplex = DUPLEX_FULL;
	link_ksettings->base.port = PORT_OTHER;

	link_ksettings->base.autoneg = AUTONEG_DISABLE;

	link_ksettings->base.speed = speed;

	return 0;
}

#endif
#ifdef HAVE_ETHTOOL_GET_SET_SETTINGS
static int mlx5i_get_settings(struct net_device *netdev,
			      struct ethtool_cmd *ecmd)
{
	u16 ib_link_width_oper;
	u16 ib_proto_oper;
	int speed, ret;

	ret = mlx5i_get_port_settings(netdev,
				      &ib_link_width_oper,
				      &ib_proto_oper);
	if (ret)
		return ret;

	speed = mlx5i_get_speed_settings(ib_link_width_oper, ib_proto_oper);
	if (speed < 0)
		return -EINVAL;

	ecmd->duplex = DUPLEX_FULL;
	ecmd->port = PORT_OTHER;// FIXME: till define IB port type 
	ecmd->phy_address = 255;
	ecmd->autoneg = AUTONEG_DISABLE;

	ethtool_cmd_speed_set(ecmd, speed);

	return 0;
}
#endif

#ifndef HAVE_NETDEV_HW_FEATURES
#if defined(HAVE_GET_SET_FLAGS) && defined(CONFIG_COMPAT_LRO_ENABLED_IPOIB)
int mlx5i_set_flags(struct net_device *dev, u32 data)
{
	int hw_support_lro = 0;

#ifdef HAVE_NETDEV_HW_FEATURES
	hw_support_lro = dev->hw_features & NETIF_F_RXCSUM;
#else
	hw_support_lro = dev->features & NETIF_F_RXCSUM;
#endif

	if ((data & ETH_FLAG_LRO) && hw_support_lro)
		dev->features |= NETIF_F_LRO;
	else
		dev->features &= ~NETIF_F_LRO;
	return 0;
}
#endif

#ifdef HAVE_GET_SET_TSO
int mlx5i_set_tso(struct net_device *dev, u32 data)
{
	if (data)
		dev->features |= (NETIF_F_TSO | NETIF_F_TSO6);
	else
		dev->features &= ~(NETIF_F_TSO | NETIF_F_TSO6);

	return 0;
}
#endif
#ifdef HAVE_GET_SET_RX_CSUM
static u32 mlx5i_get_rx_csum(struct net_device *dev)
{
	return dev->features & NETIF_F_RXCSUM;
}
#endif
#endif

const struct ethtool_ops mlx5i_ethtool_ops = {
#ifdef HAVE_SUPPORTED_COALESCE_PARAM
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS |
				     ETHTOOL_COALESCE_MAX_FRAMES |
				     ETHTOOL_COALESCE_USE_ADAPTIVE,
#endif
	.get_drvinfo        = mlx5i_get_drvinfo,
	.get_strings        = mlx5i_get_strings,
	.get_sset_count     = mlx5i_get_sset_count,
	.get_ethtool_stats  = mlx5i_get_ethtool_stats,
	.get_ringparam      = mlx5i_get_ringparam,
	.set_ringparam      = mlx5i_set_ringparam,
	.flash_device       = mlx5i_flash_device,
#ifdef HAVE_GET_SET_CHANNELS
	.get_channels       = mlx5i_get_channels,
	.set_channels       = mlx5i_set_channels,
#endif
	.get_coalesce       = mlx5i_get_coalesce,
	.set_coalesce       = mlx5i_set_coalesce,
#ifdef HAVE_GET_TS_INFO
	.get_ts_info        = mlx5i_get_ts_info,
#endif
#ifdef HAVE_GET_SET_LINK_KSETTINGS
	.get_link_ksettings = mlx5i_get_link_ksettings,
#endif
#ifdef HAVE_ETHTOOL_GET_SET_SETTINGS
	.get_settings       = mlx5i_get_settings,
#endif
	.get_link           = ethtool_op_get_link,
/* IPoIB current code supports HW_FEATURES and doesn't
 * support EXTENDED_HW_FEATURES. If support for EXTENDED_HW_FEATURES
 * is added then this code and the set function should be masked
 *  with LEGACY_ETHTOOL_OPS.
 */
#ifndef HAVE_NETDEV_HW_FEATURES
#ifdef HAVE_GET_SET_FLAGS
#if defined (CONFIG_COMPAT_LRO_ENABLED_IPOIB)
	.set_flags          = mlx5i_set_flags,
#endif
	.get_flags          = ethtool_op_get_flags,
#endif
#ifdef HAVE_GET_SET_TSO
       .set_tso             = mlx5i_set_tso,
#endif
#ifdef HAVE_GET_SET_RX_CSUM
	.get_rx_csum        = mlx5i_get_rx_csum,
#endif
#endif
};

const struct ethtool_ops mlx5i_pkey_ethtool_ops = {
	.get_drvinfo        = mlx5i_get_drvinfo,
	.get_link           = ethtool_op_get_link,
#ifdef HAVE_GET_TS_INFO
	.get_ts_info        = mlx5i_get_ts_info,
#endif
#ifdef HAVE_GET_SET_LINK_KSETTINGS
	.get_link_ksettings = mlx5i_get_link_ksettings,
#endif
#ifdef HAVE_ETHTOOL_GET_SET_SETTINGS
	.get_settings	    = mlx5i_get_settings,
#endif
};

#ifdef HAVE_ETHTOOL_OPS_EXT
const struct ethtool_ops_ext mlx5i_ethtool_ops_ext = {
	.size			= sizeof(struct ethtool_ops_ext),
#ifdef HAVE_GET_SET_CHANNELS_EXT
	.get_channels		= mlx5i_get_channels,
	.set_channels		= mlx5i_set_channels,
#endif
};

const struct ethtool_ops_ext mlx5i_pkey_ethtool_ops_ext = {
	.size			= sizeof(struct ethtool_ops_ext),
};
#endif /* HAVE_ETHTOOL_OPS_EXT */
