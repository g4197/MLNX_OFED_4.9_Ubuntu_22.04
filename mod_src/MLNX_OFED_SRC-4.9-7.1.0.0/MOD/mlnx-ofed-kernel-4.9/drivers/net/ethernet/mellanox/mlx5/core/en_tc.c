/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
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
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
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

#include <net/sch_generic.h>
#include <net/pkt_cls.h>
#ifdef HAVE_TC_GACT_H
#include <net/tc_act/tc_gact.h>
#endif
#ifdef HAVE_IS_TCF_SKBEDIT_MARK
#include <net/tc_act/tc_skbedit.h>
#endif
#include <linux/mlx5/fs.h>
#include <linux/mlx5/device.h>
#include <lib/devcom.h>
#ifdef HAVE_TC_FLOWER_OFFLOAD
#include <linux/rhashtable.h>
#include <linux/refcount.h>
#endif
#include <net/switchdev.h>
#ifdef HAVE_TC_FLOWER_OFFLOAD
#include <net/tc_act/tc_mirred.h>
#endif
#ifdef HAVE_IS_TCF_VLAN
#include <net/tc_act/tc_vlan.h>
#endif
#ifdef HAVE_TCF_TUNNEL_INFO
#include <net/tc_act/tc_tunnel_key.h>
#endif
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
#include <linux/tc_act/tc_pedit.h>
#include <net/tc_act/tc_pedit.h>
#endif
#ifdef HAVE_TCA_CSUM_UPDATE_FLAG_IPV4HDR
#include <net/tc_act/tc_csum.h>
#endif
#include <linux/completion.h>
#ifdef HAVE_TCA_CSUM_UPDATE_FLAG_IPV4HDR
#include <net/tc_act/tc_csum.h>
#endif
#ifdef HAVE_MINIFLOW
#include <net/tc_act/tc_ct.h>
#endif
#include <net/arp.h>
#ifdef HAVE_TC_FLOWER_OFFLOAD
#include <net/flow_dissector.h>
#endif
#ifdef HAVE_IPV6_STUBS_H
#include <net/ipv6_stubs.h>
#endif
#include <net/bonding.h>
#include "en.h"
#include "en_rep.h"
#include "en_tc.h"
#include "eswitch.h"
#ifdef HAVE_TC_FLOWER_OFFLOAD
 #include "miniflow.h"
#endif
#include "fs_core.h"
#include "en/port.h"
#include "en/tc_tun.h"
#include "lib/devcom.h"
#include "lib/geneve.h"
#include <linux/mlx5/vport.h>
#include <net/flow_offload.h>

#include <net/tc_act/tc_pedit.h>

#if defined(HAVE_TC_CLS_FLOWER_OFFLOAD_COMMON) && \
    defined(HAVE_IS_TCF_GACT_GOTO_CHAIN) && \
    defined(HAVE_FLOWER_MULTI_MASK)
#define PRIO_CHAIN_SUPPORT 1
#endif

#if defined(HAVE_TC_FLOWER_OFFLOAD) && \
    !defined(HAVE_SWITCHDEV_PORT_SAME_PARENT_ID)
#include <net/bonding.h>

bool switchdev_port_same_parent_id(struct net_device *a,
				   struct net_device *b)
{
	struct mlx5e_priv *priv_a, *priv_b;
	struct mlx5_eswitch *peer_esw;
	struct mlx5_devcom *devcom;
	struct net_device *ndev;
	struct bonding *bond;
	bool ret = true;

	if (netif_is_bond_master(b)) {
		bond = netdev_priv(b);
		if (!bond_has_slaves(bond))
			return false;

		rcu_read_lock();
#ifdef for_each_netdev_in_bond_rcu
		for_each_netdev_in_bond_rcu(b, ndev) {
#else
		for_each_netdev_in_bond(b, ndev) {
#endif
			ret &= switchdev_port_same_parent_id(a, ndev);
			if (!ret)
				break;
		}
		rcu_read_unlock();
		return ret;
	}

	if (!(a->features & NETIF_F_HW_TC) || !(b->features & NETIF_F_HW_TC))
		return false;

	priv_a = netdev_priv(a);
	priv_b = netdev_priv(b);

	if (!priv_a->mdev->priv.eswitch || !priv_b->mdev->priv.eswitch)
		return false;

	if (priv_a->mdev->priv.eswitch->mode != MLX5_ESWITCH_OFFLOADS ||
	    priv_b->mdev->priv.eswitch->mode != MLX5_ESWITCH_OFFLOADS)
		return false;

	if (priv_a->mdev == priv_b->mdev)
		return true;

	devcom = priv_a->mdev->priv.devcom;
	peer_esw = mlx5_devcom_get_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
	if (!peer_esw)
		return false;

	ret = (peer_esw->dev == priv_b->mdev);
	mlx5_devcom_release_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
	return ret;
}
#endif

#ifdef HAVE_TC_FLOWER_OFFLOAD

#define MLX5E_TC_TABLE_NUM_GROUPS 4
#define MLX5E_TC_TABLE_MAX_GROUP_SIZE BIT(16)

struct mlx5e_hairpin {
	struct mlx5_hairpin *pair;

	struct mlx5_core_dev *func_mdev;
	struct mlx5e_priv *func_priv;
	u32 tdn;
	u32 tirn;

	int num_channels;
	struct mlx5e_rqt indir_rqt;
	u32 indir_tirn[MLX5E_NUM_INDIR_TIRS];
	struct mlx5e_ttc_table ttc;
};

struct mlx5e_hairpin_entry {
	/* a node of a hash table which keeps all the  hairpin entries */
	struct hlist_node hairpin_hlist;

	/* protects flows list */
	spinlock_t flows_lock;
	/* flows sharing the same hairpin */
	struct list_head flows;
	/* hpe's that were not fully initialized when dead peer update event
	 * function traversed them.
	 */
	struct list_head dead_peer_wait_list;

	u16 peer_vhca_id;
	u8 prio;
	struct mlx5e_hairpin *hp;
	refcount_t refcnt;
	struct completion hw_res_created;
};

static void mlx5e_tc_del_flow(struct mlx5e_priv *priv,
			      struct mlx5e_tc_flow *flow);

static struct mlx5e_tc_flow *mlx5e_flow_get(struct mlx5e_tc_flow *flow)
{
	if (!flow || !refcount_inc_not_zero(&flow->refcnt))
		return ERR_PTR(-EINVAL);
	return flow;
}

void mlx5e_flow_put_lock(struct mlx5e_priv *priv,
			 struct mlx5e_tc_flow *flow, bool lock)
{
	if (refcount_dec_and_test(&flow->refcnt)) {
		if (flow->dep_lock && lock)
			spin_lock(flow->dep_lock);
		if (!list_empty(&flow->nft_node))
			list_del_init(&flow->nft_node);
		if (flow->dep_lock && lock)
			spin_unlock(flow->dep_lock);
		mlx5e_tc_del_flow(priv, flow);
		kfree_rcu(flow, rcu_head);
	}
}

void mlx5e_flow_put(struct mlx5e_priv *priv,
		    struct mlx5e_tc_flow *flow)
{
	mlx5e_flow_put_lock(priv, flow, true);
}

static void __flow_flag_set(struct mlx5e_tc_flow *flow, unsigned long flag)
{
	/* Complete all memory stores before setting bit. */
	smp_mb__before_atomic();
	set_bit(flag, &flow->flags);
}

#define flow_flag_set(flow, flag) __flow_flag_set(flow, MLX5E_TC_FLOW_FLAG_##flag)

static bool __flow_flag_test_and_set(struct mlx5e_tc_flow *flow,
				     unsigned long flag)
{
	/* test_and_set_bit() provides all necessary barriers */
	return test_and_set_bit(flag, &flow->flags);
}

#define flow_flag_test_and_set(flow, flag)			\
	__flow_flag_test_and_set(flow,				\
				 MLX5E_TC_FLOW_FLAG_##flag)

static void __flow_flag_clear(struct mlx5e_tc_flow *flow, unsigned long flag)
{
	/* Complete all memory stores before clearing bit. */
	smp_mb__before_atomic();
	clear_bit(flag, &flow->flags);
}

#define flow_flag_clear(flow, flag) __flow_flag_clear(flow, \
						      MLX5E_TC_FLOW_FLAG_##flag)

static bool __flow_flag_test(struct mlx5e_tc_flow *flow, unsigned long flag)
{
	bool ret = test_bit(flag, &flow->flags);

	/* Read fields of flow structure only after checking flags. */
	smp_mb__after_atomic();
	return ret;
}

#define flow_flag_test(flow, flag) __flow_flag_test(flow, \
						    MLX5E_TC_FLOW_FLAG_##flag)

static bool mlx5e_is_eswitch_flow(struct mlx5e_tc_flow *flow)
{
	return flow_flag_test(flow, ESWITCH);
}

static bool mlx5e_is_offloaded_flow(struct mlx5e_tc_flow *flow)
{
	return flow_flag_test(flow, OFFLOADED);
}

static bool mlx5e_is_simple_flow(struct mlx5e_tc_flow *flow)
{
	return flow_flag_test(flow, SIMPLE);
}

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
struct mod_hdr_key {
	int num_actions;
	void *actions;
};

struct mlx5e_mod_hdr_entry {
	/* a node of a hash table which keeps all the mod_hdr entries */
	struct hlist_node mod_hdr_hlist;

	/* protects flows list */
	spinlock_t flows_lock;
	/* flows sharing the same mod_hdr entry */
	struct list_head flows;

	struct mod_hdr_key key;

	struct mlx5_modify_hdr *modify_hdr;

	refcount_t		refcnt;
	struct completion hw_res_created;

	//struct rcu_head rcu;
};

static inline u32 hash_mod_hdr_info(struct mod_hdr_key *key)
{
	return jhash(key->actions,
		     key->num_actions * MLX5_MH_ACT_SZ, 0);
}

static inline int cmp_mod_hdr_info(struct mod_hdr_key *a,
				   struct mod_hdr_key *b)
{
	if (a->num_actions != b->num_actions)
		return 1;

	return memcmp(a->actions, b->actions, a->num_actions * MLX5_MH_ACT_SZ);
}

static struct mod_hdr_tbl *
get_mod_hdr_table(struct mlx5e_priv *priv, int namespace)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	return namespace == MLX5_FLOW_NAMESPACE_FDB ? &esw->offloads.mod_hdr :
		&priv->fs.tc.mod_hdr;
}

static struct mlx5e_mod_hdr_entry *
mlx5e_mod_hdr_get(struct mod_hdr_tbl *tbl, struct mod_hdr_key *key, u32 hash_key)
{
	struct mlx5e_mod_hdr_entry *mh, *found = NULL;

	hash_for_each_possible(tbl->hlist, mh, mod_hdr_hlist, hash_key) {
		if (!cmp_mod_hdr_info(&mh->key, key)) {
			refcount_inc(&mh->refcnt);
			found = mh;
			break;
		}
	}

	return found;
}

static void mlx5e_mod_hdr_put(struct mlx5e_priv *priv,
			      struct mlx5e_mod_hdr_entry *mh,
			      int namespace)
{
	struct mod_hdr_tbl *tbl = get_mod_hdr_table(priv, namespace);

	if (!refcount_dec_and_mutex_lock(&mh->refcnt, &tbl->lock))
		return;
	hash_del(&mh->mod_hdr_hlist);
	mutex_unlock(&tbl->lock);

	WARN_ON(!list_empty(&mh->flows));
	if (!IS_ERR_OR_NULL(mh->modify_hdr))
		mlx5_modify_header_dealloc(priv->mdev, mh->modify_hdr);

	kfree(mh);
}

static int mlx5e_attach_mod_hdr(struct mlx5e_priv *priv,
				struct mlx5e_tc_flow *flow,
				struct mlx5e_tc_flow_parse_attr *parse_attr)
{
	bool is_eswitch_flow = mlx5e_is_eswitch_flow(flow);
	int num_actions, actions_size, namespace, err;
	struct mlx5e_mod_hdr_entry *mh;
	u32 hash_key;
	struct mod_hdr_tbl *tbl;
	struct mod_hdr_key key;

	num_actions  = parse_attr->num_mod_hdr_actions;
	actions_size = MLX5_MH_ACT_SZ * num_actions;

	key.actions = parse_attr->mod_hdr_actions;
	key.num_actions = num_actions;

	hash_key = hash_mod_hdr_info(&key);

	namespace = is_eswitch_flow ?
		MLX5_FLOW_NAMESPACE_FDB : MLX5_FLOW_NAMESPACE_KERNEL;
	tbl = get_mod_hdr_table(priv, namespace);

	mutex_lock(&tbl->lock);
	mh = mlx5e_mod_hdr_get(tbl, &key, hash_key);
	if (mh) {
		mutex_unlock(&tbl->lock);
		wait_for_completion(&mh->hw_res_created);
		mutex_lock(&tbl->lock);

		if (IS_ERR(mh->modify_hdr)) {
			err = PTR_ERR(mh->modify_hdr);
			goto out_err;
		}
		goto attach_flow;
	}

	mh = kzalloc(sizeof(*mh) + actions_size, GFP_KERNEL);
	if (!mh) {
		err = -ENOMEM;
		goto alloc_mh_error;
	}

	mh->key.actions = (void *)mh + sizeof(*mh);
	memcpy(mh->key.actions, key.actions, actions_size);
	mh->key.num_actions = num_actions;
	spin_lock_init(&mh->flows_lock);
	INIT_LIST_HEAD(&mh->flows);
	refcount_set(&mh->refcnt, 1);
	init_completion(&mh->hw_res_created);

	hash_add(tbl->hlist, &mh->mod_hdr_hlist, hash_key);
	mutex_unlock(&tbl->lock);
	mh->modify_hdr = mlx5_modify_header_alloc(priv->mdev, namespace,
						  mh->key.num_actions,
						  mh->key.actions);

	mutex_lock(&tbl->lock);
	complete_all(&mh->hw_res_created);
	if (IS_ERR(mh->modify_hdr)) {
		err = PTR_ERR(mh->modify_hdr);
		goto out_err;
	}

attach_flow:
	mutex_unlock(&tbl->lock);
	flow->mh = mh;
	spin_lock(&mh->flows_lock);
	list_add(&flow->mod_hdr, &mh->flows);
	spin_unlock(&mh->flows_lock);
	if (is_eswitch_flow)
		flow->esw_attr->modify_hdr = mh->modify_hdr;
	else
		flow->nic_attr->modify_hdr = mh->modify_hdr;

	return 0;

out_err:
	mutex_unlock(&tbl->lock);
	mlx5e_mod_hdr_put(priv, mh, namespace);
	return err;

alloc_mh_error:
	mutex_unlock(&tbl->lock);
	return err;
}

static void mlx5e_detach_mod_hdr(struct mlx5e_priv *priv,
				 struct mlx5e_tc_flow *flow)
{
	int namespace = mlx5e_is_eswitch_flow(flow) ? MLX5_FLOW_NAMESPACE_FDB :
		MLX5_FLOW_NAMESPACE_KERNEL;

	/* flow wasn't fully initialized */
	if (!flow->mh)
		return;

	spin_lock(&flow->mh->flows_lock);
	list_del(&flow->mod_hdr);
	spin_unlock(&flow->mh->flows_lock);

	mlx5e_mod_hdr_put(priv, flow->mh, namespace);
	flow->mh = NULL;
}
#endif /* HAVE_TCF_PEDIT_TCFP_KEYS_EX */

static
struct mlx5_core_dev *mlx5e_hairpin_get_mdev(struct net *net, int ifindex)
{
	struct net_device *netdev;
	struct mlx5e_priv *priv;

	netdev = __dev_get_by_index(net, ifindex);
	priv = netdev_priv(netdev);
	return priv->mdev;
}

static int mlx5e_hairpin_create_transport(struct mlx5e_hairpin *hp)
{
	u32 in[MLX5_ST_SZ_DW(create_tir_in)] = {0};
	void *tirc;
	int err;

	err = mlx5_core_alloc_transport_domain(hp->func_mdev, &hp->tdn);
	if (err)
		goto alloc_tdn_err;

	tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);

	MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_DIRECT);
	MLX5_SET(tirc, tirc, inline_rqn, hp->pair->rqn[0]);
	MLX5_SET(tirc, tirc, transport_domain, hp->tdn);

	err = mlx5_core_create_tir(hp->func_mdev, in, MLX5_ST_SZ_BYTES(create_tir_in), &hp->tirn);
	if (err)
		goto create_tir_err;

	return 0;

create_tir_err:
	mlx5_core_dealloc_transport_domain(hp->func_mdev, hp->tdn);
alloc_tdn_err:
	return err;
}

static void mlx5e_hairpin_destroy_transport(struct mlx5e_hairpin *hp)
{
	mlx5_core_destroy_tir(hp->func_mdev, hp->tirn);
	mlx5_core_dealloc_transport_domain(hp->func_mdev, hp->tdn);
}

static void mlx5e_hairpin_fill_rqt_rqns(struct mlx5e_hairpin *hp, void *rqtc)
{
	u32 indirection_rqt[MLX5E_INDIR_RQT_SIZE], rqn;
	struct mlx5e_priv *priv = hp->func_priv;
	int i, ix, sz = MLX5E_INDIR_RQT_SIZE;

	mlx5e_build_default_indir_rqt(indirection_rqt, sz,
				      hp->num_channels);

	for (i = 0; i < sz; i++) {
		ix = i;
		if (priv->rss_params.hfunc == ETH_RSS_HASH_XOR)
			ix = mlx5e_bits_invert(i, ilog2(sz));
		ix = indirection_rqt[ix];
		rqn = hp->pair->rqn[ix];
		MLX5_SET(rqtc, rqtc, rq_num[i], rqn);
	}
}

static int mlx5e_hairpin_create_indirect_rqt(struct mlx5e_hairpin *hp)
{
	int inlen, err, sz = MLX5E_INDIR_RQT_SIZE;
	struct mlx5e_priv *priv = hp->func_priv;
	struct mlx5_core_dev *mdev = priv->mdev;
	void *rqtc;
	u32 *in;

	inlen = MLX5_ST_SZ_BYTES(create_rqt_in) + sizeof(u32) * sz;
	in = kvzalloc(inlen, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	rqtc = MLX5_ADDR_OF(create_rqt_in, in, rqt_context);

	MLX5_SET(rqtc, rqtc, rqt_actual_size, sz);
	MLX5_SET(rqtc, rqtc, rqt_max_size, sz);

	mlx5e_hairpin_fill_rqt_rqns(hp, rqtc);

	err = mlx5_core_create_rqt(mdev, in, inlen, &hp->indir_rqt.rqtn);
	if (!err)
		hp->indir_rqt.enabled = true;

	kvfree(in);
	return err;
}

static int mlx5e_hairpin_create_indirect_tirs(struct mlx5e_hairpin *hp)
{
	struct mlx5e_priv *priv = hp->func_priv;
	u32 in[MLX5_ST_SZ_DW(create_tir_in)];
	int tt, i, err;
	void *tirc;

	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++) {
		struct mlx5e_tirc_config ttconfig = mlx5e_tirc_get_default_config(tt);

		memset(in, 0, MLX5_ST_SZ_BYTES(create_tir_in));
		tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);

		MLX5_SET(tirc, tirc, transport_domain, hp->tdn);
		MLX5_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_INDIRECT);
		MLX5_SET(tirc, tirc, indirect_table, hp->indir_rqt.rqtn);
		mlx5e_build_indir_tir_ctx_hash(&priv->rss_params, &ttconfig, tirc, false);

		err = mlx5_core_create_tir(hp->func_mdev, in,
					   MLX5_ST_SZ_BYTES(create_tir_in), &hp->indir_tirn[tt]);
		if (err) {
			mlx5_core_warn(hp->func_mdev, "create indirect tirs failed, %d\n", err);
			goto err_destroy_tirs;
		}
	}
	return 0;

err_destroy_tirs:
	for (i = 0; i < tt; i++)
		mlx5_core_destroy_tir(hp->func_mdev, hp->indir_tirn[i]);
	return err;
}

static void mlx5e_hairpin_destroy_indirect_tirs(struct mlx5e_hairpin *hp)
{
	int tt;

	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++)
		mlx5_core_destroy_tir(hp->func_mdev, hp->indir_tirn[tt]);
}

static void mlx5e_hairpin_set_ttc_params(struct mlx5e_hairpin *hp,
					 struct ttc_params *ttc_params)
{
	struct mlx5_flow_table_attr *ft_attr = &ttc_params->ft_attr;
	int tt;

	memset(ttc_params, 0, sizeof(*ttc_params));

	ttc_params->any_tt_tirn = hp->tirn;

	for (tt = 0; tt < MLX5E_NUM_INDIR_TIRS; tt++)
		ttc_params->indir_tirn[tt] = hp->indir_tirn[tt];

	ft_attr->max_fte = MLX5E_TTC_TABLE_SIZE;
	ft_attr->level = MLX5E_TC_TTC_FT_LEVEL;
	ft_attr->prio = MLX5E_TC_PRIO;
}

static int mlx5e_hairpin_rss_init(struct mlx5e_hairpin *hp)
{
	struct mlx5e_priv *priv = hp->func_priv;
	struct ttc_params ttc_params;
	int err;

	err = mlx5e_hairpin_create_indirect_rqt(hp);
	if (err)
		return err;

	err = mlx5e_hairpin_create_indirect_tirs(hp);
	if (err)
		goto err_create_indirect_tirs;

	mlx5e_hairpin_set_ttc_params(hp, &ttc_params);
	err = mlx5e_create_ttc_table(priv, &ttc_params, &hp->ttc);
	if (err)
		goto err_create_ttc_table;

	netdev_dbg(priv->netdev, "add hairpin: using %d channels rss ttc table id %x\n",
		   hp->num_channels, hp->ttc.ft.t->id);

	return 0;

err_create_ttc_table:
	mlx5e_hairpin_destroy_indirect_tirs(hp);
err_create_indirect_tirs:
	mlx5e_destroy_rqt(priv, &hp->indir_rqt);

	return err;
}

