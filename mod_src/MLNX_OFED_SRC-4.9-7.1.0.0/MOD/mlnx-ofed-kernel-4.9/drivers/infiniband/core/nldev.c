/*
 * Copyright (c) 2017 Mellanox Technologies. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/module.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/mutex.h>
#include <net/netlink.h>
#include <rdma/rdma_cm.h>
#include <rdma/rdma_netlink.h>

#include "core_priv.h"
#include "cma_priv.h"
#include "restrack.h"
#include "uverbs.h"

typedef int (*res_fill_func_t)(struct sk_buff*, bool,
			       struct rdma_restrack_entry*, uint32_t);

static const struct nla_policy nldev_policy[RDMA_NLDEV_ATTR_MAX] = {
	[RDMA_NLDEV_ATTR_DEV_INDEX]     = { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_DEV_NAME]	= { .type = NLA_NUL_STRING,
					    .len = IB_DEVICE_NAME_MAX - 1},
	[RDMA_NLDEV_ATTR_PORT_INDEX]	= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_FW_VERSION]	= { .type = NLA_NUL_STRING,
					    .len = IB_FW_VERSION_NAME_MAX - 1},
	[RDMA_NLDEV_ATTR_NODE_GUID]	= { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_STAT_COUNTER]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_STAT_COUNTER_ENTRY]	= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_STAT_COUNTER_ID]       = { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_STAT_HWCOUNTERS]       = { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY]  = { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_NAME] = { .type = NLA_NUL_STRING },
	[RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_VALUE] = { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_SYS_IMAGE_GUID] = { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_STAT_AUTO_MODE_MASK]	= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_STAT_MODE]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_STAT_RES]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_SUBNET_PREFIX]	= { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_LID]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_SM_LID]	= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_LMC]		= { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_PORT_STATE]	= { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_PORT_PHYS_STATE] = { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_DEV_NODE_TYPE] = { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_RES_SUMMARY]	= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY]	= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY_NAME] = { .type = NLA_NUL_STRING,
					     .len = 16 },
	[RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY_CURR] = { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_RES_QP]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_QP_ENTRY]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_LQPN]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_RQPN]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_RQ_PSN]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_SQ_PSN]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_PATH_MIG_STATE] = { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_RES_TYPE]		= { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_RES_STATE]		= { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_RES_PID]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_KERN_NAME]		= { .type = NLA_NUL_STRING,
						    .len = TASK_COMM_LEN },
	[RDMA_NLDEV_ATTR_RES_CM_ID]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_CM_ID_ENTRY]	= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_PS]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_SRC_ADDR]	= {
			.len = sizeof(struct __kernel_sockaddr_storage) },
	[RDMA_NLDEV_ATTR_RES_DST_ADDR]	= {
			.len = sizeof(struct __kernel_sockaddr_storage) },
	[RDMA_NLDEV_ATTR_RES_CQ]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_CQ_ENTRY]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_CQE]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_USECNT]		= { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_RES_POLL_CTX]		= { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_RES_MR]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_MR_ENTRY]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_RKEY]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_LKEY]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_IOVA]		= { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_RES_MRLEN]		= { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_RES_PD]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_PD_ENTRY]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_RES_LOCAL_DMA_LKEY]	= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_UNSAFE_GLOBAL_RKEY] = { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_NDEV_INDEX]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_NDEV_NAME]		= { .type = NLA_NUL_STRING,
						    .len = IFNAMSIZ },
	[RDMA_NLDEV_ATTR_DRIVER]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_DRIVER_ENTRY]		= { .type = NLA_NESTED },
	[RDMA_NLDEV_ATTR_DRIVER_STRING]		= { .type = NLA_NUL_STRING,
				    .len = RDMA_NLDEV_ATTR_ENTRY_STRLEN },
	[RDMA_NLDEV_ATTR_DRIVER_PRINT_TYPE]	= { .type = NLA_U8 },
#ifdef HAVE_NLA_S32
       [RDMA_NLDEV_ATTR_DRIVER_S32]		= { .type = NLA_S32 },
#endif
       [RDMA_NLDEV_ATTR_DRIVER_U32]		= { .type = NLA_U32 },
#ifdef HAVE_NLA_S32
       [RDMA_NLDEV_ATTR_DRIVER_S64]		= { .type = NLA_S64 },
#endif
	[RDMA_NLDEV_ATTR_DRIVER_U64]		= { .type = NLA_U64 },
	[RDMA_NLDEV_ATTR_RES_PDN]		= { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_CQN]               = { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_MRN]               = { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_CM_IDN]            = { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_RES_CTXN]              = { .type = NLA_U32 },
	[RDMA_NLDEV_ATTR_LINK_TYPE]		= { .type = NLA_NUL_STRING,
				    .len = RDMA_NLDEV_ATTR_ENTRY_STRLEN },
	[RDMA_NLDEV_SYS_ATTR_NETNS_MODE]	= { .type = NLA_U8 },
	[RDMA_NLDEV_ATTR_DEV_PROTOCOL]		= { .type = NLA_NUL_STRING,
				    .len = RDMA_NLDEV_ATTR_ENTRY_STRLEN },
	[RDMA_NLDEV_NET_NS_FD]			= { .type = NLA_U32 },
};

static int put_driver_name_print_type(struct sk_buff *msg, const char *name,
				      enum rdma_nldev_print_type print_type)
{
	if (nla_put_string(msg, RDMA_NLDEV_ATTR_DRIVER_STRING, name))
		return -EMSGSIZE;
	if (print_type != RDMA_NLDEV_PRINT_TYPE_UNSPEC &&
	    nla_put_u8(msg, RDMA_NLDEV_ATTR_DRIVER_PRINT_TYPE, print_type))
		return -EMSGSIZE;

	return 0;
}

static int _rdma_nl_put_driver_u32(struct sk_buff *msg, const char *name,
				   enum rdma_nldev_print_type print_type,
				   u32 value)
{
	if (put_driver_name_print_type(msg, name, print_type))
		return -EMSGSIZE;
	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_DRIVER_U32, value))
		return -EMSGSIZE;

	return 0;
}

static int _rdma_nl_put_driver_u64(struct sk_buff *msg, const char *name,
				   enum rdma_nldev_print_type print_type,
				   u64 value)
{
	if (put_driver_name_print_type(msg, name, print_type))
		return -EMSGSIZE;
	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_DRIVER_U64, value,
			      RDMA_NLDEV_ATTR_PAD))
		return -EMSGSIZE;

	return 0;
}

int rdma_nl_put_driver_string(struct sk_buff *msg, const char *name,
			      const char *str)
{
	if (put_driver_name_print_type(msg, name,
				       RDMA_NLDEV_PRINT_TYPE_UNSPEC))
		return -EMSGSIZE;
	if (nla_put_string(msg, RDMA_NLDEV_ATTR_DRIVER_STRING, str))
		return -EMSGSIZE;

	return 0;
}
EXPORT_SYMBOL(rdma_nl_put_driver_string);

int rdma_nl_put_driver_u32(struct sk_buff *msg, const char *name, u32 value)
{
	return _rdma_nl_put_driver_u32(msg, name, RDMA_NLDEV_PRINT_TYPE_UNSPEC,
				       value);
}
EXPORT_SYMBOL(rdma_nl_put_driver_u32);

int rdma_nl_put_driver_u32_hex(struct sk_buff *msg, const char *name,
			       u32 value)
{
	return _rdma_nl_put_driver_u32(msg, name, RDMA_NLDEV_PRINT_TYPE_HEX,
				       value);
}
EXPORT_SYMBOL(rdma_nl_put_driver_u32_hex);

int rdma_nl_put_driver_u64(struct sk_buff *msg, const char *name, u64 value)
{
	return _rdma_nl_put_driver_u64(msg, name, RDMA_NLDEV_PRINT_TYPE_UNSPEC,
				       value);
}
EXPORT_SYMBOL(rdma_nl_put_driver_u64);

int rdma_nl_put_driver_u64_hex(struct sk_buff *msg, const char *name, u64 value)
{
	return _rdma_nl_put_driver_u64(msg, name, RDMA_NLDEV_PRINT_TYPE_HEX,
				       value);
}
EXPORT_SYMBOL(rdma_nl_put_driver_u64_hex);

static int fill_nldev_handle(struct sk_buff *msg, struct ib_device *device)
{
	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_DEV_INDEX, device->index))
		return -EMSGSIZE;
	if (nla_put_string(msg, RDMA_NLDEV_ATTR_DEV_NAME,
			   dev_name(&device->dev)))
		return -EMSGSIZE;

	return 0;
}

static int fill_dev_info(struct sk_buff *msg, struct ib_device *device)
{
	char fw[IB_FW_VERSION_NAME_MAX];
	int ret = 0;
	u8 port;

	if (fill_nldev_handle(msg, device))
		return -EMSGSIZE;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, rdma_end_port(device)))
		return -EMSGSIZE;

	BUILD_BUG_ON(sizeof(device->attrs.device_cap_flags) != sizeof(u64));
	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_CAP_FLAGS,
			      device->attrs.device_cap_flags,
			      RDMA_NLDEV_ATTR_PAD))
		return -EMSGSIZE;

	ib_get_device_fw_str(device, fw);
	/* Device without FW has strlen(fw) = 0 */
	if (strlen(fw) && nla_put_string(msg, RDMA_NLDEV_ATTR_FW_VERSION, fw))
		return -EMSGSIZE;

	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_NODE_GUID,
			      be64_to_cpu(device->node_guid),
			      RDMA_NLDEV_ATTR_PAD))
		return -EMSGSIZE;
	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_SYS_IMAGE_GUID,
			      be64_to_cpu(device->attrs.sys_image_guid),
			      RDMA_NLDEV_ATTR_PAD))
		return -EMSGSIZE;
	if (nla_put_u8(msg, RDMA_NLDEV_ATTR_DEV_NODE_TYPE, device->node_type))
		return -EMSGSIZE;

	/*
	 * Link type is determined on first port and mlx4 device
	 * which can potentially have two different link type for the same
	 * IB device is considered as better to be avoided in the future,
	 */
	port = rdma_start_port(device);
	if (rdma_cap_opa_mad(device, port))
		ret = nla_put_string(msg, RDMA_NLDEV_ATTR_DEV_PROTOCOL, "opa");
	else if (rdma_protocol_ib(device, port))
		ret = nla_put_string(msg, RDMA_NLDEV_ATTR_DEV_PROTOCOL, "ib");
	else if (rdma_protocol_iwarp(device, port))
		ret = nla_put_string(msg, RDMA_NLDEV_ATTR_DEV_PROTOCOL, "iw");
	else if (rdma_protocol_roce(device, port))
		ret = nla_put_string(msg, RDMA_NLDEV_ATTR_DEV_PROTOCOL, "roce");
	else if (rdma_protocol_usnic(device, port))
		ret = nla_put_string(msg, RDMA_NLDEV_ATTR_DEV_PROTOCOL,
				     "usnic");
	return ret;
}

