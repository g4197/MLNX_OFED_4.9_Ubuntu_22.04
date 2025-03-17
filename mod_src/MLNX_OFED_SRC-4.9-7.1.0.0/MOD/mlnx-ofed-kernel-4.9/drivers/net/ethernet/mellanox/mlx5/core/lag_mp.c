// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2019 Mellanox Technologies. */

#include <linux/netdevice.h>
#ifdef HAVE_FIB_INFO_NH
#include <net/nexthop.h>
#endif
#include <net/ip_fib.h>
#include "mlx5_core.h"
#include "eswitch.h"
#include "lib/mlx5.h"

#if defined(MLX_USE_LAG_COMPAT) || defined(HAVE_LAG_TX_TYPE)
#define MLX_LAG_SUPPORTED
#endif

#ifdef MLX_LAG_SUPPORTED
#include "lag.h"

#ifdef HAVE_FIB_NH_NOTIFIER_INFO
static bool mlx5_lag_multipath_check_prereq(struct mlx5_lag *ldev)
{
	if (!ldev->pf[0].dev || !ldev->pf[1].dev)
		return false;

	return mlx5_esw_multipath_prereq(ldev->pf[0].dev, ldev->pf[1].dev);
}
#endif

static bool __mlx5_lag_is_multipath(struct mlx5_lag *ldev)
{
	return !!(ldev->flags & MLX5_LAG_FLAG_MULTIPATH);
}

bool mlx5_lag_is_multipath(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	bool res;

	ldev = mlx5_lag_dev_get(dev);
	res  = ldev && __mlx5_lag_is_multipath(ldev);

	return res;
}

/**
 * Set lag port affinity
 *
 * @ldev: lag device
 * @port:
 *     0 - set normal affinity.
 *     1 - set affinity to port 1.
 *     2 - set affinity to port 2.
 *
 **/
#ifdef HAVE_FIB_NH_NOTIFIER_INFO
static void mlx5_lag_set_port_affinity(struct mlx5_lag *ldev, int port)
{
	struct lag_tracker tracker;

	if (!__mlx5_lag_is_multipath(ldev))
		return;

	switch (port) {
	case 0:
		tracker.netdev_state[0].tx_enabled = true;
		tracker.netdev_state[1].tx_enabled = true;
		tracker.netdev_state[0].link_up = true;
		tracker.netdev_state[1].link_up = true;
		break;
	case 1:
		tracker.netdev_state[0].tx_enabled = true;
		tracker.netdev_state[0].link_up = true;
		tracker.netdev_state[1].tx_enabled = false;
		tracker.netdev_state[1].link_up = false;
		break;
	case 2:
		tracker.netdev_state[0].tx_enabled = false;
		tracker.netdev_state[0].link_up = false;
		tracker.netdev_state[1].tx_enabled = true;
		tracker.netdev_state[1].link_up = true;
		break;
	default:
		mlx5_core_warn(ldev->pf[0].dev, "Invalid affinity port %d",
			       port);
		return;
	}

	if (tracker.netdev_state[0].tx_enabled)
		mlx5_notifier_call_chain(ldev->pf[0].dev->priv.events,
					 MLX5_DEV_EVENT_PORT_AFFINITY,
					 (void *)0);

	if (tracker.netdev_state[1].tx_enabled)
		mlx5_notifier_call_chain(ldev->pf[1].dev->priv.events,
					 MLX5_DEV_EVENT_PORT_AFFINITY,
					 (void *)0);

	mlx5_modify_lag(ldev, &tracker);
}

static void mlx5_lag_fib_event_flush(struct notifier_block *nb)
{
	struct lag_mp *mp = container_of(nb, struct lag_mp, fib_nb);

	flush_workqueue(mp->wq);
}

struct mlx5_fib_event_work {
	struct work_struct work;
	struct mlx5_lag *ldev;
	unsigned long event;
	union {
		struct fib_entry_notifier_info fen_info;
		struct fib_nh_notifier_info fnh_info;
	};
};

static void mlx5_lag_fib_route_event(struct mlx5_lag *ldev,
				     unsigned long event,
				     struct fib_info *fi)
{
	struct lag_mp *mp = &ldev->lag_mp;

	/* Handle delete event */
	if (event == FIB_EVENT_ENTRY_DEL) {
		/* stop track */
		if (mp->mfi == fi)
			mp->mfi = NULL;
		return;
	}