static void mlx5e_hairpin_rss_cleanup(struct mlx5e_hairpin *hp)
{
	struct mlx5e_priv *priv = hp->func_priv;

	mlx5e_destroy_ttc_table(priv, &hp->ttc);
	mlx5e_hairpin_destroy_indirect_tirs(hp);
	mlx5e_destroy_rqt(priv, &hp->indir_rqt);
}

static struct mlx5e_hairpin *
mlx5e_hairpin_create(struct mlx5e_priv *priv, struct mlx5_hairpin_params *params,
		     int peer_ifindex)
{
	struct mlx5_core_dev *func_mdev, *peer_mdev;
	struct mlx5e_hairpin *hp;
	struct mlx5_hairpin *pair;
	int err;

	hp = kzalloc(sizeof(*hp), GFP_KERNEL);
	if (!hp)
		return ERR_PTR(-ENOMEM);

	func_mdev = priv->mdev;
	peer_mdev = mlx5e_hairpin_get_mdev(dev_net(priv->netdev), peer_ifindex);

	pair = mlx5_core_hairpin_create(func_mdev, peer_mdev, params);
	if (IS_ERR(pair)) {
		err = PTR_ERR(pair);
		goto create_pair_err;
	}
	hp->pair = pair;
	hp->func_mdev = func_mdev;
	hp->func_priv = priv;
	hp->num_channels = params->num_channels;

	err = mlx5e_hairpin_create_transport(hp);
	if (err)
		goto create_transport_err;

	if (hp->num_channels > 1) {
		err = mlx5e_hairpin_rss_init(hp);
		if (err)
			goto rss_init_err;
	}

	return hp;

rss_init_err:
	mlx5e_hairpin_destroy_transport(hp);
create_transport_err:
	mlx5_core_hairpin_destroy(hp->pair);
create_pair_err:
	kfree(hp);
	return ERR_PTR(err);
}

static void mlx5e_hairpin_destroy(struct mlx5e_hairpin *hp)
{
	if (hp->num_channels > 1)
		mlx5e_hairpin_rss_cleanup(hp);
	mlx5e_hairpin_destroy_transport(hp);
	mlx5_core_hairpin_destroy(hp->pair);
	kvfree(hp);
}

static inline u32 hash_hairpin_info(u16 peer_vhca_id, u8 prio)
{
	return (peer_vhca_id << 16 | prio);
}

static struct mlx5e_hairpin_entry *mlx5e_hairpin_get(struct mlx5e_priv *priv,
						     u16 peer_vhca_id, u8 prio)
{
	struct mlx5e_hairpin_entry *hpe;
	u32 hash_key = hash_hairpin_info(peer_vhca_id, prio);

	hash_for_each_possible(priv->fs.tc.hairpin_tbl, hpe,
			       hairpin_hlist, hash_key) {
		if (hpe->peer_vhca_id == peer_vhca_id && hpe->prio == prio) {
			refcount_inc(&hpe->refcnt);
			return hpe;
		}
	}

	return NULL;
}

static void mlx5e_hairpin_put(struct mlx5e_priv *priv,
			      struct mlx5e_hairpin_entry *hpe)
{
	/* no more hairpin flows for us, release the hairpin pair */
	if (!refcount_dec_and_mutex_lock(&hpe->refcnt, &priv->fs.tc.hairpin_tbl_lock))
		return;
	hash_del(&hpe->hairpin_hlist);
	mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);

	if (!IS_ERR_OR_NULL(hpe->hp)) {
		netdev_dbg(priv->netdev, "del hairpin: peer %s\n",
			   dev_name(hpe->hp->pair->peer_mdev->device));

		mlx5e_hairpin_destroy(hpe->hp);
	}

	WARN_ON(!list_empty(&hpe->flows));
	kfree(hpe);
}

#define UNKNOWN_MATCH_PRIO 8
static int mlx5e_hairpin_get_prio(struct mlx5e_priv *priv,
				  struct mlx5_flow_spec *spec, u8 *match_prio,
				  struct netlink_ext_ack *extack)
{
	void *headers_c, *headers_v;
	u8 prio_val, prio_mask = 0;
	bool vlan_present;

#ifdef HAVE_IEEE_DCBNL_ETS
#ifdef CONFIG_MLX5_CORE_EN_DCB
	if (priv->dcbx_dp.trust_state != MLX5_QPTS_TRUST_PCP) {
		NL_SET_ERR_MSG_MOD(extack,
				   "only PCP trust state supported for hairpin");
		return -EOPNOTSUPP;
	}
#endif
#endif
	headers_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria, outer_headers);
	headers_v = MLX5_ADDR_OF(fte_match_param, spec->match_value, outer_headers);

	vlan_present = MLX5_GET(fte_match_set_lyr_2_4, headers_v, cvlan_tag);
	if (vlan_present) {
		prio_mask = MLX5_GET(fte_match_set_lyr_2_4, headers_c, first_prio);
		prio_val = MLX5_GET(fte_match_set_lyr_2_4, headers_v, first_prio);
	}

	if (!vlan_present || !prio_mask) {
		prio_val = UNKNOWN_MATCH_PRIO;
	} else if (prio_mask != 0x7) {
		NL_SET_ERR_MSG_MOD(extack,
				   "masked priority match not supported for hairpin");
		return -EOPNOTSUPP;
	}

	*match_prio = prio_val;
	return 0;
}

static int mlx5e_hairpin_flow_add(struct mlx5e_priv *priv,
				  struct mlx5e_tc_flow *flow,
				  struct mlx5e_tc_flow_parse_attr *parse_attr,
				  struct netlink_ext_ack *extack)
{
	int peer_ifindex = parse_attr->mirred_ifindex[0];
	struct mlx5_hairpin_params params;
	struct mlx5_core_dev *peer_mdev;
	struct mlx5e_hairpin_entry *hpe;
	struct mlx5e_hairpin *hp;
	u64 link_speed64;
	u32 link_speed;
	u8 match_prio;
	u16 peer_id;
	int err;

	peer_mdev = mlx5e_hairpin_get_mdev(dev_net(priv->netdev), peer_ifindex);
	if (!MLX5_CAP_GEN(priv->mdev, hairpin) || !MLX5_CAP_GEN(peer_mdev, hairpin)) {
		NL_SET_ERR_MSG_MOD(extack, "hairpin is not supported");
		return -EOPNOTSUPP;
	}

	peer_id = MLX5_CAP_GEN(peer_mdev, vhca_id);
	err = mlx5e_hairpin_get_prio(priv, &parse_attr->spec, &match_prio, extack);
	if (err)
		return err;

	mutex_lock(&priv->fs.tc.hairpin_tbl_lock);
	hpe = mlx5e_hairpin_get(priv, peer_id, match_prio);
	if (hpe) {
		mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);
		wait_for_completion(&hpe->hw_res_created);
		mutex_lock(&priv->fs.tc.hairpin_tbl_lock);

		if (IS_ERR(hpe->hp))
			goto create_hairpin_err;
		goto attach_flow;
	}

	hpe = kzalloc(sizeof(*hpe), GFP_KERNEL);
	if (!hpe) {
		err = -ENOMEM;
		goto alloc_hairpin_err;
	}

	spin_lock_init(&hpe->flows_lock);
	INIT_LIST_HEAD(&hpe->flows);
	INIT_LIST_HEAD(&hpe->dead_peer_wait_list);
	hpe->peer_vhca_id = peer_id;
	hpe->prio = match_prio;
	refcount_set(&hpe->refcnt, 1);
	init_completion(&hpe->hw_res_created);

	hash_add(priv->fs.tc.hairpin_tbl, &hpe->hairpin_hlist,
		 hash_hairpin_info(peer_id, match_prio));
	mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);

	params.log_data_size = 15;
	params.log_data_size = min_t(u8, params.log_data_size,
				     MLX5_CAP_GEN(priv->mdev, log_max_hairpin_wq_data_sz));
	params.log_data_size = max_t(u8, params.log_data_size,
				     MLX5_CAP_GEN(priv->mdev, log_min_hairpin_wq_data_sz));

	params.log_num_packets = params.log_data_size -
				 MLX5_MPWRQ_MIN_LOG_STRIDE_SZ(priv->mdev);
	params.log_num_packets = min_t(u8, params.log_num_packets,
				       MLX5_CAP_GEN(priv->mdev, log_max_hairpin_num_packets));

	params.q_counter = priv->q_counter;
	/* set hairpin pair per each 50Gbs share of the link */
	mlx5e_port_max_linkspeed(priv->mdev, &link_speed);
	link_speed = max_t(u32, link_speed, 50000);
	link_speed64 = link_speed;
	do_div(link_speed64, 50000);
	params.num_channels = link_speed64;

	hp = mlx5e_hairpin_create(priv, &params, peer_ifindex);
	mutex_lock(&priv->fs.tc.hairpin_tbl_lock);
	hpe->hp = hp;
	complete_all(&hpe->hw_res_created);
	if (IS_ERR(hp))
		goto create_hairpin_err;

	netdev_dbg(priv->netdev, "add hairpin: tirn %x rqn %x peer %s sqn %x prio %d (log) data %d packets %d\n",
		   hp->tirn, hp->pair->rqn[0],
		   dev_name(hp->pair->peer_mdev->device),
		   hp->pair->sqn[0], match_prio, params.log_data_size, params.log_num_packets);

attach_flow:
	if (hpe->hp->num_channels > 1) {
		flow_flag_set(flow, HAIRPIN_RSS);
		flow->nic_attr->hairpin_ft = hpe->hp->ttc.ft.t;
	} else {
		flow->nic_attr->hairpin_tirn = hpe->hp->tirn;
	}
	mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);

	flow->hpe = hpe;
	spin_lock(&hpe->flows_lock);
	list_add(&flow->hairpin, &hpe->flows);
	spin_unlock(&hpe->flows_lock);

	return 0;

create_hairpin_err:
	err = PTR_ERR(hpe->hp);
	mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);
	mlx5e_hairpin_put(priv, hpe);
	return err;

alloc_hairpin_err:
	mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);
	return err;
}

static void mlx5e_hairpin_flow_del(struct mlx5e_priv *priv,
				   struct mlx5e_tc_flow *flow)
{
	/* flow wasn't fully initialized */
	if (!flow->hpe)
		return;

	spin_lock(&flow->hpe->flows_lock);
	list_del(&flow->hairpin);
	spin_unlock(&flow->hpe->flows_lock);

	mlx5e_hairpin_put(priv, flow->hpe);
	flow->hpe = NULL;
}

static int
mlx5e_tc_add_nic_flow(struct mlx5e_priv *priv,
		      struct mlx5e_tc_flow_parse_attr *parse_attr,
		      struct mlx5e_tc_flow *flow,
		      struct netlink_ext_ack *extack)
{
	struct mlx5_flow_context *flow_context = &parse_attr->spec.flow_context;
	struct mlx5_nic_flow_attr *attr = flow->nic_attr;
	struct mlx5_core_dev *dev = priv->mdev;
	struct mlx5_flow_destination dest[2] = {};
	struct mlx5_flow_act flow_act = {
		.action = attr->action,
		.flags    = FLOW_ACT_NO_APPEND,
	};
	struct mlx5_fc *counter = NULL;
	int err, dest_ix = 0;

	flow_context->flags |= FLOW_CONTEXT_HAS_TAG;
	flow_context->flow_tag = attr->flow_tag;

	if (flow_flag_test(flow, HAIRPIN)) {
		err = mlx5e_hairpin_flow_add(priv, flow, parse_attr, extack);
		if (err)
			return err;

		if (flow_flag_test(flow, HAIRPIN_RSS)) {
			dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE;
			dest[dest_ix].ft = attr->hairpin_ft;
		} else {
			dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_TIR;
			dest[dest_ix].tir_num = attr->hairpin_tirn;
		}
		dest_ix++;
	} else if (attr->action & MLX5_FLOW_CONTEXT_ACTION_FWD_DEST) {
		dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE;
		dest[dest_ix].ft = priv->fs.vlan.ft.t;
		dest_ix++;
	}

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT) {
		counter = mlx5_fc_create(dev, true);
		if (IS_ERR(counter))
			return PTR_ERR(counter);

		dest[dest_ix].type = MLX5_FLOW_DESTINATION_TYPE_COUNTER;
		dest[dest_ix].counter_id = mlx5_fc_id(counter);
		dest_ix++;
		attr->counter = counter;
	}

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		err = mlx5e_attach_mod_hdr(priv, flow, parse_attr);
		flow_act.modify_hdr = attr->modify_hdr;
		kfree(parse_attr->mod_hdr_actions);
		if (err)
			return err;
	}
#endif

	mutex_lock(&priv->fs.tc.t_lock);
	if (IS_ERR_OR_NULL(priv->fs.tc.t)) {
		int tc_grp_size, tc_tbl_size;
		u32 max_flow_counter;

		max_flow_counter = (MLX5_CAP_GEN(dev, max_flow_counter_31_16) << 16) |
				    MLX5_CAP_GEN(dev, max_flow_counter_15_0);

		tc_grp_size = min_t(int, max_flow_counter, MLX5E_TC_TABLE_MAX_GROUP_SIZE);

		tc_tbl_size = min_t(int, tc_grp_size * MLX5E_TC_TABLE_NUM_GROUPS,
				    BIT(MLX5_CAP_FLOWTABLE_NIC_RX(dev, log_max_ft_size)));

		priv->fs.tc.t =
			mlx5_create_auto_grouped_flow_table(priv->fs.ns,
							    MLX5E_TC_PRIO,
							    tc_tbl_size,
							    MLX5E_TC_TABLE_NUM_GROUPS,
							    MLX5E_TC_FT_LEVEL, 0);
		if (IS_ERR(priv->fs.tc.t)) {
			mutex_unlock(&priv->fs.tc.t_lock);
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
					   "Failed to create tc offload table\n");
#endif
			netdev_err(priv->netdev,
				   "Failed to create tc offload table\n");
			return PTR_ERR(priv->fs.tc.t);
		}
	}

	if (attr->match_level != MLX5_MATCH_NONE)
		parse_attr->spec.match_criteria_enable |= MLX5_MATCH_OUTER_HEADERS;

	flow->rule[0] = mlx5_add_flow_rules(priv->fs.tc.t, &parse_attr->spec,
					    &flow_act, dest, dest_ix);
	mutex_unlock(&priv->fs.tc.t_lock);

	if (IS_ERR(flow->rule[0]))
		return PTR_ERR(flow->rule[0]);

	return 0;
}

static void mlx5e_tc_del_nic_flow(struct mlx5e_priv *priv,
				  struct mlx5e_tc_flow *flow)
{
	struct mlx5_nic_flow_attr *attr = flow->nic_attr;
	struct mlx5_fc *counter = NULL;

	counter = attr->counter;
	if (!IS_ERR_OR_NULL(flow->rule[0]))
		mlx5_del_flow_rules(flow->rule[0]);
	mlx5_fc_destroy(priv->mdev, counter);

	mutex_lock(&priv->fs.tc.t_lock);
	if (!mlx5e_tc_num_filters(priv, MLX5_TC_FLAG(NIC_OFFLOAD)) && priv->fs.tc.t) {
		mlx5_destroy_flow_table(priv->fs.tc.t);
		priv->fs.tc.t = NULL;
	}
	mutex_unlock(&priv->fs.tc.t_lock);

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR)
		mlx5e_detach_mod_hdr(priv, flow);
#endif

	if (flow_flag_test(flow, HAIRPIN))
		mlx5e_hairpin_flow_del(priv, flow);
}

#ifdef HAVE_TCF_TUNNEL_INFO
static void mlx5e_detach_encap(struct mlx5e_priv *priv,
			       struct mlx5e_tc_flow *flow, int out_index);

static int mlx5e_attach_encap(struct mlx5e_priv *priv,
			      struct mlx5e_tc_flow *flow,
			      struct net_device *mirred_dev,
			      int out_index,
			      struct netlink_ext_ack *extack,
			      struct net_device **encap_dev,
			      bool *encap_valid);

static struct mlx5_flow_handle *
mlx5e_tc_offload_fdb_rules(struct mlx5_eswitch *esw,
			   struct mlx5e_tc_flow *flow,
			   struct mlx5_flow_spec *spec,
			   struct mlx5_esw_flow_attr *attr)
{
	struct mlx5_flow_handle *rule;

	rule = mlx5_eswitch_add_offloaded_rule(esw, spec, attr);
	if (IS_ERR(rule))
		return rule;

	if (attr->split_count) {
		flow->rule[1] = mlx5_eswitch_add_fwd_rule(esw, spec, attr);
		if (IS_ERR(flow->rule[1])) {
			mlx5_eswitch_del_offloaded_rule(esw, rule, attr);
			return flow->rule[1];
		}
	}

	return rule;
}
#endif

static void
mlx5e_tc_unoffload_fdb_rules(struct mlx5_eswitch *esw,
			     struct mlx5e_tc_flow *flow,
			   struct mlx5_esw_flow_attr *attr)
{
	flow_flag_clear(flow, OFFLOADED);

	if (attr->split_count)
		mlx5_eswitch_del_fwd_rule(esw, flow->rule[1], attr);

	mlx5_eswitch_del_offloaded_rule(esw, flow->rule[0], attr);
}

#ifdef HAVE_TCF_TUNNEL_INFO
static struct mlx5_flow_handle *
mlx5e_tc_offload_to_slow_path(struct mlx5_eswitch *esw,
			      struct mlx5e_tc_flow *flow,
			      struct mlx5_flow_spec *spec,
			      struct mlx5_esw_flow_attr *slow_attr)
{
	struct mlx5_flow_handle *rule;

	memcpy(slow_attr, flow->esw_attr, sizeof(*slow_attr));
	slow_attr->action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	slow_attr->split_count = 0;
	slow_attr->dest_chain = FDB_SLOW_PATH_CHAIN;

	rule = mlx5e_tc_offload_fdb_rules(esw, flow, spec, slow_attr);
	if (!IS_ERR(rule))
		flow_flag_set(flow, SLOW);

	return rule;
}
#endif

static void
mlx5e_tc_unoffload_from_slow_path(struct mlx5_eswitch *esw,
				  struct mlx5e_tc_flow *flow,
				  struct mlx5_esw_flow_attr *slow_attr)
{
	memcpy(slow_attr, flow->esw_attr, sizeof(*slow_attr));
	slow_attr->action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	slow_attr->split_count = 0;
	slow_attr->dest_chain = FDB_SLOW_PATH_CHAIN;
	mlx5e_tc_unoffload_fdb_rules(esw, flow, slow_attr);
	flow_flag_clear(flow, SLOW);
}

/* Caller must obtain uplink_priv->unready_flows_lock mutex before calling this
 * function.
 */
static void unready_flow_add(struct mlx5e_tc_flow *flow,
			     struct list_head *unready_flows)
{
	flow_flag_set(flow, NOT_READY);
	list_add_tail(&flow->unready, unready_flows);
}

/* Caller must obtain uplink_priv->unready_flows_lock mutex before calling this
 * function.
 */
static void unready_flow_del(struct mlx5e_tc_flow *flow)
{
	list_del(&flow->unready);
	flow_flag_clear(flow, NOT_READY);
}

static void add_unready_flow(struct mlx5e_tc_flow *flow)
{
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *rpriv;
	struct mlx5_eswitch *esw;

	esw = flow->priv->mdev->priv.eswitch;
	rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &rpriv->uplink_priv;

	mutex_lock(&uplink_priv->unready_flows_lock);
	unready_flow_add(flow, &uplink_priv->unready_flows);
	mutex_unlock(&uplink_priv->unready_flows_lock);
}

static void remove_unready_flow(struct mlx5e_tc_flow *flow)
{
	struct mlx5_rep_uplink_priv *uplink_priv;
	struct mlx5e_rep_priv *rpriv;
	struct mlx5_eswitch *esw;

	esw = flow->priv->mdev->priv.eswitch;
	rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
	uplink_priv = &rpriv->uplink_priv;

	mutex_lock(&uplink_priv->unready_flows_lock);
	unready_flow_del(flow);
	mutex_unlock(&uplink_priv->unready_flows_lock);
}

int
mlx5e_tc_add_fdb_flow(struct mlx5e_priv *priv,
		      struct mlx5e_tc_flow *flow,
		      struct netlink_ext_ack *extack)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	u32 max_chain = mlx5_eswitch_get_chain_range(esw);
	struct mlx5_esw_flow_attr *attr = flow->esw_attr;
	struct mlx5e_tc_flow_parse_attr *parse_attr = attr->parse_attr;
	u16 max_prio = mlx5_eswitch_get_prio_range(esw);
	struct mlx5_fc *counter = NULL;
#ifdef HAVE_TCF_TUNNEL_INFO
	struct net_device *out_dev, *encap_dev = NULL;
	struct mlx5e_rep_priv *rpriv;
	struct mlx5e_priv *out_priv;
	bool encap_valid = true;
#endif
	int err = 0;
	int out_index;

#ifdef HAVE_MINIFLOW
	if (!mlx5_eswitch_prios_supported(esw))
		attr->prio = 1;
#else
	if (!mlx5_eswitch_prios_supported(esw) && attr->prio != 1) {
		NL_SET_ERR_MSG(extack, "E-switch priorities unsupported, upgrade FW");
		return -EOPNOTSUPP;
	}
#endif

	if (attr->chain > max_chain) {
		NL_SET_ERR_MSG(extack, "Requested chain is out of supported range");
		return -EOPNOTSUPP;
	}

	if (attr->prio > max_prio) {
		NL_SET_ERR_MSG(extack, "Requested priority is out of supported range");
		return -EOPNOTSUPP;
	}

