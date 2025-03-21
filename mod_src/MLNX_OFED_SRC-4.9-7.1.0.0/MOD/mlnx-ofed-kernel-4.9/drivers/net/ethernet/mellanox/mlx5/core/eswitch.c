/*
 * Copyright (c) 2015, Mellanox Technologies. All rights reserved.
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

#include <linux/etherdevice.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/mlx5_ifc.h>
#include <linux/mlx5/vport.h>
#include <linux/mlx5/fs.h>
#include "mlx5_core.h"
#include "lib/eq.h"
#include "eswitch.h"
#include "fs_core.h"
#include "ecpf.h"

enum {
	MLX5_ACTION_NONE = 0,
	MLX5_ACTION_ADD  = 1,
	MLX5_ACTION_DEL  = 2,
};

enum esw_vst_mode {
	ESW_VST_MODE_BASIC,
	ESW_VST_MODE_STEERING,
	ESW_VST_MODE_INSERT_ALWAYS,
};

/* Vport UC/MC hash node */
struct vport_addr {
	struct l2addr_node     node;
	u8                     action;
	u16                    vport;
	struct mlx5_flow_handle *flow_rule;
	bool mpfs; /* UC MAC was added to MPFs */
	/* A flag indicating that mac was added due to mc promiscuous vport */
	bool mc_promisc;
};

#ifdef HAVE_IDA_SIMPLE_GET
DEFINE_IDA(mlx5e_vport_match_ida);
#else
static DECLARE_BITMAP(mlx5e_vport_match_map, VHCA_VPORT_MATCH_ID_SIZE);
#endif
DEFINE_MUTEX(mlx5e_vport_match_ida_mutex);

u16 esw_get_unique_match_id(void)
{
	u16 match_id;

	mutex_lock(&mlx5e_vport_match_ida_mutex);
#ifdef HAVE_IDA_SIMPLE_GET
	match_id = ida_simple_get(&mlx5e_vport_match_ida, 0,
				  VHCA_VPORT_MATCH_ID_SIZE, GFP_KERNEL);
#else
	match_id = find_first_zero_bit(mlx5e_vport_match_map,
				       VHCA_VPORT_MATCH_ID_SIZE);
	if (match_id < VHCA_VPORT_MATCH_ID_SIZE)
		bitmap_set(mlx5e_vport_match_map, match_id, 1);
#endif
	mutex_unlock(&mlx5e_vport_match_ida_mutex);

	return match_id;
}

void esw_free_unique_match_id(u16 match_id)
{
	mutex_lock(&mlx5e_vport_match_ida_mutex);
	ida_simple_remove(&mlx5e_vport_match_ida, match_id);
	mutex_unlock(&mlx5e_vport_match_ida_mutex);
}

static void esw_destroy_legacy_fdb_table(struct mlx5_eswitch *esw);
static void esw_cleanup_vepa_rules(struct mlx5_eswitch *esw);

int mlx5_esw_set_uplink_rep_mode(struct mlx5_core_dev *mdev, int mode)
{
	struct mlx5_eswitch *esw = mdev->priv.eswitch;

	if (mode != MLX5_ESW_UPLINK_REP_MODE_NEW_NETDEV &&
	    mode != MLX5_ESW_UPLINK_REP_MODE_NIC_NETDEV)
		return -EOPNOTSUPP;

	esw->offloads.uplink_rep_mode = mode;
	return 0;
}

struct mlx5_vport *__must_check
mlx5_eswitch_get_vport(struct mlx5_eswitch *esw, u16 vport_num)
{
	u16 idx;

	if (!esw || !MLX5_CAP_GEN(esw->dev, vport_group_manager))
		return ERR_PTR(-EPERM);

	idx = mlx5_eswitch_vport_num_to_index(esw, vport_num);

	if (idx > esw->total_vports - 1) {
		esw_debug(esw->dev, "vport out of range: num(0x%x), idx(0x%x)\n",
			  vport_num, idx);
		return ERR_PTR(-EINVAL);
	}

	return &esw->vports[idx];
}

static bool is_esw_manager_vport(const struct mlx5_eswitch *esw, u16 vport_num)
{
	return esw->manager_vport == vport_num;
}

static int arm_vport_context_events_cmd(struct mlx5_core_dev *dev, u16 vport,
					u32 events_mask)
{
	int in[MLX5_ST_SZ_DW(modify_nic_vport_context_in)]   = {0};
	int out[MLX5_ST_SZ_DW(modify_nic_vport_context_out)] = {0};
	void *nic_vport_ctx;

	MLX5_SET(modify_nic_vport_context_in, in,
		 opcode, MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT);
	MLX5_SET(modify_nic_vport_context_in, in, field_select.change_event, 1);
	MLX5_SET(modify_nic_vport_context_in, in, vport_number, vport);
	MLX5_SET(modify_nic_vport_context_in, in, other_vport, 1);
	nic_vport_ctx = MLX5_ADDR_OF(modify_nic_vport_context_in,
				     in, nic_vport_context);

	MLX5_SET(nic_vport_context, nic_vport_ctx, arm_change_event, 1);

	if (events_mask & MLX5_VPORT_UC_ADDR_CHANGE)
		MLX5_SET(nic_vport_context, nic_vport_ctx,
			 event_on_uc_address_change, 1);
	if (events_mask & MLX5_VPORT_MC_ADDR_CHANGE)
		MLX5_SET(nic_vport_context, nic_vport_ctx,
			 event_on_mc_address_change, 1);
	if (events_mask & MLX5_VPORT_PROMISC_CHANGE)
		MLX5_SET(nic_vport_context, nic_vport_ctx,
			 event_on_promisc_change, 1);

	return mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
}

/* E-Switch vport context HW commands */
static int modify_esw_vport_context_cmd(struct mlx5_core_dev *dev, u16 vport,
					bool other_vport,
					void *in, int inlen)
{
	u32 out[MLX5_ST_SZ_DW(modify_esw_vport_context_out)] = {0};

	MLX5_SET(modify_esw_vport_context_in, in, opcode,
		 MLX5_CMD_OP_MODIFY_ESW_VPORT_CONTEXT);
	MLX5_SET(modify_esw_vport_context_in, in, vport_number, vport);
	MLX5_SET(modify_esw_vport_context_in, in, other_vport,
		 other_vport ? 1 : 0);
	return mlx5_cmd_exec(dev, in, inlen, out, sizeof(out));
}

int mlx5_eswitch_modify_esw_vport_context(struct mlx5_eswitch *esw, u16 vport,
					  bool other_vport,
					  void *in, int inlen)
{
	return modify_esw_vport_context_cmd(esw->dev, vport, other_vport,
					    in, inlen);
}

static int query_esw_vport_context_cmd(struct mlx5_core_dev *dev, u16 vport,
				       bool other_vport,
				       void *out, int outlen)
{
	u32 in[MLX5_ST_SZ_DW(query_esw_vport_context_in)] = {};

	MLX5_SET(query_esw_vport_context_in, in, opcode,
		 MLX5_CMD_OP_QUERY_ESW_VPORT_CONTEXT);
	MLX5_SET(modify_esw_vport_context_in, in, vport_number, vport);
	MLX5_SET(modify_esw_vport_context_in, in, other_vport,
		 other_vport ? 1 : 0);
	return mlx5_cmd_exec(dev, in, sizeof(in), out, outlen);
}

int mlx5_eswitch_query_esw_vport_context(struct mlx5_eswitch *esw, u16 vport,
					 bool other_vport,
					 void *out, int outlen)
{
	return query_esw_vport_context_cmd(esw->dev, vport, other_vport,
					   out, outlen);
}
EXPORT_SYMBOL_GPL(mlx5_eswitch_query_esw_vport_context);

static int modify_esw_vport_cvlan(struct mlx5_core_dev *dev, u16 vport,
				  u16 vlan, u8 qos, u8 set_flags,
				  enum esw_vst_mode vst_mode)
{
	u32 in[MLX5_ST_SZ_DW(modify_esw_vport_context_in)] = {0};

	if (!MLX5_CAP_ESW(dev, vport_cvlan_strip) ||
	    !MLX5_CAP_ESW(dev, vport_cvlan_insert_if_not_exist))
		return -EOPNOTSUPP;

	esw_debug(dev, "Set Vport[%d] VLAN %d qos %d set=%x\n",
		  vport, vlan, qos, set_flags);

	if (set_flags & SET_VLAN_STRIP)
		MLX5_SET(modify_esw_vport_context_in, in,
			 esw_vport_context.vport_cvlan_strip, 1);

	if (set_flags & SET_VLAN_INSERT) {
		if (vst_mode == ESW_VST_MODE_INSERT_ALWAYS) {
			/* insert either if vlan exist in packet or not */
			MLX5_SET(modify_esw_vport_context_in, in,
				 esw_vport_context.vport_cvlan_insert,
				 MLX5_VPORT_CVLAN_INSERT_ALWAYS);
		} else {
			/* insert only if no vlan in packet */
			MLX5_SET(modify_esw_vport_context_in, in,
				 esw_vport_context.vport_cvlan_insert,
				 MLX5_VPORT_CVLAN_INSERT_WHEN_NO_CVLAN);
		}
		MLX5_SET(modify_esw_vport_context_in, in,
			 esw_vport_context.cvlan_pcp, qos);
		MLX5_SET(modify_esw_vport_context_in, in,
			 esw_vport_context.cvlan_id, vlan);
	}

	MLX5_SET(modify_esw_vport_context_in, in,
		 field_select.vport_cvlan_strip, 1);
	MLX5_SET(modify_esw_vport_context_in, in,
		 field_select.vport_cvlan_insert, 1);

	return modify_esw_vport_context_cmd(dev, vport, true, in, sizeof(in));
}

/* E-Switch FDB */
static struct mlx5_flow_handle *
__esw_fdb_set_vport_rule(struct mlx5_eswitch *esw, u16 vport, bool rx_rule,
			 u8 mac_c[ETH_ALEN], u8 mac_v[ETH_ALEN])
{
	int match_header = (is_zero_ether_addr(mac_c) ? 0 :
			    MLX5_MATCH_OUTER_HEADERS);
	struct mlx5_flow_handle *flow_rule = NULL;
	struct mlx5_flow_act flow_act = {0};
	struct mlx5_flow_destination dest = {};
	struct mlx5_flow_spec *spec;
	void *mv_misc = NULL;
	void *mc_misc = NULL;
	u8 *dmac_v = NULL;
	u8 *dmac_c = NULL;

	if (rx_rule)
		match_header |= MLX5_MATCH_MISC_PARAMETERS;

	spec = kvzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return NULL;

	dmac_v = MLX5_ADDR_OF(fte_match_param, spec->match_value,
			      outer_headers.dmac_47_16);
	dmac_c = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
			      outer_headers.dmac_47_16);

	if (match_header & MLX5_MATCH_OUTER_HEADERS) {
		ether_addr_copy(dmac_v, mac_v);
		ether_addr_copy(dmac_c, mac_c);
	}

	if (match_header & MLX5_MATCH_MISC_PARAMETERS) {
		mv_misc  = MLX5_ADDR_OF(fte_match_param, spec->match_value,
					misc_parameters);
		mc_misc  = MLX5_ADDR_OF(fte_match_param, spec->match_criteria,
					misc_parameters);
		MLX5_SET(fte_match_set_misc, mv_misc, source_port, MLX5_VPORT_UPLINK);
		MLX5_SET_TO_ONES(fte_match_set_misc, mc_misc, source_port);
	}

	dest.type = MLX5_FLOW_DESTINATION_TYPE_VPORT;
	dest.vport.num = vport;

	esw_debug(esw->dev,
		  "\tFDB add rule dmac_v(%pM) dmac_c(%pM) -> vport(%d)\n",
		  dmac_v, dmac_c, vport);
	spec->match_criteria_enable = match_header;
	flow_act.action =  MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	flow_rule =
		mlx5_add_flow_rules(esw->fdb_table.legacy.fdb, spec,
				    &flow_act, &dest, 1);
	if (IS_ERR(flow_rule)) {
		esw_warn(esw->dev,
			 "FDB: Failed to add flow rule: dmac_v(%pM) dmac_c(%pM) -> vport(%d), err(%ld)\n",
			 dmac_v, dmac_c, vport, PTR_ERR(flow_rule));
		flow_rule = NULL;
	}

	kvfree(spec);
	return flow_rule;
}

static struct mlx5_flow_handle *
esw_fdb_set_vport_rule(struct mlx5_eswitch *esw, u8 mac[ETH_ALEN], u16 vport)
{
	u8 mac_c[ETH_ALEN];

	eth_broadcast_addr(mac_c);
	return __esw_fdb_set_vport_rule(esw, vport, false, mac_c, mac);
}

static struct mlx5_flow_handle *
esw_fdb_set_vport_allmulti_rule(struct mlx5_eswitch *esw, u16 vport)
{
	u8 mac_c[ETH_ALEN];
	u8 mac_v[ETH_ALEN];

	eth_zero_addr(mac_c);
	eth_zero_addr(mac_v);
	mac_c[0] = 0x01;
	mac_v[0] = 0x01;
	return __esw_fdb_set_vport_rule(esw, vport, false, mac_c, mac_v);
}

static struct mlx5_flow_handle *
esw_fdb_set_vport_promisc_rule(struct mlx5_eswitch *esw, u16 vport)
{
	u8 mac_c[ETH_ALEN];
	u8 mac_v[ETH_ALEN];

	eth_zero_addr(mac_c);
	eth_zero_addr(mac_v);
	return __esw_fdb_set_vport_rule(esw, vport, true, mac_c, mac_v);
}

enum {
	LEGACY_VEPA_PRIO = 0,
	LEGACY_FDB_PRIO,
};

static int esw_create_legacy_vepa_table(struct mlx5_eswitch *esw)
{
	struct mlx5_core_dev *dev = esw->dev;
	struct mlx5_flow_namespace *root_ns;
	struct mlx5_flow_table *fdb;
	int err;

	root_ns = mlx5_get_fdb_sub_ns(dev, 0);
	if (!root_ns) {
		esw_warn(dev, "Failed to get FDB flow namespace\n");
		return -EOPNOTSUPP;
	}

	/* num FTE 2, num FG 2 */
	fdb = mlx5_create_auto_grouped_flow_table(root_ns, LEGACY_VEPA_PRIO,
						  2, 2, 0, 0);
	if (IS_ERR(fdb)) {
		err = PTR_ERR(fdb);
		esw_warn(dev, "Failed to create VEPA FDB err %d\n", err);
		return err;
	}
	esw->fdb_table.legacy.vepa_fdb = fdb;

	return 0;
}

static int esw_create_legacy_fdb_table(struct mlx5_eswitch *esw)
{
	int inlen = MLX5_ST_SZ_BYTES(create_flow_group_in);
	struct mlx5_flow_table_attr ft_attr = {};
	struct mlx5_core_dev *dev = esw->dev;
	struct mlx5_flow_namespace *root_ns;
	struct mlx5_flow_table *fdb;
	struct mlx5_flow_group *g;
	void *match_criteria;
	int table_size;
	u32 *flow_group_in;
	u8 *dmac;
	int err = 0;

	esw_debug(dev, "Create FDB log_max_size(%d)\n",
		  MLX5_CAP_ESW_FLOWTABLE_FDB(dev, log_max_ft_size));

	root_ns = mlx5_get_fdb_sub_ns(dev, 0);
	if (!root_ns) {
		esw_warn(dev, "Failed to get FDB flow namespace\n");
		return -EOPNOTSUPP;
	}

	flow_group_in = kvzalloc(inlen, GFP_KERNEL);
	if (!flow_group_in)
		return -ENOMEM;

	table_size = BIT(MLX5_CAP_ESW_FLOWTABLE_FDB(dev, log_max_ft_size));
	ft_attr.max_fte = table_size;
	ft_attr.prio = LEGACY_FDB_PRIO;
	fdb = mlx5_create_flow_table(root_ns, &ft_attr);
	if (IS_ERR(fdb)) {
		err = PTR_ERR(fdb);
		esw_warn(dev, "Failed to create FDB Table err %d\n", err);
		goto out;
	}
	esw->fdb_table.legacy.fdb = fdb;

	/* Addresses group : Full match unicast/multicast addresses */
	MLX5_SET(create_flow_group_in, flow_group_in, match_criteria_enable,
		 MLX5_MATCH_OUTER_HEADERS);
	match_criteria = MLX5_ADDR_OF(create_flow_group_in, flow_group_in, match_criteria);
	dmac = MLX5_ADDR_OF(fte_match_param, match_criteria, outer_headers.dmac_47_16);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, 0);
	/* Preserve 2 entries for allmulti and promisc rules*/
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, table_size - 3);
	eth_broadcast_addr(dmac);
	g = mlx5_create_flow_group(fdb, flow_group_in);
	if (IS_ERR(g)) {
		err = PTR_ERR(g);
		esw_warn(dev, "Failed to create flow group err(%d)\n", err);
		goto out;
	}
	esw->fdb_table.legacy.addr_grp = g;

	/* Allmulti group : One rule that forwards any mcast traffic */
	MLX5_SET(create_flow_group_in, flow_group_in, match_criteria_enable,
		 MLX5_MATCH_OUTER_HEADERS);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, table_size - 2);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, table_size - 2);
	eth_zero_addr(dmac);
	dmac[0] = 0x01;
	g = mlx5_create_flow_group(fdb, flow_group_in);
	if (IS_ERR(g)) {
		err = PTR_ERR(g);
		esw_warn(dev, "Failed to create allmulti flow group err(%d)\n", err);
		goto out;
	}
	esw->fdb_table.legacy.allmulti_grp = g;

	/* Promiscuous group :
	 * One rule that forward all unmatched traffic from previous groups
	 */
	eth_zero_addr(dmac);
	MLX5_SET(create_flow_group_in, flow_group_in, match_criteria_enable,
		 MLX5_MATCH_MISC_PARAMETERS);
	MLX5_SET_TO_ONES(fte_match_param, match_criteria, misc_parameters.source_port);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, table_size - 1);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, table_size - 1);
	g = mlx5_create_flow_group(fdb, flow_group_in);
	if (IS_ERR(g)) {
		err = PTR_ERR(g);
		esw_warn(dev, "Failed to create promisc flow group err(%d)\n", err);
		goto out;
	}
	esw->fdb_table.legacy.promisc_grp = g;