	/* Handle add/replace event */
	if (fi->fib_nhs == 1) {
		if (__mlx5_lag_is_active(ldev)) {
#ifdef HAVE_FIB_NH_DEV
			struct net_device *nh_dev = fi->fib_nh[0].fib_nh_dev;
#else
			struct net_device *nh_dev = fi->fib_nh[0].nh_dev;
#endif
			int i = mlx5_lag_dev_get_netdev_idx(ldev, nh_dev);

			mlx5_lag_set_port_affinity(ldev, ++i);
		}
		return;
	}

	if (fi->fib_nhs != 2)
		return;

	/* Verify next hops are ports of the same hca */
#ifdef HAVE_FIB_NH_DEV
	if (!(fi->fib_nh[0].fib_nh_dev == ldev->pf[0].netdev &&
	      fi->fib_nh[1].fib_nh_dev == ldev->pf[1].netdev) &&
	    !(fi->fib_nh[0].fib_nh_dev == ldev->pf[1].netdev &&
	      fi->fib_nh[1].fib_nh_dev == ldev->pf[0].netdev)) {
#else
	if (!(fi->fib_nh[0].nh_dev == ldev->pf[0].netdev &&
	      fi->fib_nh[1].nh_dev == ldev->pf[1].netdev) &&
	    !(fi->fib_nh[0].nh_dev == ldev->pf[1].netdev &&
	      fi->fib_nh[1].nh_dev == ldev->pf[0].netdev)) {
#endif
		mlx5_core_warn(ldev->pf[0].dev, "Multipath offload require two ports of the same HCA\n");
		return;
	}

	/* First time we see multipath route */
	if (!mp->mfi && !__mlx5_lag_is_active(ldev)) {
		struct lag_tracker tracker;

		tracker = ldev->tracker;
		mlx5_activate_lag(ldev, &tracker, MLX5_LAG_FLAG_MULTIPATH, false);
	}

	mlx5_lag_set_port_affinity(ldev, 0);
	mp->mfi = fi;
}

static void mlx5_lag_fib_nexthop_event(struct mlx5_lag *ldev,
				       unsigned long event,
				       struct fib_nh *fib_nh,
				       struct fib_info *fi)
{
	struct lag_mp *mp = &ldev->lag_mp;

	/* Check the nh event is related to the route */
	if (!mp->mfi || mp->mfi != fi)
		return;