#ifdef HAVE_TCF_TUNNEL_INFO
	for (out_index = 0; out_index < MLX5_MAX_FLOW_FWD_VPORTS; out_index++) {
		int mirred_ifindex;

		if (!(attr->dests[out_index].flags & MLX5_ESW_DEST_ENCAP))
			continue;

		mirred_ifindex = parse_attr->mirred_ifindex[out_index];
		out_dev = __dev_get_by_index(dev_net(priv->netdev),
					     mirred_ifindex);
		err = mlx5e_attach_encap(priv, flow, out_dev, out_index,
					 extack,
					 &encap_dev, &encap_valid);
		if (err)
			return err;

		out_priv = netdev_priv(encap_dev);
		rpriv = out_priv->ppriv;
		attr->dests[out_index].rep = rpriv->rep;
		attr->dests[out_index].mdev = out_priv->mdev;
	}
#endif /* HAVE_TCF_TUNNEL_INFO */

	err = mlx5_eswitch_add_vlan_action(esw, attr);
	if (err)
		return err;

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR) {
		err = mlx5e_attach_mod_hdr(priv, flow, parse_attr);
		kfree(parse_attr->mod_hdr_actions);
		parse_attr->mod_hdr_actions = NULL;
		if (err)
			return err;
	}
#endif

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT) {
		counter = mlx5_fc_create(attr->counter_dev, true);
		if (IS_ERR(counter))
			return PTR_ERR(counter);

		attr->counter = counter;
	}

#ifdef HAVE_TCF_TUNNEL_INFO
	/* we get here if one of the following takes place:
	 * (1) there's no error
	 * (2) there's an encap action and we don't have valid neigh
	 */
	if (!encap_valid) {
		/* continue with goto slow path rule instead */
		struct mlx5_esw_flow_attr slow_attr;

		flow->rule[0] = mlx5e_tc_offload_to_slow_path(esw, flow, &parse_attr->spec, &slow_attr);
	} else {
		flow->rule[0] = mlx5e_tc_offload_fdb_rules(esw, flow, &parse_attr->spec, attr);
	}

	if (IS_ERR(flow->rule[0]))
		return PTR_ERR(flow->rule[0]);
	else
		flow_flag_set(flow, OFFLOADED);
#endif

	return 0;
}

static bool mlx5_flow_has_geneve_opt(struct mlx5e_tc_flow *flow)
{
	struct mlx5_flow_spec *spec = &flow->esw_attr->parse_attr->spec;
	void *headers_v = MLX5_ADDR_OF(fte_match_param,
				       spec->match_value,
				       misc_parameters_3);
	u32 geneve_tlv_opt_0_data = MLX5_GET(fte_match_set_misc3,
					     headers_v,
					     geneve_tlv_option_0_data);

	return !!geneve_tlv_opt_0_data;
}

static void mlx5e_tc_del_fdb_flow_simple(struct mlx5e_priv *priv,
					 struct mlx5e_tc_flow *flow)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_esw_flow_attr *attr = flow->esw_attr;
	struct mlx5_esw_flow_attr slow_attr;
	int out_index;

	if (flow_flag_test(flow, NOT_READY)) {
		remove_unready_flow(flow);
		return;
	}

	if (mlx5e_is_offloaded_flow(flow)) {
		if (flow_flag_test(flow, SLOW))
			mlx5e_tc_unoffload_from_slow_path(esw, flow, &slow_attr);
		else
			mlx5e_tc_unoffload_fdb_rules(esw, flow, attr);
	}


	if (mlx5_flow_has_geneve_opt(flow))
		mlx5_geneve_tlv_option_del(priv->mdev->geneve);

	mlx5_eswitch_del_vlan_action(esw, attr);

#ifdef HAVE_TCF_TUNNEL_INFO
	for (out_index = 0; out_index < MLX5_MAX_FLOW_FWD_VPORTS; out_index++)
		if (attr->dests[out_index].flags & MLX5_ESW_DEST_ENCAP)
			mlx5e_detach_encap(priv, flow, out_index);
#endif

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR)
		mlx5e_detach_mod_hdr(priv, flow);
#endif

	if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT)
		mlx5_fc_destroy(attr->counter_dev, attr->counter);
}

static void mlx5e_tc_del_fdb_flow(struct mlx5e_priv *priv,
				  struct mlx5e_tc_flow *flow)
{
	struct mlx5_esw_flow_attr *attr = flow->esw_attr;

	if (mlx5e_is_simple_flow(flow)) {
		mlx5e_tc_del_fdb_flow_simple(priv, flow);
	} else {
		mlx5e_del_miniflow_list(flow);

		if (attr->action & MLX5_FLOW_CONTEXT_ACTION_COUNT)
			mlx5_fc_destroy(priv->mdev, flow->dummy_counter);
	}

#if defined(HAVE_TCF_PEDIT_TCFP_KEYS_EX) && defined(HAVE_TCF_TUNNEL_INFO)
	if (attr->parse_attr) {
		kfree(attr->parse_attr->mod_hdr_actions);
		kvfree(attr->parse_attr);
	}
#endif
}

#if defined(HAVE_TCF_TUNNEL_INFO) || defined(HAVE_TC_CLSFLOWER_STATS)
static struct mlx5_fc *mlx5e_tc_get_counter(struct mlx5e_tc_flow *flow)
{
	if (mlx5e_is_eswitch_flow(flow))
		return flow->esw_attr->counter;
	else
		return flow->nic_attr->counter;
}
#endif

#ifdef HAVE_TCF_TUNNEL_INFO
void mlx5e_tc_encap_flows_add(struct mlx5e_priv *priv,
			      struct mlx5e_encap_entry *e,
			      struct list_head *flow_list)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_esw_flow_attr slow_attr, *esw_attr;
	struct mlx5_flow_handle *rule;
	struct mlx5_flow_spec *spec;
	struct mlx5e_tc_flow *flow;
	int err;

	e->pkt_reformat = mlx5_packet_reformat_alloc(priv->mdev,
						     e->reformat_type,
						     e->encap_size, e->encap_header,
						     MLX5_FLOW_NAMESPACE_FDB);
	if (IS_ERR(e->pkt_reformat)) {
		mlx5_core_warn(priv->mdev, "Failed to offload cached encapsulation header, %lu\n",
			       PTR_ERR(e->pkt_reformat));
		return;
	}
	e->flags |= MLX5_ENCAP_ENTRY_VALID;
	mlx5e_rep_queue_neigh_stats_work(priv);

	list_for_each_entry(flow, flow_list, tmp_list) {
		bool all_flow_encaps_valid = true;
		int i;

		if (!mlx5e_is_offloaded_flow(flow))
			continue;
		esw_attr = flow->esw_attr;
		spec = &esw_attr->parse_attr->spec;

		esw_attr->dests[flow->tmp_efi_index].pkt_reformat = e->pkt_reformat;
		esw_attr->dests[flow->tmp_efi_index].flags |= MLX5_ESW_DEST_ENCAP_VALID;
		/* Flow can be associated with multiple encap entries.
		 * Before offloading the flow verify that all of them have
		 * a valid neighbour.
		 */
		for (i = 0; i < MLX5_MAX_FLOW_FWD_VPORTS; i++) {
			if (!(esw_attr->dests[i].flags & MLX5_ESW_DEST_ENCAP))
				continue;
			if (!(esw_attr->dests[i].flags & MLX5_ESW_DEST_ENCAP_VALID)) {
				all_flow_encaps_valid = false;
				break;
			}
		}
		/* Do not offload flows with unresolved neighbors */
		if (!all_flow_encaps_valid)
			continue;
		/* update from slow path rule to encap rule */
		rule = mlx5e_tc_offload_fdb_rules(esw, flow, spec, esw_attr);
		if (IS_ERR(rule)) {
			err = PTR_ERR(rule);
			mlx5_core_warn(priv->mdev, "Failed to update cached encapsulation flow, %d\n",
				       err);
			continue;
		}

		mlx5e_tc_unoffload_from_slow_path(esw, flow, &slow_attr);
		flow->rule[0] = rule;
		/* was unset when slow path rule removed */
		flow_flag_set(flow, OFFLOADED);
	}
}

void mlx5e_tc_encap_flows_del(struct mlx5e_priv *priv,
			      struct mlx5e_encap_entry *e,
			      struct list_head *flow_list)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_esw_flow_attr slow_attr;
	struct mlx5_flow_handle *rule;
	struct mlx5_flow_spec *spec;
	struct mlx5e_tc_flow *flow;
	struct encap_id_entry *ei;
	int err;

	list_for_each_entry(flow, flow_list, tmp_list) {
		if (!mlx5e_is_offloaded_flow(flow))
			continue;
		spec = &flow->esw_attr->parse_attr->spec;

		/* update from encap rule to slow path rule */
		rule = mlx5e_tc_offload_to_slow_path(esw, flow, spec, &slow_attr);
		/* mark the flow's encap dest as non-valid */
		flow->esw_attr->dests[flow->tmp_efi_index].flags &= ~MLX5_ESW_DEST_ENCAP_VALID;

		if (IS_ERR(rule)) {
			err = PTR_ERR(rule);
			mlx5_core_warn(priv->mdev, "Failed to update slow path (encap) flow, %d\n",
				       err);
			continue;
		}

		mlx5e_tc_unoffload_fdb_rules(esw, flow, flow->esw_attr);
		flow->rule[0] = rule;
		/* was unset when fast path rule removed */
		flow_flag_set(flow, OFFLOADED);
	}

	/* we know that the encap is valid */
	e->flags &= ~MLX5_ENCAP_ENTRY_VALID;
	/* dealloc encap_id in mlx5e_encap_put() when refcnt is 0
	 * since some flows may still use it now. Otherwise, firmware
	 * may complain and won't reuse this encap_id anymore.
	 */
	ei = kzalloc(sizeof(*ei), GFP_KERNEL);
	if (!ei) {
		mlx5_core_warn(priv->mdev, "Failed to alloc encap_id_entry");
		mlx5_packet_reformat_dealloc(priv->mdev, e->pkt_reformat);
	} else {
		ei->pkt_reformat = e->pkt_reformat;
		list_add(&ei->list, &e->encap_id_list);
	}
}

/* Takes reference to all flows attached to encap and adds the flows to
 * flow_list using 'tmp_list' list_head in mlx5e_tc_flow.
 */
void mlx5e_take_all_encap_flows(struct mlx5e_encap_entry *e, struct list_head *flow_list)
{
	struct encap_flow_item *efi;
	struct mlx5e_tc_flow *flow;

	list_for_each_entry(efi, &e->flows, list) {
		flow = container_of(efi, struct mlx5e_tc_flow, encaps[efi->index]);
		if (IS_ERR(mlx5e_flow_get(flow)))
			continue;
		wait_for_completion(&flow->init_done);

		flow->tmp_efi_index = efi->index;
		list_add(&flow->tmp_list, flow_list);
	}
}

/* Iterate over tmp_list of flows attached to flow_list head. */
void mlx5e_put_encap_flow_list(struct mlx5e_priv *priv, struct list_head *flow_list)
{
	struct mlx5e_tc_flow *flow, *tmp;

	list_for_each_entry_safe(flow, tmp, flow_list, tmp_list)
		mlx5e_flow_put(priv, flow);
}

static struct mlx5e_encap_entry *
mlx5e_get_next_valid_encap(struct mlx5e_neigh_hash_entry *nhe,
			   struct mlx5e_encap_entry *e)
{
	struct mlx5e_encap_entry *next = NULL;

retry:
	rcu_read_lock();

	/* find encap with non-zero reference counter value */
	for (next = e ?
		     list_next_or_null_rcu(&nhe->encap_list,
					   &e->encap_list,
					   struct mlx5e_encap_entry,
					   encap_list) :
		     list_first_or_null_rcu(&nhe->encap_list,
					    struct mlx5e_encap_entry,
					    encap_list);
	     next;
	     next = list_next_or_null_rcu(&nhe->encap_list,
					  &next->encap_list,
					  struct mlx5e_encap_entry,
					  encap_list))
		if (mlx5e_encap_take(next))
			break;

	rcu_read_unlock();

	/* release starting encap */
	if (e)
		mlx5e_encap_put(netdev_priv(e->out_dev), e);
	if (!next)
		return next;

	/* wait for encap to be fully initialized */
	wait_for_completion(&next->hw_res_created);
	/* continue searching if encap entry is not in valid state after completion */
	if (!(next->flags & MLX5_ENCAP_ENTRY_VALID)) {
		e = next;
		goto retry;
	}

	return next;
}
#endif /* HAVE_TCF_TUNNEL_INFO */

void mlx5e_tc_update_neigh_used_value(struct mlx5e_neigh_hash_entry *nhe)
{
	struct mlx5e_neigh *m_neigh = &nhe->m_neigh;
#ifdef HAVE_TCF_TUNNEL_INFO
	struct mlx5e_encap_entry *e = NULL;
	u64 bytes, packets, lastuse = 0;
	struct mlx5e_tc_flow *flow;
	struct mlx5_fc *counter;
#endif
	struct neigh_table *tbl;
	bool neigh_used = false;
	struct neighbour *n;

	if (m_neigh->family == AF_INET) {
		tbl = &arp_tbl;
#if defined(HAVE_IPV6_STUBS_H) && IS_ENABLED(CONFIG_IPV6)
	} else if (m_neigh->family == AF_INET6) {
		if (!ipv6_stub || !ipv6_stub->nd_tbl)
			return;
		tbl = ipv6_stub->nd_tbl;	
#endif
	} else {
		return;
	}

#ifdef HAVE_TCF_TUNNEL_INFO
	/* mlx5e_get_next_valid_encap() releases previous encap before returning
	 * next one.
	 */
	while ((e = mlx5e_get_next_valid_encap(nhe, e)) != NULL) {
		struct mlx5e_priv *priv = netdev_priv(e->out_dev);
		struct encap_flow_item *efi, *tmp;
		struct mlx5_eswitch *esw =
			priv->mdev->priv.eswitch;
		LIST_HEAD(flow_list);

		mutex_lock(&esw->offloads.encap_tbl_lock);
		list_for_each_entry_safe(efi, tmp, &e->flows, list) {
			flow = container_of(efi, struct mlx5e_tc_flow,
					    encaps[efi->index]);
			if (IS_ERR(mlx5e_flow_get(flow)))
				continue;
			list_add(&flow->tmp_list, &flow_list);

			if (mlx5e_is_offloaded_flow(flow)) {
				counter = mlx5e_tc_get_counter(flow);
				mlx5_fc_query_cached(counter, &bytes, &packets, &lastuse);
				if (time_after((unsigned long)lastuse, nhe->reported_lastuse)) {
					neigh_used = true;
					break;
				}
			}
		}
		mutex_unlock(&esw->offloads.encap_tbl_lock);


		mlx5e_put_encap_flow_list(priv, &flow_list);
		if (neigh_used) {
			/* release current encap before breaking the loop */
			mlx5e_encap_put(priv, e);
			break;
		}

	}
#endif /* HAVE_TCF_TUNNEL_INFO */

	if (neigh_used) {
		nhe->reported_lastuse = jiffies;

		/* find the relevant neigh according to the cached device and
		 * dst ip pair
		 */
		n = neigh_lookup(tbl, &m_neigh->dst_ip, m_neigh->dev);
		if (!n)
			return;

		neigh_event_send(n, NULL);
		neigh_release(n);
	}
}

#ifdef HAVE_TCF_TUNNEL_INFO
static void mlx5e_encap_dealloc(struct mlx5e_priv *priv, struct mlx5e_encap_entry *e)
{
	struct encap_id_entry *ei, *tmp;

	WARN_ON(!list_empty(&e->flows));

	if (e->compl_result > 0) {
		mlx5e_rep_encap_entry_detach(netdev_priv(e->out_dev), e);

		if (e->flags & MLX5_ENCAP_ENTRY_VALID)
			mlx5_packet_reformat_dealloc(priv->mdev, e->pkt_reformat);

		list_for_each_entry_safe(ei, tmp, &e->encap_id_list, list) {
			mlx5_packet_reformat_dealloc(priv->mdev, ei->pkt_reformat);
			kfree(ei);
		}
	}

	kfree(e->encap_header);
	kfree_rcu(e, rcu);
}

void mlx5e_encap_put(struct mlx5e_priv *priv, struct mlx5e_encap_entry *e)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	if (!refcount_dec_and_mutex_lock(&e->refcnt, &esw->offloads.encap_tbl_lock))
		return;
	hash_del_rcu(&e->encap_hlist);
	mutex_unlock(&esw->offloads.encap_tbl_lock);

	mlx5e_encap_dealloc(priv, e);
}

static void mlx5e_detach_encap(struct mlx5e_priv *priv,
			       struct mlx5e_tc_flow *flow, int out_index)
{
	struct mlx5e_encap_entry *e = flow->encaps[out_index].e;
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	/* flow wasn't fully initialized */
	if (!e)
		return;

	mutex_lock(&esw->offloads.encap_tbl_lock);
	list_del(&flow->encaps[out_index].list);
	flow->encaps[out_index].e = NULL;
	if (!refcount_dec_and_test(&e->refcnt)) {
		mutex_unlock(&esw->offloads.encap_tbl_lock);
		return;
	}
	hash_del_rcu(&e->encap_hlist);
	mutex_unlock(&esw->offloads.encap_tbl_lock);

	mlx5e_encap_dealloc(priv, e);
}
#endif /* HAVE_TCF_TUNNEL_INFO */

static void __mlx5e_tc_del_fdb_peer_flow(struct mlx5e_tc_flow *flow)
{
	struct mlx5_eswitch *esw = flow->priv->mdev->priv.eswitch;

	if (!flow_flag_test(flow, DUP))
		return;

	mutex_lock(&esw->offloads.peer_mutex);
	list_del(&flow->peer);
	mutex_unlock(&esw->offloads.peer_mutex);

	flow_flag_clear(flow, DUP);

	if (refcount_dec_and_test(&flow->peer_flow->refcnt)) {
		mlx5e_tc_del_fdb_flow(flow->peer_flow->priv, flow->peer_flow);
		kfree(flow->peer_flow);
	}

	flow->peer_flow = NULL;
}

static void mlx5e_tc_del_fdb_peer_flow(struct mlx5e_tc_flow *flow)
{
	struct mlx5_core_dev *dev = flow->priv->mdev;
	struct mlx5_devcom *devcom = dev->priv.devcom;
	struct mlx5_eswitch *peer_esw;

	if (!flow_flag_test(flow, DUP))
		return;

	peer_esw = mlx5_devcom_get_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
	if (!peer_esw)
		return;

	__mlx5e_tc_del_fdb_peer_flow(flow);
	mlx5_devcom_release_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
}

static void mlx5e_tc_del_flow(struct mlx5e_priv *priv,
			      struct mlx5e_tc_flow *flow)
{
	if (mlx5e_is_eswitch_flow(flow)) {
		mlx5e_tc_del_fdb_peer_flow(flow);
		mlx5e_tc_del_fdb_flow(priv, flow);
	} else {
		mlx5e_tc_del_nic_flow(priv, flow);
	}
}

#ifdef HAVE_TCF_TUNNEL_INFO
static int parse_tunnel_attr(struct mlx5e_priv *priv,
			     struct mlx5_flow_spec *spec,
			     struct tc_cls_flower_offload *f,
			     struct net_device *filter_dev, u8 *match_level
#ifndef HAVE_TC_SETUP_FLOW_ACTION
				 , struct flow_rule *rule
#endif
				)
{
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
	struct netlink_ext_ack *extack = f->common.extack;
#endif
	void *headers_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
				       outer_headers);
	void *headers_v = MLX5_ADDR_OF(fte_match_param, spec->match_value,
				       outer_headers);
#ifdef HAVE_TC_SETUP_FLOW_ACTION
	struct flow_rule *rule = tc_cls_flower_offload_flow_rule(f);
#endif
	struct flow_match_control enc_control;
	int err;

	err = mlx5e_tc_tun_parse(filter_dev, priv, spec, f,
				 headers_c, headers_v, match_level
#ifndef HAVE_TC_SETUP_FLOW_ACTION
				 , rule
#endif
				       );
	if (err) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
		NL_SET_ERR_MSG_MOD(extack,
#else
		netdev_err(priv->netdev,
#endif
				   "failed to parse tunnel attributes");
		return err;
	}

	flow_rule_match_enc_control(rule, &enc_control);

	if (enc_control.key->addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_enc_ipv4_addrs(rule, &match);
		MLX5_SET(fte_match_set_lyr_2_4, headers_c,
			 src_ipv4_src_ipv6.ipv4_layout.ipv4,
			 ntohl(match.mask->src));
		MLX5_SET(fte_match_set_lyr_2_4, headers_v,
			 src_ipv4_src_ipv6.ipv4_layout.ipv4,
			 ntohl(match.key->src));

		MLX5_SET(fte_match_set_lyr_2_4, headers_c,
			 dst_ipv4_dst_ipv6.ipv4_layout.ipv4,
			 ntohl(match.mask->dst));
		MLX5_SET(fte_match_set_lyr_2_4, headers_v,
			 dst_ipv4_dst_ipv6.ipv4_layout.ipv4,
			 ntohl(match.key->dst));

		MLX5_SET_TO_ONES(fte_match_set_lyr_2_4, headers_c, ethertype);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ethertype, ETH_P_IP);
	} else if (enc_control.key->addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_enc_ipv6_addrs(rule, &match);
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       &match.mask->src, MLX5_FLD_SZ_BYTES(ipv6_layout, ipv6));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       &match.key->src, MLX5_FLD_SZ_BYTES(ipv6_layout, ipv6));

		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       &match.mask->dst, MLX5_FLD_SZ_BYTES(ipv6_layout, ipv6));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       &match.key->dst, MLX5_FLD_SZ_BYTES(ipv6_layout, ipv6));

		MLX5_SET_TO_ONES(fte_match_set_lyr_2_4, headers_c, ethertype);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ethertype, ETH_P_IPV6);
	}