out:
	if (err)
		esw_destroy_legacy_fdb_table(esw);

	kvfree(flow_group_in);
	return err;
}

static void esw_destroy_legacy_vepa_table(struct mlx5_eswitch *esw)
{
	esw_debug(esw->dev, "Destroy VEPA Table\n");
	if (!esw->fdb_table.legacy.vepa_fdb)
		return;

	mlx5_destroy_flow_table(esw->fdb_table.legacy.vepa_fdb);
	esw->fdb_table.legacy.vepa_fdb = NULL;
}

static void esw_destroy_legacy_fdb_table(struct mlx5_eswitch *esw)
{
	esw_debug(esw->dev, "Destroy FDB Table\n");
	if (!esw->fdb_table.legacy.fdb)
		return;

	if (esw->fdb_table.legacy.promisc_grp)
		mlx5_destroy_flow_group(esw->fdb_table.legacy.promisc_grp);
	if (esw->fdb_table.legacy.allmulti_grp)
		mlx5_destroy_flow_group(esw->fdb_table.legacy.allmulti_grp);
	if (esw->fdb_table.legacy.addr_grp)
		mlx5_destroy_flow_group(esw->fdb_table.legacy.addr_grp);
	mlx5_destroy_flow_table(esw->fdb_table.legacy.fdb);

	esw->fdb_table.legacy.fdb = NULL;
	esw->fdb_table.legacy.addr_grp = NULL;
	esw->fdb_table.legacy.allmulti_grp = NULL;
	esw->fdb_table.legacy.promisc_grp = NULL;
}

static int esw_create_legacy_table(struct mlx5_eswitch *esw)
{
	int err;

	memset(&esw->fdb_table.legacy, 0, sizeof(struct legacy_fdb));

	err = esw_create_legacy_vepa_table(esw);
	if (err)
		return err;

	err = esw_create_legacy_fdb_table(esw);
	if (err)
		esw_destroy_legacy_vepa_table(esw);

	return err;
}

/* Vport context events */
#define MLX5_LEGACY_SRIOV_VPORT_EVENTS (MLX5_VPORT_UC_ADDR_CHANGE | \
					MLX5_VPORT_MC_ADDR_CHANGE | \
					MLX5_VPORT_VLAN_CHANGE | \
					MLX5_VPORT_PROMISC_CHANGE)

static int esw_legacy_enable(struct mlx5_eswitch *esw)
{
	struct mlx5_vport *vport;
	int ret, i;

	ret = esw_create_legacy_table(esw);
	if (ret)
		return ret;

	mlx5_esw_for_each_vf_vport(esw, i, vport, esw->esw_funcs.num_vfs)
		vport->info.link_state = MLX5_VPORT_ADMIN_STATE_AUTO;

	mlx5_eswitch_enable_pf_vf_vports(esw, MLX5_LEGACY_SRIOV_VPORT_EVENTS);
	return 0;
}

static void esw_destroy_legacy_table(struct mlx5_eswitch *esw)
{
	esw_cleanup_vepa_rules(esw);
	esw_destroy_legacy_fdb_table(esw);
	esw_destroy_legacy_vepa_table(esw);
}

static void esw_legacy_disable(struct mlx5_eswitch *esw)
{
	struct esw_mc_addr *mc_promisc;

	mlx5_eswitch_disable_pf_vf_vports(esw);

	mc_promisc = &esw->mc_promisc;
	if (mc_promisc->uplink_rule)
		mlx5_del_flow_rules(mc_promisc->uplink_rule);

	esw_destroy_legacy_table(esw);
}

/* E-Switch vport UC/MC lists management */
typedef int (*vport_addr_action)(struct mlx5_eswitch *esw,
				 struct vport_addr *vaddr);

static int esw_add_uc_addr(struct mlx5_eswitch *esw, struct vport_addr *vaddr)
{
	u8 *mac = vaddr->node.addr;
	u16 vport = vaddr->vport;
	int err;

	/* Skip mlx5_mpfs_add_mac for eswitch_managers,
	 * it is already done by its netdev in mlx5e_execute_l2_action
	 */
	if (is_esw_manager_vport(esw, vport))
		goto fdb_add;

	err = mlx5_mpfs_add_mac(esw->dev, mac);
	if (err) {
		esw_warn(esw->dev,
			 "Failed to add L2 table mac(%pM) for vport(0x%x), err(%d)\n",
			 mac, vport, err);
		return err;
	}
	vaddr->mpfs = true;

fdb_add:
	/* SRIOV is enabled: Forward UC MAC to vport */
	if (esw->fdb_table.legacy.fdb && esw->mode == MLX5_ESWITCH_LEGACY)
		vaddr->flow_rule = esw_fdb_set_vport_rule(esw, mac, vport);

	esw_debug(esw->dev, "\tADDED UC MAC: vport[%d] %pM fr(%p)\n",
		  vport, mac, vaddr->flow_rule);

	return 0;
}

static int esw_del_uc_addr(struct mlx5_eswitch *esw, struct vport_addr *vaddr)
{
	u8 *mac = vaddr->node.addr;
	u16 vport = vaddr->vport;
	int err = 0;

	/* Skip mlx5_mpfs_del_mac for eswitch managerss,
	 * it is already done by its netdev in mlx5e_execute_l2_action
	 */
	if (!vaddr->mpfs || is_esw_manager_vport(esw, vport))
		goto fdb_del;

	err = mlx5_mpfs_del_mac(esw->dev, mac);
	if (err)
		esw_warn(esw->dev,
			 "Failed to del L2 table mac(%pM) for vport(%d), err(%d)\n",
			 mac, vport, err);
	vaddr->mpfs = false;

fdb_del:
	if (vaddr->flow_rule)
		mlx5_del_flow_rules(vaddr->flow_rule);
	vaddr->flow_rule = NULL;

	return 0;
}

static void update_allmulti_vports(struct mlx5_eswitch *esw,
				   struct vport_addr *vaddr,
				   struct esw_mc_addr *esw_mc)
{
	u8 *mac = vaddr->node.addr;
	struct mlx5_vport *vport;
	u16 i, vport_num;
	COMPAT_HL_NODE

	mlx5_esw_for_all_vports(esw, i, vport) {
		struct hlist_head *vport_hash = vport->mc_list;
		struct vport_addr *iter_vaddr =
					l2addr_hash_find(vport_hash,
							 mac,
							 struct vport_addr);
		vport_num = vport->vport;
		if (IS_ERR_OR_NULL(vport->allmulti_rule) ||
		    vaddr->vport == vport_num)
			continue;
		switch (vaddr->action) {
		case MLX5_ACTION_ADD:
			if (iter_vaddr)
				continue;
			iter_vaddr = l2addr_hash_add(vport_hash, mac,
						     struct vport_addr,
						     GFP_KERNEL);
			if (!iter_vaddr) {
				esw_warn(esw->dev,
					 "ALL-MULTI: Failed to add MAC(%pM) to vport[%d] DB\n",
					 mac, vport_num);
				continue;
			}
			iter_vaddr->vport = vport_num;
			iter_vaddr->flow_rule =
					esw_fdb_set_vport_rule(esw,
							       mac,
							       vport_num);
			iter_vaddr->mc_promisc = true;
			break;
		case MLX5_ACTION_DEL:
			if (!iter_vaddr)
				continue;
			mlx5_del_flow_rules(iter_vaddr->flow_rule);
			l2addr_hash_del(iter_vaddr);
			break;
		}
	}
}

static int esw_add_mc_addr(struct mlx5_eswitch *esw, struct vport_addr *vaddr)
{
	struct hlist_head *hash = esw->mc_table;
	struct esw_mc_addr *esw_mc;
	u8 *mac = vaddr->node.addr;
	u16 vport = vaddr->vport;
	COMPAT_HL_NODE

	if (!esw->fdb_table.legacy.fdb)
		return 0;

	esw_mc = l2addr_hash_find(hash, mac, struct esw_mc_addr);
	if (esw_mc)
		goto add;

	esw_mc = l2addr_hash_add(hash, mac, struct esw_mc_addr, GFP_KERNEL);
	if (!esw_mc)
		return -ENOMEM;

	esw_mc->uplink_rule = /* Forward MC MAC to Uplink */
		esw_fdb_set_vport_rule(esw, mac, MLX5_VPORT_UPLINK);

	/* Add this multicast mac to all the mc promiscuous vports */
	update_allmulti_vports(esw, vaddr, esw_mc);

add:
	/* If the multicast mac is added as a result of mc promiscuous vport,
	 * don't increment the multicast ref count
	 */
	if (!vaddr->mc_promisc)
		esw_mc->refcnt++;

	/* Forward MC MAC to vport */
	vaddr->flow_rule = esw_fdb_set_vport_rule(esw, mac, vport);
	esw_debug(esw->dev,
		  "\tADDED MC MAC: vport[%d] %pM fr(%p) refcnt(%d) uplinkfr(%p)\n",
		  vport, mac, vaddr->flow_rule,
		  esw_mc->refcnt, esw_mc->uplink_rule);
	return 0;
}

static int esw_del_mc_addr(struct mlx5_eswitch *esw, struct vport_addr *vaddr)
{
	struct hlist_head *hash = esw->mc_table;
	struct esw_mc_addr *esw_mc;
	u8 *mac = vaddr->node.addr;
	u16 vport = vaddr->vport;
	COMPAT_HL_NODE

	if (!esw->fdb_table.legacy.fdb)
		return 0;

	esw_mc = l2addr_hash_find(hash, mac, struct esw_mc_addr);
	if (!esw_mc) {
		esw_warn(esw->dev,
			 "Failed to find eswitch MC addr for MAC(%pM) vport(%d)",
			 mac, vport);
		return -EINVAL;
	}
	esw_debug(esw->dev,
		  "\tDELETE MC MAC: vport[%d] %pM fr(%p) refcnt(%d) uplinkfr(%p)\n",
		  vport, mac, vaddr->flow_rule, esw_mc->refcnt,
		  esw_mc->uplink_rule);

	if (vaddr->flow_rule)
		mlx5_del_flow_rules(vaddr->flow_rule);
	vaddr->flow_rule = NULL;

	/* If the multicast mac is added as a result of mc promiscuous vport,
	 * don't decrement the multicast ref count.
	 */
	if (vaddr->mc_promisc || (--esw_mc->refcnt > 0))
		return 0;

	/* Remove this multicast mac from all the mc promiscuous vports */
	update_allmulti_vports(esw, vaddr, esw_mc);

	if (esw_mc->uplink_rule) {
		mlx5_del_flow_rules(esw_mc->uplink_rule);
		esw_mc->uplink_rule = NULL;
	}

	l2addr_hash_del(esw_mc);
	return 0;
}

/* Apply vport UC/MC list to HW l2 table and FDB table */
static void esw_apply_vport_addr_list(struct mlx5_eswitch *esw,
				      struct mlx5_vport *vport, int list_type)
{
	bool is_uc = list_type == MLX5_NVPRT_LIST_TYPE_UC;
	vport_addr_action vport_addr_add;
	vport_addr_action vport_addr_del;
	struct vport_addr *addr;
	struct l2addr_node *node;
	struct hlist_head *hash;
	struct hlist_node *tmp;
	int hi;
	COMPAT_HL_NODE

	vport_addr_add = is_uc ? esw_add_uc_addr :
				 esw_add_mc_addr;
	vport_addr_del = is_uc ? esw_del_uc_addr :
				 esw_del_mc_addr;

	hash = is_uc ? vport->uc_list : vport->mc_list;
	for_each_l2hash_node(node, tmp, hash, hi) {
		addr = container_of(node, struct vport_addr, node);
		switch (addr->action) {
		case MLX5_ACTION_ADD:
			vport_addr_add(esw, addr);
			addr->action = MLX5_ACTION_NONE;
			break;
		case MLX5_ACTION_DEL:
			vport_addr_del(esw, addr);
			l2addr_hash_del(addr);
			break;
		}
	}
}

/* Sync vport UC/MC list from vport context */
static void esw_update_vport_addr_list(struct mlx5_eswitch *esw,
				       struct mlx5_vport *vport, int list_type)
{
	bool is_uc = list_type == MLX5_NVPRT_LIST_TYPE_UC;
	u8 (*mac_list)[ETH_ALEN];
	struct l2addr_node *node;
	struct vport_addr *addr;
	struct hlist_head *hash;
	struct hlist_node *tmp;
	int size;
	int err;
	int hi;
	int i;
	COMPAT_HL_NODE

	size = is_uc ? MLX5_MAX_UC_PER_VPORT(esw->dev) :
		       MLX5_MAX_MC_PER_VPORT(esw->dev);

	mac_list = kcalloc(size, ETH_ALEN, GFP_KERNEL);
	if (!mac_list)
		return;

	hash = is_uc ? vport->uc_list : vport->mc_list;

	for_each_l2hash_node(node, tmp, hash, hi) {
		addr = container_of(node, struct vport_addr, node);
		addr->action = MLX5_ACTION_DEL;
	}

	if (!vport->enabled)
		goto out;

	err = mlx5_query_nic_vport_mac_list(esw->dev, vport->vport, list_type,
					    mac_list, &size);
	if (err)
		goto out;
	esw_debug(esw->dev, "vport[%d] context update %s list size (%d)\n",
		  vport->vport, is_uc ? "UC" : "MC", size);

	for (i = 0; i < size; i++) {
		if (is_uc && !is_valid_ether_addr(mac_list[i]))
			continue;

		if (!is_uc && !is_multicast_ether_addr(mac_list[i]))
			continue;

		addr = l2addr_hash_find(hash, mac_list[i], struct vport_addr);
		if (addr) {
			addr->action = MLX5_ACTION_NONE;
			/* If this mac was previously added because of allmulti
			 * promiscuous rx mode, its now converted to be original
			 * vport mac.
			 */
			if (addr->mc_promisc) {
				struct esw_mc_addr *esw_mc =
					l2addr_hash_find(esw->mc_table,
							 mac_list[i],
							 struct esw_mc_addr);
				if (!esw_mc) {
					esw_warn(esw->dev,
						 "Failed to MAC(%pM) in mcast DB\n",
						 mac_list[i]);
					continue;
				}
				esw_mc->refcnt++;
				addr->mc_promisc = false;
			}
			continue;
		}

		addr = l2addr_hash_add(hash, mac_list[i], struct vport_addr,
				       GFP_KERNEL);
		if (!addr) {
			esw_warn(esw->dev,
				 "Failed to add MAC(%pM) to vport[%d] DB\n",
				 mac_list[i], vport->vport);
			continue;
		}
		addr->vport = vport->vport;
		addr->action = MLX5_ACTION_ADD;
	}
out:
	kfree(mac_list);
}

static void esw_update_acl_trunk_bitmap(struct mlx5_eswitch *esw, u32 vport_num)
{
	struct mlx5_vport *vport = &esw->vports[vport_num];

	bitmap_and(vport->acl_vlan_8021q_bitmap, vport->req_vlan_bitmap,
		   vport->info.vlan_trunk_8021q_bitmap, VLAN_N_VID);
}

static int esw_vport_egress_config(struct mlx5_eswitch *esw,
				   struct mlx5_vport *vport);
static int esw_vport_ingress_config(struct mlx5_eswitch *esw,
				    struct mlx5_vport *vport);

/* Sync vport vlan list from vport context */
static void esw_update_vport_vlan_list(struct mlx5_eswitch *esw, u32 vport_num)
{
	struct mlx5_vport *vport = &esw->vports[vport_num];
	DECLARE_BITMAP(tmp_vlans_bitmap, VLAN_N_VID);
	int err;

	if (!vport->enabled)
		return;

	bitmap_copy(tmp_vlans_bitmap, vport->req_vlan_bitmap, VLAN_N_VID);
	bitmap_zero(vport->req_vlan_bitmap, VLAN_N_VID);

	err = mlx5_query_nic_vport_vlans(esw->dev, vport_num, vport->req_vlan_bitmap);
	if (err)
		return;

	bitmap_xor(tmp_vlans_bitmap, tmp_vlans_bitmap, vport->req_vlan_bitmap, VLAN_N_VID);
	if (!bitmap_weight(tmp_vlans_bitmap, VLAN_N_VID))
		return;

	esw_update_acl_trunk_bitmap(esw, vport_num);
	esw_vport_egress_config(esw, vport);
	esw_vport_ingress_config(esw, vport);
}

/* Sync vport UC/MC list from vport context
 * Must be called after esw_update_vport_addr_list
 */
static void esw_update_vport_mc_promisc(struct mlx5_eswitch *esw,
					struct mlx5_vport *vport)
{
	struct l2addr_node *node;
	struct vport_addr *addr;
	struct hlist_head *hash;
	struct hlist_node *tmp;
	int hi;
	COMPAT_HL_NODE

	hash = vport->mc_list;

	for_each_l2hash_node(node, tmp, esw->mc_table, hi) {
		u8 *mac = node->addr;

		addr = l2addr_hash_find(hash, mac, struct vport_addr);
		if (addr) {
			if (addr->action == MLX5_ACTION_DEL)
				addr->action = MLX5_ACTION_NONE;
			continue;
		}
		addr = l2addr_hash_add(hash, mac, struct vport_addr,
				       GFP_KERNEL);
		if (!addr) {
			esw_warn(esw->dev,
				 "Failed to add allmulti MAC(%pM) to vport[%d] DB\n",
				 mac, vport->vport);
			continue;
		}
		addr->vport = vport->vport;
		addr->action = MLX5_ACTION_ADD;
		addr->mc_promisc = true;
	}
}