static int fill_port_info(struct sk_buff *msg,
			  struct ib_device *device, u32 port,
			  const struct net *net)
{
	struct net_device *netdev = NULL;
	struct ib_port_attr attr;
	int ret;
	u64 cap_flags = 0;

	if (fill_nldev_handle(msg, device))
		return -EMSGSIZE;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, port))
		return -EMSGSIZE;

	ret = ib_query_port(device, port, &attr);
	if (ret)
		return ret;

	if (rdma_protocol_ib(device, port)) {
		BUILD_BUG_ON((sizeof(attr.port_cap_flags) +
				sizeof(attr.port_cap_flags2)) > sizeof(u64));
		cap_flags = attr.port_cap_flags |
			((u64)attr.port_cap_flags2 << 32);
		if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_CAP_FLAGS,
				      cap_flags, RDMA_NLDEV_ATTR_PAD))
			return -EMSGSIZE;
		if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_SUBNET_PREFIX,
				      attr.subnet_prefix, RDMA_NLDEV_ATTR_PAD))
			return -EMSGSIZE;
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_LID, attr.lid))
			return -EMSGSIZE;
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_SM_LID, attr.sm_lid))
			return -EMSGSIZE;
		if (nla_put_u8(msg, RDMA_NLDEV_ATTR_LMC, attr.lmc))
			return -EMSGSIZE;
	}
	if (nla_put_u8(msg, RDMA_NLDEV_ATTR_PORT_STATE, attr.state))
		return -EMSGSIZE;
	if (nla_put_u8(msg, RDMA_NLDEV_ATTR_PORT_PHYS_STATE, attr.phys_state))
		return -EMSGSIZE;

	netdev = ib_device_get_netdev(device, port);
	if (netdev && net_eq(dev_net(netdev), net)) {
		ret = nla_put_u32(msg,
				  RDMA_NLDEV_ATTR_NDEV_INDEX, netdev->ifindex);
		if (ret)
			goto out;
		ret = nla_put_string(msg,
				     RDMA_NLDEV_ATTR_NDEV_NAME, netdev->name);
	}

out:
	if (netdev)
		dev_put(netdev);
	return ret;
}

static int fill_res_info_entry(struct sk_buff *msg,
			       const char *name, u64 curr)
{
	struct nlattr *entry_attr;
#ifdef HAVE_NLA_NEST_START_NOFLAG
	entry_attr = nla_nest_start_noflag(msg,
					   RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY);
#else
	entry_attr = nla_nest_start(msg, RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY);
#endif
	if (!entry_attr)
		return -EMSGSIZE;

	if (nla_put_string(msg, RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY_NAME, name))
		goto err;
	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_RES_SUMMARY_ENTRY_CURR, curr,
			      RDMA_NLDEV_ATTR_PAD))
		goto err;

	nla_nest_end(msg, entry_attr);
	return 0;

err:
	nla_nest_cancel(msg, entry_attr);
	return -EMSGSIZE;
}

static int fill_res_info(struct sk_buff *msg, struct ib_device *device)
{
	static const char * const names[RDMA_RESTRACK_MAX] = {
		[RDMA_RESTRACK_PD] = "pd",
		[RDMA_RESTRACK_CQ] = "cq",
		[RDMA_RESTRACK_QP] = "qp",
		[RDMA_RESTRACK_CM_ID] = "cm_id",
		[RDMA_RESTRACK_MR] = "mr",
		[RDMA_RESTRACK_CTX] = "ctx",
	};

	struct nlattr *table_attr;
	int ret, i, curr;

	if (fill_nldev_handle(msg, device))
		return -EMSGSIZE;

#ifdef HAVE_NLA_NEST_START_NOFLAG
	table_attr = nla_nest_start_noflag(msg, RDMA_NLDEV_ATTR_RES_SUMMARY);
#else
	table_attr = nla_nest_start(msg, RDMA_NLDEV_ATTR_RES_SUMMARY);
#endif
	if (!table_attr)
		return -EMSGSIZE;

	for (i = 0; i < RDMA_RESTRACK_MAX; i++) {
		if (!names[i])
			continue;
		curr = rdma_restrack_count(device, i);
		ret = fill_res_info_entry(msg, names[i], curr);
		if (ret)
			goto err;
	}

	nla_nest_end(msg, table_attr);
	return 0;

err:
	nla_nest_cancel(msg, table_attr);
	return ret;
}

static int fill_res_name_pid(struct sk_buff *msg,
			     struct rdma_restrack_entry *res)
{
	/*
	 * For user resources, user is should read /proc/PID/comm to get the
	 * name of the task file.
	 */
	if (rdma_is_kernel_res(res)) {
		if (nla_put_string(msg, RDMA_NLDEV_ATTR_RES_KERN_NAME,
		    res->kern_name))
			return -EMSGSIZE;
	} else {
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PID,
		    task_pid_vnr(res->task)))
			return -EMSGSIZE;
	}
	return 0;
}

static bool fill_res_entry(struct ib_device *dev, struct sk_buff *msg,
			   struct rdma_restrack_entry *res)
{
	if (!dev->ops.fill_res_entry)
		return false;
	return dev->ops.fill_res_entry(msg, res);
}

static bool fill_stat_entry(struct ib_device *dev, struct sk_buff *msg,
			    struct rdma_restrack_entry *res)
{
	if (!dev->ops.fill_stat_entry)
		return false;
	return dev->ops.fill_stat_entry(msg, res);
}

static int fill_res_qp_entry(struct sk_buff *msg, bool has_cap_net_admin,
			     struct rdma_restrack_entry *res, uint32_t port)
{
	struct ib_qp *qp = container_of(res, struct ib_qp, res);
	struct ib_device *dev = qp->device;
	struct ib_qp_init_attr qp_init_attr;
	struct ib_qp_attr qp_attr;
	int ret;

	ret = ib_query_qp(qp, &qp_attr, 0, &qp_init_attr);
	if (ret)
		return ret;

	if (port && port != qp_attr.port_num)
		return -EAGAIN;