#ifdef HAVE_FLOW_DISSECTOR_KEY_ENC_IP
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IP)) {
		struct flow_match_ip match;

		flow_rule_match_enc_ip(rule, &match);
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_ecn,
			 match.mask->tos & 0x3);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_ecn,
			 match.key->tos & 0x3);

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_dscp,
			 match.mask->tos >> 2);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_dscp,
			 match.key->tos  >> 2);

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ttl_hoplimit,
			 match.mask->ttl);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ttl_hoplimit,
			 match.key->ttl);

		if (match.mask->ttl &&
		    !MLX5_CAP_ESW_FLOWTABLE_FDB
			(priv->mdev,
			 ft_field_support.outer_ipv4_ttl)) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
#else
			netdev_err(priv->netdev,
#endif
					   "Matching on TTL is not supported");
			return -EOPNOTSUPP;
		}

	}
#endif

	/* Enforce DMAC when offloading incoming tunneled flows.
	 * Flow counters require a match on the DMAC.
	 */
	MLX5_SET_TO_ONES(fte_match_set_lyr_2_4, headers_c, dmac_47_16);
	MLX5_SET_TO_ONES(fte_match_set_lyr_2_4, headers_c, dmac_15_0);
	ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				     dmac_47_16), priv->netdev->dev_addr);

	/* let software handle IP fragments */
	MLX5_SET(fte_match_set_lyr_2_4, headers_c, frag, 1);
	MLX5_SET(fte_match_set_lyr_2_4, headers_v, frag, 0);

	return 0;
}
#endif /* HAVE_TCF_TUNNEL_INFO */

static void *get_match_headers_criteria(u32 flags,
					struct mlx5_flow_spec *spec)
{
	return (flags & MLX5_FLOW_CONTEXT_ACTION_DECAP) ?
		MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
			     inner_headers) :
		MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
			     outer_headers);
}

static void *get_match_headers_value(u32 flags,
				     struct mlx5_flow_spec *spec)
{
	return (flags & MLX5_FLOW_CONTEXT_ACTION_DECAP) ?
		MLX5_ADDR_OF(fte_match_param, spec->match_value,
			     inner_headers) :
		MLX5_ADDR_OF(fte_match_param, spec->match_value,
			     outer_headers);
}

static int __parse_cls_flower(struct mlx5e_priv *priv,
			      struct mlx5_flow_spec *spec,
			      struct tc_cls_flower_offload *f,
			      struct net_device *filter_dev,
			      u8 *inner_match_level, u8 *outer_match_level,
			      bool *is_tunnel_flow
#ifndef HAVE_TC_SETUP_FLOW_ACTION
			      , struct flow_rule *rule
#endif
			      )

{
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
	struct netlink_ext_ack *extack = f->common.extack;
#endif
	void *headers_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
				       outer_headers);
	void *headers_v = MLX5_ADDR_OF(fte_match_param, spec->match_value,
				       outer_headers);
#ifdef HAVE_FLOW_DISSECTOR_KEY_CVLAN
	void *misc_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
				    misc_parameters);
	void *misc_v = MLX5_ADDR_OF(fte_match_param, spec->match_value,
				    misc_parameters);
#endif

#ifdef HAVE_TC_SETUP_FLOW_ACTION
	struct flow_rule *rule = tc_cls_flower_offload_flow_rule(f);
#endif
	struct flow_dissector *dissector = rule->match.dissector;
	u16 addr_type = 0;
	u8 ip_proto = 0;
	u8 *match_level;

	match_level = outer_match_level;

	if (dissector->used_keys &
	    ~(BIT(FLOW_DISSECTOR_KEY_CONTROL) |
	      BIT(FLOW_DISSECTOR_KEY_BASIC) |
	      BIT(FLOW_DISSECTOR_KEY_ETH_ADDRS) |
#ifdef HAVE_FLOW_DISSECTOR_KEY_VLAN
	      BIT(FLOW_DISSECTOR_KEY_VLAN) |
#else
	      BIT(FLOW_DISSECTOR_KEY_VLANID) |
#endif
#ifdef HAVE_FLOW_DISSECTOR_KEY_CVLAN
	      BIT(FLOW_DISSECTOR_KEY_CVLAN) |
#endif
	      BIT(FLOW_DISSECTOR_KEY_IPV4_ADDRS) |
	      BIT(FLOW_DISSECTOR_KEY_IPV6_ADDRS) |
#ifdef HAVE_TCF_TUNNEL_INFO
	      BIT(FLOW_DISSECTOR_KEY_PORTS) |
	      BIT(FLOW_DISSECTOR_KEY_ENC_KEYID) |
	      BIT(FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS) |
	      BIT(FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS) |
	      BIT(FLOW_DISSECTOR_KEY_ENC_PORTS)	|
	      BIT(FLOW_DISSECTOR_KEY_ENC_CONTROL) |
#else
	      BIT(FLOW_DISSECTOR_KEY_PORTS) |
#endif
#ifdef HAVE_FLOW_DISSECTOR_KEY_TCP
	      BIT(FLOW_DISSECTOR_KEY_TCP) |
	      BIT(FLOW_DISSECTOR_KEY_IP)  |
#endif
#ifdef HAVE_FLOW_DISSECTOR_KEY_IP
	      BIT(FLOW_DISSECTOR_KEY_IP)  |
#endif
#ifdef HAVE_FLOW_DISSECTOR_KEY_ENC_IP
	      BIT(FLOW_DISSECTOR_KEY_ENC_IP) |
#endif
#ifdef HAVE_FLOW_DISSECTOR_KEY_ENC_OPTS
	      BIT(FLOW_DISSECTOR_KEY_ENC_OPTS))) {
#else
		0)) {
#endif
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
		NL_SET_ERR_MSG_MOD(extack, "Unsupported key");
#endif
		netdev_warn(priv->netdev, "Unsupported key used: 0x%x\n",
			    dissector->used_keys);
		return -EOPNOTSUPP;
	}

#ifdef HAVE_TCF_TUNNEL_INFO
	if ((flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS) ||
	     flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_KEYID) ||
	     flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_PORTS)
#ifdef HAVE_FLOW_DISSECTOR_KEY_ENC_OPTS
	     || flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_OPTS)
#endif
	     ) &&
	    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ENC_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_enc_control(rule, &match);
		switch (match.key->addr_type) {
		case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
			if (parse_tunnel_attr(priv, spec, f, filter_dev, outer_match_level
#ifndef HAVE_TC_SETUP_FLOW_ACTION
					      , rule
#endif

						))
				return -EOPNOTSUPP;
			break;
		default:
			return -EOPNOTSUPP;
		}

		/* At this point, header pointers should point to the inner
		 * headers, outer header were already set by parse_tunnel_attr
		 */
		match_level = inner_match_level;
		headers_c = get_match_headers_criteria(MLX5_FLOW_CONTEXT_ACTION_DECAP,
						       spec);
		headers_v = get_match_headers_value(MLX5_FLOW_CONTEXT_ACTION_DECAP,
						    spec);
		*is_tunnel_flow = true;
	}
#endif /* HAVE_TCF_TUNNEL_INFO */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ethertype,
			 ntohs(match.mask->n_proto));
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ethertype,
			 ntohs(match.key->n_proto));

		if (match.mask->n_proto)
			*match_level = MLX5_MATCH_L2;
	}
#ifdef HAVE_FLOW_DISSECTOR_KEY_VLAN
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN) ||
	    is_vlan_dev(filter_dev)) {
		struct flow_dissector_key_vlan filter_dev_mask;
		struct flow_dissector_key_vlan filter_dev_key;
		struct flow_match_vlan match;

		if (is_vlan_dev(filter_dev)) {
			match.key = &filter_dev_key;
			match.key->vlan_id = vlan_dev_vlan_id(filter_dev);
#ifdef HAVE_FLOW_DISSECTOR_KEY_VLAN_TPID
			match.key->vlan_tpid = vlan_dev_vlan_proto(filter_dev);
#endif
			match.key->vlan_priority = 0;
			match.mask = &filter_dev_mask;
			memset(match.mask, 0xff, sizeof(*match.mask));
			match.mask->vlan_priority = 0;
		} else {
			flow_rule_match_vlan(rule, &match);
		}
#ifdef HAVE_FLOW_DISSECTOR_KEY_CVLAN
		if (match.mask->vlan_id ||
		    match.mask->vlan_priority ||
		    match.mask->vlan_tpid) {
			if (match.key->vlan_tpid == htons(ETH_P_8021AD)) {
				MLX5_SET(fte_match_set_lyr_2_4, headers_c,
					 svlan_tag, 1);
				MLX5_SET(fte_match_set_lyr_2_4, headers_v,
					 svlan_tag, 1);
			} else {
				MLX5_SET(fte_match_set_lyr_2_4, headers_c,
					 cvlan_tag, 1);
				MLX5_SET(fte_match_set_lyr_2_4, headers_v,
					 cvlan_tag, 1);
			}
#else
		if (match.mask->vlan_id || match.mask->vlan_priority) {
			MLX5_SET(fte_match_set_lyr_2_4, headers_c, cvlan_tag, 1);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, cvlan_tag, 1);
#endif /* HAVE_FLOW_DISSECTOR_KEY_CVLAN */

			MLX5_SET(fte_match_set_lyr_2_4, headers_c, first_vid,
				 match.mask->vlan_id);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, first_vid,
				 match.key->vlan_id);

			MLX5_SET(fte_match_set_lyr_2_4, headers_c, first_prio,
				 match.mask->vlan_priority);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, first_prio,
				 match.key->vlan_priority);

			*match_level = MLX5_MATCH_L2;
		}
#else /* HAVE_FLOW_DISSECTOR_KEY_VLAN */
	if (dissector_uses_key(f->dissector, FLOW_DISSECTOR_KEY_VLANID)) {
		struct flow_dissector_key_tags *key =
			skb_flow_dissector_target(f->dissector,
						  FLOW_DISSECTOR_KEY_VLANID,
						  f->key);
		struct flow_dissector_key_tags *mask =
			skb_flow_dissector_target(f->dissector,
						  FLOW_DISSECTOR_KEY_VLANID,
						  f->mask);
		if (mask->vlan_id) {
			MLX5_SET(fte_match_set_lyr_2_4, headers_c, cvlan_tag, 1);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, cvlan_tag, 1);
			MLX5_SET(fte_match_set_lyr_2_4, headers_c, first_vid, mask->vlan_id);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, first_vid, key->vlan_id);

			*match_level = MLX5_MATCH_L2;
		}
#endif /* HAVE_FLOW_DISSECTOR_KEY_VLAN */
	} else if (*match_level != MLX5_MATCH_NONE) {
		/* cvlan_tag enabled in match criteria and
		 * disabled in match value means both S & C tags
		 * don't exist (untagged of both)
		 */
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, cvlan_tag, 1);
		*match_level = MLX5_MATCH_L2;
	}

#ifdef HAVE_FLOW_DISSECTOR_KEY_CVLAN 
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN)) {
		struct flow_match_vlan match;

		flow_rule_match_cvlan(rule, &match);
		if (match.mask->vlan_id ||
		    match.mask->vlan_priority ||
		    match.mask->vlan_tpid) {
			if (match.key->vlan_tpid == htons(ETH_P_8021AD)) {
				MLX5_SET(fte_match_set_misc, misc_c,
					 outer_second_svlan_tag, 1);
				MLX5_SET(fte_match_set_misc, misc_v,
					 outer_second_svlan_tag, 1);
			} else {
				MLX5_SET(fte_match_set_misc, misc_c,
					 outer_second_cvlan_tag, 1);
				MLX5_SET(fte_match_set_misc, misc_v,
					 outer_second_cvlan_tag, 1);
			}

			MLX5_SET(fte_match_set_misc, misc_c, outer_second_vid,
				 match.mask->vlan_id);
			MLX5_SET(fte_match_set_misc, misc_v, outer_second_vid,
				 match.key->vlan_id);
			MLX5_SET(fte_match_set_misc, misc_c, outer_second_prio,
				 match.mask->vlan_priority);
			MLX5_SET(fte_match_set_misc, misc_v, outer_second_prio,
				 match.key->vlan_priority);

			*match_level = MLX5_MATCH_L2;
		}
	}
#endif /* HAVE_FLOW_DISSECTOR_KEY_CVLAN */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs match;

		flow_rule_match_eth_addrs(rule, &match);
		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
					     dmac_47_16),
				match.mask->dst);
		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
					     dmac_47_16),
				match.key->dst);

		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
					     smac_47_16),
				match.mask->src);
		ether_addr_copy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
					     smac_47_16),
				match.key->src);

		if (!is_zero_ether_addr(match.mask->src) ||
		    !is_zero_ether_addr(match.mask->dst))
			*match_level = MLX5_MATCH_L2;
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control match;

		flow_rule_match_control(rule, &match);
		addr_type = match.key->addr_type;

		/* the HW doesn't support frag first/later */
		if (match.mask->flags & FLOW_DIS_FIRST_FRAG)
			return -EOPNOTSUPP;

		if (match.mask->flags & FLOW_DIS_IS_FRAGMENT) {
			MLX5_SET(fte_match_set_lyr_2_4, headers_c, frag, 1);
			MLX5_SET(fte_match_set_lyr_2_4, headers_v, frag,
				 match.key->flags & FLOW_DIS_IS_FRAGMENT);

			/* the HW doesn't need L3 inline to match on frag=no */
			if (!(match.key->flags & FLOW_DIS_IS_FRAGMENT))
				*match_level = MLX5_MATCH_L2;
	/* ***  L2 attributes parsing up to here *** */
			else
				*match_level = MLX5_MATCH_L3;
		}
	}

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic match;

		flow_rule_match_basic(rule, &match);
		ip_proto = match.key->ip_proto;

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_protocol,
			 match.mask->ip_proto);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_protocol,
			 match.key->ip_proto);

		if (match.mask->ip_proto)
			*match_level = MLX5_MATCH_L3;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV4_ADDRS) {
		struct flow_match_ipv4_addrs match;

		flow_rule_match_ipv4_addrs(rule, &match);
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    src_ipv4_src_ipv6.ipv4_layout.ipv4),
		       &match.mask->src, sizeof(match.mask->src));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    src_ipv4_src_ipv6.ipv4_layout.ipv4),
		       &match.key->src, sizeof(match.key->src));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    dst_ipv4_dst_ipv6.ipv4_layout.ipv4),
		       &match.mask->dst, sizeof(match.mask->dst));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    dst_ipv4_dst_ipv6.ipv4_layout.ipv4),
		       &match.key->dst, sizeof(match.key->dst));

		if (match.mask->src || match.mask->dst)
			*match_level = MLX5_MATCH_L3;
	}

	if (addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs match;

		flow_rule_match_ipv6_addrs(rule, &match);
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       &match.mask->src, sizeof(match.mask->src));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    src_ipv4_src_ipv6.ipv6_layout.ipv6),
		       &match.key->src, sizeof(match.key->src));

		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_c,
				    dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       &match.mask->dst, sizeof(match.mask->dst));
		memcpy(MLX5_ADDR_OF(fte_match_set_lyr_2_4, headers_v,
				    dst_ipv4_dst_ipv6.ipv6_layout.ipv6),
		       &match.key->dst, sizeof(match.key->dst));

		if (ipv6_addr_type(&match.mask->src) != IPV6_ADDR_ANY ||
		    ipv6_addr_type(&match.mask->dst) != IPV6_ADDR_ANY)
			*match_level = MLX5_MATCH_L3;
	}

#ifdef HAVE_FLOW_DISSECTOR_KEY_IP
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IP)) {
		struct flow_match_ip match;

		flow_rule_match_ip(rule, &match);
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_ecn,
			 match.mask->tos & 0x3);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_ecn,
			 match.key->tos & 0x3);

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ip_dscp,
			 match.mask->tos >> 2);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ip_dscp,
			 match.key->tos  >> 2);

		MLX5_SET(fte_match_set_lyr_2_4, headers_c, ttl_hoplimit,
			 match.mask->ttl);
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, ttl_hoplimit,
			 match.key->ttl);

		if (match.mask->ttl &&
		    !MLX5_CAP_ESW_FLOWTABLE_FDB(priv->mdev,
						ft_field_support.outer_ipv4_ttl)) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
#else
			netdev_err(priv->netdev,
#endif
					   "Matching on TTL is not supported");
			return -EOPNOTSUPP;
		}

		if (match.mask->tos || match.mask->ttl)
			*match_level = MLX5_MATCH_L3;
	}
#endif

	/* ***  L3 attributes parsing up to here *** */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports match;

		flow_rule_match_ports(rule, &match);
		switch (ip_proto) {
		case IPPROTO_TCP:
			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 tcp_sport, ntohs(match.mask->src));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 tcp_sport, ntohs(match.key->src));

			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 tcp_dport, ntohs(match.mask->dst));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 tcp_dport, ntohs(match.key->dst));
			break;

		case IPPROTO_UDP:
			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 udp_sport, ntohs(match.mask->src));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 udp_sport, ntohs(match.key->src));

			MLX5_SET(fte_match_set_lyr_2_4, headers_c,
				 udp_dport, ntohs(match.mask->dst));
			MLX5_SET(fte_match_set_lyr_2_4, headers_v,
				 udp_dport, ntohs(match.key->dst));
			break;
		default:
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
					   "Only UDP and TCP transports are supported for L4 matching");
#endif
			netdev_err(priv->netdev,
				   "Only UDP and TCP transport are supported\n");
			return -EINVAL;
		}

		if (match.mask->src || match.mask->dst)
			*match_level = MLX5_MATCH_L4;
	}

#ifdef HAVE_FLOW_DISSECTOR_KEY_TCP
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_TCP)) {
		struct flow_match_tcp match;

		flow_rule_match_tcp(rule, &match);
		MLX5_SET(fte_match_set_lyr_2_4, headers_c, tcp_flags,
			 ntohs(match.mask->flags));
		MLX5_SET(fte_match_set_lyr_2_4, headers_v, tcp_flags,
			 ntohs(match.key->flags));

		if (match.mask->flags)
			*match_level = MLX5_MATCH_L4;
	}
#endif
	return 0;
}

#ifdef HAVE_MINIFLOW
static bool is_valid_ct_state(struct mlx5e_priv *priv,
			      struct tc_cls_flower_offload *f)
{
	u8 ct_state = (f->ct_state_key & f->ct_state_mask);

	/* We can't offload new and invalid CT states.
	 * Related state needs more investigation.
	 */

	/* -trk */
	if (!ct_state)
		return true;

	/* +new */
	if (ct_state & TCA_FLOWER_KEY_CT_FLAGS_NEW)
		goto err;

	/* +inv */
	if (ct_state & TCA_FLOWER_KEY_CT_FLAGS_INVALID)
		goto err;

	/* +rel */
	if (ct_state & TCA_FLOWER_KEY_CT_FLAGS_RELATED)
		goto err;

	/* +est */
	if (ct_state & TCA_FLOWER_KEY_CT_FLAGS_ESTABLISHED)
		return true;

	/* -new exclusively */
	if (!(f->ct_state_key & TCA_FLOWER_KEY_CT_FLAGS_NEW) &&
	    (f->ct_state_mask & TCA_FLOWER_KEY_CT_FLAGS_NEW))
		return true;

err:
	netdev_dbg(priv->netdev, "Unsupported ct_state used: key/mask: %x/%x\n",
		   f->ct_state_key, f->ct_state_mask);

	return false;
}
#endif

static int parse_cls_flower(struct mlx5e_priv *priv,
			    struct mlx5e_tc_flow *flow,
			    struct mlx5_flow_spec *spec,
			    struct tc_cls_flower_offload *f,
			    struct net_device *filter_dev
#ifndef HAVE_TC_SETUP_FLOW_ACTION
			    , struct flow_rule *rule
#endif
				)
{
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
	struct netlink_ext_ack *extack = f->common.extack;
#endif
	struct mlx5_core_dev *dev = priv->mdev;
	struct mlx5_eswitch *esw = dev->priv.eswitch;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *rep;
	bool is_eswitch_flow, is_tunnel_flow;
	u8 inner_match_level, outer_match_level;
	u8 non_tunnel_match_level;
	int err;

#ifdef HAVE_MINIFLOW
	if (!is_valid_ct_state(priv, f))
		return -EOPNOTSUPP;
#endif

	inner_match_level = MLX5_MATCH_NONE;
	outer_match_level = MLX5_MATCH_NONE;
	is_tunnel_flow = false;

	err = __parse_cls_flower(priv, spec, f, filter_dev,
				 &inner_match_level, &outer_match_level,
				 &is_tunnel_flow
#ifndef HAVE_TC_SETUP_FLOW_ACTION
				 , rule
#endif
				);

	non_tunnel_match_level = (inner_match_level == MLX5_MATCH_NONE) ?
				 outer_match_level : inner_match_level;

	is_eswitch_flow = mlx5e_is_eswitch_flow(flow);
	if (!err && is_eswitch_flow) {
		rep = rpriv->rep;
		if (rep->vport != MLX5_VPORT_UPLINK &&
		    (esw->offloads.inline_mode != MLX5_INLINE_MODE_NONE &&
		    esw->offloads.inline_mode < non_tunnel_match_level)) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
					   "Flow is not offloaded due to min inline setting");
#endif
			netdev_warn(priv->netdev,
				    "Flow is not offloaded due to min inline setting, required %d actual %d\n",
				    non_tunnel_match_level, esw->offloads.inline_mode);
			return -EOPNOTSUPP;
		}
	}

	if (is_eswitch_flow) {
		flow->esw_attr->inner_match_level = inner_match_level;
		flow->esw_attr->outer_match_level = outer_match_level;
		flow->esw_attr->is_tunnel_flow = is_tunnel_flow;
	} else {
		flow->nic_attr->match_level = non_tunnel_match_level;
	}

	return err;
}
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
static int pedit_header_offsets[] = {
	[FLOW_ACT_MANGLE_HDR_TYPE_ETH] = offsetof(struct pedit_headers, eth),
	[FLOW_ACT_MANGLE_HDR_TYPE_IP4] = offsetof(struct pedit_headers, ip4),
	[FLOW_ACT_MANGLE_HDR_TYPE_IP6] = offsetof(struct pedit_headers, ip6),
	[FLOW_ACT_MANGLE_HDR_TYPE_TCP] = offsetof(struct pedit_headers, tcp),
	[FLOW_ACT_MANGLE_HDR_TYPE_UDP] = offsetof(struct pedit_headers, udp),
};