/* Apply vport rx mode to HW FDB table */
static void esw_apply_vport_rx_mode(struct mlx5_eswitch *esw,
				    struct mlx5_vport *vport,
				    bool promisc, bool mc_promisc)
{
	struct esw_mc_addr *allmulti_addr = &esw->mc_promisc;

	if (IS_ERR_OR_NULL(vport->allmulti_rule) != mc_promisc)
		goto promisc;

	if (mc_promisc) {
		vport->allmulti_rule =
			esw_fdb_set_vport_allmulti_rule(esw, vport->vport);
		if (!allmulti_addr->uplink_rule)
			allmulti_addr->uplink_rule =
				esw_fdb_set_vport_allmulti_rule(esw,
								MLX5_VPORT_UPLINK);
		allmulti_addr->refcnt++;
	} else if (vport->allmulti_rule) {
		mlx5_del_flow_rules(vport->allmulti_rule);
		vport->allmulti_rule = NULL;

		if (--allmulti_addr->refcnt > 0)
			goto promisc;

		if (allmulti_addr->uplink_rule)
			mlx5_del_flow_rules(allmulti_addr->uplink_rule);
		allmulti_addr->uplink_rule = NULL;
	}

promisc:
	if (IS_ERR_OR_NULL(vport->promisc_rule) != promisc)
		return;

	if (promisc) {
		vport->promisc_rule =
			esw_fdb_set_vport_promisc_rule(esw, vport->vport);
	} else if (vport->promisc_rule) {
		mlx5_del_flow_rules(vport->promisc_rule);
		vport->promisc_rule = NULL;
	}
}

/* Sync vport rx mode from vport context */
static void esw_update_vport_rx_mode(struct mlx5_eswitch *esw,
				     struct mlx5_vport *vport)
{
	struct esw_mc_addr *allmulti_addr = &esw->mc_promisc;
	struct mlx5_core_dev *dev = vport->dev;
	int promisc_all = 0;
	int promisc_uc = 0;
	int promisc_mc = 0;
	int err;

	err = mlx5_query_nic_vport_promisc(esw->dev,
					   vport->vport,
					   &promisc_uc,
					   &promisc_mc,
					   &promisc_all);
	if (err) {
		if (!pci_channel_offline(dev->pdev) &&
		    dev->state != MLX5_DEVICE_STATE_INTERNAL_ERROR)
			return;

		/* EEH or PCI error. Delete promisc, multi and uplink multi rules */
		if (vport->allmulti_rule) {
			mlx5_del_flow_rules(vport->allmulti_rule);
			vport->allmulti_rule = NULL;

			allmulti_addr->refcnt --;
			if (!allmulti_addr->refcnt && allmulti_addr->uplink_rule) {
				mlx5_del_flow_rules(allmulti_addr->uplink_rule);
				allmulti_addr->uplink_rule = NULL;
			}
		}

		if (vport->promisc_rule) {
			mlx5_del_flow_rules(vport->promisc_rule);
			vport->promisc_rule = NULL;
		}

       	return;
	}
	esw_debug(esw->dev, "vport[%d] context update rx mode promisc_all=%d, all_multi=%d\n",
		  vport->vport, promisc_all, promisc_mc);

	if (!vport->info.trusted || !vport->enabled) {
		promisc_uc = 0;
		promisc_mc = 0;
		promisc_all = 0;
	}

	esw_apply_vport_rx_mode(esw, vport, promisc_all,
				(promisc_all || promisc_mc));
}

static void esw_vport_change_handle_locked(struct mlx5_vport *vport)
{
	struct mlx5_core_dev *dev = vport->dev;
	struct mlx5_eswitch *esw = dev->priv.eswitch;
	u8 mac[ETH_ALEN];

	mlx5_query_other_nic_vport_mac_address(dev, vport->vport, mac);
	esw_debug(dev, "vport[%d] Context Changed: perm mac: %pM\n",
		  vport->vport, mac);

	if (vport->enabled_events & MLX5_VPORT_UC_ADDR_CHANGE) {
		esw_update_vport_addr_list(esw, vport, MLX5_NVPRT_LIST_TYPE_UC);
		esw_apply_vport_addr_list(esw, vport, MLX5_NVPRT_LIST_TYPE_UC);
	}

	if (vport->enabled_events & MLX5_VPORT_MC_ADDR_CHANGE)
		esw_update_vport_addr_list(esw, vport, MLX5_NVPRT_LIST_TYPE_MC);
		
	if (vport->enabled_events & MLX5_VPORT_VLAN_CHANGE)
		esw_update_vport_vlan_list(esw, vport->vport);


	if (vport->enabled_events & MLX5_VPORT_PROMISC_CHANGE) {
		esw_update_vport_rx_mode(esw, vport);
		if (!IS_ERR_OR_NULL(vport->allmulti_rule))
			esw_update_vport_mc_promisc(esw, vport);
	}

	if (vport->enabled_events & (MLX5_VPORT_PROMISC_CHANGE | MLX5_VPORT_MC_ADDR_CHANGE))
		esw_apply_vport_addr_list(esw, vport, MLX5_NVPRT_LIST_TYPE_MC);

	esw_debug(esw->dev, "vport[%d] Context Changed: Done\n", vport->vport);
	if (vport->enabled)
		arm_vport_context_events_cmd(dev, vport->vport,
					     vport->enabled_events);
}

static void esw_vport_change_handler(struct work_struct *work)
{
	struct mlx5_vport *vport =
		container_of(work, struct mlx5_vport, vport_change_handler);
	struct mlx5_eswitch *esw = vport->dev->priv.eswitch;

	mutex_lock(&esw->state_lock);
	esw_vport_change_handle_locked(vport);
	mutex_unlock(&esw->state_lock);
}

int esw_vport_enable_egress_acl(struct mlx5_eswitch *esw,
				struct mlx5_vport *vport)
{
	int inlen = MLX5_ST_SZ_BYTES(create_flow_group_in);
	struct mlx5_flow_group *untagged_grp = NULL;
	struct mlx5_flow_group *vlan_grp = NULL;
	struct mlx5_flow_group *drop_grp = NULL;
	struct mlx5_core_dev *dev = esw->dev;
	struct mlx5_flow_namespace *root_ns;
	struct mlx5_flow_table *acl;
	/* The egress acl table contains 3 groups:
	 * 1)Allow untagged traffic
	 * 2)Allow tagged traffic with vlan_tag=vst_vlan_id/vgt+_vlan_id
	 * 3)Drop all other traffic
	 */
	int table_size = VLAN_N_VID + 2;
	void *match_criteria;
	u32 *flow_group_in;
	int err = 0;

	if (!MLX5_CAP_ESW_EGRESS_ACL(dev, ft_support))
		return -EOPNOTSUPP;

	if (!IS_ERR_OR_NULL(vport->egress.acl))
		return 0;

	esw_debug(dev, "Create vport[%d] egress ACL log_max_size(%d)\n",
		  vport->vport, MLX5_CAP_ESW_EGRESS_ACL(dev, log_max_ft_size));

	root_ns = mlx5_get_flow_vport_acl_namespace(dev, MLX5_FLOW_NAMESPACE_ESW_EGRESS,
			mlx5_eswitch_vport_num_to_index(esw, vport->vport));
	if (!root_ns) {
		esw_warn(dev, "Failed to get E-Switch egress flow namespace for vport (%d)\n", vport->vport);
		return -EOPNOTSUPP;
	}

	flow_group_in = kvzalloc(inlen, GFP_KERNEL);
	if (!flow_group_in)
		return -ENOMEM;

	acl = mlx5_create_vport_flow_table(root_ns, 0, table_size, 0, vport->vport);
	if (IS_ERR(acl)) {
		err = PTR_ERR(acl);
		esw_warn(dev, "Failed to create E-Switch vport[%d] egress flow Table, err(%d)\n",
			 vport->vport, err);
		goto out;
	}

	MLX5_SET(create_flow_group_in, flow_group_in, match_criteria_enable, MLX5_MATCH_OUTER_HEADERS);
	match_criteria = MLX5_ADDR_OF(create_flow_group_in, flow_group_in, match_criteria);

	/* Create flow group for allowed untagged flow rule */
	MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.cvlan_tag);
	MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.svlan_tag);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, 0);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, 0);

	untagged_grp = mlx5_create_flow_group(acl, flow_group_in);
	if (IS_ERR(untagged_grp)) {
		err = PTR_ERR(untagged_grp);
		esw_warn(dev, "Failed to create E-Switch vport[%d] egress untagged flow group, err(%d)\n",
			 vport->vport, err);
		goto out;
	}

	memset(flow_group_in, 0, inlen);
	MLX5_SET(create_flow_group_in, flow_group_in, match_criteria_enable, MLX5_MATCH_OUTER_HEADERS);

	/* Create flow group for allowed tagged flow rules */
	MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.cvlan_tag);

	if (esw->mode != MLX5_ESWITCH_OFFLOADS)
		MLX5_SET_TO_ONES(fte_match_param, match_criteria,
				 outer_headers.svlan_tag);

	MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.first_vid);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, 1);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, VLAN_N_VID);

	vlan_grp = mlx5_create_flow_group(acl, flow_group_in);
	if (IS_ERR(vlan_grp)) {
		err = PTR_ERR(vlan_grp);
		esw_warn(dev, "Failed to create E-Switch vport[%d] egress allowed vlans flow group, err(%d)\n",
			 vport->vport, err);
		goto out;
	}

	/* Create flow group for drop rule */
	memset(flow_group_in, 0, inlen);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, VLAN_N_VID + 1);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, VLAN_N_VID + 1);
	drop_grp = mlx5_create_flow_group(acl, flow_group_in);
	if (IS_ERR(drop_grp)) {
		err = PTR_ERR(drop_grp);
		esw_warn(dev, "Failed to create E-Switch vport[%d] egress drop flow group, err(%d)\n",
			 vport->vport, err);
		goto out;
	}

	vport->egress.acl = acl;
	vport->egress.drop_grp = drop_grp;
	vport->egress.allowed_vlans_grp = vlan_grp;
	vport->egress.allow_untagged_grp = untagged_grp;

out:
	if (err) {
		if (!IS_ERR_OR_NULL(vlan_grp))
			mlx5_destroy_flow_group(vlan_grp);
		if (!IS_ERR_OR_NULL(untagged_grp))
			mlx5_destroy_flow_group(untagged_grp);
		if (!IS_ERR_OR_NULL(acl))
			mlx5_destroy_flow_table(acl);
	}

	kvfree(flow_group_in);
	return err;
}

void esw_vport_cleanup_egress_rules(struct mlx5_eswitch *esw,
				    struct mlx5_vport *vport)
{

	struct mlx5_acl_vlan *trunk_vlan_rule, *tmp;

	if (!IS_ERR_OR_NULL(vport->egress.allow_vst_vlan))
		mlx5_del_flow_rules(vport->egress.allow_vst_vlan);

	list_for_each_entry_safe(trunk_vlan_rule, tmp,
				 &vport->egress.allow_vlans_rules, list) {
		mlx5_del_flow_rules(trunk_vlan_rule->acl_vlan_rule);
		list_del(&trunk_vlan_rule->list);
		kfree(trunk_vlan_rule);
	}

	if (!IS_ERR_OR_NULL(vport->egress.drop_rule))
		mlx5_del_flow_rules(vport->egress.drop_rule);

	if (!IS_ERR_OR_NULL(vport->egress.allow_untagged_rule))
		mlx5_del_flow_rules(vport->egress.allow_untagged_rule);

	if (!IS_ERR_OR_NULL(vport->egress.allowed_vlan))
		mlx5_del_flow_rules(vport->egress.allowed_vlan);

	if (!IS_ERR_OR_NULL(vport->egress.bounce_rule))
		mlx5_del_flow_rules(vport->egress.bounce_rule);

	vport->egress.allow_untagged_rule = NULL;
	vport->egress.allow_vst_vlan = NULL;
	vport->egress.drop_rule = NULL;
	vport->egress.bounce_rule = NULL;
	vport->egress.allowed_vlan = NULL;
}

void esw_vport_disable_egress_acl(struct mlx5_eswitch *esw,
				  struct mlx5_vport *vport)
{
	if (IS_ERR_OR_NULL(vport->egress.acl))
		return;

	esw_debug(esw->dev, "Destroy vport[%d] E-Switch egress ACL\n", vport->vport);

	esw_vport_cleanup_egress_rules(esw, vport);
	if (!IS_ERR_OR_NULL(vport->egress.allow_untagged_grp))
		mlx5_destroy_flow_group(vport->egress.allow_untagged_grp);
	if (!IS_ERR_OR_NULL(vport->egress.allowed_vlans_grp))
		mlx5_destroy_flow_group(vport->egress.allowed_vlans_grp);
	if (!IS_ERR_OR_NULL(vport->egress.drop_grp))
		mlx5_destroy_flow_group(vport->egress.drop_grp);
	if (!IS_ERR_OR_NULL(vport->egress.bounce_grp))
		mlx5_destroy_flow_group(vport->egress.bounce_grp);
	if (!IS_ERR_OR_NULL(vport->egress.acl))
		mlx5_destroy_flow_table(vport->egress.acl);

	vport->egress.allow_untagged_grp = NULL;
	vport->egress.allowed_vlans_grp = NULL;
	vport->egress.drop_grp = NULL;
	vport->egress.bounce_grp = NULL;
	vport->egress.acl = NULL;
}

static inline enum esw_vst_mode esw_get_vst_mode(struct mlx5_eswitch *esw)
{
	/*  vst mode precedence:
	 *  if vst steering mode is supported use it
	 *  if not, look for vst vport insert always support
	 *  if both not supported, we use basic vst, can't support QinQ
	 */
	if (MLX5_CAP_ESW_EGRESS_ACL(esw->dev, pop_vlan) &&
	    MLX5_CAP_ESW_INGRESS_ACL(esw->dev, push_vlan))
		return ESW_VST_MODE_STEERING;
	else if (MLX5_CAP_ESW(esw->dev, vport_cvlan_insert_always))
		return ESW_VST_MODE_INSERT_ALWAYS;
	else
		return ESW_VST_MODE_BASIC;
}

int esw_vport_enable_ingress_acl(struct mlx5_eswitch *esw,
				 struct mlx5_vport *vport)
{
	bool need_vlan_filter = !!bitmap_weight(vport->info.vlan_trunk_8021q_bitmap,
						VLAN_N_VID);
	enum esw_vst_mode vst_mode = esw_get_vst_mode(esw);
	int inlen = MLX5_ST_SZ_BYTES(create_flow_group_in);
	struct mlx5_flow_group *untagged_spoof_grp = NULL;
	struct mlx5_flow_group *tagged_spoof_grp = NULL;
	struct mlx5_flow_group *drop_grp = NULL;
	struct mlx5_core_dev *dev = esw->dev;
	struct mlx5_flow_namespace *root_ns;
	struct mlx5_flow_table *acl;
	void *match_criteria;
	u32 *flow_group_in;
	/* The ingress acl table contains 4 groups
	 * (2 active rules at the same time -
	 *      1 allow rule from one of the first 3 groups.
	 *      1 drop rule from the last group):
	 * 1)Allow untagged traffic with smac=original mac.
	 * 2)Allow untagged traffic.
	 * 3)Allow tagged traffic with smac=original mac.
	 * 4)Drop all other traffic.
	 */
	int table_size = need_vlan_filter ? 8192 : 4;
	bool push_on_any_pkt = false;
	int allow_grp_sz = 1;
	int err = 0;

	if (!MLX5_CAP_ESW_INGRESS_ACL(dev, ft_support))
		return -EOPNOTSUPP;

	if (!IS_ERR_OR_NULL(vport->ingress.acl))
		return 0;

	esw_debug(dev, "Create vport[%d] ingress ACL log_max_size(%d)\n",
		  vport->vport, MLX5_CAP_ESW_INGRESS_ACL(dev, log_max_ft_size));

	root_ns = mlx5_get_flow_vport_acl_namespace(dev, MLX5_FLOW_NAMESPACE_ESW_INGRESS,
			mlx5_eswitch_vport_num_to_index(esw, vport->vport));
	if (!root_ns) {
		esw_warn(dev, "Failed to get E-Switch ingress flow namespace for vport (%d)\n", vport->vport);
		return -EOPNOTSUPP;
	}

	flow_group_in = kvzalloc(inlen, GFP_KERNEL);
	if (!flow_group_in)
		return -ENOMEM;

	acl = mlx5_create_vport_flow_table(root_ns, 0, table_size, 0, vport->vport);
	if (IS_ERR(acl)) {
		err = PTR_ERR(acl);
		esw_warn(dev, "Failed to create E-Switch vport[%d] ingress flow Table, err(%d)\n",
			 vport->vport, err);
		goto out;
	}
	vport->ingress.acl = acl;

	match_criteria = MLX5_ADDR_OF(create_flow_group_in, flow_group_in, match_criteria);