	/* In create_qp() port is not set yet */
	if (qp_attr.port_num &&
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, qp_attr.port_num))
		goto err;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_LQPN, qp->qp_num))
		goto err;
	if (qp->qp_type == IB_QPT_RC || qp->qp_type == IB_QPT_UC) {
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_RQPN,
				qp_attr.dest_qp_num))
			goto err;
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_RQ_PSN,
				qp_attr.rq_psn))
			goto err;
	}

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_SQ_PSN, qp_attr.sq_psn))
		goto err;

	if (qp->qp_type == IB_QPT_RC || qp->qp_type == IB_QPT_UC ||
	    qp->qp_type == IB_QPT_XRC_INI || qp->qp_type == IB_QPT_XRC_TGT) {
		if (nla_put_u8(msg, RDMA_NLDEV_ATTR_RES_PATH_MIG_STATE,
			       qp_attr.path_mig_state))
			goto err;
	}
	if (nla_put_u8(msg, RDMA_NLDEV_ATTR_RES_TYPE, qp->qp_type))
		goto err;
	if (nla_put_u8(msg, RDMA_NLDEV_ATTR_RES_STATE, qp_attr.qp_state))
		goto err;

	if (!rdma_is_kernel_res(res) &&
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PDN, qp->pd->res.id))
		goto err;

	if (fill_res_name_pid(msg, res))
		goto err;

	if (fill_res_entry(dev, msg, res))
		goto err;

	return 0;

err:	return -EMSGSIZE;
}

static int fill_res_cm_id_entry(struct sk_buff *msg, bool has_cap_net_admin,
				struct rdma_restrack_entry *res, uint32_t port)
{
	struct rdma_id_private *id_priv =
				container_of(res, struct rdma_id_private, res);
	struct ib_device *dev = id_priv->id.device;
	struct rdma_cm_id *cm_id = &id_priv->id;

	if (port && port != cm_id->port_num)
		return 0;

	if (cm_id->port_num &&
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, cm_id->port_num))
		goto err;

	if (id_priv->qp_num) {
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_LQPN, id_priv->qp_num))
			goto err;
		if (nla_put_u8(msg, RDMA_NLDEV_ATTR_RES_TYPE, cm_id->qp_type))
			goto err;
	}

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PS, cm_id->ps))
		goto err;

	if (nla_put_u8(msg, RDMA_NLDEV_ATTR_RES_STATE, id_priv->state))
		goto err;

	if (cm_id->route.addr.src_addr.ss_family &&
	    nla_put(msg, RDMA_NLDEV_ATTR_RES_SRC_ADDR,
		    sizeof(cm_id->route.addr.src_addr),
		    &cm_id->route.addr.src_addr))
		goto err;
	if (cm_id->route.addr.dst_addr.ss_family &&
	    nla_put(msg, RDMA_NLDEV_ATTR_RES_DST_ADDR,
		    sizeof(cm_id->route.addr.dst_addr),
		    &cm_id->route.addr.dst_addr))
		goto err;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_CM_IDN, res->id))
		goto err;

	if (fill_res_name_pid(msg, res))
		goto err;

	if (fill_res_entry(dev, msg, res))
		goto err;

	return 0;

err: return -EMSGSIZE;
}

static int fill_res_cq_entry(struct sk_buff *msg, bool has_cap_net_admin,
			     struct rdma_restrack_entry *res, uint32_t port)
{
	struct ib_cq *cq = container_of(res, struct ib_cq, res);
	struct ib_device *dev = cq->device;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_CQE, cq->cqe))
		goto err;
	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_RES_USECNT,
			      atomic_read(&cq->usecnt), RDMA_NLDEV_ATTR_PAD))
		goto err;

	/* Poll context is only valid for kernel CQs */
	if (rdma_is_kernel_res(res) &&
	    nla_put_u8(msg, RDMA_NLDEV_ATTR_RES_POLL_CTX, cq->poll_ctx))
		goto err;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_CQN, res->id))
		goto err;
	if (!rdma_is_kernel_res(res) &&
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_CTXN,
			cq->uobject->uevent.uobject.context->res.id))
		goto err;

	if (fill_res_name_pid(msg, res))
		goto err;

	if (fill_res_entry(dev, msg, res))
		goto err;

	return 0;

err:	return -EMSGSIZE;
}

static int fill_res_mr_entry(struct sk_buff *msg, bool has_cap_net_admin,
			     struct rdma_restrack_entry *res, uint32_t port)
{
	struct ib_mr *mr = container_of(res, struct ib_mr, res);
	struct ib_device *dev = mr->pd->device;

	if (has_cap_net_admin) {
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_RKEY, mr->rkey))
			goto err;
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_LKEY, mr->lkey))
			goto err;
	}

	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_RES_MRLEN, mr->length,
			      RDMA_NLDEV_ATTR_PAD))
		goto err;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_MRN, res->id))
		goto err;

	if (!rdma_is_kernel_res(res) &&
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PDN, mr->pd->res.id))
		goto err;

	if (fill_res_name_pid(msg, res))
		goto err;

	if (fill_res_entry(dev, msg, res))
		goto err;

	return 0;

err:	return -EMSGSIZE;
}

static int fill_res_pd_entry(struct sk_buff *msg, bool has_cap_net_admin,
			     struct rdma_restrack_entry *res, uint32_t port)
{
	struct ib_pd *pd = container_of(res, struct ib_pd, res);
	struct ib_device *dev = pd->device;

	if (has_cap_net_admin) {
		if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_LOCAL_DMA_LKEY,
				pd->local_dma_lkey))
			goto err;
		if ((pd->flags & IB_PD_UNSAFE_GLOBAL_RKEY) &&
		    nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_UNSAFE_GLOBAL_RKEY,
				pd->unsafe_global_rkey))
			goto err;
	}
	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_RES_USECNT,
			      atomic_read(&pd->usecnt), RDMA_NLDEV_ATTR_PAD))
		goto err;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_PDN, res->id))
		goto err;

	if (!rdma_is_kernel_res(res) &&
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_CTXN,
			pd->uobject->context->res.id))
		goto err;

	if (fill_res_name_pid(msg, res))
		goto err;

	if (fill_res_entry(dev, msg, res))
		goto err;

	return 0;

err:	return -EMSGSIZE;
}

static int fill_stat_counter_mode(struct sk_buff *msg,
				  struct rdma_counter *counter)
{
	struct rdma_counter_mode *m = &counter->mode;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_STAT_MODE, m->mode))
		return -EMSGSIZE;

	if (m->mode == RDMA_COUNTER_MODE_AUTO)
		if ((m->mask & RDMA_COUNTER_MASK_QP_TYPE) &&
		    nla_put_u8(msg, RDMA_NLDEV_ATTR_RES_TYPE, m->param.qp_type))
			return -EMSGSIZE;

	return 0;
}

static int fill_stat_counter_qp_entry(struct sk_buff *msg, u32 qpn)
{
	struct nlattr *entry_attr;

	entry_attr = nla_nest_start(msg, RDMA_NLDEV_ATTR_RES_QP_ENTRY);
	if (!entry_attr)
		return -EMSGSIZE;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_LQPN, qpn))
		goto err;

	nla_nest_end(msg, entry_attr);
	return 0;

err:
	nla_nest_cancel(msg, entry_attr);
	return -EMSGSIZE;
}

static int fill_stat_counter_qps(struct sk_buff *msg,
				 struct rdma_counter *counter)
{
	struct rdma_restrack_entry *res;
	struct rdma_restrack_root *rt;
	struct nlattr *table_attr;
	struct ib_qp *qp = NULL;
	unsigned long id = 0;
	int ret = 0;

	table_attr = nla_nest_start(msg, RDMA_NLDEV_ATTR_RES_QP);

	rt = &counter->device->res[RDMA_RESTRACK_QP];
	xa_lock(&rt->xa);
	xa_for_each(&rt->xa, id, res) {
		if (!rdma_is_visible_in_pid_ns(res))
			continue;

		qp = container_of(res, struct ib_qp, res);
		if (qp->qp_type == IB_QPT_RAW_PACKET && !capable(CAP_NET_RAW))
			continue;

		if (!qp->counter || (qp->counter->id != counter->id))
			continue;

		ret = fill_stat_counter_qp_entry(msg, qp->qp_num);
		if (ret)
			goto err;
	}

	xa_unlock(&rt->xa);
	nla_nest_end(msg, table_attr);
	return 0;

err:
	xa_unlock(&rt->xa);
	nla_nest_cancel(msg, table_attr);
	return ret;
}