	/* nh added/removed */
	if (event == FIB_EVENT_NH_DEL) {
#ifdef HAVE_FIB_NH_DEV
		int i = mlx5_lag_dev_get_netdev_idx(ldev, fib_nh->fib_nh_dev);
#else
		int i = mlx5_lag_dev_get_netdev_idx(ldev, fib_nh->nh_dev);
#endif

		if (i >= 0) {
			i = (i + 1) % 2 + 1; /* peer port */
			mlx5_lag_set_port_affinity(ldev, i);
		}
	} else if (event == FIB_EVENT_NH_ADD &&
		   fi->fib_nhs == 2) {
		mlx5_lag_set_port_affinity(ldev, 0);
	}
}

static void mlx5_lag_fib_update(struct work_struct *work)
{
	struct mlx5_fib_event_work *fib_work =
		container_of(work, struct mlx5_fib_event_work, work);
	struct mlx5_lag *ldev = fib_work->ldev;
	struct fib_nh *fib_nh;

	/* Protect internal structures from changes */
	rtnl_lock();
	switch (fib_work->event) {
	case FIB_EVENT_ENTRY_REPLACE: /* fall through */
	case FIB_EVENT_ENTRY_APPEND: /* fall through */
	case FIB_EVENT_ENTRY_ADD: /* fall through */
	case FIB_EVENT_ENTRY_DEL:
		mlx5_lag_fib_route_event(ldev, fib_work->event,
					 fib_work->fen_info.fi);
		fib_info_put(fib_work->fen_info.fi);
		break;
	case FIB_EVENT_NH_ADD: /* fall through */
	case FIB_EVENT_NH_DEL:
		fib_nh = fib_work->fnh_info.fib_nh;
		mlx5_lag_fib_nexthop_event(ldev,
					   fib_work->event,
					   fib_work->fnh_info.fib_nh,
					   fib_nh->nh_parent);
		fib_info_put(fib_work->fnh_info.fib_nh->nh_parent);
		break;
	}

	rtnl_unlock();
	kfree(fib_work);
}

static struct mlx5_fib_event_work *
mlx5_lag_init_fib_work(struct mlx5_lag *ldev, unsigned long event)
{
	struct mlx5_fib_event_work *fib_work;

	fib_work = kzalloc(sizeof(*fib_work), GFP_ATOMIC);
	if (WARN_ON(!fib_work))
		return NULL;

	INIT_WORK(&fib_work->work, mlx5_lag_fib_update);
	fib_work->ldev = ldev;
	fib_work->event = event;

	return fib_work;
}

static int mlx5_lag_fib_event(struct notifier_block *nb,
			      unsigned long event,
			      void *ptr)
{
	struct lag_mp *mp = container_of(nb, struct lag_mp, fib_nb);
	struct mlx5_lag *ldev = container_of(mp, struct mlx5_lag, lag_mp);
	struct fib_notifier_info *info = ptr;
	struct mlx5_fib_event_work *fib_work;
	struct fib_entry_notifier_info *fen_info;
	struct fib_nh_notifier_info *fnh_info;
	struct fib_info *fi;
#ifdef HAVE_FIB_INFO_NH
	struct net_device *fib_dev;
#endif

	if (info->family != AF_INET)
		return NOTIFY_DONE;

	if (!mlx5_lag_multipath_check_prereq(ldev))
		return NOTIFY_DONE;

	switch (event) {
	case FIB_EVENT_ENTRY_REPLACE: /* fall through */
	case FIB_EVENT_ENTRY_APPEND: /* fall through */
	case FIB_EVENT_ENTRY_ADD: /* fall through */
	case FIB_EVENT_ENTRY_DEL:
		fen_info = container_of(info, struct fib_entry_notifier_info,
					info);
		fi = fen_info->fi;
#ifdef HAVE_FIB_INFO_NH
		fib_dev = fib_info_nh(fen_info->fi, 0)->fib_nh_dev;
		if (fib_dev != ldev->pf[0].netdev &&
		    fib_dev != ldev->pf[1].netdev) {
#else
		if (fi->fib_dev != ldev->pf[0].netdev &&
		    fi->fib_dev != ldev->pf[1].netdev) {
#endif
			return NOTIFY_DONE;
		}
		fib_work = mlx5_lag_init_fib_work(ldev, event);
		if (!fib_work)
			return NOTIFY_DONE;
		fib_work->fen_info = *fen_info;
		/* Take reference on fib_info to prevent it from being
		 * freed while work is queued. Release it afterwards.
		 */
		fib_info_hold(fib_work->fen_info.fi);
		break;
	case FIB_EVENT_NH_ADD: /* fall through */
	case FIB_EVENT_NH_DEL:
		fnh_info = container_of(info, struct fib_nh_notifier_info,
					info);
		fib_work = mlx5_lag_init_fib_work(ldev, event);
		if (!fib_work)
			return NOTIFY_DONE;
		fib_work->fnh_info = *fnh_info;
		fib_info_hold(fib_work->fnh_info.fib_nh->nh_parent);
		break;
	default:
		return NOTIFY_DONE;
	}

	queue_work(mp->wq, &fib_work->work);

	return NOTIFY_DONE;
}

int mlx5_lag_mp_init(struct mlx5_lag *ldev)
{
	struct lag_mp *mp = &ldev->lag_mp;
	int err;

	if (mp->fib_nb.notifier_call)
		return 0;

	mp->wq = create_singlethread_workqueue("mlx5_lag_mp");
	if (!mp->wq)
		return -ENOMEM;

	mp->fib_nb.notifier_call = mlx5_lag_fib_event;
#ifdef HAVE_REGISTER_FIB_NOTIFIER_HAS_4_PARAMS
	err = register_fib_notifier(&init_net, &mp->fib_nb,
				    mlx5_lag_fib_event_flush, NULL);
#else
	err = register_fib_notifier(&mp->fib_nb,
				    mlx5_lag_fib_event_flush);
#endif
	if (err) {
		destroy_workqueue(mp->wq);
		mp->fib_nb.notifier_call = NULL;
	}

	return err;
}

void mlx5_lag_mp_cleanup(struct mlx5_lag *ldev)
{
	struct lag_mp *mp = &ldev->lag_mp;

	if (!mp->fib_nb.notifier_call)
		return;

#ifdef HAVE_REGISTER_FIB_NOTIFIER_HAS_4_PARAMS
	unregister_fib_notifier(&init_net, &mp->fib_nb);
#else
	unregister_fib_notifier(&mp->fib_nb);
#endif
	destroy_workqueue(mp->wq);
	mp->fib_nb.notifier_call = NULL;
}
#endif /* HAVE_FIB_NH_NOTIFIER_INFO */
#endif /* MLX_LAG_SUPPORTED */