	if (esw->mode == MLX5_ESWITCH_OFFLOADS) {
		MLX5_SET(create_flow_group_in, flow_group_in,
			 match_criteria_enable, MLX5_MATCH_OUTER_HEADERS);

		MLX5_SET_TO_ONES(fte_match_param, match_criteria,
				 outer_headers.cvlan_tag);
	} else {
		push_on_any_pkt = (vst_mode != ESW_VST_MODE_BASIC) &&
				  !vport->info.spoofchk && !need_vlan_filter;
		if (!push_on_any_pkt)
			MLX5_SET(create_flow_group_in, flow_group_in,
				 match_criteria_enable, MLX5_MATCH_OUTER_HEADERS);

		if (need_vlan_filter || (vst_mode == ESW_VST_MODE_BASIC &&
					 (vport->info.vlan || vport->info.qos)))
			MLX5_SET_TO_ONES(fte_match_param, match_criteria,
					 outer_headers.cvlan_tag);

		if (vport->info.spoofchk) {
			MLX5_SET_TO_ONES(fte_match_param, match_criteria,
					 outer_headers.smac_47_16);
			MLX5_SET_TO_ONES(fte_match_param, match_criteria,
					 outer_headers.smac_15_0);
		}
	}

	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, 0);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, 0);

	untagged_spoof_grp = mlx5_create_flow_group(acl, flow_group_in);
	if (IS_ERR(untagged_spoof_grp)) {
		err = PTR_ERR(untagged_spoof_grp);
		esw_warn(dev, "Failed to create E-Switch vport[%d] ingress untagged spoofchk flow group, err(%d)\n",
			 vport->vport, err);
		goto out;
	}

	if (esw->mode == MLX5_ESWITCH_OFFLOADS)
		goto drop_grp;

	if (push_on_any_pkt)
		goto set_grp;

	if (!need_vlan_filter)
		goto drop_grp;

	memset(flow_group_in, 0, inlen);
	MLX5_SET(create_flow_group_in, flow_group_in, match_criteria_enable, MLX5_MATCH_OUTER_HEADERS);
	if (vport->info.spoofchk) {
		MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.smac_47_16);
		MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.smac_15_0);
	}
	MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.cvlan_tag);
	MLX5_SET_TO_ONES(fte_match_param, match_criteria, outer_headers.first_vid);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, 1);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, VLAN_N_VID);
	allow_grp_sz = VLAN_N_VID + 1;

	tagged_spoof_grp = mlx5_create_flow_group(acl, flow_group_in);
	if (IS_ERR(tagged_spoof_grp)) {
		err = PTR_ERR(tagged_spoof_grp);
		esw_warn(dev, "Failed to create E-Switch vport[%d] ingress spoofchk flow group, err(%d)\n",
			 vport->vport, err);
		goto out;
	}

drop_grp:
	memset(flow_group_in, 0, inlen);
	MLX5_SET(create_flow_group_in, flow_group_in, start_flow_index, allow_grp_sz);
	MLX5_SET(create_flow_group_in, flow_group_in, end_flow_index, allow_grp_sz);

	drop_grp = mlx5_create_flow_group(acl, flow_group_in);
	if (IS_ERR(drop_grp)) {
		err = PTR_ERR(drop_grp);
		esw_warn(dev, "Failed to create E-Switch vport[%d] ingress drop flow group, err(%d)\n",
			 vport->vport, err);
		goto out;
	}
set_grp:
	vport->ingress.allow_untagged_spoofchk_grp = untagged_spoof_grp;
	vport->ingress.allow_tagged_spoofchk_grp = tagged_spoof_grp;
	vport->ingress.drop_grp = drop_grp;

out:
	if (err) {
		if (!IS_ERR_OR_NULL(tagged_spoof_grp))
			mlx5_destroy_flow_group(tagged_spoof_grp);
		if (!IS_ERR_OR_NULL(untagged_spoof_grp))
			mlx5_destroy_flow_group(untagged_spoof_grp);
		if (!IS_ERR_OR_NULL(vport->ingress.acl))
			mlx5_destroy_flow_table(vport->ingress.acl);
	}

	kvfree(flow_group_in);
	return err;
}

void esw_vport_cleanup_ingress_rules(struct mlx5_eswitch *esw,
				     struct mlx5_vport *vport)
{
	struct mlx5_acl_vlan *trunk_vlan_rule, *tmp;

	if (!IS_ERR_OR_NULL(vport->ingress.drop_rule))
		mlx5_del_flow_rules(vport->ingress.drop_rule);

	list_for_each_entry_safe(trunk_vlan_rule, tmp,
				 &vport->ingress.allow_vlans_rules, list) {
		mlx5_del_flow_rules(trunk_vlan_rule->acl_vlan_rule);
		list_del(&trunk_vlan_rule->list);
		kfree(trunk_vlan_rule);
	}

	if (!IS_ERR_OR_NULL(vport->ingress.allow_untagged_rule))
		mlx5_del_flow_rules(vport->ingress.allow_untagged_rule);

	if (!IS_ERR_OR_NULL(vport->ingress.allow_rule))
		mlx5_del_flow_rules(vport->ingress.allow_rule);

	vport->ingress.allow_rule = NULL;
	vport->ingress.drop_rule = NULL;
	vport->ingress.allow_untagged_rule = NULL;

	esw_vport_del_ingress_acl_modify_metadata(esw, vport);
}

void esw_vport_disable_ingress_acl(struct mlx5_eswitch *esw,
				   struct mlx5_vport *vport)
{
	if (IS_ERR_OR_NULL(vport->ingress.acl))
		return;

	esw_debug(esw->dev, "Destroy vport[%d] E-Switch ingress ACL\n", vport->vport);

	esw_vport_cleanup_ingress_rules(esw, vport);
	if (!IS_ERR_OR_NULL(vport->ingress.allow_tagged_spoofchk_grp))
		mlx5_destroy_flow_group(vport->ingress.allow_tagged_spoofchk_grp);

	if (!IS_ERR_OR_NULL(vport->ingress.allow_untagged_spoofchk_grp))
		mlx5_destroy_flow_group(vport->ingress.allow_untagged_spoofchk_grp);

	if (!IS_ERR_OR_NULL(vport->ingress.drop_grp))
		mlx5_destroy_flow_group(vport->ingress.drop_grp);

	mlx5_destroy_flow_table(vport->ingress.acl);
	vport->ingress.acl = NULL;
	vport->ingress.drop_grp = NULL;
	vport->ingress.allow_tagged_spoofchk_grp = NULL;
	vport->ingress.allow_untagged_spoofchk_grp = NULL;
}

static int esw_vport_ingress_config(struct mlx5_eswitch *esw,
				    struct mlx5_vport *vport)
{
	bool need_vlan_filter = !!bitmap_weight(vport->info.vlan_trunk_8021q_bitmap,
						VLAN_N_VID);
	enum esw_vst_mode vst_mode = esw_get_vst_mode(esw);
	struct mlx5_acl_vlan *trunk_vlan_rule;
	struct mlx5_fc *counter = vport->ingress.drop_counter;
	struct mlx5_flow_destination drop_ctr_dst = {0};
	struct mlx5_flow_destination *dst = NULL;
	struct mlx5_flow_act flow_act = {0};
	struct mlx5_flow_spec *spec;
	bool need_acl_table = true;
	bool push_on_any_pkt;
	u16 vlan_id = 0;
	int dest_num = 0;
	int err = 0;
	u8 *smac_v;

	if ((vport->info.vlan || vport->info.qos) && need_vlan_filter) {
		mlx5_core_warn(esw->dev,
			       "vport[%d] configure ingress rules failed, Cannot enable both VGT+ and VST\n",
			       vport->vport);
		return -EPERM;
	}

	need_acl_table = vport->info.vlan || vport->info.qos || vport->info.spoofchk
			|| need_vlan_filter;

	esw_vport_cleanup_ingress_rules(esw, vport);

	esw_vport_disable_ingress_acl(esw, vport);
	if (!need_acl_table)
		return 0;

	err = esw_vport_enable_ingress_acl(esw, vport);
	if (err) {
		mlx5_core_warn(esw->dev,
			       "failed to enable ingress acl (%d) on vport[%d]\n",
			       err, vport->vport);
		return err;
	}

	esw_debug(esw->dev,
		  "vport[%d] configure ingress rules, vlan(%d) qos(%d) vst_mode (%d)\n",
		  vport->vport, vport->info.vlan, vport->info.qos, vst_mode);

	spec = kvzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec) {
		err = -ENOMEM;
		goto out;
	}

	push_on_any_pkt = (vst_mode != ESW_VST_MODE_BASIC) &&
			  !vport->info.spoofchk && !need_vlan_filter;
	if (!push_on_any_pkt)
		spec->match_criteria_enable = MLX5_MATCH_OUTER_HEADERS;

	flow_act.action = MLX5_FLOW_CONTEXT_ACTION_ALLOW;
	if (vst_mode == ESW_VST_MODE_STEERING &&
	    (vport->info.vlan || vport->info.qos)) {
		flow_act.action |= MLX5_FLOW_CONTEXT_ACTION_VLAN_PUSH;
		flow_act.vlan[0].prio = vport->info.qos;
		flow_act.vlan[0].vid = vport->info.vlan;
		flow_act.vlan[0].ethtype = ntohs(vport->info.vlan_proto);
	}

	if (need_vlan_filter || (vst_mode == ESW_VST_MODE_BASIC &&
				 (vport->info.vlan || vport->info.qos)))
		MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.cvlan_tag);

	if (vport->info.spoofchk) {
		MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.smac_47_16);
		MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.smac_15_0);
		smac_v = MLX5_ADDR_OF(fte_match_param,
				      spec->match_value,
				      outer_headers.smac_47_16);
		ether_addr_copy(smac_v, vport->info.mac);
	}

	/* Allow untagged */
	if (!need_vlan_filter ||
	    (need_vlan_filter && test_bit(0, vport->info.vlan_trunk_8021q_bitmap))) {
		vport->ingress.allow_untagged_rule =
			mlx5_add_flow_rules(vport->ingress.acl, spec,
					    &flow_act, NULL, 0);
		if (IS_ERR(vport->ingress.allow_untagged_rule)) {
			err = PTR_ERR(vport->ingress.allow_untagged_rule);
			esw_warn(esw->dev,
				 "vport[%d] configure ingress allow rule, err(%d)\n",
				 vport->vport, err);
			vport->ingress.allow_untagged_rule = NULL;
			goto out;
		}
	}

	if (push_on_any_pkt)
		goto out;

	if (!need_vlan_filter)
		goto drop_rule;

	MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.cvlan_tag);
	MLX5_SET_TO_ONES(fte_match_param, spec->match_value, outer_headers.cvlan_tag);
	MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.first_vid);

	/* VGT+ rules */
	for_each_set_bit(vlan_id, vport->acl_vlan_8021q_bitmap, VLAN_N_VID) {
		trunk_vlan_rule = kzalloc(sizeof(*trunk_vlan_rule), GFP_KERNEL);
		if (!trunk_vlan_rule) {
			err = -ENOMEM;
			goto out;
		}

		MLX5_SET(fte_match_param, spec->match_value, outer_headers.first_vid,
			 vlan_id);
		trunk_vlan_rule->acl_vlan_rule =
			mlx5_add_flow_rules(vport->ingress.acl, spec, &flow_act, NULL, 0);
		if (IS_ERR(trunk_vlan_rule->acl_vlan_rule)) {
			err = PTR_ERR(trunk_vlan_rule->acl_vlan_rule);
			esw_warn(esw->dev,
				 "vport[%d] configure ingress allowed vlan rule failed, err(%d)\n",
				 vport->vport, err);
			trunk_vlan_rule->acl_vlan_rule = NULL;
			goto out;
		}
		list_add(&trunk_vlan_rule->list, &vport->ingress.allow_vlans_rules);
	}

drop_rule:
	memset(spec, 0, sizeof(*spec));
	memset(&flow_act, 0, sizeof(flow_act));
	flow_act.action = MLX5_FLOW_CONTEXT_ACTION_DROP;

	/* Attach drop flow counter */
	if (counter) {
		flow_act.action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
		drop_ctr_dst.type = MLX5_FLOW_DESTINATION_TYPE_COUNTER;
		drop_ctr_dst.counter_id = mlx5_fc_id(counter);
		dst = &drop_ctr_dst;
		dest_num++;
	}
	vport->ingress.drop_rule =
		mlx5_add_flow_rules(vport->ingress.acl, spec,
				    &flow_act, dst, dest_num);
	if (IS_ERR(vport->ingress.drop_rule)) {
		err = PTR_ERR(vport->ingress.drop_rule);
		esw_warn(esw->dev,
			 "vport[%d] configure ingress drop rule, err(%d)\n",
			 vport->vport, err);
		vport->ingress.drop_rule = NULL;
		goto out;
	}

out:
	if (err)
		esw_vport_cleanup_ingress_rules(esw, vport);
	kvfree(spec);
	return err;
}

static int esw_vport_egress_config(struct mlx5_eswitch *esw,
				   struct mlx5_vport *vport)
{
	bool need_vlan_filter = !!bitmap_weight(vport->info.vlan_trunk_8021q_bitmap,
						VLAN_N_VID);
	bool need_acl_table = vport->info.vlan || vport->info.qos ||
			      need_vlan_filter;
	enum esw_vst_mode vst_mode = esw_get_vst_mode(esw);
	struct mlx5_acl_vlan *trunk_vlan_rule;
	struct mlx5_fc *counter = vport->egress.drop_counter;
	struct mlx5_flow_destination drop_ctr_dst = {0};
	struct mlx5_flow_destination *dst = NULL;
	struct mlx5_flow_act flow_act = {0};
	struct mlx5_flow_spec *spec;
	int dest_num = 0;
	u16 vlan_id = 0;
	int err = 0;

	esw_vport_cleanup_egress_rules(esw, vport);

	if (!need_acl_table) {
		esw_vport_disable_egress_acl(esw, vport);
		return 0;
	}

	err = esw_vport_enable_egress_acl(esw, vport);
	if (err) {
		mlx5_core_warn(esw->dev,
			       "failed to enable egress acl (%d) on vport[%d]\n",
			       err, vport->vport);
		return err;
	}

	esw_debug(esw->dev,
		  "vport[%d] configure egress rules, vlan(%d) qos(%d)\n",
		  vport->vport, vport->info.vlan, vport->info.qos);

	spec = kvzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec) {
		err = -ENOMEM;
		goto out;
	}

	MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.cvlan_tag);
	MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.svlan_tag);
	spec->match_criteria_enable = MLX5_MATCH_OUTER_HEADERS;
	flow_act.action = MLX5_FLOW_CONTEXT_ACTION_ALLOW;

	/* Allow untagged */
	if (need_vlan_filter && test_bit(0, vport->info.vlan_trunk_8021q_bitmap)) {
		vport->egress.allow_untagged_rule =
			mlx5_add_flow_rules(vport->egress.acl, spec,
					    &flow_act, NULL, 0);
		if (IS_ERR(vport->egress.allow_untagged_rule)) {
			err = PTR_ERR(vport->egress.allow_untagged_rule);
			esw_warn(esw->dev,
				 "vport[%d] configure egress allow rule, err(%d)\n",
				 vport->vport, err);
			vport->egress.allow_untagged_rule = NULL;
		}
	}

	/* Allowed vlan rule */
	if (vport->info.vlan_proto == htons(ETH_P_8021Q))
		MLX5_SET_TO_ONES(fte_match_param, spec->match_value, outer_headers.cvlan_tag);
	else
		MLX5_SET_TO_ONES(fte_match_param, spec->match_value, outer_headers.svlan_tag);
	MLX5_SET_TO_ONES(fte_match_param, spec->match_criteria, outer_headers.first_vid);

	/* VST rule */
	if (vport->info.vlan || vport->info.qos) {
		MLX5_SET(fte_match_param, spec->match_value, outer_headers.first_vid, vport->info.vlan);

		if (vst_mode == ESW_VST_MODE_STEERING)
			flow_act.action |= MLX5_FLOW_CONTEXT_ACTION_VLAN_POP;
		vport->egress.allow_vst_vlan =
			mlx5_add_flow_rules(vport->egress.acl, spec,
					    &flow_act, NULL, 0);
		if (IS_ERR(vport->egress.allow_vst_vlan)) {
			err = PTR_ERR(vport->egress.allow_vst_vlan);
			esw_warn(esw->dev,
				 "vport[%d] configure egress allowed vlan rule failed, err(%d)\n",
				 vport->vport, err);
			vport->egress.allow_vst_vlan = NULL;
			goto out;
		}
	}

	/* VGT+ rules */
	for_each_set_bit(vlan_id, vport->acl_vlan_8021q_bitmap, VLAN_N_VID) {
		trunk_vlan_rule = kzalloc(sizeof(*trunk_vlan_rule), GFP_KERNEL);
		if (!trunk_vlan_rule) {
			err = -ENOMEM;
			goto out;
		}

		MLX5_SET(fte_match_param, spec->match_value, outer_headers.first_vid,
			 vlan_id);
		trunk_vlan_rule->acl_vlan_rule =
			mlx5_add_flow_rules(vport->egress.acl, spec, &flow_act, NULL, 0);
		if (IS_ERR(trunk_vlan_rule->acl_vlan_rule)) {
			err = PTR_ERR(trunk_vlan_rule->acl_vlan_rule);
			esw_warn(esw->dev,
				 "vport[%d] configure egress allowed vlan rule failed, err(%d)\n",
				 vport->vport, err);
			trunk_vlan_rule->acl_vlan_rule = NULL;
			goto out;
		}
		list_add(&trunk_vlan_rule->list, &vport->egress.allow_vlans_rules);
	}

	/* Drop others rule (star rule) */
	memset(spec, 0, sizeof(*spec));
	flow_act.action = MLX5_FLOW_CONTEXT_ACTION_DROP;

	/* Attach egress drop flow counter */
	if (counter) {
		flow_act.action |= MLX5_FLOW_CONTEXT_ACTION_COUNT;
		drop_ctr_dst.type = MLX5_FLOW_DESTINATION_TYPE_COUNTER;
		drop_ctr_dst.counter_id = mlx5_fc_id(counter);
		dst = &drop_ctr_dst;
		dest_num++;
	}
	vport->egress.drop_rule =
		mlx5_add_flow_rules(vport->egress.acl, spec,
				    &flow_act, dst, dest_num);
	if (IS_ERR(vport->egress.drop_rule)) {
		err = PTR_ERR(vport->egress.drop_rule);
		esw_warn(esw->dev,
			 "vport[%d] configure egress drop rule failed, err(%d)\n",
			 vport->vport, err);
		vport->egress.drop_rule = NULL;
	}