int rdma_nl_stat_hwcounter_entry(struct sk_buff *msg, const char *name,
				 u64 value)
{
	struct nlattr *entry_attr;

	entry_attr = nla_nest_start(msg, RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY);
	if (!entry_attr)
		return -EMSGSIZE;

	if (nla_put_string(msg, RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_NAME,
			   name))
		goto err;
	if (nla_put_u64_64bit(msg, RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_VALUE,
			      value, RDMA_NLDEV_ATTR_PAD))
		goto err;

	nla_nest_end(msg, entry_attr);
	return 0;

err:
	nla_nest_cancel(msg, entry_attr);
	return -EMSGSIZE;
}
EXPORT_SYMBOL(rdma_nl_stat_hwcounter_entry);

static int fill_stat_mr_entry(struct sk_buff *msg, bool has_cap_net_admin,
			      struct rdma_restrack_entry *res, uint32_t port)
{
	struct ib_mr *mr = container_of(res, struct ib_mr, res);
	struct ib_device *dev = mr->pd->device;

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_MRN, res->id))
		goto err;

	if (fill_stat_entry(dev, msg, res))
		goto err;

	return 0;

err:
	return -EMSGSIZE;
}

static int fill_stat_counter_hwcounters(struct sk_buff *msg,
					struct rdma_counter *counter)
{
	struct rdma_hw_stats *st = counter->stats;
	struct nlattr *table_attr;
	int i;

	table_attr = nla_nest_start(msg, RDMA_NLDEV_ATTR_STAT_HWCOUNTERS);
	if (!table_attr)
		return -EMSGSIZE;

	for (i = 0; i < st->num_counters; i++)
		if (rdma_nl_stat_hwcounter_entry(msg, st->names[i], st->value[i]))
			goto err;

	nla_nest_end(msg, table_attr);
	return 0;

err:
	nla_nest_cancel(msg, table_attr);
	return -EMSGSIZE;
}

static int fill_res_counter_entry(struct sk_buff *msg, bool has_cap_net_admin,
				  struct rdma_restrack_entry *res,
				  uint32_t port)
{
	struct rdma_counter *counter =
		container_of(res, struct rdma_counter, res);

	if (port && port != counter->port)
		return -EAGAIN;

	/* Dump it even query failed */
	rdma_counter_query_stats(counter);

	if (nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, counter->port) ||
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_STAT_COUNTER_ID, counter->id) ||
	    fill_res_name_pid(msg, &counter->res) ||
	    fill_stat_counter_mode(msg, counter) ||
	    fill_stat_counter_qps(msg, counter) ||
	    fill_stat_counter_hwcounters(msg, counter))
		return -EMSGSIZE;

	return 0;
}

#ifdef HAVE_NETLINK_EXT_ACK
static int nldev_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct netlink_ext_ack *extack)
#else
static int nldev_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh)
#endif
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	struct sk_buff *msg;
	u32 index;
	int err;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	err = nlmsg_parse_deprecated(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif/*HAVE_NLMSG_PARSE_DEPRECATED*/
#ifdef HAVE_NETLINK_EXT_ACK
				     nldev_policy, extack);
#else
			  nldev_policy, NULL);
#endif
	if (err || !tb[RDMA_NLDEV_ATTR_DEV_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);

	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		err = -ENOMEM;
		goto err;
	}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_GET),
			0, 0);

	err = fill_dev_info(msg, device);
	if (err)
		goto err_free;

	nlmsg_end(msg, nlh);

	ib_device_put(device);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_free:
	nlmsg_free(msg);
err:
	ib_device_put(device);
	return err;
}

#ifdef HAVE_NETLINK_EXT_ACK
static int nldev_set_doit(struct sk_buff *skb, struct nlmsghdr *nlh,
			  struct netlink_ext_ack *extack)
#else
static int nldev_set_doit(struct sk_buff *skb, struct nlmsghdr *nlh)
#endif
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	u32 index;
	int err;

#ifdef HAVE_NETLINK_EXT_ACK
#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	err = nlmsg_parse_deprecated(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
				     nldev_policy, extack);
#else
	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
	                                     nldev_policy, extack);
#endif /*HAVE_NLMSG_PARSE_DEPRECATED*/
#else
	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1, nldev_policy, NULL);
#endif
	if (err || !tb[RDMA_NLDEV_ATTR_DEV_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	if (tb[RDMA_NLDEV_ATTR_DEV_NAME]) {
		char name[IB_DEVICE_NAME_MAX] = {};

#ifdef HAVE_NLA_STRSCPY
		nla_strscpy(name, tb[RDMA_NLDEV_ATTR_DEV_NAME],
				IB_DEVICE_NAME_MAX);
#else
		nla_strlcpy(name, tb[RDMA_NLDEV_ATTR_DEV_NAME],
				IB_DEVICE_NAME_MAX);
#endif
		err = ib_device_rename(device, name);
		goto done;
	}

	if (tb[RDMA_NLDEV_NET_NS_FD]) {
		u32 ns_fd;

		ns_fd = nla_get_u32(tb[RDMA_NLDEV_NET_NS_FD]);
		err = ib_device_set_netns_put(skb, device, ns_fd);
		goto put_done;
	}

done:
	ib_device_put(device);
put_done:
	return err;
}

static int _nldev_get_dumpit(struct ib_device *device,
			     struct sk_buff *skb,
			     struct netlink_callback *cb,
			     unsigned int idx)
{
	int start = cb->args[0];
	struct nlmsghdr *nlh;

	if (idx < start)
		return 0;

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).pid, cb->nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_GET),
			0, NLM_F_MULTI);

	if (fill_dev_info(skb, device)) {
		nlmsg_cancel(skb, nlh);
		goto out;
	}

	nlmsg_end(skb, nlh);

	idx++;

out:	cb->args[0] = idx;
	return skb->len;
}

static int nldev_get_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	/*
	 * There is no need to take lock, because
	 * we are relying on ib_core's locking.
	 */
	return ib_enum_all_devs(_nldev_get_dumpit, skb, cb);
}

#ifdef HAVE_NETLINK_EXT_ACK
static int nldev_port_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh,
			       struct netlink_ext_ack *extack)
#else
static int nldev_port_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh)
#endif
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	struct sk_buff *msg;
	u32 index;
	u32 port;
	int err;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	err = nlmsg_parse_deprecated(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif /*HAVE_NLMSG_PARSE_DEPRECATED*/
#ifdef HAVE_NETLINK_EXT_ACK
       			     nldev_policy, extack);
#else
			  nldev_policy, NULL);
#endif
	if (err ||
	    !tb[RDMA_NLDEV_ATTR_DEV_INDEX] ||
	    !tb[RDMA_NLDEV_ATTR_PORT_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	port = nla_get_u32(tb[RDMA_NLDEV_ATTR_PORT_INDEX]);
	if (!rdma_is_port_valid(device, port)) {
		err = -EINVAL;
		goto err;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		err = -ENOMEM;
		goto err;
	}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_GET),
			0, 0);

	err = fill_port_info(msg, device, port, sock_net(skb->sk));
	if (err)
		goto err_free;

	nlmsg_end(msg, nlh);
	ib_device_put(device);

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
       return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
       return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_free:
	nlmsg_free(msg);
err:
	ib_device_put(device);
	return err;
}

static int nldev_port_get_dumpit(struct sk_buff *skb,
				 struct netlink_callback *cb)
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	int start = cb->args[0];
	struct nlmsghdr *nlh;
	u32 idx = 0;
	u32 ifindex;
	int err;
	unsigned int p;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	err = nlmsg_parse_deprecated(cb->nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	err = nlmsg_parse(cb->nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif
				     nldev_policy, NULL);
	if (err || !tb[RDMA_NLDEV_ATTR_DEV_INDEX])
		return -EINVAL;

	ifindex = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), ifindex);
	if (!device)
		return -EINVAL;

	rdma_for_each_port (device, p) {
		/*
		 * The dumpit function returns all information from specific
		 * index. This specific index is taken from the netlink
		 * messages request sent by user and it is available
		 * in cb->args[0].
		 *
		 * Usually, the user doesn't fill this field and it causes
		 * to return everything.
		 *
		 */
		if (idx < start) {
			idx++;
			continue;
		}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
		nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid,
#else
		nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).pid,
#endif
				cb->nlh->nlmsg_seq,
				RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
						 RDMA_NLDEV_CMD_PORT_GET),
				0, NLM_F_MULTI);

		if (fill_port_info(skb, device, p, sock_net(skb->sk))) {
			nlmsg_cancel(skb, nlh);
			goto out;
		}
		idx++;
		nlmsg_end(skb, nlh);
	}

