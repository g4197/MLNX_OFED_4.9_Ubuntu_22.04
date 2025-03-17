/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2019 Mellanox Technologies. */

#ifndef __MLX5E_EN_HEALTH_H
#define __MLX5E_EN_HEALTH_H

#include "en.h"
#include "diag/rsc_dump.h"

#define MLX5E_RX_ERR_CQE(cqe) (get_cqe_opcode(cqe) != MLX5_CQE_RESP_SEND)

static inline bool cqe_syndrome_needs_recover(u8 syndrome)
{
	return syndrome == MLX5_CQE_SYNDROME_LOCAL_QP_OP_ERR ||
	       syndrome == MLX5_CQE_SYNDROME_LOCAL_PROT_ERR ||
	       syndrome == MLX5_CQE_SYNDROME_WR_FLUSH_ERR;
}

int mlx5e_reporter_tx_create(struct mlx5e_priv *priv);
void mlx5e_reporter_tx_destroy(struct mlx5e_priv *priv);
void mlx5e_reporter_tx_err_cqe(struct mlx5e_txqsq *sq);
int mlx5e_reporter_tx_timeout(struct mlx5e_txqsq *sq);

#ifdef HAVE_DEVLINK_HEALTH_REPORT_SUPPORT
int mlx5e_reporter_cq_diagnose(struct mlx5e_cq *cq, struct devlink_fmsg *fmsg);
int mlx5e_reporter_cq_common_diagnose(struct mlx5e_cq *cq, struct devlink_fmsg *fmsg);
int mlx5e_reporter_named_obj_nest_start(struct devlink_fmsg *fmsg, char *name);
int mlx5e_reporter_named_obj_nest_end(struct devlink_fmsg *fmsg);
#endif /* HAVE_DEVLINK_HEALTH_REPORT_SUPPORT */

int mlx5e_reporter_rx_create(struct mlx5e_priv *priv);
void mlx5e_reporter_rx_destroy(struct mlx5e_priv *priv);
void mlx5e_reporter_icosq_cqe_err(struct mlx5e_icosq *icosq);
void mlx5e_reporter_rq_cqe_err(struct mlx5e_rq *rq);
void mlx5e_reporter_rx_timeout(struct mlx5e_rq *rq);

#define MLX5E_REPORTER_PER_Q_MAX_LEN 256

struct mlx5e_err_ctx {
	int (*recover)(void *ctx);
#ifdef HAVE_DEVLINK_HEALTH_REPORT_SUPPORT
	int (*dump)(struct mlx5e_priv *priv, struct devlink_fmsg *fmsg, void *ctx);
#endif /* HAVE_DEVLINK_HEALTH_REPORT_SUPPORT */
	void *ctx;
};

int mlx5e_health_sq_to_ready(struct mlx5e_channel *channel, u32 sqn);
int mlx5e_health_channel_eq_recover(struct mlx5_eq_comp *eq, struct mlx5e_channel *channel);
int mlx5e_health_recover_channels(struct mlx5e_priv *priv);
int mlx5e_health_report(struct mlx5e_priv *priv,
			struct devlink_health_reporter *reporter, char *err_str,
			struct mlx5e_err_ctx *err_ctx);
int mlx5e_health_create_reporters(struct mlx5e_priv *priv);
void mlx5e_health_destroy_reporters(struct mlx5e_priv *priv);
void mlx5e_health_channels_update(struct mlx5e_priv *priv);
#ifdef HAVE_DEVLINK_HEALTH_REPORT_SUPPORT
int mlx5e_health_rsc_fmsg_dump(struct mlx5e_priv *priv, struct mlx5_rsc_key *key,
			       struct devlink_fmsg *fmsg);
int mlx5e_health_queue_dump(struct mlx5e_priv *priv, struct devlink_fmsg *fmsg,
			    int queue_idx, char *lbl);

#ifndef HAVE_DEVLINK_FMSG_BINARY_PUT

#include <net/genetlink.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>

#define DEVLINK_FMSG_MAX_SIZE (GENLMSG_DEFAULT_SIZE - GENL_HDRLEN - NLA_HDRLEN)

struct devlink_fmsg {
        struct list_head item_list;
};

struct devlink_fmsg_item {
        struct list_head list;
        int attrtype;
        u8 nla_type;
        u16 len;
        int value[0];
};

static inline int devlink_fmsg_binary_put(struct devlink_fmsg *fmsg,
                                  const void *value, u16 value_len)
{
        struct devlink_fmsg_item *item;

        if (value_len > DEVLINK_FMSG_MAX_SIZE)
                return -EMSGSIZE;

        item = kzalloc(sizeof(*item) + value_len, GFP_KERNEL);
        if (!item)
                return -ENOMEM;

        item->nla_type = NLA_BINARY;
        item->len = value_len;
        item->attrtype = DEVLINK_ATTR_FMSG_OBJ_VALUE_DATA;
        memcpy(&item->value, value, item->len);
        list_add_tail(&item->list, &fmsg->item_list);

        return 0;
}
#endif /* HAVE_DEVLINK_FMSG_BINARY_PUT */

#endif /* HAVE_DEVLINK_HEALTH_REPORT_SUPPORT */

#endif