out:
	if (err)
		esw_vport_cleanup_egress_rules(esw, vport);
	kvfree(spec);
	return err;
}

/* Vport QoS management */
static struct mlx5_vgroup *esw_create_vgroup(struct mlx5_eswitch *esw,
					     u32 group_id)
{
	u32 tsar_ctx[MLX5_ST_SZ_DW(scheduling_context)] = {0};
	struct mlx5_core_dev *dev = esw->dev;
	struct mlx5_vgroup *group;
	u32 tsar_ix;
	int err = 0;

	MLX5_SET(scheduling_context, tsar_ctx, parent_element_id,
		 esw->qos.root_tsar_ix);
	err = mlx5_create_scheduling_element_cmd(dev,
						 SCHEDULING_HIERARCHY_E_SWITCH,
						 tsar_ctx,
						 &tsar_ix);
	if (err) {
		esw_warn(dev, "E-Switch create TSAR for group %d failed (%d)\n",
			 group_id, err);
		return ERR_PTR(err);
	}

	group = kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group) {
		err = -ENOMEM;
		goto clean_tsar;
	}

	group->group_id = group_id;
	group->tsar_ix = tsar_ix;
	group->dev = dev;

	list_add_tail(&group->list, &esw->qos.groups);
	err = mlx5_create_vf_group_sysfs(dev, group->group_id, &group->kobj);
	if (err)
		goto clean_group;

	return group;

clean_group:
	kfree(group);

clean_tsar:
	mlx5_destroy_scheduling_element_cmd(dev,
					    SCHEDULING_HIERARCHY_E_SWITCH,
					    tsar_ix);

	return ERR_PTR(err);
}

static void esw_destroy_vgroup(struct mlx5_eswitch *esw,
			       struct mlx5_vgroup *group)
{
	int err;

	if (group->num_vports)
		esw_warn(esw->dev, "E-Switch destroying group TSAR but group not empty  (group:%d)\n",
			 group->group_id);

	err = mlx5_destroy_scheduling_element_cmd(esw->dev,
						  SCHEDULING_HIERARCHY_E_SWITCH,
						  group->tsar_ix);
	if (err)
		esw_warn(esw->dev, "E-Switch destroy TSAR_ID %d failed (%d)\n",
			 group->tsar_ix, err);

	mlx5_destroy_vf_group_sysfs(esw->dev, &group->kobj);
	list_del(&group->list);
	kfree(group);
}

static void esw_create_tsar(struct mlx5_eswitch *esw)
{
	u32 tsar_ctx[MLX5_ST_SZ_DW(scheduling_context)] = {0};
	struct mlx5_core_dev *dev = esw->dev;
	int err;

	if (!MLX5_CAP_GEN(dev, qos) || !MLX5_CAP_QOS(dev, esw_scheduling))
		return;

	if (esw->qos.enabled)
		return;

	err = mlx5_create_scheduling_element_cmd(dev,
						 SCHEDULING_HIERARCHY_E_SWITCH,
						 tsar_ctx,
						 &esw->qos.root_tsar_ix);
	if (err) {
		esw_warn(esw->dev, "E-Switch create root TSAR failed (%d)\n",
			 err);
		return;
	}

	if (MLX5_CAP_QOS(dev, log_esw_max_sched_depth)) {
		INIT_LIST_HEAD(&esw->qos.groups);

		esw->qos.group0 = esw_create_vgroup(esw, 0);
		if (IS_ERR(esw->qos.group0)) {
			esw_warn(esw->dev, "E-Switch create rate group 0 failed (%d)\n",
				 err);
			goto clean_root;
		}
	} else {
		esw->qos.group0 = NULL;
	}

	esw->qos.enabled = true;
	return; 

clean_root:
	mlx5_destroy_scheduling_element_cmd(esw->dev,
					    SCHEDULING_HIERARCHY_E_SWITCH,
					    esw->qos.root_tsar_ix);
	return;
}

static void esw_destroy_tsar(struct mlx5_eswitch *esw)
{
	int err;

	if (!esw->qos.enabled)
		return;

	if (esw->qos.group0)
		esw_destroy_vgroup(esw, esw->qos.group0);

	err = mlx5_destroy_scheduling_element_cmd(esw->dev,
						  SCHEDULING_HIERARCHY_E_SWITCH,
						  esw->qos.root_tsar_ix);
	if (err)
		esw_warn(esw->dev, "E-Switch destroy TSAR failed (%d)\n", err);

	esw->qos.enabled = false;
}

static int esw_vport_create_sched_element(struct mlx5_eswitch *esw,
					  struct mlx5_vport *vport,
					  u32 max_rate, u32 bw_share)
{
	u32 sched_ctx[MLX5_ST_SZ_DW(scheduling_context)] = {0};
	struct mlx5_core_dev *dev = esw->dev;
	struct mlx5_vgroup *group = vport->qos.group;
	void *vport_elem;
	u32 parent_tsar_ix;
	int err = 0;

	parent_tsar_ix = group ? group->tsar_ix : esw->qos.root_tsar_ix;
	MLX5_SET(scheduling_context, sched_ctx, element_type,
		 SCHEDULING_CONTEXT_ELEMENT_TYPE_VPORT);
	vport_elem = MLX5_ADDR_OF(scheduling_context, sched_ctx,
				  element_attributes);
	MLX5_SET(vport_element, vport_elem, vport_number, vport->vport);
	MLX5_SET(scheduling_context, sched_ctx, parent_element_id,
		 parent_tsar_ix);
	MLX5_SET(scheduling_context, sched_ctx, max_average_bw,
		 max_rate);
	MLX5_SET(scheduling_context, sched_ctx, bw_share, bw_share);

	err = mlx5_create_scheduling_element_cmd(dev,
						 SCHEDULING_HIERARCHY_E_SWITCH,
						 sched_ctx,
						 &vport->qos.esw_tsar_ix);
	if (err) {
		esw_warn(esw->dev, "E-Switch create TSAR vport element failed (vport=%d,err=%d)\n",
			 vport->vport, err);
		return err;
	}

	if (group && vport->vport != MLX5_VPORT_PF)
		group->num_vports++;

	return 0;
}

static int esw_vport_enable_qos(struct mlx5_eswitch *esw, struct mlx5_vport *vport)
{
	struct mlx5_core_dev *dev = esw->dev;
	int err = 0;

	if (!esw->qos.enabled || !MLX5_CAP_GEN(dev, qos) ||
	    !MLX5_CAP_QOS(dev, esw_scheduling))
		return 0;

	if (vport->qos.enabled)
		return -EEXIST;

	vport->qos.group = esw->qos.group0;

	err = esw_vport_create_sched_element(esw, vport, vport->info.max_rate,
					     vport->qos.bw_share);
	if (err)
		return err;

	vport->qos.enabled = true;
	return 0;
}

static void esw_vport_disable_qos(struct mlx5_eswitch *esw,
				  struct mlx5_vport *vport)
{
	int err;
	struct mlx5_vgroup *group = vport->qos.group;

	if (!vport->qos.enabled)
		return;

	err = mlx5_destroy_scheduling_element_cmd(esw->dev,
						  SCHEDULING_HIERARCHY_E_SWITCH,
						  vport->qos.esw_tsar_ix);
	if (err)
		esw_warn(esw->dev, "E-Switch destroy TSAR vport element failed (vport=%d,err=%d)\n",
			 vport->vport, err);

	if (group) {
		if (vport->vport != MLX5_VPORT_PF)
			group->num_vports--;
		if (group->group_id && !group->num_vports)
			esw_destroy_vgroup(esw, group);
	}

	vport->qos.enabled = false;
}

static int esw_tsar_qos_config(struct mlx5_core_dev *dev, u32 *sched_ctx,
			       u32 parent_ix, u32 tsar_ix,
			       u32 max_rate, u32 bw_share)
{
	u32 bitmask = 0;

	if (!MLX5_CAP_GEN(dev, qos) || !MLX5_CAP_QOS(dev, esw_scheduling))
		return -EOPNOTSUPP;

	MLX5_SET(scheduling_context, sched_ctx, parent_element_id,
		 parent_ix);
	MLX5_SET(scheduling_context, sched_ctx, max_average_bw,
		 max_rate);
	MLX5_SET(scheduling_context, sched_ctx, bw_share, bw_share);
	bitmask |= MODIFY_SCHEDULING_ELEMENT_IN_MODIFY_BITMASK_MAX_AVERAGE_BW;
	bitmask |= MODIFY_SCHEDULING_ELEMENT_IN_MODIFY_BITMASK_BW_SHARE;

	return  mlx5_modify_scheduling_element_cmd(dev,
						   SCHEDULING_HIERARCHY_E_SWITCH,
						   sched_ctx,
						   tsar_ix,
						   bitmask);
}

static int esw_vgroup_qos_config(struct mlx5_eswitch *esw,
				 struct mlx5_vgroup *group,
				 u32 max_rate, u32 bw_share)
{
	u32 sched_ctx[MLX5_ST_SZ_DW(scheduling_context)] = {0};
	struct mlx5_core_dev *dev = esw->dev;
	int err;

	err = esw_tsar_qos_config(dev, sched_ctx,
				  esw->qos.root_tsar_ix, group->tsar_ix,
				  max_rate, bw_share);
	if (err) {
		esw_warn(esw->dev, "E-Switch modify group TSAR element failed (group=%d,err=%d)\n",
			 group->group_id, err);
		return err;
	}

	return 0;
}

static int esw_vport_qos_config(struct mlx5_eswitch *esw,
				struct mlx5_vport *vport,
				u32 max_rate, u32 bw_share)
{
	u32 sched_ctx[MLX5_ST_SZ_DW(scheduling_context)] = {0};
	struct mlx5_core_dev *dev = esw->dev;
	u32 parent_tsar_ix;
	struct mlx5_vgroup *group = vport->qos.group;
	void *vport_elem;
	int err = 0;

	if (!vport->qos.enabled)
		return -EIO;

	parent_tsar_ix = group ? group->tsar_ix : esw->qos.root_tsar_ix;
	MLX5_SET(scheduling_context, sched_ctx, element_type,
		 SCHEDULING_CONTEXT_ELEMENT_TYPE_VPORT);
	vport_elem = MLX5_ADDR_OF(scheduling_context, sched_ctx,
				  element_attributes);
	MLX5_SET(vport_element, vport_elem, vport_number, vport->vport);

	err = esw_tsar_qos_config(dev, sched_ctx,
				  parent_tsar_ix, vport->qos.esw_tsar_ix,
				  max_rate, bw_share);
	if (err) {
		esw_warn(esw->dev, "E-Switch modify TSAR vport element failed (vport=%d,err=%d)\n",
			 vport->vport, err);
		return err;
	}

	return 0;
}

static void node_guid_gen_from_mac(u64 *node_guid, u8 mac[ETH_ALEN])
{
	((u8 *)node_guid)[7] = mac[0];
	((u8 *)node_guid)[6] = mac[1];
	((u8 *)node_guid)[5] = mac[2];
	((u8 *)node_guid)[4] = 0xff;
	((u8 *)node_guid)[3] = 0xfe;
	((u8 *)node_guid)[2] = mac[3];
	((u8 *)node_guid)[1] = mac[4];
	((u8 *)node_guid)[0] = mac[5];
}

static void esw_apply_vport_conf(struct mlx5_eswitch *esw,
				 struct mlx5_vport *vport)
{
	enum esw_vst_mode vst_mode = esw_get_vst_mode(esw);
	u16 vport_num = vport->vport;

	if (is_esw_manager_vport(esw, vport_num))
		return;

	mlx5_modify_vport_admin_state(esw->dev,
				      MLX5_VPORT_STATE_OP_MOD_ESW_VPORT,
				      vport_num, 1,
				      vport->info.link_state);

	/* Host PF has its own mac/guid. */
	if (vport_num) {
		mlx5_modify_other_nic_vport_mac_address(esw->dev, vport_num,
							vport->info.mac);
		mlx5_modify_other_nic_vport_node_guid(esw->dev, vport_num,
						      vport->info.node_guid);
	}

	if (vst_mode != ESW_VST_MODE_STEERING)
		modify_esw_vport_cvlan(esw->dev, vport_num,
				       vport->info.vlan, vport->info.qos,
				       (vport->info.vlan || vport->info.qos),
				       vst_mode);

	/* Only legacy mode needs ACLs */
	if (esw->mode == MLX5_ESWITCH_LEGACY) {
		esw_vport_ingress_config(esw, vport);
		esw_vport_egress_config(esw, vport);
	}
}

static void esw_vport_create_drop_counters(struct mlx5_vport *vport)
{
	struct mlx5_core_dev *dev = vport->dev;

	if (MLX5_CAP_ESW_INGRESS_ACL(dev, flow_counter)) {
		vport->ingress.drop_counter = mlx5_fc_create(dev, false);
		if (IS_ERR(vport->ingress.drop_counter)) {
			esw_warn(dev,
				 "vport[%d] configure ingress drop rule counter failed\n",
				 vport->vport);
			vport->ingress.drop_counter = NULL;
		}
	}

	if (MLX5_CAP_ESW_EGRESS_ACL(dev, flow_counter)) {
		vport->egress.drop_counter = mlx5_fc_create(dev, false);
		if (IS_ERR(vport->egress.drop_counter)) {
			esw_warn(dev,
				 "vport[%d] configure egress drop rule counter failed\n",
				 vport->vport);
			vport->egress.drop_counter = NULL;
		}
	}
}

static void esw_vport_destroy_drop_counters(struct mlx5_vport *vport)
{
	struct mlx5_core_dev *dev = vport->dev;

	if (vport->ingress.drop_counter)
		mlx5_fc_destroy(dev, vport->ingress.drop_counter);
	if (vport->egress.drop_counter)
		mlx5_fc_destroy(dev, vport->egress.drop_counter);
}

void mlx5_eswitch_enable_vport(struct mlx5_eswitch *esw,
			       struct mlx5_vport *vport,
			       enum mlx5_eswitch_vport_event enabled_events)
{
	u16 vport_num = vport->vport;

	mutex_lock(&esw->state_lock);
	if (vport->enabled)
		goto unlock_out;

	esw_debug(esw->dev, "Enabling VPORT(%d)\n", vport_num);

	bitmap_zero(vport->req_vlan_bitmap, VLAN_N_VID);
	bitmap_zero(vport->acl_vlan_8021q_bitmap, VLAN_N_VID);
	bitmap_zero(vport->info.vlan_trunk_8021q_bitmap, VLAN_N_VID);
	/* Create steering drop counters for ingress and egress ACLs */
	if (!is_esw_manager_vport(esw, vport_num) &&
	    esw->mode == MLX5_ESWITCH_LEGACY)
 		esw_vport_create_drop_counters(vport);

	/* Restore old vport configuration */
	esw_apply_vport_conf(esw, vport);

	/* Attach vport to the eswitch rate limiter */
	if (esw_vport_enable_qos(esw, vport))
		esw_warn(esw->dev, "Failed to attach vport %d to eswitch rate limiter", vport_num);

	/* Sync with current vport context */
	vport->enabled_events = enabled_events;
	vport->enabled = true;

	/* Esw manager is trusted by default. Host PF (vport 0) is trusted as well
	 * in smartNIC as it's a vport group manager.
	 */
	if (is_esw_manager_vport(esw, vport_num) ||
	    (!vport_num && mlx5_core_is_ecpf(esw->dev)))
		vport->info.trusted = true;

	esw_vport_change_handle_locked(vport);

	esw->enabled_vports++;
	esw_debug(esw->dev, "Enabled VPORT(%d)\n", vport_num);
unlock_out:
	mutex_unlock(&esw->state_lock);
}

void mlx5_eswitch_disable_vport(struct mlx5_eswitch *esw,
				struct mlx5_vport *vport)
{
	u16 vport_num = vport->vport;

	mutex_lock(&esw->state_lock);
	if (!vport->enabled)
		goto done;

	esw_debug(esw->dev, "Disabling vport(%d)\n", vport_num);
	/* Mark this vport as disabled to discard new events */
	vport->enabled = false;

	/* Disable events from this vport */
	arm_vport_context_events_cmd(esw->dev, vport->vport, 0);
	/* We don't assume VFs will cleanup after themselves.
	 * Calling vport change handler while vport is disabled will cleanup
	 * the vport resources.
	 */
	esw_vport_change_handle_locked(vport);
	vport->enabled_events = 0;
	esw_vport_disable_qos(esw, vport);
	if (!is_esw_manager_vport(esw, vport_num) &&
 	    esw->mode == MLX5_ESWITCH_LEGACY) {
		mlx5_modify_vport_admin_state(esw->dev,
					      MLX5_VPORT_STATE_OP_MOD_ESW_VPORT,
					      vport_num, 1,
					      MLX5_VPORT_ADMIN_STATE_DOWN);
		esw_vport_disable_egress_acl(esw, vport);
		esw_vport_disable_ingress_acl(esw, vport);
		esw_vport_destroy_drop_counters(vport);
	}
	esw->enabled_vports--;

done:
	mutex_unlock(&esw->state_lock);
}

static int eswitch_vport_event(struct notifier_block *nb,
			       unsigned long type, void *data)
{
	struct mlx5_eswitch *esw = mlx5_nb_cof(nb, struct mlx5_eswitch, nb);
	struct mlx5_eqe *eqe = data;
	struct mlx5_vport *vport;
	u16 vport_num;

	vport_num = be16_to_cpu(eqe->data.vport_change.vport_num);
	vport = mlx5_eswitch_get_vport(esw, vport_num);

	if (!IS_ERR(vport))
		queue_work(esw->work_queue, &vport->vport_change_handler);

	return NOTIFY_OK;
}

/**
 * mlx5_esw_query_functions - Returns raw output about functions state
 * @dev:	Pointer to device to query
 *
 * mlx5_esw_query_functions() allocates and returns functions changed
 * raw output memory pointer from device on success. Otherwise returns ERR_PTR.
 * Caller must free the memory using kvfree() when valid pointer is returned.
 */