out:
	ib_device_put(device);
	cb->args[0] = idx;
	return skb->len;
}

#ifdef HAVE_NETLINK_EXT_ACK
static int nldev_res_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh,
			      struct netlink_ext_ack *extack)
#else
static int nldev_res_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh)
#endif
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	struct sk_buff *msg;
	u32 index;
	int ret;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	ret = nlmsg_parse_deprecated(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	ret = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif /*HAVE_NLMSG_PARSE_DEPRECATED*/
#ifdef HAVE_NETLINK_EXT_ACK
       			     nldev_policy, extack);
#else
                          nldev_policy, NULL);
#endif

	if (ret || !tb[RDMA_NLDEV_ATTR_DEV_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err;
	}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_RES_GET),
			0, 0);

	ret = fill_res_info(msg, device);
	if (ret)
		goto err_free;

	nlmsg_end(msg, nlh);
	ib_device_put(device);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
       return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
       return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_free:
	nlmsg_free(msg);
err:
	ib_device_put(device);
	return ret;
}

static int _nldev_res_get_dumpit(struct ib_device *device,
				 struct sk_buff *skb,
				 struct netlink_callback *cb,
				 unsigned int idx)
{
	int start = cb->args[0];
	struct nlmsghdr *nlh;

	if (idx < start)
		return 0;

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).pid, cb->nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_RES_GET),
			0, NLM_F_MULTI);

	if (fill_res_info(skb, device)) {
		nlmsg_cancel(skb, nlh);
		goto out;
	}
	nlmsg_end(skb, nlh);

	idx++;

out:
	cb->args[0] = idx;
	return skb->len;
}

static int nldev_res_get_dumpit(struct sk_buff *skb,
				struct netlink_callback *cb)
{
	return ib_enum_all_devs(_nldev_res_get_dumpit, skb, cb);
}

struct nldev_fill_res_entry {
	enum rdma_nldev_attr nldev_attr;
	enum rdma_nldev_command nldev_cmd;
	u8 flags;
	u32 entry;
	u32 id;
};

enum nldev_res_flags {
	NLDEV_PER_DEV = 1 << 0,
};

static const struct nldev_fill_res_entry fill_entries[RDMA_RESTRACK_MAX] = {
	[RDMA_RESTRACK_QP] = {
		.nldev_cmd = RDMA_NLDEV_CMD_RES_QP_GET,
		.nldev_attr = RDMA_NLDEV_ATTR_RES_QP,
		.entry = RDMA_NLDEV_ATTR_RES_QP_ENTRY,
		.id = RDMA_NLDEV_ATTR_RES_LQPN,
	},
	[RDMA_RESTRACK_CM_ID] = {
		.nldev_cmd = RDMA_NLDEV_CMD_RES_CM_ID_GET,
		.nldev_attr = RDMA_NLDEV_ATTR_RES_CM_ID,
		.entry = RDMA_NLDEV_ATTR_RES_CM_ID_ENTRY,
		.id = RDMA_NLDEV_ATTR_RES_CM_IDN,
	},
	[RDMA_RESTRACK_CQ] = {
		.nldev_cmd = RDMA_NLDEV_CMD_RES_CQ_GET,
		.nldev_attr = RDMA_NLDEV_ATTR_RES_CQ,
		.flags = NLDEV_PER_DEV,
		.entry = RDMA_NLDEV_ATTR_RES_CQ_ENTRY,
		.id = RDMA_NLDEV_ATTR_RES_CQN,
	},
	[RDMA_RESTRACK_MR] = {
		.nldev_cmd = RDMA_NLDEV_CMD_RES_MR_GET,
		.nldev_attr = RDMA_NLDEV_ATTR_RES_MR,
		.flags = NLDEV_PER_DEV,
		.entry = RDMA_NLDEV_ATTR_RES_MR_ENTRY,
		.id = RDMA_NLDEV_ATTR_RES_MRN,
	},
	[RDMA_RESTRACK_PD] = {
		.nldev_cmd = RDMA_NLDEV_CMD_RES_PD_GET,
		.nldev_attr = RDMA_NLDEV_ATTR_RES_PD,
		.flags = NLDEV_PER_DEV,
		.entry = RDMA_NLDEV_ATTR_RES_PD_ENTRY,
		.id = RDMA_NLDEV_ATTR_RES_PDN,
	},
	[RDMA_RESTRACK_COUNTER] = {
		.nldev_cmd = RDMA_NLDEV_CMD_STAT_GET,
		.nldev_attr = RDMA_NLDEV_ATTR_STAT_COUNTER,
		.entry = RDMA_NLDEV_ATTR_STAT_COUNTER_ENTRY,
		.id = RDMA_NLDEV_ATTR_STAT_COUNTER_ID,
	},
};

static int res_get_common_doit(struct sk_buff *skb, struct nlmsghdr *nlh,
#ifdef HAVE_NETLINK_EXT_ACK
			       struct netlink_ext_ack *extack,
#endif
			       enum rdma_restrack_type res_type,
			       res_fill_func_t fill_func)
{
	const struct nldev_fill_res_entry *fe = &fill_entries[res_type];
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct rdma_restrack_entry *res;
	struct ib_device *device;
	u32 index, id, port = 0;
	bool has_cap_net_admin;
	struct sk_buff *msg;
	int ret;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	ret = nlmsg_parse_deprecated(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	ret = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif
				     nldev_policy, extack);
	if (ret || !tb[RDMA_NLDEV_ATTR_DEV_INDEX] || !fe->id || !tb[fe->id])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	if (tb[RDMA_NLDEV_ATTR_PORT_INDEX]) {
		port = nla_get_u32(tb[RDMA_NLDEV_ATTR_PORT_INDEX]);
		if (!rdma_is_port_valid(device, port)) {
			ret = -EINVAL;
			goto err;
		}
	}

	if ((port && fe->flags & NLDEV_PER_DEV) ||
	    (!port && ~fe->flags & NLDEV_PER_DEV)) {
		ret = -EINVAL;
		goto err;
	}

	id = nla_get_u32(tb[fe->id]);
	res = rdma_restrack_get_byid(device, res_type, id);
	if (IS_ERR(res)) {
		ret = PTR_ERR(res);
		goto err;
	}

	if (!rdma_is_visible_in_pid_ns(res)) {
		ret = -ENOENT;
		goto err_get;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err;
	}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, fe->nldev_cmd),
			0, 0);

	if (fill_nldev_handle(msg, device)) {
		ret = -EMSGSIZE;
		goto err_free;
	}

#ifdef HAVE_NETLINK_CAPABLE
       has_cap_net_admin = netlink_capable(skb, CAP_NET_ADMIN);
#else
       has_cap_net_admin =  (sock_net(skb->sk) == &init_net);
#endif

	ret = fill_func(msg, has_cap_net_admin, res, port);

	rdma_restrack_put(res);
	if (ret)
		goto err_free;

	nlmsg_end(msg, nlh);
	ib_device_put(device);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_free:
	nlmsg_free(msg);
err_get:
	rdma_restrack_put(res);
err:
	ib_device_put(device);
	return ret;
}