#define pedit_header(_ph, _htype) ((void *)(_ph) + pedit_header_offsets[_htype])

static int set_pedit_val(u8 hdr_type, u32 mask, u32 val, u32 offset,
			 struct pedit_headers_action *hdrs)
{
	u32 *curr_pmask, *curr_pval;

	curr_pmask = (u32 *)(pedit_header(&hdrs->masks, hdr_type) + offset);
	curr_pval  = (u32 *)(pedit_header(&hdrs->vals, hdr_type) + offset);

	if (*curr_pmask & mask)  /* disallow acting twice on the same location */
		goto out_err;

	*curr_pmask |= mask;
	*curr_pval  |= (val & mask);

	return 0;

out_err:
	return -EOPNOTSUPP;
}

struct mlx5_fields {
	u8  field;
	u8  field_bsize;
	u32 field_mask;
	u32 offset;
	u32 match_offset;
};

#define OFFLOAD(fw_field, field_bsize, field_mask, field, off, match_field) \
		{MLX5_ACTION_IN_FIELD_OUT_ ## fw_field, field_bsize, field_mask, \
		 offsetof(struct pedit_headers, field) + (off), \
		 MLX5_BYTE_OFF(fte_match_set_lyr_2_4, match_field)}

/* masked values are the same and there are no rewrites that do not have a
 * match.
 */
#define SAME_VAL_MASK(type, valp, maskp, matchvalp, matchmaskp) ({ \
	type matchmaskx = *(type *)(matchmaskp); \
	type matchvalx = *(type *)(matchvalp); \
	type maskx = *(type *)(maskp); \
	type valx = *(type *)(valp); \
	\
	(valx & maskx) == (matchvalx & matchmaskx) && !(maskx & (maskx ^ \
								 matchmaskx)); \
})

static bool cmp_val_mask(void *valp, void *maskp, void *matchvalp,
			 void *matchmaskp, u8 bsize)
{
	bool same = false;

	switch (bsize) {
	case 8:
		same = SAME_VAL_MASK(u8, valp, maskp, matchvalp, matchmaskp);
		break;
	case 16:
		same = SAME_VAL_MASK(u16, valp, maskp, matchvalp, matchmaskp);
		break;
	case 32:
		same = SAME_VAL_MASK(u32, valp, maskp, matchvalp, matchmaskp);
		break;
	}

	return same;
}

static struct mlx5_fields fields[] = {
	OFFLOAD(DMAC_47_16, 32, U32_MAX, eth.h_dest[0], 0, dmac_47_16),
	OFFLOAD(DMAC_15_0,  16, U16_MAX, eth.h_dest[4], 0, dmac_15_0),
	OFFLOAD(SMAC_47_16, 32, U32_MAX, eth.h_source[0], 0, smac_47_16),
	OFFLOAD(SMAC_15_0,  16, U16_MAX, eth.h_source[4], 0, smac_15_0),
	OFFLOAD(ETHERTYPE,  16, U16_MAX, eth.h_proto, 0, ethertype),
	OFFLOAD(FIRST_VID,  16, U16_MAX, vlan.h_vlan_TCI, 0, first_vid),

	OFFLOAD(IP_DSCP, 8,    0xfc, ip4.tos,   0, ip_dscp),
	OFFLOAD(IP_TTL,  8,  U8_MAX, ip4.ttl,   0, ttl_hoplimit),
	OFFLOAD(SIPV4,  32, U32_MAX, ip4.saddr, 0, src_ipv4_src_ipv6.ipv4_layout.ipv4),
	OFFLOAD(DIPV4,  32, U32_MAX, ip4.daddr, 0, dst_ipv4_dst_ipv6.ipv4_layout.ipv4),

	OFFLOAD(SIPV6_127_96, 32, U32_MAX, ip6.saddr.s6_addr32[0], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[0]),
	OFFLOAD(SIPV6_95_64,  32, U32_MAX, ip6.saddr.s6_addr32[1], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[4]),
	OFFLOAD(SIPV6_63_32,  32, U32_MAX, ip6.saddr.s6_addr32[2], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[8]),
	OFFLOAD(SIPV6_31_0,   32, U32_MAX, ip6.saddr.s6_addr32[3], 0,
		src_ipv4_src_ipv6.ipv6_layout.ipv6[12]),
	OFFLOAD(DIPV6_127_96, 32, U32_MAX, ip6.daddr.s6_addr32[0], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[0]),
	OFFLOAD(DIPV6_95_64,  32, U32_MAX, ip6.daddr.s6_addr32[1], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[4]),
	OFFLOAD(DIPV6_63_32,  32, U32_MAX, ip6.daddr.s6_addr32[2], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[8]),
	OFFLOAD(DIPV6_31_0,   32, U32_MAX, ip6.daddr.s6_addr32[3], 0,
		dst_ipv4_dst_ipv6.ipv6_layout.ipv6[12]),
	OFFLOAD(IPV6_HOPLIMIT, 8,  U8_MAX, ip6.hop_limit, 0, ttl_hoplimit),

	OFFLOAD(TCP_SPORT, 16, U16_MAX, tcp.source,  0, tcp_sport),
	OFFLOAD(TCP_DPORT, 16, U16_MAX, tcp.dest,    0, tcp_dport),
	/* in linux iphdr tcp_flags is 8 bits long */
	OFFLOAD(TCP_FLAGS,  8,  U8_MAX, tcp.ack_seq, 5, tcp_flags),

	OFFLOAD(UDP_SPORT, 16, U16_MAX, udp.source, 0, udp_sport),
	OFFLOAD(UDP_DPORT, 16, U16_MAX, udp.dest,   0, udp_dport),
};

/* On input attr->max_mod_hdr_actions tells how many HW actions can be parsed at
 * max from the SW pedit action. On success, attr->num_mod_hdr_actions
 * says how many HW actions were actually parsed.
 */
static int offload_pedit_fields(struct pedit_headers_action *hdrs,
				struct mlx5e_tc_flow_parse_attr *parse_attr,
				u32 *action_flags,
				struct netlink_ext_ack *extack)
{
	struct pedit_headers *set_masks, *add_masks, *set_vals, *add_vals;
	int i, action_size, nactions, max_actions, first, last, next_z;
	void *headers_c, *headers_v, *action, *vals_p;
	u32 *s_masks_p, *a_masks_p, s_mask, a_mask;
	struct mlx5_fields *f;
	unsigned long mask;
	__be32 mask_be32;
	__be16 mask_be16;
	u8 cmd;

	headers_c = get_match_headers_criteria(*action_flags, &parse_attr->spec);
	headers_v = get_match_headers_value(*action_flags, &parse_attr->spec);

	set_masks = &hdrs[0].masks;
	add_masks = &hdrs[1].masks;
	set_vals = &hdrs[0].vals;
	add_vals = &hdrs[1].vals;

	action_size = MLX5_MH_ACT_SZ;
	action = parse_attr->mod_hdr_actions +
		 parse_attr->num_mod_hdr_actions * action_size;

	max_actions = parse_attr->max_mod_hdr_actions;
	nactions = parse_attr->num_mod_hdr_actions;

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		bool skip;

		f = &fields[i];
		/* avoid seeing bits set from previous iterations */
		s_mask = 0;
		a_mask = 0;

		s_masks_p = (void *)set_masks + f->offset;
		a_masks_p = (void *)add_masks + f->offset;

		s_mask = *s_masks_p & f->field_mask;
		a_mask = *a_masks_p & f->field_mask;

		if (!s_mask && !a_mask) /* nothing to offload here */
			continue;

		if (s_mask && a_mask) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
					   "can't set and add to the same HW field");
#endif
			printk(KERN_WARNING "mlx5: can't set and add to the same HW field (%x)\n", f->field);
			return -EOPNOTSUPP;
		}

		if (nactions == max_actions) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
					   "too many pedit actions, can't offload");
#endif
			printk(KERN_WARNING "mlx5: parsed %d pedit actions, can't do more\n", nactions);
			return -EOPNOTSUPP;
		}

		skip = false;
		if (s_mask) {
			void *match_mask = headers_c + f->match_offset;
			void *match_val = headers_v + f->match_offset;

			cmd  = MLX5_ACTION_TYPE_SET;
			mask = s_mask;
			vals_p = (void *)set_vals + f->offset;
			/* don't rewrite if we have a match on the same value */
			if (cmp_val_mask(vals_p, s_masks_p, match_val,
					 match_mask, f->field_bsize))
				skip = true;
			/* clear to denote we consumed this field */
			*s_masks_p &= ~f->field_mask;
		} else {
			cmd  = MLX5_ACTION_TYPE_ADD;
			mask = a_mask;
			vals_p = (void *)add_vals + f->offset;
			/* add 0 is no change */
			if ((*(u32 *)vals_p & f->field_mask) == 0)
				skip = true;
			/* clear to denote we consumed this field */
			*a_masks_p &= ~f->field_mask;
		}
		if (skip)
			continue;

		if (f->field_bsize == 32) {
			mask_be32 = (__be32)mask;
			mask = (__force unsigned long)cpu_to_le32(be32_to_cpu(mask_be32));
		} else if (f->field_bsize == 16) {
			mask_be32 = (__be32)mask;
			mask_be16 = *(__be16 *)&mask_be32;
			mask = (__force unsigned long)cpu_to_le16(be16_to_cpu(mask_be16));
		}

		first = find_first_bit(&mask, f->field_bsize);
		next_z = find_next_zero_bit(&mask, f->field_bsize, first);
		last  = find_last_bit(&mask, f->field_bsize);
		if (first < next_z && next_z < last) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
					   "rewrite of few sub-fields isn't supported");
#endif
			printk(KERN_WARNING "mlx5: rewrite of few sub-fields (mask %lx) isn't offloaded\n",
			       mask);
			return -EOPNOTSUPP;
		}

		MLX5_SET(set_action_in, action, action_type, cmd);
		MLX5_SET(set_action_in, action, field, f->field);

		if (cmd == MLX5_ACTION_TYPE_SET) {
			int start;

			/* if field is bit sized it can start not from first bit */
			start = find_first_bit((unsigned long *)&f->field_mask,
					       f->field_bsize);

			MLX5_SET(set_action_in, action, offset, first - start);
			/* length is num of bits to be written, zero means length of 32 */
			MLX5_SET(set_action_in, action, length, (last - first + 1));
		}

		if (f->field_bsize == 32)
			MLX5_SET(set_action_in, action, data, ntohl(*(__be32 *)vals_p) >> first);
		else if (f->field_bsize == 16)
			MLX5_SET(set_action_in, action, data, ntohs(*(__be16 *)vals_p) >> first);
		else if (f->field_bsize == 8)
			MLX5_SET(set_action_in, action, data, *(u8 *)vals_p >> first);

		action += action_size;
		nactions++;
	}

	parse_attr->num_mod_hdr_actions = nactions;
	return 0;
}

static int mlx5e_flow_namespace_max_modify_action(struct mlx5_core_dev *mdev,
						  int namespace)
{
	if (namespace == MLX5_FLOW_NAMESPACE_FDB) /* FDB offloading */
		return MLX5_CAP_ESW_FLOWTABLE_FDB(mdev, max_modify_header_actions);
	else /* namespace is MLX5_FLOW_NAMESPACE_KERNEL - NIC offloading */
		return MLX5_CAP_FLOWTABLE_NIC_RX(mdev, max_modify_header_actions);
}

int alloc_mod_hdr_actions(struct mlx5e_priv *priv,
			  struct pedit_headers_action *hdrs,
			  int namespace,
			  struct mlx5e_tc_flow_parse_attr *parse_attr,
			  gfp_t flags)
{
	int nkeys, action_size, max_actions;

	nkeys = hdrs[TCA_PEDIT_KEY_EX_CMD_SET].pedits +
		hdrs[TCA_PEDIT_KEY_EX_CMD_ADD].pedits;
	action_size = MLX5_MH_ACT_SZ;

	max_actions = mlx5e_flow_namespace_max_modify_action(priv->mdev, namespace);
	/* can get up to crazingly 16 HW actions in 32 bits pedit SW key */
	max_actions = min(max_actions, nkeys * 16);

	parse_attr->mod_hdr_actions = kcalloc(max_actions, action_size, flags);
	if (!parse_attr->mod_hdr_actions)
		return -ENOMEM;

	parse_attr->max_mod_hdr_actions = max_actions;
	return 0;
}

static const struct pedit_headers zero_masks = {};

int parse_tc_pedit_action(struct mlx5e_priv *priv,
			  const struct flow_action_entry *act, int namespace,
			  struct mlx5e_tc_flow_parse_attr *parse_attr,
			  struct pedit_headers_action *hdrs,
			  struct netlink_ext_ack *extack)
{
	u8 cmd = (act->id == FLOW_ACTION_MANGLE) ? 0 : 1;
	int err = -EOPNOTSUPP;
	u32 mask, val, offset;
	u8 htype;

	htype = act->mangle.htype;
	err = -EOPNOTSUPP; /* can't be all optimistic */

	if (htype == FLOW_ACT_MANGLE_UNSPEC) {
	       	NL_SET_ERR_MSG_MOD(extack, "legacy pedit isn't offloaded");
		goto out_err;
	}

	if (!mlx5e_flow_namespace_max_modify_action(priv->mdev, namespace)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "The pedit offload action is not supported");
		goto out_err;
	}

	mask = act->mangle.mask;
	val = act->mangle.val;
	offset = act->mangle.offset;

	err = set_pedit_val(htype, ~mask, val, offset, &hdrs[cmd]);
	if (err)
		goto out_err;

	hdrs[cmd].pedits++;

	return 0;
out_err:
	return err;
}

int alloc_tc_pedit_action(struct mlx5e_priv *priv, int namespace,
			  struct mlx5e_tc_flow_parse_attr *parse_attr,
			  struct pedit_headers_action *hdrs,
			  u32 *action_flags,
			  struct netlink_ext_ack *extack)
{
	struct pedit_headers *cmd_masks;
	int err;
	u8 cmd;

	if (!parse_attr->mod_hdr_actions) {
		err = alloc_mod_hdr_actions(priv, hdrs, namespace, parse_attr, GFP_KERNEL);
		if (err)
			goto out_err;
	}

       err = offload_pedit_fields(hdrs, parse_attr, action_flags, extack);
	if (err < 0)
		goto out_dealloc_parsed_actions;

	for (cmd = 0; cmd < __PEDIT_CMD_MAX; cmd++) {
		cmd_masks = &hdrs[cmd].masks;
		if (memcmp(cmd_masks, &zero_masks, sizeof(zero_masks))) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
			NL_SET_ERR_MSG_MOD(extack,
					   "attempt to offload an unsupported field");
#endif
			netdev_warn(priv->netdev, "attempt to offload an unsupported field (cmd %d)\n", cmd);
			print_hex_dump(KERN_WARNING, "mask: ", DUMP_PREFIX_ADDRESS,
				       16, 1, cmd_masks, sizeof(zero_masks), true);
			err = -EOPNOTSUPP;
			goto out_dealloc_parsed_actions;
		}
	}

	return 0;

out_dealloc_parsed_actions:
	kfree(parse_attr->mod_hdr_actions);
out_err:
	return err;
}
#endif /* HAVE_TCF_PEDIT_TCFP_KEYS_EX */

#ifdef HAVE_TCA_CSUM_UPDATE_FLAG_IPV4HDR
static bool csum_offload_supported(struct mlx5e_priv *priv,
				   u32 action,
				   u32 update_flags,
				   struct netlink_ext_ack *extack)
{
	u32 prot_flags = TCA_CSUM_UPDATE_FLAG_IPV4HDR | TCA_CSUM_UPDATE_FLAG_TCP |
			 TCA_CSUM_UPDATE_FLAG_UDP;

	/*  The HW recalcs checksums only if re-writing headers */
	if (!(action & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR)) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
		NL_SET_ERR_MSG_MOD(extack,
				   "TC csum action is only offloaded with pedit");
#endif
		netdev_warn(priv->netdev,
			    "TC csum action is only offloaded with pedit\n");
		return false;
	}

	if (update_flags & ~prot_flags) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
		NL_SET_ERR_MSG_MOD(extack,
				   "can't offload TC csum action for some header/s");
#endif
		netdev_warn(priv->netdev,
			    "can't offload TC csum action for some header/s - flags %#x\n",
			    update_flags);
		return false;
	}

	return true;
}
#endif

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
struct ip_ttl_word {
	__u8	ttl;
	__u8	protocol;
	__sum16	check;
};

struct ipv6_hoplimit_word {
	__be16	payload_len;
	__u8	nexthdr;
	__u8	hop_limit;
};

static bool is_action_keys_supported(const struct flow_action_entry *act)
{
	u32 mask, offset;
	u8 htype;

	htype = act->mangle.htype;
	offset = act->mangle.offset;
	mask = ~act->mangle.mask;
	/* For IPv4 & IPv6 header check 4 byte word,
	 * to determine that modified fields
	 * are NOT ttl & hop_limit only.
	 */
	if (htype == FLOW_ACT_MANGLE_HDR_TYPE_IP4) {
		struct ip_ttl_word *ttl_word =
			(struct ip_ttl_word *)&mask;

		if (offset != offsetof(struct iphdr, ttl) ||
		    ttl_word->protocol ||
		    ttl_word->check) {
			return true;
		}
	} else if (htype == FLOW_ACT_MANGLE_HDR_TYPE_IP6) {
		struct ipv6_hoplimit_word *hoplimit_word =
			(struct ipv6_hoplimit_word *)&mask;

		if (offset != offsetof(struct ipv6hdr, payload_len) ||
		    hoplimit_word->payload_len ||
		    hoplimit_word->nexthdr) {
			return true;
		}
	}
	return false;
}

static bool modify_header_match_supported(struct mlx5_flow_spec *spec,
					  struct flow_action *flow_action,
					  u32 actions,
					  struct netlink_ext_ack *extack)
{
	const struct flow_action_entry *act;
	bool modify_ip_header;
	void *headers_v;
	u16 ethertype;
	u8 ip_proto;
	int i;

	headers_v = get_match_headers_value(actions, spec);
	ethertype = MLX5_GET(fte_match_set_lyr_2_4, headers_v, ethertype);

	/* for non-IP we only re-write MACs, so we're okay */
	if (ethertype != ETH_P_IP && ethertype != ETH_P_IPV6)
		goto out_ok;

	modify_ip_header = false;
	flow_action_for_each(i, act, flow_action) {
		if (act->id != FLOW_ACTION_MANGLE &&
		    act->id != FLOW_ACTION_ADD)
			continue;

		if (is_action_keys_supported(act)) {
			modify_ip_header = true;
			break;
		}
	}

	ip_proto = MLX5_GET(fte_match_set_lyr_2_4, headers_v, ip_protocol);
	if (modify_ip_header && ip_proto != IPPROTO_TCP &&
	    ip_proto != IPPROTO_UDP && ip_proto != IPPROTO_ICMP) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
		NL_SET_ERR_MSG_MOD(extack,
				   "can't offload re-write of non TCP/UDP");
#endif
		pr_info("can't offload re-write of ip proto %d\n", ip_proto);
		return false;
	}

out_ok:
	return true;
}
#endif /* HAVE_TCF_PEDIT_TCFP_KEYS_EX */

static bool actions_match_supported(struct mlx5e_priv *priv,
				    struct flow_action *flow_action,
				    struct mlx5e_tc_flow_parse_attr *parse_attr,
				    struct mlx5e_tc_flow *flow,
				    struct netlink_ext_ack *extack)
{
	u32 actions;

	if (mlx5e_is_eswitch_flow(flow))
		actions = flow->esw_attr->action;
	else
		actions = flow->nic_attr->action;

#if 0
	if (flow_flag_test(flow, EGRESS) &&
	    !((actions & MLX5_FLOW_CONTEXT_ACTION_DECAP) ||
	      (actions & MLX5_FLOW_CONTEXT_ACTION_VLAN_POP) ||
	      (actions & MLX5_FLOW_CONTEXT_ACTION_DROP)))
		return false;
#endif
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	if (actions & MLX5_FLOW_CONTEXT_ACTION_MOD_HDR)
		return modify_header_match_supported(&parse_attr->spec, flow_action, actions, extack);
#endif
	return true;
}

static bool same_hw_devs(struct mlx5e_priv *priv, struct mlx5e_priv *peer_priv)
{
	struct mlx5_core_dev *fmdev, *pmdev;
	u64 fsystem_guid, psystem_guid;

	fmdev = priv->mdev;
	pmdev = peer_priv->mdev;

	fsystem_guid = mlx5_query_nic_system_image_guid(fmdev);
	psystem_guid = mlx5_query_nic_system_image_guid(pmdev);

	return (fsystem_guid == psystem_guid);
}