const u32 *mlx5_esw_query_functions(struct mlx5_core_dev *dev)
{
	int outlen = MLX5_ST_SZ_BYTES(query_esw_functions_out);
	u32 in[MLX5_ST_SZ_DW(query_esw_functions_in)] = {};
	u16 max_sfs;
	u32 *out;
	int err;

	max_sfs = mlx5_eswitch_max_sfs(dev);
	/* Device interface is array of 64-bits */
	if (max_sfs)
		outlen += DIV_ROUND_UP(max_sfs, BITS_PER_TYPE(__be64)) * sizeof(__be64);

	out = kvzalloc(outlen, GFP_KERNEL);
	if (!out)
		return ERR_PTR(-ENOMEM);

	MLX5_SET(query_esw_functions_in, in, opcode,
		 MLX5_CMD_OP_QUERY_ESW_FUNCTIONS);

	err = mlx5_cmd_exec(dev, in, sizeof(in), out, outlen);
	if (!err)
		return out;

	kvfree(out);
	return ERR_PTR(err);
}

static void mlx5_eswitch_event_handlers_register(struct mlx5_eswitch *esw)
{
	MLX5_NB_INIT(&esw->nb, eswitch_vport_event, NIC_VPORT_CHANGE);
	mlx5_eq_notifier_register(esw->dev, &esw->nb);

	if (esw->mode == MLX5_ESWITCH_OFFLOADS && mlx5_eswitch_is_funcs_handler(esw->dev)) {
		MLX5_NB_INIT(&esw->esw_funcs.nb, mlx5_esw_funcs_changed_handler,
			     ESW_FUNCTIONS_CHANGED);
		mlx5_eq_notifier_register(esw->dev, &esw->esw_funcs.nb);
	}
}

static void mlx5_eswitch_event_handlers_unregister(struct mlx5_eswitch *esw)
{
	if (esw->mode == MLX5_ESWITCH_OFFLOADS && mlx5_eswitch_is_funcs_handler(esw->dev))
		mlx5_eq_notifier_unregister(esw->dev, &esw->esw_funcs.nb);

	mlx5_eq_notifier_unregister(esw->dev, &esw->nb);

	flush_workqueue(esw->work_queue);
}

static void mlx5_eswitch_clear_vf_vports_info(struct mlx5_eswitch *esw)
{
	struct mlx5_vport *vport;
	int i;

	mlx5_esw_for_each_vf_vport(esw, i, vport, esw->esw_funcs.num_vfs) {
		memset(&vport->qos, 0, sizeof(vport->qos));
		memset(&vport->info, 0, sizeof(vport->info));
		vport->info.link_state = MLX5_VPORT_ADMIN_STATE_AUTO;
	}
}

/* Public E-Switch API */
#define ESW_ALLOWED(esw) ((esw) && MLX5_ESWITCH_MANAGER((esw)->dev))

/* mlx5_eswitch_enable_pf_vf_vports() enables vports of PF, ECPF and VFs
 * whichever are present on the eswitch.
 */
void
mlx5_eswitch_enable_pf_vf_vports(struct mlx5_eswitch *esw,
				 enum mlx5_eswitch_vport_event enabled_events)
{
	struct mlx5_vport *vport;
	int i;

	/* Enable PF vport */
	vport = mlx5_eswitch_get_vport(esw, MLX5_VPORT_PF);
	mlx5_eswitch_enable_vport(esw, vport, enabled_events);

	/* Enable ECPF vports */
	if (mlx5_ecpf_vport_exists(esw->dev)) {
		vport = mlx5_eswitch_get_vport(esw, MLX5_VPORT_ECPF);
		mlx5_eswitch_enable_vport(esw, vport, enabled_events);
	}

	/* Enable VF vports */
	mlx5_esw_for_each_vf_vport(esw, i, vport, esw->esw_funcs.num_vfs)
		mlx5_eswitch_enable_vport(esw, vport, enabled_events);
}

/* mlx5_eswitch_disable_pf_vf_vports() disables vports of PF, ECPF and VFs
 * whichever are previously enabled on the eswitch.
 */
void mlx5_eswitch_disable_pf_vf_vports(struct mlx5_eswitch *esw)
{
	struct mlx5_vport *vport;
	int i;

	mlx5_esw_for_all_vports_reverse(esw, i, vport)
		mlx5_eswitch_disable_vport(esw, vport);
}

static void mlx5_eswitch_reload_interfaces(struct mlx5_eswitch *esw)
{
	int up_mode = esw->offloads.uplink_rep_mode;

	mlx5_reload_interfaces(esw->dev,
			       MLX5_INTERFACE_PROTOCOL_ETH,
			       MLX5_INTERFACE_PROTOCOL_IB,
			       up_mode == MLX5_ESW_UPLINK_REP_MODE_NEW_NETDEV,
			       true);
}

static void
mlx5_eswitch_update_num_of_vfs(struct mlx5_eswitch *esw, int num_vfs)
{
	const u32 *out;

	WARN_ON_ONCE(esw->mode != MLX5_ESWITCH_NONE);

	if (!mlx5_core_is_ecpf_esw_manager(esw->dev)) {
		esw->esw_funcs.num_vfs = num_vfs;
		return;
	}

	out = mlx5_esw_query_functions(esw->dev);
	if (IS_ERR(out))
		return;

	esw->esw_funcs.num_vfs = MLX5_GET(query_esw_functions_out, out,
					  host_params_context.host_num_of_vfs);
	kvfree(out);
}

int mlx5_eswitch_enable_locked(struct mlx5_eswitch *esw, int mode,
			       int num_vfs)
{
	int err;

	if (!ESW_ALLOWED(esw) ||
	    !MLX5_CAP_ESW_FLOWTABLE_FDB(esw->dev, ft_support)) {
		esw_warn(esw->dev, "E-Switch FDB is not supported, aborting ...\n");
		return -EOPNOTSUPP;
	}

	lockdep_assert_held(&esw->mode_lock);

	if (num_vfs >= 0)
		mlx5_eswitch_update_num_of_vfs(esw, num_vfs);

	if (!MLX5_CAP_ESW_INGRESS_ACL(esw->dev, ft_support))
		esw_warn(esw->dev, "E-Switch ingress ACL is not supported by FW\n");

	if (!MLX5_CAP_ESW_EGRESS_ACL(esw->dev, ft_support))
		esw_warn(esw->dev, "E-Switch engress ACL is not supported by FW\n");

	esw_create_tsar(esw);

	esw->mode = mode;

	if (mode == MLX5_ESWITCH_LEGACY) {
		err = esw_legacy_enable(esw);
	} else {
		mlx5_eswitch_reload_interfaces(esw);
		err = esw_offloads_enable(esw);
	}

	if (err)
		goto abort;

	mlx5_eswitch_event_handlers_register(esw);

	esw_info(esw->dev, "Enable: mode(%s), nvfs(%d), active vports(%d)\n",
		 mode == MLX5_ESWITCH_LEGACY ? "LEGACY" : "OFFLOADS",
		 esw->esw_funcs.num_vfs, esw->enabled_vports);

	return 0;

abort:
	esw_destroy_tsar(esw);
	esw->mode = MLX5_ESWITCH_NONE;

	if (mode == MLX5_ESWITCH_OFFLOADS)
		mlx5_eswitch_reload_interfaces(esw);

	return err;
}

/**
 * mlx5_eswitch_enable - Enable eswitch
 * @esw:	Pointer to eswitch
 * @mode:	Eswitch mode to enable
 * @num_vfs:	Enable eswitch swich for given number of VFs. This is optional.
 * 		Valid value are 0, > 0 and -1. Caller should pass num_vfs > 0
 * 		when enabling eswitch for vf vports. Caller should pass num_vfs
 * 		= 0, when eswitch is enabled without sriov VFs or when caller
 * 		is unaware of the sriov state of the host PF on ECPF based
 * 		eswitch. Caller should pass < 0 when num_vfs should be
 * 		completely ignored. This is typically the case when eswitch
 * 		is enabled without sriov regardless of PF/ECPF system.
 * mlx5_eswitch_enable() Enables eswitch in either legacy or offloads mode.
 * If num_vfs >=0 is provided, it setup VF related eswitch vports. It retrns
 * 0 on success or error code on failure.
 */
int mlx5_eswitch_enable(struct mlx5_eswitch *esw, int mode, int num_vfs)
{
	int ret;

	if (!ESW_ALLOWED(esw))
		return 0;

	mutex_lock(&esw->mode_lock);
	ret = mlx5_eswitch_enable_locked(esw, mode, num_vfs);
	mutex_unlock(&esw->mode_lock);
	return ret;
}

void mlx5_eswitch_disable_locked(struct mlx5_eswitch *esw, bool clear_vf)
{
	int old_mode;

	lockdep_assert_held(&esw->mode_lock);

	if (esw->mode == MLX5_ESWITCH_NONE)
		return;

	esw_info(esw->dev, "Disable: mode(%s), nvfs(%d), active vports(%d)\n",
		 esw->mode == MLX5_ESWITCH_LEGACY ? "LEGACY" : "OFFLOADS",
		 esw->esw_funcs.num_vfs, esw->enabled_vports);

	mlx5_eswitch_event_handlers_unregister(esw);

	if (esw->mode == MLX5_ESWITCH_LEGACY)
		esw_legacy_disable(esw);
	else if (esw->mode == MLX5_ESWITCH_OFFLOADS)
		esw_offloads_disable(esw);

	esw_destroy_tsar(esw);

	old_mode = esw->mode;
	esw->mode = MLX5_ESWITCH_NONE;

	if (old_mode == MLX5_ESWITCH_OFFLOADS)
		mlx5_eswitch_reload_interfaces(esw);
	if (clear_vf)
		mlx5_eswitch_clear_vf_vports_info(esw);
}

void mlx5_eswitch_disable(struct mlx5_eswitch *esw, bool clear_vf)
{
	if (!ESW_ALLOWED(esw))
		return;

	mutex_lock(&esw->mode_lock);
	mlx5_eswitch_disable_locked(esw, clear_vf);
	mutex_unlock(&esw->mode_lock);
}

int mlx5_eswitch_init(struct mlx5_core_dev *dev)
{
	struct mlx5_eswitch *esw;
	bool access_other_hca_roce;
	int total_vports;
	struct mlx5_vport *vport;
	int err, i = 0, j;

	if (!MLX5_VPORT_MANAGER(dev))
		return 0;

	total_vports = mlx5_eswitch_get_total_vports(dev);

	esw_info(dev,
		 "Total vports %d, per vport: max uc(%d) max mc(%d)\n",
		 total_vports,
		 MLX5_MAX_UC_PER_VPORT(dev),
		 MLX5_MAX_MC_PER_VPORT(dev));

	esw = kzalloc(sizeof(*esw), GFP_KERNEL);
	if (!esw)
		return -ENOMEM;

	esw->dev = dev;
	esw->manager_vport = mlx5_eswitch_manager_vport(dev);
	esw->first_host_vport = mlx5_eswitch_first_host_vport_num(dev);

	esw->work_queue = create_singlethread_workqueue("mlx5_esw_wq");
	if (!esw->work_queue) {
		err = -ENOMEM;
		goto abort;
	}

	esw->vports = kcalloc(total_vports, sizeof(struct mlx5_vport),
			      GFP_KERNEL);
	if (!esw->vports) {
		err = -ENOMEM;
		goto abort;
	}

	esw->total_vports = total_vports;

	err = esw_offloads_init_reps(esw);
	if (err)
		goto abort;

#ifdef HAVE_TCF_TUNNEL_INFO
	mutex_init(&esw->offloads.encap_tbl_lock);
	hash_init(esw->offloads.encap_tbl);
#endif
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	mutex_init(&esw->offloads.mod_hdr.lock);
	hash_init(esw->offloads.mod_hdr.hlist);
#endif
	atomic64_set(&esw->offloads.num_flows, 0);
	mutex_init(&esw->state_lock);
	mutex_init(&esw->mode_lock);

	access_other_hca_roce = MLX5_CAP_GEN(dev, vhca_group_manager) &&
				MLX5_CAP_GEN(dev, access_other_hca_roce);

	mlx5_esw_for_all_vports(esw, i, vport) {
		vport->vport = mlx5_eswitch_index_to_vport_num(esw, i);
		vport->info.link_state = MLX5_VPORT_ADMIN_STATE_AUTO;
		vport->info.vlan_proto = htons(ETH_P_8021Q);
		vport->info.roce = true;
		vport->match_id = esw_get_unique_match_id();
#ifdef HAVE_IDA_SIMPLE_GET
		if (vport->match_id < 0) {
#else
		if (vport->match_id >= VHCA_VPORT_MATCH_ID_SIZE) {
#endif
			err = -ENOSPC;
			goto abort;
		}
		if (access_other_hca_roce &&
		    vport->vport != MLX5_VPORT_UPLINK &&
		    !mlx5_eswitch_is_sf_vport(esw, vport->vport))
			mlx5_get_other_hca_cap_roce(dev, vport->vport,
					&vport->info.roce);
		vport->dev = dev;
		INIT_WORK(&vport->vport_change_handler,
			  esw_vport_change_handler);
		INIT_LIST_HEAD(&vport->egress.allow_vlans_rules);
		INIT_LIST_HEAD(&vport->ingress.allow_vlans_rules);
	}

	esw->enabled_vports = 0;
	esw->mode = MLX5_ESWITCH_NONE;
	esw->offloads.inline_mode = MLX5_INLINE_MODE_NONE;
	esw->offloads.uplink_rep_mode = MLX5_ESW_UPLINK_REP_MODE_NEW_NETDEV;
	if (MLX5_CAP_ESW_FLOWTABLE_FDB(dev, reformat) &&
	    MLX5_CAP_ESW_FLOWTABLE_FDB(dev, decap))
		esw->offloads.encap = DEVLINK_ESWITCH_ENCAP_MODE_BASIC;
	else
		esw->offloads.encap = DEVLINK_ESWITCH_ENCAP_MODE_NONE;

	dev->priv.eswitch = esw;
	INIT_WORK(&esw->handler.start_handler, esw_offloads_start_handler);
	INIT_WORK(&esw->handler.stop_handler, esw_offloads_stop_handler);
	return 0;
abort:
	if (esw->work_queue)
		destroy_workqueue(esw->work_queue);
	esw_offloads_cleanup_reps(esw);
	mutex_lock(&mlx5e_vport_match_ida_mutex);
	mlx5_esw_for_all_vports(esw, j, vport) {
		if (j == i)
			break;
#ifdef HAVE_IDA_SIMPLE_GET
		ida_simple_remove(&mlx5e_vport_match_ida,
				  esw->vports[j].match_id);
#else
		bitmap_clear(mlx5e_vport_match_map, esw->vports[j].match_id, 1);
#endif
	}
	mutex_unlock(&mlx5e_vport_match_ida_mutex);
	kfree(esw->vports);
	kfree(esw);
	return err;
}

int mlx5_eswitch_vport_modify_other_hca_cap_roce(struct mlx5_eswitch *esw,
						 struct mlx5_vport *vport, bool value)
{
	int err = 0;

	if (!(MLX5_CAP_GEN(esw->dev, vhca_group_manager) &&
	      MLX5_CAP_GEN(esw->dev, access_other_hca_roce)))
		return -EOPNOTSUPP;

	mutex_lock(&esw->state_lock);

	if (vport->info.roce == value)
		goto out;

	err = mlx5_modify_other_hca_cap_roce(esw->dev, vport->vport, value);
	if (!err)
		vport->info.roce = value;

out:
	mutex_unlock(&esw->state_lock);
	return err;
}

int mlx5_eswitch_vport_get_other_hca_cap_roce(struct mlx5_eswitch *esw,
					      struct mlx5_vport *vport, bool *value)
{
	if (!(MLX5_CAP_GEN(esw->dev, vhca_group_manager) &&
	      MLX5_CAP_GEN(esw->dev, access_other_hca_roce)))
		return -EOPNOTSUPP;

	mutex_lock(&esw->state_lock);
	*value = vport->info.roce;
	mutex_unlock(&esw->state_lock);

	return 0;
}

void mlx5_eswitch_cleanup(struct mlx5_eswitch *esw)
{
	struct mlx5_vport *vport;
	int i;

	if (!esw || !MLX5_VPORT_MANAGER(esw->dev))
		return;

	esw_info(esw->dev, "cleanup\n");

	esw->dev->priv.eswitch = NULL;
	mutex_lock(&mlx5e_vport_match_ida_mutex);
	mlx5_esw_for_all_vports(esw, i, vport)
#ifdef HAVE_IDA_SIMPLE_GET
		ida_simple_remove(&mlx5e_vport_match_ida, vport->match_id);
#else
		bitmap_clear(mlx5e_vport_match_map, vport->match_id, 1);
#endif
	mutex_unlock(&mlx5e_vport_match_ida_mutex);
	flush_work(&esw->handler.start_handler);
	flush_work(&esw->handler.stop_handler);
	destroy_workqueue(esw->work_queue);
	esw_offloads_cleanup_reps(esw);
	mutex_destroy(&esw->mode_lock);
	mutex_destroy(&esw->state_lock);
#ifdef HAVE_TCF_PEDIT_TCFP_KEYS_EX
	mutex_destroy(&esw->offloads.mod_hdr.lock);
	mutex_destroy(&esw->offloads.encap_tbl_lock);
#endif
	kfree(esw->vports);
	kfree(esw);
}

/* Vport Administration */
int mlx5_eswitch_set_vport_mac(struct mlx5_eswitch *esw,
			       u16 vport, u8 mac[ETH_ALEN])
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	u64 node_guid;
	int err = 0;

	if (IS_ERR(evport))
		return PTR_ERR(evport);
	if (is_multicast_ether_addr(mac))
		return -EINVAL;

	mutex_lock(&esw->state_lock);

	if (evport->info.spoofchk && !is_valid_ether_addr(mac))
		mlx5_core_warn(esw->dev,
			       "Set invalid MAC while spoofchk is on, vport(%d)\n",
			       vport);

	err = mlx5_modify_other_nic_vport_mac_address(esw->dev, vport, mac);
	if (err) {
		mlx5_core_warn(esw->dev,
			       "Failed to mlx5_modify_nic_vport_mac vport(%d) err=(%d)\n",
			       vport, err);
		goto unlock;
	}

	node_guid_gen_from_mac(&node_guid, mac);
	err = mlx5_modify_other_nic_vport_node_guid(esw->dev, vport, node_guid);
	if (err)
		mlx5_core_warn(esw->dev,
			       "Failed to set vport %d node guid, err = %d. RDMA_CM will not function properly for this VF.\n",
			       vport, err);

	ether_addr_copy(evport->info.mac, mac);
	evport->info.node_guid = node_guid;
	if (evport->enabled && esw->mode == MLX5_ESWITCH_LEGACY)
		err = esw_vport_ingress_config(esw, evport);

unlock:
	mutex_unlock(&esw->state_lock);
	return err;
}