static int res_get_common_dumpit(struct sk_buff *skb,
				 struct netlink_callback *cb,
				 enum rdma_restrack_type res_type,
				 res_fill_func_t fill_func)
{
	const struct nldev_fill_res_entry *fe = &fill_entries[res_type];
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct rdma_restrack_entry *res;
	struct rdma_restrack_root *rt;
	int err, ret = 0, idx = 0;
	struct nlattr *table_attr;
	struct nlattr *entry_attr;
	struct ib_device *device;
	int start = cb->args[0];
	bool has_cap_net_admin;
	struct nlmsghdr *nlh;
	unsigned long id;
	u32 index, port = 0;
	bool filled = false;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	err = nlmsg_parse_deprecated(cb->nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	err = nlmsg_parse(cb->nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif
				     nldev_policy, NULL);
	/*
	 * Right now, we are expecting the device index to get res information,
	 * but it is possible to extend this code to return all devices in
	 * one shot by checking the existence of RDMA_NLDEV_ATTR_DEV_INDEX.
	 * if it doesn't exist, we will iterate over all devices.
	 *
	 * But it is not needed for now.
	 */
	if (err || !tb[RDMA_NLDEV_ATTR_DEV_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	/*
	 * If no PORT_INDEX is supplied, we will return all QPs from that device
	 */
	if (tb[RDMA_NLDEV_ATTR_PORT_INDEX]) {
		port = nla_get_u32(tb[RDMA_NLDEV_ATTR_PORT_INDEX]);
		if (!rdma_is_port_valid(device, port)) {
			ret = -EINVAL;
			goto err_index;
		}
	}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).pid, cb->nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, fe->nldev_cmd),
			0, NLM_F_MULTI);

	if (fill_nldev_handle(skb, device)) {
		ret = -EMSGSIZE;
		goto err;
	}

#ifdef HAVE_NLA_NEST_START_NOFLAG
	table_attr = nla_nest_start_noflag(skb, fe->nldev_attr);
#else
	table_attr = nla_nest_start(skb, fe->nldev_attr);
#endif
	if (!table_attr) {
		ret = -EMSGSIZE;
		goto err;
	}

#ifdef HAVE_NETLINK_CAPABLE
       has_cap_net_admin = netlink_capable(cb->skb, CAP_NET_ADMIN);
#else
       has_cap_net_admin =  (sock_net(cb->skb->sk) == &init_net);
#endif

	rt = &device->res[res_type];
	xa_lock(&rt->xa);
	/*
	 * FIXME: if the skip ahead is something common this loop should
	 * use xas_for_each & xas_pause to optimize, we can have a lot of
	 * objects.
	 */
	xa_for_each(&rt->xa, id, res) {
		if (!rdma_is_visible_in_pid_ns(res))
			continue;

		if (idx < start || !rdma_restrack_get(res))
			goto next;

		xa_unlock(&rt->xa);

		filled = true;

#ifdef HAVE_NLA_NEST_START_NOFLAG
		entry_attr = nla_nest_start_noflag(skb, fe->entry);
#else
		entry_attr = nla_nest_start(skb, fe->entry);
#endif
		if (!entry_attr) {
			ret = -EMSGSIZE;
			rdma_restrack_put(res);
			goto msg_full;
		}

		ret = fill_func(skb, has_cap_net_admin, res, port);

		rdma_restrack_put(res);

		if (ret) {
			nla_nest_cancel(skb, entry_attr);
			if (ret == -EMSGSIZE)
				goto msg_full;
			if (ret == -EAGAIN)
				goto again;
			goto res_err;
		}
		nla_nest_end(skb, entry_attr);
again:		xa_lock(&rt->xa);
next:		idx++;
	}
	xa_unlock(&rt->xa);

msg_full:
	nla_nest_end(skb, table_attr);
	nlmsg_end(skb, nlh);
	cb->args[0] = idx;

	/*
	 * No more entries to fill, cancel the message and
	 * return 0 to mark end of dumpit.
	 */
	if (!filled)
		goto err;

	ib_device_put(device);
	return skb->len;

res_err:
	nla_nest_cancel(skb, table_attr);
err:
	nlmsg_cancel(skb, nlh);

err_index:
	ib_device_put(device);
	return ret;
}

#ifdef HAVE_NETLINK_EXT_ACK
#define RES_GET_FUNCS(name, type)					       \
	static int nldev_res_get_##name##_dumpit(struct sk_buff *skb,	       \
						 struct netlink_callback *cb)  \
	{								       \
		return res_get_common_dumpit(skb, cb, type,		       \
					     fill_res_##name##_entry);	       \
	}								       \
	static int nldev_res_get_##name##_doit(struct sk_buff *skb,	       \
					       struct nlmsghdr *nlh,	       \
					       struct netlink_ext_ack *extack) \
	{								       \
		return res_get_common_doit(skb, nlh, extack, type,	       \
					   fill_res_##name##_entry);	       \
	}
#else
#define RES_GET_FUNCS(name, type)                                              \
	static int nldev_res_get_##name##_dumpit(struct sk_buff *skb,          \
						 struct netlink_callback *cb)  \
	{                                                                      \
		return res_get_common_dumpit(skb, cb, type,                    \
				fill_res_##name##_entry);                      \
	}                                                                      \
	static int nldev_res_get_##name##_doit(struct sk_buff *skb,            \
					       struct nlmsghdr *nlh)           \
	{                                                                      \
		return res_get_common_doit(skb, nlh, type,                     \
				fill_res_##name##_entry);                      \
	}
#endif

RES_GET_FUNCS(qp, RDMA_RESTRACK_QP);
RES_GET_FUNCS(cm_id, RDMA_RESTRACK_CM_ID);
RES_GET_FUNCS(cq, RDMA_RESTRACK_CQ);
RES_GET_FUNCS(pd, RDMA_RESTRACK_PD);
RES_GET_FUNCS(mr, RDMA_RESTRACK_MR);
RES_GET_FUNCS(counter, RDMA_RESTRACK_COUNTER);

static LIST_HEAD(link_ops);
static DECLARE_RWSEM(link_ops_rwsem);

static const struct rdma_link_ops *link_ops_get(const char *type)
{
	const struct rdma_link_ops *ops;

	list_for_each_entry(ops, &link_ops, list) {
		if (!strcmp(ops->type, type))
			goto out;
	}
	ops = NULL;
out:
	return ops;
}

void rdma_link_register(struct rdma_link_ops *ops)
{
	down_write(&link_ops_rwsem);
	if (WARN_ON_ONCE(link_ops_get(ops->type)))
		goto out;
	list_add(&ops->list, &link_ops);
out:
	up_write(&link_ops_rwsem);
}
EXPORT_SYMBOL(rdma_link_register);

void rdma_link_unregister(struct rdma_link_ops *ops)
{
	down_write(&link_ops_rwsem);
	list_del(&ops->list);
	up_write(&link_ops_rwsem);
}
EXPORT_SYMBOL(rdma_link_unregister);

static int nldev_newlink(struct sk_buff *skb, struct nlmsghdr *nlh
#ifdef HAVE_NETLINK_EXT_ACK
			  ,struct netlink_ext_ack *extack
#endif
			  )
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	char ibdev_name[IB_DEVICE_NAME_MAX];
	const struct rdma_link_ops *ops;
	char ndev_name[IFNAMSIZ];
	struct net_device *ndev;
	char type[IFNAMSIZ];
	int err;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	err = nlmsg_parse_deprecated(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif
				     nldev_policy, extack);
	if (err || !tb[RDMA_NLDEV_ATTR_DEV_NAME] ||
	    !tb[RDMA_NLDEV_ATTR_LINK_TYPE] || !tb[RDMA_NLDEV_ATTR_NDEV_NAME])
		return -EINVAL;

#ifdef HAVE_NLA_STRSCPY
	nla_strscpy(ibdev_name, tb[RDMA_NLDEV_ATTR_DEV_NAME],
		    sizeof(ibdev_name));
#else
	nla_strlcpy(ibdev_name, tb[RDMA_NLDEV_ATTR_DEV_NAME],
		    sizeof(ibdev_name));
#endif
	if (strchr(ibdev_name, '%'))
		return -EINVAL;

#ifdef HAVE_NLA_STRSCPY
	nla_strscpy(type, tb[RDMA_NLDEV_ATTR_LINK_TYPE], sizeof(type));
	nla_strscpy(ndev_name, tb[RDMA_NLDEV_ATTR_NDEV_NAME],
		    sizeof(ndev_name));
#else
	nla_strlcpy(type, tb[RDMA_NLDEV_ATTR_LINK_TYPE], sizeof(type));
	nla_strlcpy(ndev_name, tb[RDMA_NLDEV_ATTR_NDEV_NAME],
		    sizeof(ndev_name));
#endif

	ndev = dev_get_by_name(sock_net(skb->sk), ndev_name);
	if (!ndev)
		return -ENODEV;

	down_read(&link_ops_rwsem);
	ops = link_ops_get(type);
#ifdef CONFIG_MODULES
	if (!ops) {
		up_read(&link_ops_rwsem);
		request_module("rdma-link-%s", type);
		down_read(&link_ops_rwsem);
		ops = link_ops_get(type);
	}
#endif
	err = ops ? ops->newlink(ibdev_name, ndev) : -EINVAL;
	up_read(&link_ops_rwsem);
	dev_put(ndev);

	return err;
}

static int nldev_dellink(struct sk_buff *skb, struct nlmsghdr *nlh
#ifdef HAVE_NETLINK_EXT_ACK
			  ,struct netlink_ext_ack *extack
#endif
			  )
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	u32 index;
	int err;