#if defined(HAVE_IS_TCF_VLAN) && defined(HAVE_TCF_PEDIT_TCFP_KEYS_EX)
static int add_vlan_rewrite_action(struct mlx5e_priv *priv, int namespace,
				   const struct flow_action_entry *act,
				   struct mlx5e_tc_flow_parse_attr *parse_attr,
				   struct pedit_headers_action *hdrs,
				   u32 *action,
				   struct netlink_ext_ack *extack)
{
	u16 mask16 = VLAN_VID_MASK;
	u16 val16 = act->vlan.vid & VLAN_VID_MASK;
	const struct flow_action_entry pedit_act = {
		.id = FLOW_ACTION_MANGLE,
		.mangle.htype = FLOW_ACT_MANGLE_HDR_TYPE_ETH,
		.mangle.offset = offsetof(struct vlan_ethhdr, h_vlan_TCI),
		.mangle.mask = ~(u32)be16_to_cpu(*(__be16 *)&mask16),
		.mangle.val = (u32)be16_to_cpu(*(__be16 *)&val16),
	};
	u8 match_prio_mask, match_prio_val;
	void *headers_c, *headers_v;
	int err;

	headers_c = get_match_headers_criteria(*action, &parse_attr->spec);
	headers_v = get_match_headers_value(*action, &parse_attr->spec);

	if (!(MLX5_GET(fte_match_set_lyr_2_4, headers_c, cvlan_tag) &&
	      MLX5_GET(fte_match_set_lyr_2_4, headers_v, cvlan_tag))) {
	       	NL_SET_ERR_MSG_MOD(extack,
       			   "VLAN rewrite action must have VLAN protocol match");
		return -EOPNOTSUPP;
	}

	match_prio_mask = MLX5_GET(fte_match_set_lyr_2_4, headers_c, first_prio);
	match_prio_val = MLX5_GET(fte_match_set_lyr_2_4, headers_v, first_prio);
	if (act->vlan.prio != (match_prio_val & match_prio_mask)) {
		NL_SET_ERR_MSG_MOD(extack, "Changing VLAN prio is not supported");
		return -EOPNOTSUPP;
	}

	err = parse_tc_pedit_action(priv, &pedit_act, namespace, parse_attr,
				    hdrs, NULL);
	*action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;

	return err;
}

static int
add_vlan_prio_tag_rewrite_action(struct mlx5e_priv *priv,
				 struct mlx5e_tc_flow_parse_attr *parse_attr,
				 struct pedit_headers_action *hdrs,
				 u32 *action, struct netlink_ext_ack *extack)
{
	const struct flow_action_entry prio_tag_act = {
		.vlan.vid = 0,
		.vlan.prio =
			MLX5_GET(fte_match_set_lyr_2_4,
				 get_match_headers_value(*action,
							 &parse_attr->spec),
				 first_prio) &
			MLX5_GET(fte_match_set_lyr_2_4,
				 get_match_headers_criteria(*action,
							    &parse_attr->spec),
				 first_prio),
	};

	return add_vlan_rewrite_action(priv, MLX5_FLOW_NAMESPACE_FDB,
				       &prio_tag_act, parse_attr, hdrs, action,
				       extack);
}
#endif /* defined(HAVE_IS_TCF_VLAN) && defined(HAVE_TCF_PEDIT_TCFP_KEYS_EX) */

static int parse_tc_nic_actions(struct mlx5e_priv *priv,
				struct flow_action *flow_action,
				struct mlx5e_tc_flow_parse_attr *parse_attr,
				struct mlx5e_tc_flow *flow,
				struct netlink_ext_ack *extack)
{
	struct mlx5_nic_flow_attr *attr = flow->nic_attr;
	struct pedit_headers_action hdrs[2] = {};
	const struct flow_action_entry *act;
	u32 action = 0;
	int i;
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	int err;
#endif

	if (!flow_action_has_entries(flow_action))
		return -EINVAL;

	attr->flow_tag = MLX5_FS_DEFAULT_FLOW_TAG;

	flow_action_for_each(i, act, flow_action) {
		switch (act->id) {
		case FLOW_ACTION_DROP:
			action |= MLX5_FLOW_CONTEXT_ACTION_DROP;
			if (MLX5_CAP_FLOWTABLE(priv->mdev,
					       flow_table_properties_nic_receive.flow_counter))
				action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
			break;
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
		case FLOW_ACTION_MANGLE:
		case FLOW_ACTION_ADD:
			err = parse_tc_pedit_action(priv, act, MLX5_FLOW_NAMESPACE_KERNEL,
						    parse_attr, hdrs, extack);
			if (err)
				return err;

			action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR |
				  MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
			break;
		case FLOW_ACTION_VLAN_MANGLE:
			err = add_vlan_rewrite_action(priv,
						      MLX5_FLOW_NAMESPACE_KERNEL,
						      act, parse_attr, hdrs,
						      &action, extack);
			if (err)
				return err;

			break;
#endif
#ifdef HAVE_TCA_CSUM_UPDATE_FLAG_IPV4HDR
		case FLOW_ACTION_CSUM:
			if (csum_offload_supported(priv, action,
						   act->csum_flags,
						   extack))
				break;

			return -EOPNOTSUPP;
#endif
		case FLOW_ACTION_REDIRECT: {
			struct net_device *peer_dev = act->dev;

			if (priv->netdev->netdev_ops == peer_dev->netdev_ops &&
			    same_hw_devs(priv, netdev_priv(peer_dev))) {
				parse_attr->mirred_ifindex[0] = peer_dev->ifindex;
				flow_flag_set(flow, HAIRPIN);
				action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST |
					  MLX5_FLOW_CONTEXT_ACTION_COUNT;
			} else {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
				NL_SET_ERR_MSG_MOD(extack,
						   "device is not on same HW, can't offload");
#endif
				netdev_warn(priv->netdev, "device %s not on same HW, can't offload\n",
					    peer_dev->name);
				return -EINVAL;
			}
			}
			break;
		case FLOW_ACTION_MARK: {
			u32 mark = act->mark;

			if (mark & ~MLX5E_TC_FLOW_ID_MASK) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Bad flow mark - only 16 bit is supported");
				return -EINVAL;
			}

			attr->flow_tag = mark;
			action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
			}
			break;
		default:
			NL_SET_ERR_MSG_MOD(extack, "The offload action is not supported");
			return -EOPNOTSUPP;
		}
	}

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	if (hdrs[TCA_PEDIT_KEY_EX_CMD_SET].pedits ||
	    hdrs[TCA_PEDIT_KEY_EX_CMD_ADD].pedits) {
		err = alloc_tc_pedit_action(priv, MLX5_FLOW_NAMESPACE_KERNEL,
					    parse_attr, hdrs, &action, extack);
		if (err)
			return err;
		/* in case all pedit actions are skipped, remove the MOD_HDR
		 * flag.
		 */
		if (parse_attr->num_mod_hdr_actions == 0) {
			action &= ~MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
			kfree(parse_attr->mod_hdr_actions);
		}
	}
#endif

	attr->action = action;
	if (!actions_match_supported(priv, flow_action, parse_attr, flow, extack))
		return -EOPNOTSUPP;

	return 0;
}

struct encap_key {
	const struct ip_tunnel_key *ip_tun_key;
	struct mlx5e_tc_tunnel *tc_tunnel;
};

#ifdef HAVE_TCF_TUNNEL_INFO
static inline int cmp_encap_info(struct encap_key *a,
				 struct encap_key *b)
{
	return memcmp(a->ip_tun_key, b->ip_tun_key, sizeof(*a->ip_tun_key)) ||
	       a->tc_tunnel->tunnel_type != b->tc_tunnel->tunnel_type;
}

static inline int hash_encap_info(struct encap_key *key)
{
	return jhash(key->ip_tun_key, sizeof(*key->ip_tun_key),
		     key->tc_tunnel->tunnel_type);
}
#endif /* HAVE_TCF_TUNNEL_INFO */

static bool is_merged_eswitch_dev(struct mlx5e_priv *priv,
				  struct net_device *peer_netdev)
{
	struct mlx5e_priv *peer_priv;

	peer_priv = netdev_priv(peer_netdev);

	return (MLX5_CAP_ESW(priv->mdev, merged_eswitch) &&
		mlx5e_eswitch_rep(priv->netdev) &&
		mlx5e_eswitch_rep(peer_netdev) &&
		same_hw_devs(priv, peer_priv));
}

#ifdef HAVE_TCF_TUNNEL_INFO
bool mlx5e_encap_take(struct mlx5e_encap_entry *e)
{
	return refcount_inc_not_zero(&e->refcnt);
}

static struct mlx5e_encap_entry *
mlx5e_encap_get(struct mlx5e_priv *priv, struct encap_key *key,
		uintptr_t hash_key)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5e_encap_entry *e;
	struct encap_key e_key;

	hash_for_each_possible_rcu(esw->offloads.encap_tbl, e,
				   encap_hlist, hash_key) {
		e_key.ip_tun_key = &e->tun_info->key;
		e_key.tc_tunnel = e->tunnel;
		if (!cmp_encap_info(&e_key, key) &&
		    mlx5e_encap_take(e))
			return e;
	}

	return NULL;
}

static bool is_duplicated_encap_entry(struct mlx5e_priv *priv,
				      struct mlx5e_tc_flow *flow,
				      int out_index,
				      struct mlx5e_encap_entry *e,
				      struct netlink_ext_ack *extack)
{
	int i;

	for (i = 0; i < out_index; i++) {
		if (flow->encaps[i].e != e)
			continue;
		NL_SET_ERR_MSG_MOD(extack, "can't duplicate encap action");
		netdev_err(priv->netdev, "can't duplicate encap action\n");
		return true;
	}

	return false;
}

static int mlx5e_attach_encap(struct mlx5e_priv *priv,
			      struct mlx5e_tc_flow *flow,
			      struct net_device *mirred_dev,
			      int out_index,
			      struct netlink_ext_ack *extack,
			      struct net_device **encap_dev,
			      bool *encap_valid)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5_esw_flow_attr *attr = flow->esw_attr;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	const struct ip_tunnel_info *tun_info;
	struct encap_key key;
	struct mlx5e_encap_entry *e;
	unsigned short family;
	uintptr_t hash_key;
	int err = 0;

	parse_attr = attr->parse_attr;
	tun_info = parse_attr->tun_info[out_index];
	family = ip_tunnel_info_af(tun_info);
	key.ip_tun_key = &tun_info->key;
	key.tc_tunnel = mlx5e_get_tc_tun(mirred_dev);

	hash_key = hash_encap_info(&key);

	mutex_lock(&esw->offloads.encap_tbl_lock);
	e = mlx5e_encap_get(priv, &key, hash_key);

	/* must verify if encap is valid or not */
	if (e) {
		/* Check that entry was not already attached to this flow */
		if (is_duplicated_encap_entry(priv, flow, out_index, e, extack)) {
			err = -EOPNOTSUPP;
			goto out_err;
		}

		mutex_unlock(&esw->offloads.encap_tbl_lock);
		wait_for_completion(&e->hw_res_created);

		/* Protect against concurrent neigh update. */
		mutex_lock(&esw->offloads.encap_tbl_lock);
		if (e->compl_result < 0) {
			err = -EREMOTEIO;
			goto out_err;
		}
		goto attach_flow;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		err = -ENOMEM;
		goto out_err;
	}

	refcount_set(&e->refcnt, 1);
	init_completion(&e->hw_res_created);

	e->tun_info = tun_info;
	err = mlx5e_tc_tun_init_encap_attr(mirred_dev, priv, e
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
					   , extack
#endif
					   );
	if (err) {
		kfree(e);
		e = NULL;
		goto out_err;
	}

	INIT_LIST_HEAD(&e->flows);
	INIT_LIST_HEAD(&e->encap_id_list);
	hash_add_rcu(esw->offloads.encap_tbl, &e->encap_hlist, hash_key);
	mutex_unlock(&esw->offloads.encap_tbl_lock);

	if (family == AF_INET)
		err = mlx5e_tc_tun_create_header_ipv4(priv, mirred_dev, e);
	else if (family == AF_INET6)
		err = mlx5e_tc_tun_create_header_ipv6(priv, mirred_dev, e);

	/* Protect against concurrent neigh update. */
	mutex_lock(&esw->offloads.encap_tbl_lock);
	complete_all(&e->hw_res_created);
	if (err) {
		e->compl_result = err;
		goto out_err;
	}
	e->compl_result = 1;

attach_flow:
	flow->encaps[out_index].e = e;
	list_add(&flow->encaps[out_index].list, &e->flows);
	flow->encaps[out_index].index = out_index;
	*encap_dev = e->out_dev;
	if (e->flags & MLX5_ENCAP_ENTRY_VALID) {
		attr->dests[out_index].pkt_reformat = e->pkt_reformat;
		attr->dests[out_index].flags |= MLX5_ESW_DEST_ENCAP_VALID;
		*encap_valid = true;
	} else {
		*encap_valid = false;
	}
	mutex_unlock(&esw->offloads.encap_tbl_lock);

	return err;

out_err:
	mutex_unlock(&esw->offloads.encap_tbl_lock);
	if (e)
		mlx5e_encap_put(priv, e);
	return err;
}
#endif /* HAVE_TCF_TUNNEL_INFO */

#ifdef HAVE_IS_TCF_VLAN
static int parse_tc_vlan_action(struct mlx5e_priv *priv,
				const struct flow_action_entry *act,
				struct mlx5_esw_flow_attr *attr,
				u32 *action,
				struct mlx5e_tc_flow_parse_attr *parse_attr,
				struct netlink_ext_ack *extack)
{
	u8 vlan_idx = attr->total_vlan;

	if (vlan_idx >= MLX5_FS_VLAN_DEPTH)
		return -EOPNOTSUPP;

	switch (act->id) {
	case FLOW_ACTION_VLAN_POP:
		if (vlan_idx) {
			if (!mlx5_eswitch_vlan_actions_supported(priv->mdev,
								 MLX5_FS_VLAN_DEPTH))
				return -EOPNOTSUPP;

			*action |= MLX5_FLOW_CONTEXT_ACTION_VLAN_POP_2;
		} else {
			*action |= MLX5_FLOW_CONTEXT_ACTION_VLAN_POP;
		}
		break;
	case FLOW_ACTION_VLAN_PUSH:
		attr->vlan_vid[vlan_idx] = act->vlan.vid;
		attr->vlan_prio[vlan_idx] = act->vlan.prio;
		attr->vlan_proto[vlan_idx] = act->vlan.proto;
		if (!attr->vlan_proto[vlan_idx])
			attr->vlan_proto[vlan_idx] = htons(ETH_P_8021Q);

		if (vlan_idx) {
			if (!mlx5_eswitch_vlan_actions_supported(priv->mdev,
								 MLX5_FS_VLAN_DEPTH))
				return -EOPNOTSUPP;

			*action |= MLX5_FLOW_CONTEXT_ACTION_VLAN_PUSH_2;
		} else {
			if (!mlx5_eswitch_vlan_actions_supported(priv->mdev, 1) &&
			    (act->vlan.proto != htons(ETH_P_8021Q) ||
			     act->vlan.prio))
				return -EOPNOTSUPP;

			*action |= MLX5_FLOW_CONTEXT_ACTION_VLAN_PUSH;
		}
		break;
	default:
		return -EINVAL;
	}

	attr->total_vlan = vlan_idx + 1;

	return 0;
}

static int add_vlan_push_action(struct mlx5e_priv *priv,
				struct mlx5_esw_flow_attr *attr,
				struct net_device **out_dev,
				u32 *action,
				struct mlx5e_tc_flow_parse_attr *parse_attr,
				struct netlink_ext_ack *extack)
{
	struct net_device *vlan_dev = *out_dev;
	struct flow_action_entry vlan_act = {
		.id = FLOW_ACTION_VLAN_PUSH,
		.vlan.vid = vlan_dev_vlan_id(vlan_dev),
		.vlan.proto = vlan_dev_vlan_proto(vlan_dev),
		.vlan.prio = 0,
	};
	int err;

	err = parse_tc_vlan_action(priv, &vlan_act, attr, action, parse_attr, extack);
	if (err)
		return err;

	*out_dev = dev_get_by_index_rcu(dev_net(vlan_dev),
					dev_get_iflink(vlan_dev));
	if (is_vlan_dev(*out_dev))
		err = add_vlan_push_action(priv, attr,
				out_dev,
				action,
				parse_attr,
				extack);

	return err;
}

static int add_vlan_pop_action(struct mlx5e_priv *priv,
			       struct mlx5_esw_flow_attr *attr,
			       u32 *action,
			       struct mlx5e_tc_flow_parse_attr *parse_attr,
			       struct netlink_ext_ack *extack)
{
	int nest_level = vlan_get_encap_level(attr->parse_attr->filter_dev);
	struct flow_action_entry vlan_act = {
		.id = FLOW_ACTION_VLAN_POP,
	};
	int err = 0;

	while (nest_level--) {
		err = parse_tc_vlan_action(priv, &vlan_act, attr, action, parse_attr, extack);
		if (err)
			return err;
	}

	return err;
}
#endif /* HAVE_IS_TCF_VLAN */

/* This must be called under rcu_read_lock() */
static struct net_device *get_active_rep_dev_from_lag(struct net_device *lag_dev)
{
	struct net_device *active;
	bool found = false;

	active = bond_option_active_slave_get_rcu(netdev_priv(lag_dev));
	if (active && mlx5e_eswitch_rep(active))
		found = true;
	return found ? active : NULL;
}

bool mlx5e_is_valid_eswitch_fwd_dev(struct mlx5e_priv *priv,
				    struct net_device *out_dev)
{
	if (is_merged_eswitch_dev(priv, out_dev))
		return true;

	return mlx5e_eswitch_rep(out_dev) &&
	       same_hw_devs(priv, netdev_priv(out_dev));
}

static bool is_duplicated_output_device(struct net_device *dev,
					struct net_device *out_dev,
					int *ifindexes, int if_count,
					struct netlink_ext_ack *extack)
{
	int i;

	for (i = 0; i < if_count; i++) {
		if (ifindexes[i] == out_dev->ifindex) {
			NL_SET_ERR_MSG_MOD(extack,
					   "can't duplicate output to same device");
			netdev_err(dev, "can't duplicate output to same device: %s\n",
				   out_dev->name);
			return true;
		}
	}

	return false;
}

static int parse_tc_fdb_actions(struct mlx5e_priv *priv,
				struct flow_action *flow_action,
				struct mlx5e_tc_flow *flow,
				struct netlink_ext_ack *extack)
{
	struct pedit_headers_action hdrs[2] = {};
#if defined(PRIO_CHAIN_SUPPORT) || defined(HAVE_TCF_PEDIT_TCFP_KEYS_EX)
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
#endif
	struct mlx5_esw_flow_attr *attr = flow->esw_attr;
	struct mlx5e_tc_flow_parse_attr *parse_attr = attr->parse_attr;
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
#ifdef HAVE_TCF_TUNNEL_INFO
	const struct ip_tunnel_info *info = NULL;
#endif
	int ifindexes[MLX5_MAX_FLOW_FWD_VPORTS];
	const struct flow_action_entry *act;
	int err, i, if_count = 0;
	bool encap = false;
	u32 action = 0;

	if (!flow_action_has_entries(flow_action))
		return -EINVAL;

	flow_action_for_each(i, act, flow_action)
		if (act->id == FLOW_ACTION_TUNNEL_DECAP) {
			action = MLX5_FLOW_CONTEXT_ACTION_DECAP;
			break;
		}