int mlx5_eswitch_get_vport_mac(struct mlx5_eswitch *esw,
			       u16 vport, u8 *mac)
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);

	if (IS_ERR(evport))
		return PTR_ERR(evport);

	mutex_lock(&esw->state_lock);
	ether_addr_copy(mac, evport->info.mac);
	mutex_unlock(&esw->state_lock);
	return 0;
}

static int mlx5_eswitch_update_vport_trunk(struct mlx5_eswitch *esw,
					   struct mlx5_vport *evport,
					   unsigned long *old_trunk) {
	DECLARE_BITMAP(diff_vlan_bm, VLAN_N_VID);
	int err = 0;

	bitmap_xor(diff_vlan_bm, old_trunk,
		   evport->info.vlan_trunk_8021q_bitmap, VLAN_N_VID);
	if (!bitmap_weight(diff_vlan_bm, VLAN_N_VID))
		return err;

	esw_update_acl_trunk_bitmap(esw, evport->vport);
	if (evport->enabled && esw->mode == MLX5_ESWITCH_LEGACY) {
		err = esw_vport_egress_config(esw, evport);
		if (!err)
			err = esw_vport_ingress_config(esw, evport);
	}
	if (err) {
		bitmap_copy(evport->info.vlan_trunk_8021q_bitmap, old_trunk, VLAN_N_VID);
		esw_update_acl_trunk_bitmap(esw, evport->vport);
		esw_vport_egress_config(esw, evport);
		esw_vport_ingress_config(esw, evport);
	}

	return err;
}

int mlx5_eswitch_add_vport_trunk_range(struct mlx5_eswitch *esw,
				       int vport, u16 start_vlan, u16 end_vlan)
{
	DECLARE_BITMAP(prev_vport_bitmap, VLAN_N_VID);
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	int err = 0;

	if (!ESW_ALLOWED(esw))
		return -EPERM;
	if (IS_ERR(evport))
		return PTR_ERR(evport);
	
	if (end_vlan > VLAN_N_VID || start_vlan > end_vlan)
		return -EINVAL;

	mutex_lock(&esw->state_lock);
	evport = &esw->vports[vport];

	if (evport->info.vlan || evport->info.qos) {
		err = -EPERM;
		mlx5_core_warn(esw->dev,
			       "VGT+ is not allowed when operating in VST mode vport(%d)\n",
			       vport);
		goto unlock;
	}

	bitmap_copy(prev_vport_bitmap, evport->info.vlan_trunk_8021q_bitmap, VLAN_N_VID);
	bitmap_set(evport->info.vlan_trunk_8021q_bitmap, start_vlan,
		   end_vlan - start_vlan + 1);
	err = mlx5_eswitch_update_vport_trunk(esw, evport, prev_vport_bitmap);

unlock:
	mutex_unlock(&esw->state_lock);

	return err;
}

int mlx5_eswitch_del_vport_trunk_range(struct mlx5_eswitch *esw,
				       int vport, u16 start_vlan, u16 end_vlan)
{
	DECLARE_BITMAP(prev_vport_bitmap, VLAN_N_VID);
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	int err = 0;

	if (!ESW_ALLOWED(esw))
		return -EPERM;
	if (IS_ERR(evport))
		return PTR_ERR(evport);

	if (end_vlan > VLAN_N_VID || start_vlan > end_vlan)
		return -EINVAL;

	mutex_lock(&esw->state_lock);
	evport = &esw->vports[vport];
	bitmap_copy(prev_vport_bitmap, evport->info.vlan_trunk_8021q_bitmap, VLAN_N_VID);
	bitmap_clear(evport->info.vlan_trunk_8021q_bitmap, start_vlan,
		     end_vlan - start_vlan + 1);
	err = mlx5_eswitch_update_vport_trunk(esw, evport, prev_vport_bitmap);
	mutex_unlock(&esw->state_lock);

	return err;
}


int mlx5_eswitch_set_vport_state(struct mlx5_eswitch *esw,
				 u16 vport, int link_state)
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	int err = 0;

	if (!ESW_ALLOWED(esw))
		return -EPERM;
	if (IS_ERR(evport))
		return PTR_ERR(evport);

	mutex_lock(&esw->state_lock);
	if (esw->mode != MLX5_ESWITCH_LEGACY) {
		err = -EOPNOTSUPP;
		goto unlock;
	}

	if (!evport->enabled && mlx5_eswitch_is_sf_vport(esw, vport))
		goto unlock;

	err = mlx5_modify_vport_admin_state(esw->dev,
					    MLX5_VPORT_STATE_OP_MOD_ESW_VPORT,
					    vport, 1, link_state);
	if (err) {
		mlx5_core_warn(esw->dev,
			       "Failed to set vport %d link state, err = %d",
			       vport, err);
		goto unlock;
	}

	evport->info.link_state = link_state;

unlock:
	mutex_unlock(&esw->state_lock);
	return err;
}

#ifdef HAVE_IFLA_VF_INFO
int mlx5_eswitch_get_vport_config(struct mlx5_eswitch *esw,
				  u16 vport, struct ifla_vf_info *ivi)
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);

	if (IS_ERR(evport))
		return PTR_ERR(evport);

	memset(ivi, 0, sizeof(*ivi));
	ivi->vf = vport - 1;

	mutex_lock(&esw->state_lock);
	ether_addr_copy(ivi->mac, evport->info.mac);
#ifdef HAVE_LINKSTATE
       ivi->linkstate = evport->info.link_state;
#endif
       ivi->vlan = evport->info.vlan;
       ivi->qos = evport->info.qos;
#ifdef HAVE_VF_VLAN_PROTO
       ivi->vlan_proto = evport->info.vlan_proto;
#endif
#ifdef HAVE_VF_INFO_SPOOFCHK
       ivi->spoofchk = evport->info.spoofchk;
#endif
#ifdef HAVE_VF_INFO_TRUST
       ivi->trusted = evport->info.trusted;
#endif
#ifdef HAVE_VF_TX_RATE_LIMITS
       ivi->min_tx_rate = evport->info.min_rate;
       ivi->max_tx_rate = evport->info.max_rate;
#else
	ivi->tx_rate = evport->info.max_rate;
#endif
       mutex_unlock(&esw->state_lock);

       return 0;
}
#endif

int __mlx5_eswitch_set_vport_vlan(struct mlx5_eswitch *esw, int vport, u16 vlan,
				  u8 qos, __be16 proto, u8 set_flags)
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	enum esw_vst_mode vst_mode;
	int err = 0;

	if (!ESW_ALLOWED(esw))
		return -EPERM;
	if (IS_ERR(evport))
		return PTR_ERR(evport);
	if (vlan > 4095 || qos > 7)
		return -EINVAL;
	if (proto != htons(ETH_P_8021Q) && proto != htons(ETH_P_8021AD))
		return -EINVAL;

	vst_mode = esw_get_vst_mode(esw);
	if (proto == htons(ETH_P_8021AD) && (vst_mode != ESW_VST_MODE_STEERING))
		return -EPROTONOSUPPORT;


	if (bitmap_weight(evport->info.vlan_trunk_8021q_bitmap, VLAN_N_VID)) {
		err = -EPERM;
		mlx5_core_warn(esw->dev,
			       "VST is not allowed when operating in VGT+ mode vport(%d)\n",
			       vport);
		return err;
	}

	if (vst_mode != ESW_VST_MODE_STEERING) {
		err = modify_esw_vport_cvlan(esw->dev, vport, vlan, qos,
					     set_flags, vst_mode);
		if (err)
			return err;
	}
	evport->info.vlan = vlan;
	evport->info.qos = qos;
	evport->info.vlan_proto = proto;
	if (evport->enabled && esw->mode == MLX5_ESWITCH_LEGACY) {
		err = esw_vport_ingress_config(esw, evport);
		if (err)
			return err;
		err = esw_vport_egress_config(esw, evport);
	}

	return err;
}

int mlx5_eswitch_set_vport_vlan(struct mlx5_eswitch *esw, int vport,
				u16 vlan, u8 qos, __be16 vlan_proto)
{
	u8 set_flags = 0;
	int err;

	if (vlan || qos)
		set_flags = SET_VLAN_STRIP | SET_VLAN_INSERT;

	mutex_lock(&esw->state_lock);
	err =  __mlx5_eswitch_set_vport_vlan(esw, vport, vlan, qos,
					     vlan_proto, set_flags);
	mutex_unlock(&esw->state_lock);

	return err;
}

int mlx5_eswitch_set_vport_spoofchk(struct mlx5_eswitch *esw,
				    u16 vport, bool spoofchk)
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	bool pschk;
	int err = 0;

	if (!ESW_ALLOWED(esw))
		return -EPERM;
	if (IS_ERR(evport))
		return PTR_ERR(evport);

	mutex_lock(&esw->state_lock);
	if (esw->mode != MLX5_ESWITCH_LEGACY) {
		err = -EOPNOTSUPP;
		goto out;
	}
	pschk = evport->info.spoofchk;
	evport->info.spoofchk = spoofchk;
	if (pschk && !is_valid_ether_addr(evport->info.mac))
		mlx5_core_warn(esw->dev,
			       "Spoofchk in set while MAC is invalid, vport(%d)\n",
			       evport->vport);
	if (evport->enabled && esw->mode == MLX5_ESWITCH_LEGACY)
		err = esw_vport_ingress_config(esw, evport);
	if (err)
		evport->info.spoofchk = pschk;

out:
	mutex_unlock(&esw->state_lock);
	return err;
}

static void esw_cleanup_vepa_rules(struct mlx5_eswitch *esw)
{
	if (esw->fdb_table.legacy.vepa_uplink_rule)
		mlx5_del_flow_rules(esw->fdb_table.legacy.vepa_uplink_rule);

	if (esw->fdb_table.legacy.vepa_star_rule)
		mlx5_del_flow_rules(esw->fdb_table.legacy.vepa_star_rule);

	esw->fdb_table.legacy.vepa_uplink_rule = NULL;
	esw->fdb_table.legacy.vepa_star_rule = NULL;
}

static int _mlx5_eswitch_set_vepa_locked(struct mlx5_eswitch *esw,
					 u8 setting)
{
	struct mlx5_flow_destination dest = {};
	struct mlx5_flow_act flow_act = {};
	struct mlx5_flow_handle *flow_rule;
	struct mlx5_flow_spec *spec;
	int err = 0;
	void *misc;

	if (!setting) {
		esw_cleanup_vepa_rules(esw);
		return 0;
	}

	if (esw->fdb_table.legacy.vepa_uplink_rule)
		return 0;

	spec = kvzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return -ENOMEM;

	/* Uplink rule forward uplink traffic to FDB */
	misc = MLX5_ADDR_OF(fte_match_param, spec->match_value, misc_parameters);
	MLX5_SET(fte_match_set_misc, misc, source_port, MLX5_VPORT_UPLINK);

	misc = MLX5_ADDR_OF(fte_match_param, spec->match_criteria, misc_parameters);
	MLX5_SET_TO_ONES(fte_match_set_misc, misc, source_port);

	spec->match_criteria_enable = MLX5_MATCH_MISC_PARAMETERS;
	dest.type = MLX5_FLOW_DESTINATION_TYPE_FLOW_TABLE;
	dest.ft = esw->fdb_table.legacy.fdb;
	flow_act.action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	flow_rule = mlx5_add_flow_rules(esw->fdb_table.legacy.vepa_fdb, spec,
					&flow_act, &dest, 1);
	if (IS_ERR(flow_rule)) {
		err = PTR_ERR(flow_rule);
		goto out;
	} else {
		esw->fdb_table.legacy.vepa_uplink_rule = flow_rule;
	}

	/* Star rule to forward all traffic to uplink vport */
	memset(spec, 0, sizeof(*spec));
	memset(&dest, 0, sizeof(dest));
	dest.type = MLX5_FLOW_DESTINATION_TYPE_VPORT;
	dest.vport.num = MLX5_VPORT_UPLINK;
	flow_act.action = MLX5_FLOW_CONTEXT_ACTION_FWD_DEST;
	flow_rule = mlx5_add_flow_rules(esw->fdb_table.legacy.vepa_fdb, spec,
					&flow_act, &dest, 1);
	if (IS_ERR(flow_rule)) {
		err = PTR_ERR(flow_rule);
		goto out;
	} else {
		esw->fdb_table.legacy.vepa_star_rule = flow_rule;
	}

out:
	kvfree(spec);
	if (err)
		esw_cleanup_vepa_rules(esw);
	return err;
}

int mlx5_eswitch_set_vepa(struct mlx5_eswitch *esw, u8 setting)
{
	int err = 0;

	if (!esw)
		return -EOPNOTSUPP;

	if (!ESW_ALLOWED(esw))
		return -EPERM;

	mutex_lock(&esw->state_lock);
	if (esw->mode != MLX5_ESWITCH_LEGACY) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = _mlx5_eswitch_set_vepa_locked(esw, setting);

out:
	mutex_unlock(&esw->state_lock);
	return err;
}

int mlx5_eswitch_get_vepa(struct mlx5_eswitch *esw, u8 *setting)
{
	int err = 0;

	if (!esw)
		return -EOPNOTSUPP;

	if (!ESW_ALLOWED(esw))
		return -EPERM;

	mutex_lock(&esw->state_lock);
	if (esw->mode != MLX5_ESWITCH_LEGACY) {
		err = -EOPNOTSUPP;
		goto out;
	}

	*setting = esw->fdb_table.legacy.vepa_uplink_rule ? 1 : 0;

out:
	mutex_unlock(&esw->state_lock);
	return err;
}

int mlx5_eswitch_set_vport_trust(struct mlx5_eswitch *esw,
				 u16 vport, bool setting)
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	int err = 0;

	if (!ESW_ALLOWED(esw))
		return -EPERM;
	if (IS_ERR(evport))
		return PTR_ERR(evport);

	mutex_lock(&esw->state_lock);
	if (esw->mode != MLX5_ESWITCH_LEGACY) {
		err = -EOPNOTSUPP;
		goto out;
	}
	evport->info.trusted = setting;
	if (evport->enabled)
		esw_vport_change_handle_locked(evport);

out:
	mutex_unlock(&esw->state_lock);
	return err;
}

static u32 calculate_min_rate_divider(struct mlx5_eswitch *esw,
				      u32 group, bool group_level)
{
	u32 fw_max_bw_share = MLX5_CAP_QOS(esw->dev, max_tsar_bw_share);
	struct mlx5_vgroup *vgroup;
	struct mlx5_vport *evport;
	u32 max_guarantee = 0;
	int i;

	if (group_level) {
		list_for_each_entry(vgroup, &esw->qos.groups, list) {
			if (vgroup->min_rate < max_guarantee)
				continue;
			max_guarantee = vgroup->min_rate;
		}
	} else {
		mlx5_esw_for_all_vports(esw, i, evport) {
			if (evport->info.group != group || !evport->enabled ||
			    evport->info.min_rate < max_guarantee)
				continue;
			max_guarantee = evport->info.min_rate;
		}
	}
	if (max_guarantee)
		return max_t(u32, max_guarantee / fw_max_bw_share, 1);
	return 0;
}

static u32 calc_bw_share(u32 min_rate, u32 divider, u32 fw_max)
{
	if (divider)
		return MLX5_RATE_TO_BW_SHARE(min_rate, divider, fw_max);

	return 0;
}

static int normalize_vports_min_rate(struct mlx5_eswitch *esw,
				     u32 group)
{
	u32 fw_max_bw_share = MLX5_CAP_QOS(esw->dev, max_tsar_bw_share);
	u32 divider = calculate_min_rate_divider(esw, group, false);
	struct mlx5_vport *evport;
	u32 bw_share;
	int err;
	int i;

	mlx5_esw_for_all_vports(esw, i, evport) {
		if (!evport->enabled || evport->info.group != group)
			continue;
		bw_share = calc_bw_share(evport->info.min_rate, divider, fw_max_bw_share);

		if (bw_share == evport->qos.bw_share)
			continue;

		err = esw_vport_qos_config(esw, evport, evport->info.max_rate,
					   bw_share);
		if (err)
			return err;

		evport->qos.bw_share = bw_share;
	}

	return 0;
}

static int normalize_vgroups_min_rate(struct mlx5_eswitch *esw, u32 divider)
{
	u32 fw_max_bw_share = MLX5_CAP_QOS(esw->dev, max_tsar_bw_share);
	struct mlx5_vgroup *vgroup;
	u32 bw_share;
	int err;

	list_for_each_entry(vgroup, &esw->qos.groups, list) {
		bw_share = calc_bw_share(vgroup->min_rate, divider, fw_max_bw_share);

		if (bw_share == vgroup->bw_share)
			continue;

		err = esw_vgroup_qos_config(esw, vgroup, vgroup->max_rate,
					    bw_share);
		if (err)
			return err;

		vgroup->bw_share = bw_share;

		/* All the group's vports need to be set with default bw_share
		 * to enable them with QOS.
		 */
		err = normalize_vports_min_rate(esw, vgroup->group_id);

		if (err)
			return err;
	}

	return 0;
}