#ifdef HAVE_NLMSG_PARSE_DEPRECATED
	err = nlmsg_parse_deprecated(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#else
	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
#endif
				     nldev_policy, extack);
	if (err || !tb[RDMA_NLDEV_ATTR_DEV_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	if (!(device->attrs.device_cap_flags & IB_DEVICE_ALLOW_USER_UNREG)) {
		ib_device_put(device);
		return -EINVAL;
	}

	ib_unregister_device_and_put(device);
	return 0;
}

static int nldev_sys_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh
#ifdef HAVE_NETLINK_EXT_ACK
			  ,struct netlink_ext_ack *extack
#endif
			  )
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct sk_buff *msg;
	int err;

	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
			  nldev_policy, extack);
	if (err)
		return err;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					 RDMA_NLDEV_CMD_SYS_GET),
			0, 0);

	err = nla_put_u8(msg, RDMA_NLDEV_SYS_ATTR_NETNS_MODE,
			 (u8)ib_devices_shared_netns);
	if (err) {
		nlmsg_free(msg);
		return err;
	}
	nlmsg_end(msg, nlh);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif
}

static int nldev_set_sys_set_doit(struct sk_buff *skb, struct nlmsghdr *nlh
#ifdef HAVE_NETLINK_EXT_ACK
			  ,struct netlink_ext_ack *extack
#endif
			  )
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	u8 enable;
	int err;

	err = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
			  nldev_policy, extack);
	if (err || !tb[RDMA_NLDEV_SYS_ATTR_NETNS_MODE])
		return -EINVAL;

	enable = nla_get_u8(tb[RDMA_NLDEV_SYS_ATTR_NETNS_MODE]);
	/* Only 0 and 1 are supported */
	if (enable > 1)
		return -EINVAL;

	err = rdma_compatdev_set(enable);
	return err;
}

static int nldev_stat_set_doit(struct sk_buff *skb, struct nlmsghdr *nlh
#ifdef HAVE_NETLINK_EXT_ACK
			  ,struct netlink_ext_ack *extack
#endif
			  )
{
	u32 index, port, mode, mask = 0, qpn, cntn = 0;
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	struct sk_buff *msg;
	int ret;

	ret = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
			  nldev_policy, extack);
	/* Currently only counter for QP is supported */
	if (ret || !tb[RDMA_NLDEV_ATTR_STAT_RES] ||
	    !tb[RDMA_NLDEV_ATTR_DEV_INDEX] ||
	    !tb[RDMA_NLDEV_ATTR_PORT_INDEX] || !tb[RDMA_NLDEV_ATTR_STAT_MODE])
		return -EINVAL;

	if (nla_get_u32(tb[RDMA_NLDEV_ATTR_STAT_RES]) != RDMA_NLDEV_ATTR_RES_QP)
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	port = nla_get_u32(tb[RDMA_NLDEV_ATTR_PORT_INDEX]);
	if (!rdma_is_port_valid(device, port)) {
		ret = -EINVAL;
		goto err;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err;
	}
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					 RDMA_NLDEV_CMD_STAT_SET),
			0, 0);

	mode = nla_get_u32(tb[RDMA_NLDEV_ATTR_STAT_MODE]);
	if (mode == RDMA_COUNTER_MODE_AUTO) {
		if (tb[RDMA_NLDEV_ATTR_STAT_AUTO_MODE_MASK])
			mask = nla_get_u32(
				tb[RDMA_NLDEV_ATTR_STAT_AUTO_MODE_MASK]);

		ret = rdma_counter_set_auto_mode(device, port,
						 mask ? true : false, mask);
		if (ret)
			goto err_msg;
	} else {
		qpn = nla_get_u32(tb[RDMA_NLDEV_ATTR_RES_LQPN]);
		if (tb[RDMA_NLDEV_ATTR_STAT_COUNTER_ID]) {
			cntn = nla_get_u32(tb[RDMA_NLDEV_ATTR_STAT_COUNTER_ID]);
			ret = rdma_counter_bind_qpn(device, port, qpn, cntn);
		} else {
			ret = rdma_counter_bind_qpn_alloc(device, port,
							  qpn, &cntn);
		}
		if (ret)
			goto err_msg;

		if (fill_nldev_handle(msg, device) ||
		    nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, port) ||
		    nla_put_u32(msg, RDMA_NLDEV_ATTR_STAT_COUNTER_ID, cntn) ||
		    nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_LQPN, qpn)) {
			ret = -EMSGSIZE;
			goto err_fill;
		}
	}

	nlmsg_end(msg, nlh);
	ib_device_put(device);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_fill:
	rdma_counter_unbind_qpn(device, port, qpn, cntn);
err_msg:
	nlmsg_free(msg);
err:
	ib_device_put(device);
	return ret;
}

static int nldev_stat_del_doit(struct sk_buff *skb, struct nlmsghdr *nlh
#ifdef HAVE_NETLINK_EXT_ACK
			  ,struct netlink_ext_ack *extack
#endif
			  )
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	struct ib_device *device;
	struct sk_buff *msg;
	u32 index, port, qpn, cntn;
	int ret;

	ret = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
			  nldev_policy, extack);
	if (ret || !tb[RDMA_NLDEV_ATTR_STAT_RES] ||
	    !tb[RDMA_NLDEV_ATTR_DEV_INDEX] || !tb[RDMA_NLDEV_ATTR_PORT_INDEX] ||
	    !tb[RDMA_NLDEV_ATTR_STAT_COUNTER_ID] ||
	    !tb[RDMA_NLDEV_ATTR_RES_LQPN])
		return -EINVAL;

	if (nla_get_u32(tb[RDMA_NLDEV_ATTR_STAT_RES]) != RDMA_NLDEV_ATTR_RES_QP)
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	port = nla_get_u32(tb[RDMA_NLDEV_ATTR_PORT_INDEX]);
	if (!rdma_is_port_valid(device, port)) {
		ret = -EINVAL;
		goto err;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err;
	}
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					 RDMA_NLDEV_CMD_STAT_SET),
			0, 0);

	cntn = nla_get_u32(tb[RDMA_NLDEV_ATTR_STAT_COUNTER_ID]);
	qpn = nla_get_u32(tb[RDMA_NLDEV_ATTR_RES_LQPN]);
	ret = rdma_counter_unbind_qpn(device, port, qpn, cntn);
	if (ret)
		goto err_unbind;

	if (fill_nldev_handle(msg, device) ||
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, port) ||
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_STAT_COUNTER_ID, cntn) ||
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_RES_LQPN, qpn)) {
		ret = -EMSGSIZE;
		goto err_fill;
	}

	nlmsg_end(msg, nlh);
	ib_device_put(device);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_fill:
	rdma_counter_bind_qpn(device, port, qpn, cntn);
err_unbind:
	nlmsg_free(msg);
err:
	ib_device_put(device);
	return ret;
}

static int stat_get_doit_default_counter(struct sk_buff *skb,
					 struct nlmsghdr *nlh,
#ifdef HAVE_NETLINK_EXT_ACK
					 struct netlink_ext_ack *extack,
					 struct nlattr *tb[])
#else
					 struct nlattr *tb[])
#endif
{
	struct rdma_hw_stats *stats;
	struct nlattr *table_attr;
	struct ib_device *device;
	int ret, num_cnts, i;
	struct sk_buff *msg;
	u32 index, port;
	u64 v;

	if (!tb[RDMA_NLDEV_ATTR_DEV_INDEX] || !tb[RDMA_NLDEV_ATTR_PORT_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	if (!device->ops.alloc_hw_stats || !device->ops.get_hw_stats) {
		ret = -EINVAL;
		goto err;
	}

	port = nla_get_u32(tb[RDMA_NLDEV_ATTR_PORT_INDEX]);
	if (!rdma_is_port_valid(device, port)) {
		ret = -EINVAL;
		goto err;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err;
	}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					 RDMA_NLDEV_CMD_STAT_GET),
			0, 0);

	if (fill_nldev_handle(msg, device) ||
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, port)) {
		ret = -EMSGSIZE;
		goto err_msg;
	}

	stats = device->port_data ? device->port_data[port].hw_stats : NULL;
	if (stats == NULL) {
		ret = -EINVAL;
		goto err_msg;
	}
	mutex_lock(&stats->lock);

	num_cnts = device->ops.get_hw_stats(device, stats, port, 0);
	if (num_cnts < 0) {
		ret = -EINVAL;
		goto err_stats;
	}

	table_attr = nla_nest_start(msg, RDMA_NLDEV_ATTR_STAT_HWCOUNTERS);
	if (!table_attr) {
		ret = -EMSGSIZE;
		goto err_stats;
	}
	for (i = 0; i < num_cnts; i++) {
		v = stats->value[i] +
			rdma_counter_get_hwstat_value(device, port, i);
		if (rdma_nl_stat_hwcounter_entry(msg, stats->names[i], v)) {
			ret = -EMSGSIZE;
			goto err_table;
		}
	}
	nla_nest_end(msg, table_attr);

	mutex_unlock(&stats->lock);
	nlmsg_end(msg, nlh);
	ib_device_put(device);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_table:
	nla_nest_cancel(msg, table_attr);