	flow_action_for_each(i, act, flow_action) {
		switch (act->id) {
		case FLOW_ACTION_DROP:
			action |= MLX5_FLOW_CONTEXT_ACTION_DROP |
				  MLX5_FLOW_CONTEXT_ACTION_COUNT;
			break;
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
		case FLOW_ACTION_MANGLE:
		case FLOW_ACTION_ADD:
#ifdef HAVE_MINIFLOW
			if (action & MLX5_FLOW_CONTEXT_ACTION_CT) {
				pr_err("CT action before HDR is not allowed");
				return -EOPNOTSUPP;
			}
#endif
			err = parse_tc_pedit_action(priv, act, MLX5_FLOW_NAMESPACE_FDB,
						    parse_attr, hdrs, extack);
			if (err)
				return err;

			action |= MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
			attr->split_count = attr->out_count;
			break;
#endif /* HAVE_TCF_PEDIT_TCFP_KEYS_EX */
#ifdef HAVE_TCA_CSUM_UPDATE_FLAG_IPV4HDR
		case FLOW_ACTION_CSUM:
			if (csum_offload_supported(priv, action,
						   act->csum_flags,
						   extack))
				break;

			return -EOPNOTSUPP;
#endif
		case FLOW_ACTION_REDIRECT:
		case FLOW_ACTION_MIRRED: {
			struct net_device *out_dev, *rep_dev;
			struct mlx5e_priv *out_priv;

			out_dev = act->dev;
			if (!out_dev) {
				/* out_dev is NULL when filters with
				 * non-existing mirred device are replayed to
				 * the driver.
				 */
				return -EINVAL;
			}

			if (attr->out_count >= MLX5_MAX_FLOW_FWD_VPORTS) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
				NL_SET_ERR_MSG_MOD(extack,
						   "can't support more output ports, can't offload forwarding");
#endif
				pr_err("can't support more than %d output ports, can't offload forwarding\n",
				       attr->out_count);
				return -EOPNOTSUPP;
			}

			action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST |
				  MLX5_FLOW_CONTEXT_ACTION_COUNT;
			if (encap) {
#ifdef HAVE_TCF_TUNNEL_INFO
				parse_attr->mirred_ifindex[attr->out_count] =
					out_dev->ifindex;
				memcpy(&parse_attr->tun_info2[attr->out_count], info,
					sizeof(struct ip_tunnel_info));
				parse_attr->tun_info[attr->out_count] =
					&parse_attr->tun_info2[attr->out_count];
				encap = false;
				attr->dests[attr->out_count].flags |=
					MLX5_ESW_DEST_ENCAP;
				attr->out_count++;
				/* attr->dests[].rep is resolved when we
				 * handle encap
				 */
#endif
#ifdef HAVE_NETDEV_PORT_SAME_PARENT_ID
			} else if (netdev_port_same_parent_id(priv->netdev, out_dev)) {
#else
			} else if (switchdev_port_same_parent_id(priv->netdev,
							  out_dev)) {
#endif
				struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
				struct net_device *uplink_dev = mlx5_eswitch_uplink_get_proto_dev(esw, REP_ETH);
				struct net_device *uplink_upper;

				if (is_duplicated_output_device(priv->netdev,
								out_dev,
								ifindexes,
								if_count,
								extack))
					return -EOPNOTSUPP;

				ifindexes[if_count] = out_dev->ifindex;
				if_count++;

				rcu_read_lock();
				uplink_upper =
					netdev_master_upper_dev_get_rcu(uplink_dev);
				if (uplink_upper &&
				    netif_is_lag_master(uplink_upper) &&
				    uplink_upper == out_dev)
					out_dev = uplink_dev;
				else if (netif_is_lag_master(out_dev)) {
					rep_dev = get_active_rep_dev_from_lag(out_dev);
					if (!rep_dev) {
						rcu_read_unlock();
						return -ENODEV;
#ifdef HAVE_NETDEV_PORT_SAME_PARENT_ID
					} else if (!netdev_port_same_parent_id(rep_dev, uplink_dev)) {
#else
					} else if (!switchdev_port_same_parent_id(rep_dev, uplink_dev)) {
#endif
						rcu_read_unlock();
						return -EINVAL;
					}
					out_dev = rep_dev;
				}
				rcu_read_unlock();
#ifdef HAVE_IS_TCF_VLAN
				if (is_vlan_dev(out_dev)) {
					err = add_vlan_push_action(priv, attr,
								   &out_dev,
								   &action,
								   parse_attr,
								   extack);
					if (err)
						return err;
				}

				if (is_vlan_dev(parse_attr->filter_dev)) {
					err = add_vlan_pop_action(priv, attr,
								  &action,
							          parse_attr,
								  extack);
					if (err)
						return err;
				}
#endif /* HAVE_IS_TCF_VLAN */
				if (!mlx5e_is_valid_eswitch_fwd_dev(priv, out_dev)) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
					NL_SET_ERR_MSG_MOD(extack,
							   "devices are not on same switch HW, can't offload forwarding");
#endif
					pr_err_once("devices %s %s not on same switch HW, can't offload forwarding\n",
						    priv->netdev->name, out_dev->name);
					pr_debug("devices %s %s not on same switch HW, can't offload forwarding\n",
						 priv->netdev->name, out_dev->name);
					return -EOPNOTSUPP;
				}
#ifdef HAVE_MINIFLOW
				if (attr->split_count == 0 &&
				    flow_flag_test(flow, EGRESS) &&
				    flow->esw_attr->is_tunnel_flow)
					action |= MLX5_FLOW_CONTEXT_ACTION_DECAP;
#endif

				out_priv = netdev_priv(out_dev);
				rpriv = out_priv->ppriv;
				attr->dests[attr->out_count].rep = rpriv->rep;
				attr->dests[attr->out_count].mdev = out_priv->mdev;
				attr->out_count++;
			} else if (parse_attr->filter_dev != priv->netdev) {
				/* All mlx5 devices are called to configure
				 * high level device filters. Therefore, the
				 * *attempt* to  install a filter on invalid
				 * eswitch should not trigger an explicit error
				 */
				return -EINVAL;
			} else {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
				NL_SET_ERR_MSG_MOD(extack,
						   "devices are not on same switch HW, can't offload forwarding");
#endif
				pr_err_once("devices %s %s not on same switch HW, can't offload forwarding\n",
					    priv->netdev->name, out_dev->name);
				pr_debug("devices %s %s not on same switch HW, can't offload forwarding\n",
					 priv->netdev->name, out_dev->name);
				return -EINVAL;
			}
			}
			break;
#ifdef HAVE_TCF_TUNNEL_INFO
		case FLOW_ACTION_TUNNEL_ENCAP:
#ifdef CONFIG_COMPAT_TCF_TUNNEL_KEY_MOD
			info = act->tunnel;
#else
			info = act->tunnel;
#endif
			if (info)
				encap = true;
			else
				return -EOPNOTSUPP;

			break;
#endif
#ifdef HAVE_IS_TCF_VLAN
		case FLOW_ACTION_VLAN_PUSH:
		case FLOW_ACTION_VLAN_POP:
			if (act->id == FLOW_ACTION_VLAN_PUSH &&
			    (action & MLX5_FLOW_CONTEXT_ACTION_VLAN_POP)) {
				/* Replace vlan pop+push with vlan modify */
#if defined(HAVE_IS_TCF_VLAN) && defined(HAVE_TCF_PEDIT_TCFP_KEYS_EX)
				action &= ~MLX5_FLOW_CONTEXT_ACTION_VLAN_POP;
				err = add_vlan_rewrite_action(priv,
							      MLX5_FLOW_NAMESPACE_FDB,
							      act, parse_attr, hdrs,
							      &action,
							      extack);
#else
				err = -EOPNOTSUPP;
#endif
			} else {
				err = parse_tc_vlan_action(priv, act, attr, &action,
							   parse_attr,
							   extack);
			}
			if (err)
				return err;

			attr->split_count = attr->out_count;
			break;
#if defined(HAVE_IS_TCF_VLAN) && defined(HAVE_TCF_PEDIT_TCFP_KEYS_EX)
		case FLOW_ACTION_VLAN_MANGLE:
			err = add_vlan_rewrite_action(priv,
						      MLX5_FLOW_NAMESPACE_FDB,
						      act, parse_attr, hdrs,
						      &action, extack);
			if (err)
				return err;

			attr->split_count = attr->out_count;
			break;
#endif
#endif /* HAVE_IS_TCF_VLAN */
#ifdef HAVE_TCF_TUNNEL_INFO
		case FLOW_ACTION_TUNNEL_DECAP:
			action |= MLX5_FLOW_CONTEXT_ACTION_DECAP;
			break;
#endif
#ifdef HAVE_MINIFLOW
		case FLOW_ACTION_CT:
			action |= MLX5_FLOW_CONTEXT_ACTION_CT;
			continue;
#endif
#ifdef PRIO_CHAIN_SUPPORT
		case FLOW_ACTION_GOTO: {
			u32 dest_chain = act->chain_index;
#ifndef HAVE_MINIFLOW
			u32 max_chain = mlx5_eswitch_get_chain_range(esw);

			if (dest_chain <= attr->chain) {
				NL_SET_ERR_MSG(extack, "Goto earlier chain isn't supported");
				return -EOPNOTSUPP;
			}
			if (dest_chain > max_chain) {
				NL_SET_ERR_MSG(extack, "Requested destination chain is out of supported range");
				return -EOPNOTSUPP;
			}
			attr->dest_chain = dest_chain;
#else /* HAVE_MINIFLOW */
			if (dest_chain == 0) {
				netdev_warn(priv->netdev, "Loop to chain 0 is not supported");
				return -EOPNOTSUPP;
			}
			if (flow_flag_test(flow, EGRESS) &&
			    flow->esw_attr->is_tunnel_flow)
				action |= MLX5_FLOW_CONTEXT_ACTION_DECAP;

			action |= MLX5_FLOW_CONTEXT_ACTION_GOTO;
#endif /* HAVE_MINIFLOW */
			action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
			continue;
		}
#endif /* PRIO_CHAIN_SUPPORT */
		default:
			NL_SET_ERR_MSG_MOD(extack, "The offload action is not supported");
			return -EOPNOTSUPP;
		}
	}

#if defined(HAVE_IS_TCF_VLAN) && defined(HAVE_TCF_PEDIT_TCFP_KEYS_EX)
	if (MLX5_CAP_GEN(esw->dev, prio_tag_required) &&
	    action & MLX5_FLOW_CONTEXT_ACTION_VLAN_POP) {
		/* For prio tag mode, replace vlan pop with rewrite vlan prio
		 * tag rewrite.
		 */
		action &= ~MLX5_FLOW_CONTEXT_ACTION_VLAN_POP;
		err = add_vlan_prio_tag_rewrite_action(priv, parse_attr, hdrs,
						       &action, extack);
		if (err)
			return err;
	}
#endif /* HAVE_IS_TCF_VLAN && HAVE_TCF_PEDIT_TCFP_KEYS_EX */

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	if (hdrs[TCA_PEDIT_KEY_EX_CMD_SET].pedits ||
	    hdrs[TCA_PEDIT_KEY_EX_CMD_ADD].pedits) {
		err = alloc_tc_pedit_action(priv, MLX5_FLOW_NAMESPACE_FDB,
					    parse_attr, hdrs, &action, extack);
		if (err)
			return err;
		/* in case all pedit actions are skipped, remove the MOD_HDR
		 * flag. we might have set split_count either by pedit or
		 * pop/push. if there is no pop/push either, reset it too.
		 */
		if (parse_attr->num_mod_hdr_actions == 0) {
			action &= ~MLX5_FLOW_CONTEXT_ACTION_MOD_HDR;
			kfree(parse_attr->mod_hdr_actions);
			if (!((action & MLX5_FLOW_CONTEXT_ACTION_VLAN_POP) ||
			      (action & MLX5_FLOW_CONTEXT_ACTION_VLAN_PUSH)))
				attr->split_count = 0;
		}
	}
#endif

#ifdef HAVE_MINIFLOW
	if ((action & MLX5_FLOW_CONTEXT_ACTION_CT) &&
	    !(action & MLX5_FLOW_CONTEXT_ACTION_GOTO)) {
		netdev_warn(priv->netdev, "CT action is not supported without goto");
		return -EOPNOTSUPP;
	}

	if ((action & MLX5_FLOW_CONTEXT_ACTION_CT) &&
	    (action & MLX5_FLOW_CONTEXT_ACTION_GOTO) &&
	    (action & MLX5_FLOW_CONTEXT_ACTION_FWD_DEST)) {
		netdev_warn_once(priv->netdev, "CT mirroring is not supported");
		return -EOPNOTSUPP;
	}
#endif

	attr->action = action;
	if (!actions_match_supported(priv, flow_action, parse_attr, flow, extack)) {
		return -EOPNOTSUPP;
	}

	if (attr->dest_chain) {
		if (attr->action & MLX5_FLOW_CONTEXT_ACTION_FWD_DEST) {
			NL_SET_ERR_MSG(extack, "Mirroring goto chain rules isn't supported");
			return -EOPNOTSUPP;
		}
		attr->action |= MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	}

	if (attr->split_count > 0 && !mlx5_esw_has_fwd_fdb(priv->mdev)) {
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
		NL_SET_ERR_MSG_MOD(extack,
				   "current firmware doesn't support split rule for port mirroring");
#endif
		netdev_warn_once(priv->netdev, "current firmware doesn't support split rule for port mirroring\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static void get_flags(int flags, unsigned long *flow_flags)
{
	unsigned long __flow_flags = 0;

	/* relevant for the new ndo */
	if (flags & MLX5_TC_FLAG(INGRESS))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_INGRESS);
	if (flags & MLX5_TC_FLAG(EGRESS))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_EGRESS);

	if (flags & MLX5_TC_FLAG(ESW_OFFLOAD))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_ESWITCH);
	if (flags & MLX5_TC_FLAG(NIC_OFFLOAD))
		__flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_NIC);

	*flow_flags = __flow_flags;
}

static const struct rhashtable_params tc_ht_params = {
	.head_offset = offsetof(struct mlx5e_tc_flow, node),
	.key_offset = offsetof(struct mlx5e_tc_flow, cookie),
	.key_len = sizeof(((struct mlx5e_tc_flow *)0)->cookie),
	.automatic_shrinking = true,
};

#ifdef CONFIG_COMPAT_CLS_FLOWER_MOD
static void get_new_flags(struct mlx5e_priv *priv, unsigned long *flags)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	if (mlx5e_eswitch_rep(priv->netdev) &&
	    MLX5_VPORT_MANAGER(priv->mdev) && esw->mode == MLX5_ESWITCH_OFFLOADS)
		*flags |= MLX5_TC_FLAG(ESW_OFFLOAD);
}
#endif

static struct rhashtable *get_tc_ht(struct mlx5e_priv *priv,
				    unsigned long flags)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	struct mlx5e_rep_priv *uplink_rpriv;

#ifdef CONFIG_COMPAT_CLS_FLOWER_MOD
	if ((flags & MLX5_TC_FLAG(ESW_OFFLOAD)) ||
	    mlx5e_eswitch_rep(priv->netdev)) {
#else
	if (flags & MLX5_TC_FLAG(ESW_OFFLOAD)) {
#endif
		uplink_rpriv = mlx5_eswitch_get_uplink_priv(esw, REP_ETH);
		return &uplink_rpriv->uplink_priv.tc_ht;
	} else /* NIC offload */
		return &priv->fs.tc.ht;
}

static bool is_peer_flow_needed(struct mlx5e_tc_flow *flow)
{
	struct mlx5_esw_flow_attr *attr = flow->esw_attr;
#ifdef HAVE_QDISC_SUPPORTS_BLOCK_SHARING
	bool is_rep_ingress = attr->in_rep->vport != MLX5_VPORT_UPLINK &&
		flow_flag_test(flow, INGRESS);
	bool act_is_encap = !!(attr->action &
			       MLX5_FLOW_CONTEXT_ACTION_PACKET_REFORMAT);
#endif
	bool esw_paired = mlx5_devcom_is_paired(attr->in_mdev->priv.devcom,
						MLX5_DEVCOM_ESW_OFFLOADS);

	if (!esw_paired)
		return false;

#ifdef HAVE_QDISC_SUPPORTS_BLOCK_SHARING
	if ((mlx5_lag_is_sriov(attr->in_mdev) ||
	     mlx5_lag_is_multipath(attr->in_mdev)) &&
	    (is_rep_ingress || act_is_encap
#ifdef HAVE_TC_SETUP_CB_EGDEV_REGISTER
	     || (flow->flags & MLX5_TC_FLAG(EGRESS))
#endif
))
		return true;

	return false;
#else
	return (mlx5_lag_is_sriov(attr->in_mdev) ||  mlx5_lag_is_multipath(attr->in_mdev));
#endif
}

void *mlx5e_lookup_tc_ht(struct mlx5e_priv *priv,
			 unsigned long *cookie,
			 int flags)
{
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);

	return rhashtable_lookup_fast(tc_ht, cookie, tc_ht_params);
}

int
mlx5e_alloc_flow(struct mlx5e_priv *priv, int attr_size,
		 u64 cookie, unsigned long flow_flags, gfp_t flags,
		 struct mlx5e_tc_flow_parse_attr **__parse_attr,
		 struct mlx5e_tc_flow **__flow)
{
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5e_tc_flow *flow;
	int out_index, err;

	flow = kzalloc(sizeof(*flow) + attr_size, flags);
	parse_attr = kvzalloc(sizeof(*parse_attr), flags);
	if (!parse_attr || !flow) {
		err = -ENOMEM;
		goto err_free;
	}

	flow->flags = flow_flags;
	flow->cookie = cookie;
	flow->priv = priv;
#ifdef HAVE_TCF_TUNNEL_INFO
	for (out_index = 0; out_index < MLX5_MAX_FLOW_FWD_VPORTS; out_index++)
		INIT_LIST_HEAD(&flow->encaps[out_index].list);
#endif
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	INIT_LIST_HEAD(&flow->mod_hdr);
#endif
	INIT_LIST_HEAD(&flow->hairpin);
	refcount_set(&flow->refcnt, 1);
	init_completion(&flow->init_done);
	INIT_LIST_HEAD(&flow->miniflow_list);
	INIT_LIST_HEAD(&flow->nft_node);

	*__flow = flow;
	*__parse_attr = parse_attr;

	return 0;

err_free:
	kfree(flow);
	kvfree(parse_attr);
	return err;
}

static void
mlx5e_flow_esw_attr_init(struct mlx5_esw_flow_attr *esw_attr,
			 struct mlx5e_priv *priv,
			 struct mlx5e_tc_flow_parse_attr *parse_attr,
			 struct tc_cls_flower_offload *f,
			 struct mlx5_eswitch_rep *in_rep,
			 struct mlx5_core_dev *in_mdev)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;

	esw_attr->parse_attr = parse_attr;
#ifdef PRIO_CHAIN_SUPPORT
	esw_attr->chain = f->common.chain_index;
	esw_attr->prio = TC_H_MAJ(f->common.prio) >> 16;
#else
	esw_attr->prio = 1;
#endif

	esw_attr->in_rep = in_rep;
	esw_attr->in_mdev = in_mdev;

	if (MLX5_CAP_ESW(esw->dev, counter_eswitch_affinity) ==
	    MLX5_COUNTER_SOURCE_ESWITCH)
		esw_attr->counter_dev = in_mdev;
	else
		esw_attr->counter_dev = priv->mdev;
}

static bool is_flow_simple(struct mlx5e_tc_flow *flow)
{
#ifdef HAVE_MINIFLOW
	if (flow->esw_attr->chain)
		return false;

	if (flow->esw_attr->action & MLX5_FLOW_CONTEXT_ACTION_GOTO)
		return false;

#endif
	return true;
}

#ifndef HAVE_TC_SETUP_FLOW_ACTION
static void build_rule_match(struct tc_cls_flower_offload *f,
			     struct flow_match *match)
{
	match->dissector = f->dissector;
	match->mask = f->mask;
	match->key = f->key;
}

static int build_rule_action(struct tc_cls_flower_offload *f,
			     struct flow_rule *rule)
{
	return tc_setup_flow_action(&rule->action, f->exts);
}

struct flow_rule *alloc_flow_rule(struct tc_cls_flower_offload *f)
{
	struct flow_rule *ret;
	int num_ent;
	int err;

	num_ent = tcf_exts_num_actions(f->exts);
	ret = kzalloc(sizeof(*ret) + num_ent * sizeof(ret->action.entries[0]),
		      GFP_KERNEL);
	if (!ret)
		return ERR_PTR(-ENOMEM);

	ret->action.num_entries = num_ent;
	err = build_rule_action(f, ret);
	if (err)
		goto out;

	build_rule_match(f, &ret->match);

	return ret;

out:
	kfree(ret);
	return ERR_PTR(err);
}

static void free_flow_rule(struct flow_rule *rule)
{
	kfree(rule);
}
#endif

static struct mlx5e_tc_flow *
__mlx5e_add_fdb_flow(struct mlx5e_priv *priv,
		     struct tc_cls_flower_offload *f,
		     unsigned long flow_flags,
		     struct net_device *filter_dev,
		     struct mlx5_eswitch_rep *in_rep,
		     struct mlx5_core_dev *in_mdev)
{
	struct flow_rule *rule;
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
	struct netlink_ext_ack *extack = f->common.extack;
#else
	struct netlink_ext_ack *extack = NULL;
#endif
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5e_tc_flow *flow;
	int attr_size, err;

#ifndef HAVE_TC_SETUP_FLOW_ACTION
	rule = alloc_flow_rule(f);
	if (IS_ERR(rule))
		return ERR_PTR(PTR_ERR(rule));
#else
	rule = tc_cls_flower_offload_flow_rule(f);
#endif

	flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_ESWITCH) | BIT(MLX5E_TC_FLOW_FLAG_SIMPLE);
	attr_size  = sizeof(struct mlx5_esw_flow_attr);
	err = mlx5e_alloc_flow(priv, attr_size, f->cookie, flow_flags, GFP_KERNEL,
			       &parse_attr, &flow);
	if (err)
		goto out;

	parse_attr->filter_dev = filter_dev;
	mlx5e_flow_esw_attr_init(flow->esw_attr,
				 priv, parse_attr,
				 f, in_rep, in_mdev);
	err = parse_cls_flower(flow->priv, flow, &parse_attr->spec,
			       f, filter_dev
#ifndef HAVE_TC_SETUP_FLOW_ACTION
			       , rule
#endif
					    );
	if (err)
		goto err_free;

	err = parse_tc_fdb_actions(priv, &rule->action, flow, extack);
	if (err)
		goto err_free;

	if (is_flow_simple(flow)) {
		err = mlx5e_tc_add_fdb_flow(priv, flow, extack);
		complete_all(&flow->init_done);
		if (err) {
			if (!(err == -ENETUNREACH && mlx5_lag_is_multipath(in_mdev)))
				goto err_free;

			add_unready_flow(flow);
		}
	} else {
		flow_flag_clear(flow, SIMPLE);
	}

#ifndef HAVE_TC_SETUP_FLOW_ACTION
	free_flow_rule(rule);
#endif
	return flow;

err_free:
	mlx5e_flow_put(priv, flow);
out:
#if !defined(HAVE_IS_TCF_TUNNEL) && defined(HAVE_TCF_TUNNEL_INFO)
#endif

#ifndef HAVE_TC_SETUP_FLOW_ACTION
	free_flow_rule(rule);
#endif
	return ERR_PTR(err);
}