int mlx5_eswitch_set_vport_rate(struct mlx5_eswitch *esw, u16 vport,
				u32 max_rate, u32 min_rate)
{
	struct mlx5_vport *evport = mlx5_eswitch_get_vport(esw, vport);
	u32 fw_max_bw_share;
	u32 previous_min_rate;
	bool min_rate_supported;
	bool max_rate_supported;
	int err = 0;
	u32 act_max_rate = max_rate;

	if (!ESW_ALLOWED(esw))
		return -EPERM;
	if (IS_ERR(evport))
		return PTR_ERR(evport);

	fw_max_bw_share = MLX5_CAP_QOS(esw->dev, max_tsar_bw_share);
	min_rate_supported = MLX5_CAP_QOS(esw->dev, esw_bw_share) &&
				fw_max_bw_share >= MLX5_MIN_BW_SHARE;
	max_rate_supported = MLX5_CAP_QOS(esw->dev, esw_rate_limit);

	if ((min_rate && !min_rate_supported) || (max_rate && !max_rate_supported))
		return -EOPNOTSUPP;

	mutex_lock(&esw->state_lock);

	if (min_rate == evport->info.min_rate)
		goto set_max_rate;

	previous_min_rate = evport->info.min_rate;
	evport->info.min_rate = min_rate;
	err = normalize_vports_min_rate(esw, evport->info.group);
	if (err) {
		evport->info.min_rate = previous_min_rate;
		goto unlock;
	}

set_max_rate:
	if (max_rate == evport->info.max_rate)
		goto unlock;

	/* If parent group has rate limit need to set to group
	 * value when new max_rate is 0.
	 */
	if (evport->qos.group && !max_rate)
		act_max_rate = (evport->qos.group)->max_rate;

	err = esw_vport_qos_config(esw, evport, act_max_rate, evport->qos.bw_share);
	if (!err)
		evport->info.max_rate = max_rate;

unlock:
	mutex_unlock(&esw->state_lock);
	return err;
}

int mlx5_eswitch_query_vport_drop_stats(struct mlx5_core_dev *dev,
					       struct mlx5_vport *vport,
					       struct mlx5_vport_drop_stats *stats)
{
	struct mlx5_eswitch *esw = dev->priv.eswitch;
	u64 rx_discard_vport_down, tx_discard_vport_down;
	u64 bytes = 0;
	int err = 0;

	if (!vport->enabled || esw->mode != MLX5_ESWITCH_LEGACY)
		return 0;

	if (vport->egress.drop_counter)
		mlx5_fc_query(dev, vport->egress.drop_counter,
			      &stats->rx_dropped, &bytes);

	if (vport->ingress.drop_counter)
		mlx5_fc_query(dev, vport->ingress.drop_counter,
			      &stats->tx_dropped, &bytes);

	if (!MLX5_CAP_GEN(dev, receive_discard_vport_down) &&
	    !MLX5_CAP_GEN(dev, transmit_discard_vport_down))
		return 0;

	err = mlx5_query_vport_down_stats(dev, vport->vport, 1,
					  &rx_discard_vport_down,
					  &tx_discard_vport_down);
	if (err)
		return err;

	if (MLX5_CAP_GEN(dev, receive_discard_vport_down))
		stats->rx_dropped += rx_discard_vport_down;
	if (MLX5_CAP_GEN(dev, transmit_discard_vport_down))
		stats->tx_dropped += tx_discard_vport_down;

	return 0;
}

int mlx5_eswitch_vport_update_group(struct mlx5_eswitch *esw, int vport_num,
				    u32 group_id)
{
	struct mlx5_vport *vport = mlx5_eswitch_get_vport(esw, vport_num);
	struct mlx5_vgroup *curr_group = vport->qos.group;
	struct mlx5_vgroup *new_group = NULL, *tmp;
	struct mlx5_core_dev *dev = esw->dev;
	int err;

	if (!MLX5_CAP_QOS(dev, log_esw_max_sched_depth))
		return -EOPNOTSUPP;

	if (!esw->qos.enabled || !MLX5_CAP_GEN(dev, qos) ||
	    !MLX5_CAP_QOS(dev, esw_scheduling))
		return 0;

	if (curr_group->group_id == group_id)
		return 0;

	mutex_lock(&esw->state_lock);
	list_for_each_entry(tmp, &esw->qos.groups, list) {
		if (tmp->group_id == group_id) {
			new_group = tmp;
			break;
		}
	}

	if (!new_group)
		new_group = esw_create_vgroup(esw, group_id);

	if (IS_ERR(new_group)) {
		err = PTR_ERR(new_group);
		mutex_unlock(&esw->state_lock);
		esw_warn(esw->dev, "E-Switch couldn't create new vgroup %d (%d)\n",
			 group_id, err);
		return err;
	}

	err = mlx5_destroy_scheduling_element_cmd(dev,
						  SCHEDULING_HIERARCHY_E_SWITCH,
						  vport->qos.esw_tsar_ix);
	if (err) {
		mutex_unlock(&esw->state_lock);
		esw_warn(dev, "E-Switch destroy TSAR vport element failed (vport=%d,err=%d)\n",
			 vport_num, err);
		return err;
	}

	vport->qos.group = new_group;

	/* If vport is unlimited, we set the group's value.
	 * Therefore, if the group is limited it will apply to
	 * the vport as well and if not, vport will remain unlimited.
	 */
	err = esw_vport_create_sched_element(esw, vport,
					     vport->info.max_rate ?
					     vport->info.max_rate :
					     new_group->max_rate,
					     vport->qos.bw_share);
	if (err) {
		vport->qos.group = curr_group;
		if (esw_vport_create_sched_element(esw, vport,
						   vport->info.max_rate ?
						   vport->info.max_rate :
						   curr_group->max_rate,
						   vport->qos.bw_share))
			esw_warn(dev, "E-Switch vport group set failed. Can't restore prev configuration (vport=%d)\n",
				 vport_num);

		if (group_id && !new_group->num_vports)
			esw_destroy_vgroup(esw, new_group);
		mutex_unlock(&esw->state_lock);

		return err;
	}

	vport->info.group = group_id;
	curr_group->num_vports--;

	/* Recalculate bw share weights of old and new groups */
	if (vport->qos.bw_share) {
		normalize_vports_min_rate(esw, curr_group->group_id);

		normalize_vports_min_rate(esw, new_group->group_id);
	}

	if (curr_group->group_id && !curr_group->num_vports)
		esw_destroy_vgroup(esw, curr_group);
	mutex_unlock(&esw->state_lock);

	return 0;
}

int mlx5_eswitch_set_vgroup_min_rate(struct mlx5_eswitch *esw, int group_id,
				     u32 min_rate)
{
	u32 fw_max_bw_share = MLX5_CAP_QOS(esw->dev, max_tsar_bw_share);
	bool min_rate_supported = MLX5_CAP_QOS(esw->dev, esw_bw_share) &&
					fw_max_bw_share >= MLX5_MIN_BW_SHARE;
	struct mlx5_vgroup *group = NULL, *tmp;
	u32 previous_min_rate;
	u32 divider;
	int err = 0;

	if (!min_rate_supported ||
	    !MLX5_CAP_QOS(esw->dev, log_esw_max_sched_depth))
		return -EOPNOTSUPP;

	mutex_lock(&esw->state_lock);
	list_for_each_entry(tmp, &esw->qos.groups, list) {
		if (tmp->group_id == group_id) {
			group = tmp;
			break;
		}
	}

	if (!group) {
		err = -EINVAL;
		goto unlock;
	}

	if (min_rate == group->min_rate)
		goto unlock;

	previous_min_rate = group->min_rate;
	group->min_rate = min_rate;
	divider = calculate_min_rate_divider(esw, group->group_id, true);
	err = normalize_vgroups_min_rate(esw, divider);
	if (err) {
		group->min_rate = previous_min_rate;
		esw_warn(esw->dev, "E-Switch group min rate setting failed for group=%d\n", group->group_id);
		/* Attempt restoring previous configuration */
		divider = calculate_min_rate_divider(esw, group->group_id, true);
		if (normalize_vgroups_min_rate(esw, divider))
			esw_warn(esw->dev, "E-Switch BW share retore failed\n");
	}

unlock:
	mutex_unlock(&esw->state_lock);

	return err;
}

int mlx5_eswitch_set_vgroup_max_rate(struct mlx5_eswitch *esw, int group_id,
				     u32 max_rate)
{
	struct mlx5_vgroup *group = NULL, *tmp;
	struct mlx5_core_dev *dev = esw->dev;
	int i, err = 0;

	if (!MLX5_CAP_QOS(esw->dev, esw_rate_limit) ||
	    !MLX5_CAP_QOS(dev, log_esw_max_sched_depth))
		return -EOPNOTSUPP;

	if (!esw->qos.enabled || !MLX5_CAP_GEN(dev, qos) ||
	    !MLX5_CAP_QOS(dev, esw_scheduling))
		return 0;

	mutex_lock(&esw->state_lock);
	list_for_each_entry(tmp, &esw->qos.groups, list) {
		if (tmp->group_id == group_id) {
			group = tmp;
			break;
		}
	}

	if (!group) {
		err = -EINVAL;
		goto unlock;
	}

	if (group->max_rate == max_rate)
		goto unlock;

	err = esw_vgroup_qos_config(esw, group, max_rate, group->bw_share);
	if (err)
		goto unlock;

	group->max_rate = max_rate;

	/* Any unlimited vports in the group should be set
	 * with the value of the group.
	 */
	for (i = 0; i < esw->enabled_vports; i++) {
		if (esw->vports[i].info.group == group_id &&
		    !esw->vports[i].info.max_rate) {
			err = esw_vport_qos_config(esw, esw->vports + i, max_rate,
						   esw->vports[i].qos.bw_share);
			if (err)
				esw_warn(esw->dev, "E-Switch vport implicit rate limit setting failed (vport=%d)\n",
					 i);
		}
	}

unlock:
	mutex_unlock(&esw->state_lock);
	return err;
}

int mlx5_eswitch_get_vport_stats(struct mlx5_eswitch *esw,
				 u16 vport_num,
				 struct ifla_vf_stats *vf_stats)
{
	struct mlx5_vport *vport = mlx5_eswitch_get_vport(esw, vport_num);
	int outlen = MLX5_ST_SZ_BYTES(query_vport_counter_out);
	u32 in[MLX5_ST_SZ_DW(query_vport_counter_in)] = {0};
#ifdef HAVE_STRUCT_IFLA_VF_STATS_RX_TX_DROPPED
	struct mlx5_vport_drop_stats stats = {0};
#endif
	int err = 0;
	u32 *out;

	if (IS_ERR(vport))
		return PTR_ERR(vport);

	if (!vport->enabled)
		return 0;

	out = kvzalloc(outlen, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	MLX5_SET(query_vport_counter_in, in, opcode,
		 MLX5_CMD_OP_QUERY_VPORT_COUNTER);
	MLX5_SET(query_vport_counter_in, in, op_mod, 0);
	MLX5_SET(query_vport_counter_in, in, vport_number, vport->vport);
	MLX5_SET(query_vport_counter_in, in, other_vport, 1);

	memset(out, 0, outlen);
	err = mlx5_cmd_exec(esw->dev, in, sizeof(in), out, outlen);
	if (err)
		goto free_out;

	#define MLX5_GET_CTR(p, x) \
		MLX5_GET64(query_vport_counter_out, p, x)

	memset(vf_stats, 0, sizeof(*vf_stats));
	vf_stats->rx_packets =
		MLX5_GET_CTR(out, received_eth_unicast.packets) +
		MLX5_GET_CTR(out, received_ib_unicast.packets) +
		MLX5_GET_CTR(out, received_eth_multicast.packets) +
		MLX5_GET_CTR(out, received_ib_multicast.packets) +
		MLX5_GET_CTR(out, received_eth_broadcast.packets);

	vf_stats->rx_bytes =
		MLX5_GET_CTR(out, received_eth_unicast.octets) +
		MLX5_GET_CTR(out, received_ib_unicast.octets) +
		MLX5_GET_CTR(out, received_eth_multicast.octets) +
		MLX5_GET_CTR(out, received_ib_multicast.octets) +
		MLX5_GET_CTR(out, received_eth_broadcast.octets);

	vf_stats->tx_packets =
		MLX5_GET_CTR(out, transmitted_eth_unicast.packets) +
		MLX5_GET_CTR(out, transmitted_ib_unicast.packets) +
		MLX5_GET_CTR(out, transmitted_eth_multicast.packets) +
		MLX5_GET_CTR(out, transmitted_ib_multicast.packets) +
		MLX5_GET_CTR(out, transmitted_eth_broadcast.packets);

	vf_stats->tx_bytes =
		MLX5_GET_CTR(out, transmitted_eth_unicast.octets) +
		MLX5_GET_CTR(out, transmitted_ib_unicast.octets) +
		MLX5_GET_CTR(out, transmitted_eth_multicast.octets) +
		MLX5_GET_CTR(out, transmitted_ib_multicast.octets) +
		MLX5_GET_CTR(out, transmitted_eth_broadcast.octets);

	vf_stats->multicast =
		MLX5_GET_CTR(out, received_eth_multicast.packets) +
		MLX5_GET_CTR(out, received_ib_multicast.packets);

	vf_stats->broadcast =
		MLX5_GET_CTR(out, received_eth_broadcast.packets);

#ifdef HAVE_STRUCT_IFLA_VF_STATS_RX_TX_DROPPED
       err = mlx5_eswitch_query_vport_drop_stats(esw->dev, vport, &stats);
       if (err)
       	goto free_out;
       vf_stats->rx_dropped = stats.rx_dropped;
       vf_stats->tx_dropped = stats.tx_dropped;
#endif
#ifdef HAVE_STRUCT_IFLA_VF_STATS_TX_BROADCAST
	vf_stats->tx_multicast =
		MLX5_GET_CTR(out, transmitted_eth_multicast.packets) +
		MLX5_GET_CTR(out, transmitted_ib_multicast.packets);

	vf_stats->tx_broadcast =
		MLX5_GET_CTR(out, transmitted_eth_broadcast.packets);
#endif

free_out:
	kvfree(out);
	return err;
}

#ifndef HAVE_STRUCT_IFLA_VF_STATS_TX_BROADCAST
int mlx5_eswitch_get_vport_stats_backport(struct mlx5_eswitch *esw,
					  int vport,
					  struct ifla_vf_stats_backport *vf_stats_backport)
{
	int outlen = MLX5_ST_SZ_BYTES(query_vport_counter_out);
	u32 in[MLX5_ST_SZ_DW(query_vport_counter_in)] = {0};
	int err = 0;
	u32 *out;

	if (!ESW_ALLOWED(esw))
		return -EPERM;

	out = kvzalloc(outlen, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	MLX5_SET(query_vport_counter_in, in, opcode,
		 MLX5_CMD_OP_QUERY_VPORT_COUNTER);
	MLX5_SET(query_vport_counter_in, in, op_mod, 0);
	MLX5_SET(query_vport_counter_in, in, vport_number, vport);
	if (vport)
		MLX5_SET(query_vport_counter_in, in, other_vport, 1);

	memset(out, 0, outlen);
	err = mlx5_cmd_exec(esw->dev, in, sizeof(in), out, outlen);
	if (err)
		goto free_out;

	#define MLX5_GET_CTR(p, x) \
		MLX5_GET64(query_vport_counter_out, p, x)

	memset(vf_stats_backport, 0, sizeof(*vf_stats_backport));
	vf_stats_backport->tx_multicast =
		MLX5_GET_CTR(out, transmitted_eth_multicast.packets) +
		MLX5_GET_CTR(out, transmitted_ib_multicast.packets);

	vf_stats_backport->tx_broadcast =
		MLX5_GET_CTR(out, transmitted_eth_broadcast.packets);

free_out:
	kvfree(out);
	return err;
}
#endif

u8 mlx5_eswitch_mode(struct mlx5_eswitch *esw)
{
	return ESW_ALLOWED(esw) ? esw->mode : MLX5_ESWITCH_NONE;
}
EXPORT_SYMBOL_GPL(mlx5_eswitch_mode);

bool mlx5_esw_lag_prereq(struct mlx5_core_dev *dev0, struct mlx5_core_dev *dev1)
{
	if ((dev0->priv.eswitch->mode == MLX5_ESWITCH_NONE &&
	     dev1->priv.eswitch->mode == MLX5_ESWITCH_NONE) ||
	    (dev0->priv.eswitch->mode == MLX5_ESWITCH_OFFLOADS &&
	     dev1->priv.eswitch->mode == MLX5_ESWITCH_OFFLOADS))
		return true;

	return false;
}

u16 mlx5_eswitch_get_encap_mode(struct mlx5_eswitch *esw)
{
	return esw->offloads.encap;
}
EXPORT_SYMBOL_GPL(mlx5_eswitch_get_encap_mode);

bool mlx5_eswitch_is_manager_vport(const struct mlx5_eswitch *esw, u16 vport_num)
{
	return ESW_ALLOWED(esw) ? is_esw_manager_vport(esw, vport_num) : false;
}
EXPORT_SYMBOL_GPL(mlx5_eswitch_is_manager_vport);

bool mlx5_esw_multipath_prereq(struct mlx5_core_dev *dev0,
			       struct mlx5_core_dev *dev1)
{
	return (dev0->priv.eswitch->mode == MLX5_ESWITCH_OFFLOADS &&
		dev1->priv.eswitch->mode == MLX5_ESWITCH_OFFLOADS);
}


