#ifndef MLX4_CQ_EXP_H
#define MLX4_CQ_EXP_H

#include <linux/types.h>
#ifdef HAVE_UAPI_LINUX_IF_ETHER_H
#include <uapi/linux/if_ether.h>
#endif

#include <linux/mlx4/device.h>
#include <linux/mlx4/doorbell.h>

int mlx4_cq_ignore_overrun(struct mlx4_dev *dev, struct mlx4_cq *cq);

#endif