err_stats:
	mutex_unlock(&stats->lock);
err_msg:
	nlmsg_free(msg);
err:
	ib_device_put(device);
	return ret;
}

static int stat_get_doit_qp(struct sk_buff *skb, struct nlmsghdr *nlh,
#ifdef HAVE_NETLINK_EXT_ACK
			    struct netlink_ext_ack *extack, struct nlattr *tb[])
#else
			    struct nlattr *tb[])
#endif

{
	static enum rdma_nl_counter_mode mode;
	static enum rdma_nl_counter_mask mask;
	struct ib_device *device;
	struct sk_buff *msg;
	u32 index, port;
	int ret;

	if (tb[RDMA_NLDEV_ATTR_STAT_COUNTER_ID])
#ifdef HAVE_NETLINK_EXT_ACK
		return nldev_res_get_counter_doit(skb, nlh, extack);
#else
		return nldev_res_get_counter_doit(skb, nlh);
#endif

	if (!tb[RDMA_NLDEV_ATTR_STAT_MODE] ||
	    !tb[RDMA_NLDEV_ATTR_DEV_INDEX] || !tb[RDMA_NLDEV_ATTR_PORT_INDEX])
		return -EINVAL;

	index = nla_get_u32(tb[RDMA_NLDEV_ATTR_DEV_INDEX]);
	device = ib_device_get_by_index(sock_net(skb->sk), index);
	if (!device)
		return -EINVAL;

	port = nla_get_u32(tb[RDMA_NLDEV_ATTR_PORT_INDEX]);
	if (!rdma_is_port_valid(device, port)) {
		ret = -EINVAL;
		goto err;
	}

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err;
	}

#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	nlh = nlmsg_put(msg, NETLINK_CB(skb).portid, nlh->nlmsg_seq,
#else
	nlh = nlmsg_put(msg, NETLINK_CB(skb).pid, nlh->nlmsg_seq,
#endif
			RDMA_NL_GET_TYPE(RDMA_NL_NLDEV,
					 RDMA_NLDEV_CMD_STAT_GET),
			0, 0);

	ret = rdma_counter_get_mode(device, port, &mode, &mask);
	if (ret)
		goto err_msg;

	if (fill_nldev_handle(msg, device) ||
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_PORT_INDEX, port) ||
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_STAT_MODE, mode))
		goto err_msg;

	if ((mode == RDMA_COUNTER_MODE_AUTO) &&
	    nla_put_u32(msg, RDMA_NLDEV_ATTR_STAT_AUTO_MODE_MASK, mask))
		goto err_msg;

	nlmsg_end(msg, nlh);
	ib_device_put(device);
#ifdef HAVE_NETLINK_SKB_PARMS_PORTID
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).portid);
#else
	return rdma_nl_unicast(sock_net(skb->sk), msg, NETLINK_CB(skb).pid);
#endif

err_msg:
	nlmsg_free(msg);
err:
	ib_device_put(device);
	return ret;
}

static int nldev_stat_get_doit(struct sk_buff *skb, struct nlmsghdr *nlh
#ifdef HAVE_NETLINK_EXT_ACK
			  ,struct netlink_ext_ack *extack
#endif
			  )
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	int ret;

	ret = nlmsg_parse(nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
			  nldev_policy, extack);
	if (ret)
		return -EINVAL;

	if (!tb[RDMA_NLDEV_ATTR_STAT_RES])
#ifdef HAVE_NETLINK_EXT_ACK
		return stat_get_doit_default_counter(skb, nlh, extack, tb);
#else
		return stat_get_doit_default_counter(skb, nlh,  tb);
#endif

	switch (nla_get_u32(tb[RDMA_NLDEV_ATTR_STAT_RES])) {
	case RDMA_NLDEV_ATTR_RES_QP:
#ifdef HAVE_NETLINK_EXT_ACK
		ret = stat_get_doit_qp(skb, nlh, extack, tb);
#else
		ret = stat_get_doit_qp(skb, nlh, tb);
#endif
		break;
	case RDMA_NLDEV_ATTR_RES_MR:
#ifdef HAVE_NETLINK_EXT_ACK
		ret = res_get_common_doit(skb, nlh, extack, RDMA_RESTRACK_MR,
#else
		ret = res_get_common_doit(skb, nlh, RDMA_RESTRACK_MR,
#endif
					  fill_stat_mr_entry);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int nldev_stat_get_dumpit(struct sk_buff *skb,
				 struct netlink_callback *cb)
{
	struct nlattr *tb[RDMA_NLDEV_ATTR_MAX];
	int ret;

	ret = nlmsg_parse(cb->nlh, 0, tb, RDMA_NLDEV_ATTR_MAX - 1,
			  nldev_policy, NULL);
	if (ret || !tb[RDMA_NLDEV_ATTR_STAT_RES])
		return -EINVAL;

	switch (nla_get_u32(tb[RDMA_NLDEV_ATTR_STAT_RES])) {
	case RDMA_NLDEV_ATTR_RES_QP:
		ret = nldev_res_get_counter_dumpit(skb, cb);
		break;
	case RDMA_NLDEV_ATTR_RES_MR:
		ret = res_get_common_dumpit(skb, cb, RDMA_RESTRACK_MR,
					    fill_stat_mr_entry);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct rdma_nl_cbs nldev_cb_table[RDMA_NLDEV_NUM_OPS] = {
	[RDMA_NLDEV_CMD_GET] = {
		.doit = nldev_get_doit,
		.dump = nldev_get_dumpit,
	},
	[RDMA_NLDEV_CMD_SET] = {
		.doit = nldev_set_doit,
		.flags = RDMA_NL_ADMIN_PERM,
	},
	[RDMA_NLDEV_CMD_NEWLINK] = {
		.doit = nldev_newlink,
		.flags = RDMA_NL_ADMIN_PERM,
	},
	[RDMA_NLDEV_CMD_DELLINK] = {
		.doit = nldev_dellink,
		.flags = RDMA_NL_ADMIN_PERM,
	},
	[RDMA_NLDEV_CMD_PORT_GET] = {
		.doit = nldev_port_get_doit,
		.dump = nldev_port_get_dumpit,
	},
	[RDMA_NLDEV_CMD_RES_GET] = {
		.doit = nldev_res_get_doit,
		.dump = nldev_res_get_dumpit,
	},
	[RDMA_NLDEV_CMD_RES_QP_GET] = {
		.doit = nldev_res_get_qp_doit,
		.dump = nldev_res_get_qp_dumpit,
	},
	[RDMA_NLDEV_CMD_RES_CM_ID_GET] = {
		.doit = nldev_res_get_cm_id_doit,
		.dump = nldev_res_get_cm_id_dumpit,
	},
	[RDMA_NLDEV_CMD_RES_CQ_GET] = {
		.doit = nldev_res_get_cq_doit,
		.dump = nldev_res_get_cq_dumpit,
	},
	[RDMA_NLDEV_CMD_RES_MR_GET] = {
		.doit = nldev_res_get_mr_doit,
		.dump = nldev_res_get_mr_dumpit,
	},
	[RDMA_NLDEV_CMD_RES_PD_GET] = {
		.doit = nldev_res_get_pd_doit,
		.dump = nldev_res_get_pd_dumpit,
	},
	[RDMA_NLDEV_CMD_SYS_GET] = {
		.doit = nldev_sys_get_doit,
	},
	[RDMA_NLDEV_CMD_SYS_SET] = {
		.doit = nldev_set_sys_set_doit,
	},
	[RDMA_NLDEV_CMD_STAT_SET] = {
		.doit = nldev_stat_set_doit,
		.flags = RDMA_NL_ADMIN_PERM,
	},
	[RDMA_NLDEV_CMD_STAT_GET] = {
		.doit = nldev_stat_get_doit,
		.dump = nldev_stat_get_dumpit,
	},
	[RDMA_NLDEV_CMD_STAT_DEL] = {
		.doit = nldev_stat_del_doit,
		.flags = RDMA_NL_ADMIN_PERM,
	},
};

void __init nldev_init(void)
{
	rdma_nl_register(RDMA_NL_NLDEV, nldev_cb_table);
}

void __exit nldev_exit(void)
{
	rdma_nl_unregister(RDMA_NL_NLDEV);
}

MODULE_ALIAS_RDMA_NETLINK(RDMA_NL_NLDEV, 5);