static int mlx5e_tc_add_fdb_peer_flow(struct tc_cls_flower_offload *f,
				      struct mlx5e_tc_flow *flow,
				      unsigned long flow_flags)
{
	struct mlx5e_priv *priv = flow->priv, *peer_priv;
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch, *peer_esw;
	struct mlx5_devcom *devcom = priv->mdev->priv.devcom;
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5e_rep_priv *peer_urpriv;
	struct mlx5e_tc_flow *peer_flow;
	struct mlx5_eswitch_rep *in_rep;
	struct mlx5_core_dev *in_mdev;
	int err = 0;

	peer_esw = mlx5_devcom_get_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
	if (!peer_esw)
		return -ENODEV;

	peer_urpriv = mlx5_eswitch_get_uplink_priv(peer_esw, REP_ETH);
	peer_priv = netdev_priv(peer_urpriv->netdev);

	/* in_mdev is assigned of which the packet originated from.
	 * So packets redirected to uplink use the same mdev of the
	 * original flow and packets redirected from uplink use the
	 * peer mdev.
	 */
	if (flow->esw_attr->in_rep->vport == MLX5_VPORT_UPLINK) {
		in_mdev = peer_priv->mdev;
		in_rep = peer_urpriv->rep;
	} else {
		in_mdev = priv->mdev;
		in_rep = flow->esw_attr->in_rep;
	}

	parse_attr = flow->esw_attr->parse_attr;
	peer_flow = __mlx5e_add_fdb_flow(peer_priv, f, flow_flags,
					 parse_attr->filter_dev,
					 in_rep, in_mdev);
	if (IS_ERR(peer_flow)) {
		err = PTR_ERR(peer_flow);
		goto out;
	}

	flow->peer_flow = peer_flow;
	flow_flag_set(flow, DUP);
	mutex_lock(&esw->offloads.peer_mutex);
	list_add_tail(&flow->peer, &esw->offloads.peer_flows);
	mutex_unlock(&esw->offloads.peer_mutex);

out:
	mlx5_devcom_release_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
	return err;
}

static int
mlx5e_add_fdb_flow(struct mlx5e_priv *priv,
		   struct tc_cls_flower_offload *f,
		   unsigned long flow_flags,
		   struct net_device *filter_dev,
		   struct mlx5e_tc_flow **__flow)
{
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5_eswitch_rep *in_rep = rpriv->rep;
	struct mlx5_core_dev *in_mdev = priv->mdev;
	struct mlx5e_tc_flow *flow;
	int err;

	flow = __mlx5e_add_fdb_flow(priv, f, flow_flags, filter_dev, in_rep,
				    in_mdev);
	if (IS_ERR(flow))
		return PTR_ERR(flow);

	if (is_peer_flow_needed(flow) && mlx5e_is_simple_flow(flow)) {
		err = mlx5e_tc_add_fdb_peer_flow(f, flow, flow_flags);
		if (err) {
			mlx5e_tc_del_fdb_flow(priv, flow);
			goto out;
		}
	}

	*__flow = flow;

	return 0;

out:
	return err;
}

static int
mlx5e_add_nic_flow(struct mlx5e_priv *priv,
		   struct tc_cls_flower_offload *f,
		   unsigned long flow_flags,
		   struct net_device *filter_dev,
		   struct mlx5e_tc_flow **__flow)
{
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
	struct netlink_ext_ack *extack = f->common.extack;
#else
	struct netlink_ext_ack *extack = NULL;
#endif
	struct mlx5e_tc_flow_parse_attr *parse_attr;
	struct mlx5e_tc_flow *flow;
	struct flow_rule *rule;
	int attr_size, err;

#if defined(HAVE_TC_CLS_OFFLOAD_EXTACK) && defined(PRIO_CHAIN_SUPPORT)
       /* multi-chain not supported for NIC rules */
	if (!tc_cls_can_offload_and_chain0(priv->netdev, &f->common))
		return -EOPNOTSUPP;
#endif /* HAVE_TC_CLS_OFFLOAD_EXTACK && PRIO_CHAIN_SUPPORT */

#ifndef HAVE_TC_SETUP_FLOW_ACTION
	rule = alloc_flow_rule(f);
	if (IS_ERR(rule))
		return PTR_ERR(rule);
#else
	rule = tc_cls_flower_offload_flow_rule(f);
#endif

	flow_flags |= BIT(MLX5E_TC_FLOW_FLAG_NIC) | BIT(MLX5E_TC_FLOW_FLAG_SIMPLE);
	attr_size  = sizeof(struct mlx5_nic_flow_attr);
	err = mlx5e_alloc_flow(priv, attr_size, f->cookie, flow_flags, GFP_KERNEL,
			       &parse_attr, &flow);
	if (err)
		goto out;

	parse_attr->filter_dev = filter_dev;
	err = parse_cls_flower(flow->priv, flow, &parse_attr->spec,
			       f, filter_dev
#ifndef HAVE_TC_SETUP_FLOW_ACTION
			       , rule
#endif

					    );
	if (err)
		goto err_free;

	err = parse_tc_nic_actions(priv, &rule->action, parse_attr, flow, extack);
	if (err)
		goto err_free;

	err = mlx5e_tc_add_nic_flow(priv, parse_attr, flow, extack);
	if (err)
		goto err_free;

	flow_flag_set(flow, OFFLOADED);
	kvfree(parse_attr);
#ifndef HAVE_TC_SETUP_FLOW_ACTION
	free_flow_rule(rule);
#endif
	*__flow = flow;

	return 0;

err_free:
	mlx5e_flow_put(priv, flow);
	kvfree(parse_attr);
out:
#ifndef HAVE_TC_SETUP_FLOW_ACTION
	free_flow_rule(rule);
#endif
	return err;
}

static int
mlx5e_tc_add_flow(struct mlx5e_priv *priv,
		  struct tc_cls_flower_offload *f,
		  unsigned long flags,
		  struct net_device *filter_dev,
		  struct mlx5e_tc_flow **flow)
{
	struct mlx5_eswitch *esw = priv->mdev->priv.eswitch;
	unsigned long flow_flags;
	int err;

	get_flags(flags, &flow_flags);
#if defined(HAVE_TC_CLS_OFFLOAD_EXTACK) && defined(HAVE_TC_CLS_FLOWER_OFFLOAD_COMMON)
	if (!tc_can_offload_extack(priv->netdev, f->common.extack))
		return -EOPNOTSUPP;
#endif

	if (esw && esw->mode == MLX5_ESWITCH_OFFLOADS)
		err = mlx5e_add_fdb_flow(priv, f, flow_flags,
					 filter_dev, flow);
	else
		err = mlx5e_add_nic_flow(priv, f, flow_flags,
					 filter_dev, flow);

	return err;
}

static bool is_flow_rule_duplicate_allowed(struct net_device *dev,
					   struct mlx5e_rep_priv *rpriv)
{
	/* Offloaded flow rule is allowed to duplicate on non-uplink representor
	 * sharing tc block with other slaves of a lag device.
	 */
	return netif_is_lag_port(dev) && rpriv->rep->vport != MLX5_VPORT_UPLINK;
}

int mlx5e_configure_flower(struct net_device *dev, struct mlx5e_priv *priv,
			   struct tc_cls_flower_offload *f, unsigned long flags)
{
#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
	struct netlink_ext_ack *extack = f->common.extack;
#endif
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);
	struct mlx5e_rep_priv *rpriv = priv->ppriv;
	struct mlx5e_tc_flow *flow;
	int err = 0;

#ifdef CONFIG_COMPAT_CLS_FLOWER_MOD
	get_new_flags(priv, &flags);
#endif

	flow = rhashtable_lookup_fast(tc_ht, &f->cookie, tc_ht_params);
	if (flow) {
		/* Same flow rule offloaded to non-uplink representor sharing
		 * tc block, just return 0.
		 */
		if (is_flow_rule_duplicate_allowed(dev, rpriv) &&
		    flow->added_dev != dev)
			goto out;

#ifdef HAVE_TC_CLS_OFFLOAD_EXTACK
		NL_SET_ERR_MSG_MOD(extack,
				   "flow cookie already exists, ignoring");
#endif
		netdev_warn_once(priv->netdev,
				 "flow cookie %lx already exists, ignoring\n",
				 f->cookie);
		err = -EEXIST;
		goto out;
	}

	err = mlx5e_tc_add_flow(priv, f, flags, dev, &flow);
	if (err)
		goto out;

	/* Flow rule offloaded to non-uplink representor sharing tc block,
	 * set the flow's owner dev.
	 */
	if (is_flow_rule_duplicate_allowed(dev, rpriv))
		flow->added_dev = dev;

#ifdef HAVE_MINIFLOW
	flow->version = miniflow_version_inc();
#endif
	err = rhashtable_lookup_insert_fast(tc_ht, &flow->node, tc_ht_params);
	if (err)
		goto err_free;

	return 0;

err_free:
	mlx5e_flow_put(priv, flow);
out:
	return err;
}
#ifdef CONFIG_COMPAT_CLS_FLOWER_MOD
EXPORT_SYMBOL(mlx5e_configure_flower);
#endif

static bool same_flow_direction(struct mlx5e_tc_flow *flow, int flags)
{
#if !(defined(HAVE_NDO_SETUP_TC_4_PARAMS) || defined(HAVE_NDO_SETUP_TC_TAKES_CHAIN_INDEX))
       bool dir_ingress = !!(flags & MLX5_TC_FLAG(INGRESS));
       bool dir_egress = !!(flags & MLX5_TC_FLAG(EGRESS));

       return flow_flag_test(flow, INGRESS) == dir_ingress &&
       	flow_flag_test(flow, EGRESS) == dir_egress;
#else
	return true;
#endif
}

static void mlx5e_flow_defered_put(struct rcu_head *head)
{
	struct mlx5e_tc_flow *flow = container_of(head, struct mlx5e_tc_flow, rcu);

	mlx5e_flow_put(flow->priv, flow);
}

int mlx5e_delete_flower(struct net_device *dev, struct mlx5e_priv *priv,
			struct tc_cls_flower_offload *f, unsigned long flags)
{
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);
	struct mlx5e_tc_flow *flow;
	int err = 0;

	rcu_read_lock();
	flow = rhashtable_lookup_fast(tc_ht, &f->cookie, tc_ht_params);
	if (!flow || !same_flow_direction(flow, flags)) {
		err = -EINVAL;
		goto errout;
	}

	/* Only delete the flow if it doesn't have MLX5E_TC_FLOW_DELETED flag
	 * set.
	 */
	if (flow_flag_test_and_set(flow, DELETED)) {
		err = -EINVAL;
		goto errout;
	}

	rhashtable_remove_fast(tc_ht, &flow->node, tc_ht_params);
	rcu_read_unlock();

	/* Protect __miniflow_merge() */
	if (!mlx5e_is_simple_flow(flow)) {
		call_rcu(&flow->rcu, mlx5e_flow_defered_put);
		return 0;
	}

	mlx5e_flow_put(priv, flow);

	return 0;

errout:
	rcu_read_unlock();
	return err;
}
#ifdef CONFIG_COMPAT_CLS_FLOWER_MOD
EXPORT_SYMBOL(mlx5e_delete_flower);
#endif

#ifdef HAVE_TC_CLSFLOWER_STATS
int mlx5e_stats_flower(struct net_device *dev, struct mlx5e_priv *priv,
		       struct tc_cls_flower_offload *f, unsigned long flags)
{
	struct mlx5_devcom *devcom = priv->mdev->priv.devcom;
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);
	struct mlx5_eswitch *peer_esw;
	struct mlx5e_tc_flow *flow;
	struct mlx5_fc *counter;
#ifndef HAVE_TCF_EXTS_STATS_UPDATE
	struct tc_action *a;
	LIST_HEAD(actions);
#endif
	u64 lastuse = 0;
	u64 packets = 0;
	u64 bytes = 0;
	int err = 0;

#ifdef CONFIG_COMPAT_CLS_FLOWER_MOD
	get_new_flags(priv, &flags);
#endif

	rcu_read_lock();
	flow = mlx5e_flow_get(rhashtable_lookup(tc_ht, &f->cookie,
						tc_ht_params));
	rcu_read_unlock();
	if (IS_ERR(flow))
		return PTR_ERR(flow);

	if (!same_flow_direction(flow, flags)) {
		err = -EINVAL;
		goto errout;
	}

	if (mlx5e_is_offloaded_flow(flow))
		counter = mlx5e_tc_get_counter(flow);
	else
		counter = flow->dummy_counter;

	if (!counter)
		goto errout;

	mlx5_fc_query_cached(counter, &bytes, &packets, &lastuse);

	/* Under multipath it's possible for one rule to be currently
	 * un-offloaded while the other rule is offloaded.
	 */
	peer_esw = mlx5_devcom_get_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
	if (!peer_esw)
		goto out;

	if (flow_flag_test(flow, DUP) &&
	    flow_flag_test(flow->peer_flow, OFFLOADED)) {
		u64 bytes2;
		u64 packets2;
		u64 lastuse2;

		counter = mlx5e_tc_get_counter(flow->peer_flow);
		if (!counter)
			goto no_peer_counter;
		mlx5_fc_query_cached(counter, &bytes2, &packets2, &lastuse2);

		bytes += bytes2;
		packets += packets2;
		lastuse = max_t(u64, lastuse, lastuse2);
	}

no_peer_counter:
	mlx5_devcom_release_peer_data(devcom, MLX5_DEVCOM_ESW_OFFLOADS);
out:
#ifdef HAVE_TC_CLS_FLOWER_OFFLOAD_HAS_STATS_FIELD
	f->stats.pkts += packets;
	f->stats.bytes += bytes;
	f->stats.lastused = max_t(u64, f->stats.lastused, lastuse);
#elif defined(HAVE_TCF_EXTS_STATS_UPDATE)
	tcf_exts_stats_update(f->exts, bytes, packets, lastuse);
#else
	preempt_disable();

#ifdef HAVE_TCF_EXTS_TO_LIST
	tcf_exts_to_list(f->exts, &actions);
	list_for_each_entry(a, &actions, list)
#else
	tc_for_each_action(a, f->exts)
#endif
#ifdef HAVE_TCF_ACTION_STATS_UPDATE
		tcf_action_stats_update(a, bytes, packets, lastuse);
#else
	{
		struct tcf_act_hdr *h = a->priv;

		spin_lock(&h->tcf_lock);
		h->tcf_tm.lastuse = max_t(u64, h->tcf_tm.lastuse, lastuse);
		h->tcf_bstats.bytes += bytes;
		h->tcf_bstats.packets += packets;
		spin_unlock(&h->tcf_lock);
	}
#endif
	preempt_enable();
#endif /* HAVE_TC_CLS_FLOWER_OFFLOAD_HAS_STATS_FIELD */

errout:
	mlx5e_flow_put(priv, flow);
	return err;
}

#ifdef CONFIG_COMPAT_CLS_FLOWER_MOD
EXPORT_SYMBOL(mlx5e_stats_flower);
#endif
#endif /* HAVE_TC_CLSFLOWER_STATS */

static void mlx5e_tc_hairpin_update_dead_peer(struct mlx5e_priv *priv,
					      struct mlx5e_priv *peer_priv)
{
	struct mlx5_core_dev *peer_mdev = peer_priv->mdev;
	struct mlx5e_hairpin_entry *hpe, *tmp;
	LIST_HEAD(init_wait_list);
	u16 peer_vhca_id;
	int bkt;

	if (!same_hw_devs(priv, peer_priv))
		return;

	peer_vhca_id = MLX5_CAP_GEN(peer_mdev, vhca_id);

	mutex_lock(&priv->fs.tc.hairpin_tbl_lock);
	hash_for_each(priv->fs.tc.hairpin_tbl, bkt, hpe, hairpin_hlist) {
		/* Save all hpe's that are being initialized concurrently to wait list. */
		if (!hpe->hp) {
			refcount_inc(&hpe->refcnt);
			list_add(&hpe->dead_peer_wait_list, &init_wait_list);
		} else if (!IS_ERR(hpe->hp) && hpe->peer_vhca_id == peer_vhca_id) {
			hpe->hp->pair->peer_gone = true;
		}
	}
	mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);

	list_for_each_entry_safe(hpe, tmp, &init_wait_list, dead_peer_wait_list) {
		wait_for_completion(&hpe->hw_res_created);

		mutex_lock(&priv->fs.tc.hairpin_tbl_lock);
		if (!IS_ERR(hpe->hp) && hpe->peer_vhca_id == peer_vhca_id)
			hpe->hp->pair->peer_gone = true;
		mutex_unlock(&priv->fs.tc.hairpin_tbl_lock);

		mlx5e_hairpin_put(priv, hpe);
	}
}

static int mlx5e_tc_netdev_event(struct notifier_block *this,
				 unsigned long event, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct mlx5e_flow_steering *fs;
	struct mlx5e_priv *peer_priv;
	struct mlx5e_tc_table *tc;
	struct mlx5e_priv *priv;

	if (ndev->netdev_ops != &mlx5e_netdev_ops ||
	    event != NETDEV_UNREGISTER ||
	    ndev->reg_state == NETREG_REGISTERED)
		return NOTIFY_DONE;

	tc = container_of(this, struct mlx5e_tc_table, netdevice_nb);
	fs = container_of(tc, struct mlx5e_flow_steering, tc);
	priv = container_of(fs, struct mlx5e_priv, fs);
	peer_priv = netdev_priv(ndev);
	if (priv == peer_priv ||
	    !(priv->netdev->features & NETIF_F_HW_TC))
		return NOTIFY_DONE;

	mlx5e_tc_hairpin_update_dead_peer(priv, peer_priv);

	return NOTIFY_DONE;
}
#endif /* HAVE_TC_FLOWER_OFFLOAD */

int mlx5e_tc_nic_init(struct mlx5e_priv *priv)
{
#ifdef HAVE_TC_FLOWER_OFFLOAD
	struct mlx5e_tc_table *tc = &priv->fs.tc;
	int err;

	mutex_init(&tc->t_lock);
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	mutex_init(&tc->mod_hdr.lock);
	hash_init(tc->mod_hdr.hlist);
#endif
	mutex_init(&tc->hairpin_tbl_lock);
	hash_init(tc->hairpin_tbl);

	err = rhashtable_init(&tc->ht, &tc_ht_params);
	if (err)
		return err;

	tc->netdevice_nb.notifier_call = mlx5e_tc_netdev_event;
	if (register_netdevice_notifier(&tc->netdevice_nb)) {
		tc->netdevice_nb.notifier_call = NULL;
		mlx5_core_warn(priv->mdev, "Failed to register netdev notifier\n");
	}

	return err;
#else
	return 0;
#endif
}

#ifdef HAVE_TC_FLOWER_OFFLOAD
static void _mlx5e_tc_del_flow(void *ptr, void *arg)
{
	struct mlx5e_tc_flow *flow = ptr;
	struct mlx5e_priv *priv = flow->priv;

	mlx5e_tc_del_flow(priv, flow);
	kfree(flow);
}
#endif

void mlx5e_tc_nic_cleanup(struct mlx5e_priv *priv)
{
#ifdef HAVE_TC_FLOWER_OFFLOAD
	struct mlx5e_tc_table *tc = &priv->fs.tc;

	if (tc->netdevice_nb.notifier_call)
		unregister_netdevice_notifier(&tc->netdevice_nb);

#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	mutex_destroy(&tc->mod_hdr.lock);
#endif
	mutex_destroy(&tc->hairpin_tbl_lock);

	rhashtable_destroy(&tc->ht);

	if (!IS_ERR_OR_NULL(tc->t)) {
		mlx5_destroy_flow_table(tc->t);
		tc->t = NULL;
	}
	mutex_destroy(&tc->t_lock);
#endif
}

int mlx5e_tc_esw_init(struct mlx5e_priv *priv)
{
#ifdef HAVE_TC_FLOWER_OFFLOAD
	struct rhashtable *tc_ht = get_tc_ht(priv, MLX5_TC_FLAG(ESW_OFFLOAD));
	int err;

	err = miniflow_cache_init(priv);
	if (err)
		return err;

	err = rhashtable_init(tc_ht, &tc_ht_params);
	if (err)
		goto err_tc_ht;

	return 0;

err_tc_ht:
	miniflow_cache_destroy(priv);
	return err;
#else
	return 0;
#endif
}

void mlx5e_tc_esw_cleanup(struct mlx5e_priv *priv)
{
#ifdef HAVE_TC_FLOWER_OFFLOAD
	struct rhashtable *tc_ht = get_tc_ht(priv, MLX5_TC_FLAG(ESW_OFFLOAD));

	rhashtable_free_and_destroy(tc_ht, _mlx5e_tc_del_flow, NULL);
	miniflow_cache_destroy(priv);
#endif
}

#ifdef HAVE_TC_FLOWER_OFFLOAD
int mlx5e_tc_num_filters(struct mlx5e_priv *priv, unsigned long flags)
{
	struct rhashtable *tc_ht = get_tc_ht(priv, flags);

	return atomic_read(&tc_ht->nelems);
}
#endif

void mlx5e_tc_clean_fdb_peer_flows(struct mlx5_eswitch *esw)
{
#ifdef HAVE_TC_FLOWER_OFFLOAD
	struct mlx5e_tc_flow *flow, *tmp;

	list_for_each_entry_safe(flow, tmp, &esw->offloads.peer_flows, peer)
		__mlx5e_tc_del_fdb_peer_flow(flow);
#endif
}

void mlx5e_tc_reoffload_flows_work(struct work_struct *work)
{
#ifdef HAVE_TC_FLOWER_OFFLOAD
#ifdef HAVE_TCF_TUNNEL_INFO
	struct mlx5_rep_uplink_priv *rpriv =
		container_of(work, struct mlx5_rep_uplink_priv,
			     reoffload_flows_work);
	struct mlx5e_tc_flow *flow, *tmp;

	mutex_lock(&rpriv->unready_flows_lock);
	list_for_each_entry_safe(flow, tmp, &rpriv->unready_flows, unready) {
		if (!mlx5e_tc_add_fdb_flow(flow->priv, flow, NULL))
			unready_flow_del(flow);
	}
	mutex_unlock(&rpriv->unready_flows_lock);
#endif
#endif
}
