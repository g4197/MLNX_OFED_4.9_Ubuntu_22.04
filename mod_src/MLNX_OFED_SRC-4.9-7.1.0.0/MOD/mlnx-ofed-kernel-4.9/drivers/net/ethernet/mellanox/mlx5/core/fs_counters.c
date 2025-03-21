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

#include <linux/mlx5/driver.h>
#include <linux/mlx5/fs.h>
#include <linux/rbtree.h>
#include "mlx5_core.h"
#include "fs_core.h"
#include "fs_cmd.h"

#define MLX5_FC_STATS_PERIOD msecs_to_jiffies(1000)
#define MLX5_FC_BULK_QUERY_ALLOC_PERIOD msecs_to_jiffies(180 * 1000)
/* Max number of counters to query in bulk read is 32K */
#define MLX5_SW_MAX_COUNTERS_BULK BIT(15)
#define MLX5_INIT_COUNTERS_BULK 8
#define MLX5_FC_POOL_MAX_THRESHOLD BIT(18)
#define MLX5_FC_POOL_USED_BUFF_RATIO 10

struct mlx5_fc_cache {
	u64 packets;
	u64 bytes;
	u64 lastuse;
};

struct mlx5_fc {
	struct list_head list;
	struct llist_node addlist;
	struct llist_node dellist;

	/* last{packets,bytes} members are used when calculating the delta since
	 * last reading
	 */
	u64 lastpackets;
	u64 lastbytes;

	struct mlx5_fc_bulk *bulk;
	u32 id;
	bool aging;
	bool dummy;

	atomic_t nr_dummies;
	struct mlx5_fc *dummies[MINIFLOW_MAX_FLOWS];

	struct mlx5_fc_cache cache ____cacheline_aligned_in_smp;
};

static void mlx5_fc_pool_init(struct mlx5_fc_pool *fc_pool, struct mlx5_core_dev *dev);
static void mlx5_fc_pool_cleanup(struct mlx5_fc_pool *fc_pool);
static struct mlx5_fc *mlx5_fc_pool_acquire_counter(struct mlx5_fc_pool *fc_pool);
static void mlx5_fc_pool_release_counter(struct mlx5_fc_pool *fc_pool, struct mlx5_fc *fc);

/* locking scheme:
 *
 * It is the responsibility of the user to prevent concurrent calls or bad
 * ordering to mlx5_fc_create(), mlx5_fc_destroy() and accessing a reference
 * to struct mlx5_fc.
 * e.g en_tc.c is protected by RTNL lock of its caller, and will never call a
 * dump (access to struct mlx5_fc) after a counter is destroyed.
 *
 * access to counter list:
 * - create (user context)
 *   - mlx5_fc_create() only adds to an addlist to be used by
 *     mlx5_fc_stats_work(). addlist is a lockless single linked list
 *     that doesn't require any additional synchronization when adding single
 *     node.
 *   - spawn thread to do the actual destroy
 *
 * - destroy (user context)
 *   - add a counter to lockless dellist
 *   - spawn thread to do the actual del
 *
 * - dump (user context)
 *   user should not call dump after destroy
 *
 * - query (single thread workqueue context)
 *   destroy/dump - no conflict (see destroy)
 *   query/dump - packets and bytes might be inconsistent (since update is not
 *                atomic)
 *   query/create - no conflict (see create)
 *   since every create/destroy spawn the work, only after necessary time has
 *   elapsed, the thread will actually query the hardware.
 */

#if defined(HAVE_IDR_RT)
#define USE_IDR 1
#elif defined(HAVE_IDR_GET_NEXT_EXPORTED) && defined(HAVE_IDR_ALLOC)
/* for now, we want to use this if it's original kernel function and
 * we don't define idr_* funcs ourselves, so it will be fast. */
void *idr_get_next_ul(struct idr *idr, unsigned long *nextid)
{
	int next = (int) *nextid;
	void *ret;

	ret = idr_get_next(idr, &next);
	*nextid = (unsigned long) next;

	return ret;
}
int idr_alloc_u32(struct idr *idr, void *ptr, u32 *nextid,
		  unsigned long max, gfp_t gfp)
{
	int err = idr_alloc(idr, ptr, *nextid, max + 1, gfp);

	if (err < 0)
		return err;

	*nextid = err;

	return 0;
}
#define USE_IDR 1
#endif

static struct list_head *mlx5_fc_counters_lookup_next(struct mlx5_core_dev *dev,
						      u32 id)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
#ifdef USE_IDR
	unsigned long next_id = (unsigned long)id + 1;
#endif
	struct mlx5_fc *counter;

#ifdef USE_IDR
	rcu_read_lock();
	/* skip counters that are in idr, but not yet in counters list */
	while ((counter = idr_get_next_ul(&fc_stats->counters_idr,
					  &next_id)) != NULL &&
	       list_empty(&counter->list))
		next_id++;
	rcu_read_unlock();
#else
	list_for_each_entry(counter, &fc_stats->counters, list)
		if (counter->id > id)
			return &counter->list;
#endif
#ifdef USE_IDR
	return counter ? &counter->list : &fc_stats->counters;
#else
	return &fc_stats->counters;
#endif
}

static void mlx5_fc_stats_insert(struct mlx5_core_dev *dev,
				 struct mlx5_fc *counter)
{
	struct list_head *next = mlx5_fc_counters_lookup_next(dev, counter->id);

	list_add_tail(&counter->list, next);
}

static void mlx5_fc_stats_remove(struct mlx5_core_dev *dev,
				 struct mlx5_fc *counter)
{
#ifdef USE_IDR
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
#endif

	list_del(&counter->list);

#ifdef USE_IDR
	spin_lock(&fc_stats->counters_idr_lock);
#ifdef HAVE_IDR_REMOVE_RETURN_VALUE 
	WARN_ON(!idr_remove(&fc_stats->counters_idr, counter->id));
#else
	idr_remove(&fc_stats->counters_idr, counter->id);
#endif
	spin_unlock(&fc_stats->counters_idr_lock);
#endif/*USE_IDR*/
}

static void fc_dummies_update(struct mlx5_fc *counter,
			      u64 dfpackets, u64 dfbytes, u64 jiffies)
{
	int nr_dummies = atomic_read(&counter->nr_dummies);
	struct mlx5_fc_cache *c;
	int i;

	for (i = 0; i < nr_dummies; i++) {
		struct mlx5_fc *dummy = counter->dummies[i];
		if (!dummy)
			continue;

		c = &dummy->cache;
		c->packets += dfpackets;
		c->bytes += dfbytes;
		c->lastuse = jiffies;
	}
}

static int get_init_bulk_query_len(struct mlx5_core_dev *dev)
{
	return min_t(int, MLX5_INIT_COUNTERS_BULK,
		     (1 << MLX5_CAP_GEN(dev, log_max_flow_counter_bulk)));
}

static int get_max_bulk_query_len(struct mlx5_core_dev *dev)
{
	return min_t(int, MLX5_SW_MAX_COUNTERS_BULK,
			  (1 << MLX5_CAP_GEN(dev, log_max_flow_counter_bulk)));
}


static void update_counter_cache(struct mlx5_fc *counter,
				 int index, u32 *bulk_raw_data,
				 struct mlx5_fc_cache *cache)
{
	void *stats = MLX5_ADDR_OF(query_flow_counter_out, bulk_raw_data,
			     flow_statistics[index]);
	u64 packets = MLX5_GET64(traffic_counter, stats, packets);
	u64 bytes = MLX5_GET64(traffic_counter, stats, octets);
	u64 dfpackets, dfbytes;

	if (cache->packets == packets)
		return;

	dfpackets = packets - cache->packets;
	dfbytes = bytes - cache->bytes;

	cache->packets = packets;
	cache->bytes = bytes;
	cache->lastuse = jiffies;

	fc_dummies_update(counter, dfpackets, dfbytes, jiffies);
}

static void mlx5_fc_stats_query_counter_range(struct mlx5_core_dev *dev,
					      struct mlx5_fc *first,
					      u32 last_id)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
	bool query_more_counters = (first->id <= last_id);
	int cur_bulk_len = fc_stats->bulk_query_len;
	u32 *data = fc_stats->bulk_query_out;
	struct mlx5_fc *counter = first;
	u32 bulk_base_id;
	int bulk_len;
	int err;

	while (query_more_counters) {
		/* first id must be aligned to 4 when using bulk query */
		bulk_base_id = counter->id & ~0x3;

		/* number of counters to query inc. the last counter */
		bulk_len = min_t(int, cur_bulk_len,
				 ALIGN(last_id - bulk_base_id + 1, 4));

		err = mlx5_cmd_fc_bulk_query(dev, bulk_base_id, bulk_len,
					     data);
		if (err) {
			mlx5_core_err(dev, "Error doing bulk query: %d\n", err);
			return;
		}
		query_more_counters = false;

		list_for_each_entry_from(counter, &fc_stats->counters, list) {
			int counter_index = counter->id - bulk_base_id;
			struct mlx5_fc_cache *cache = &counter->cache;

			if (counter->id >= bulk_base_id + bulk_len) {
				query_more_counters = true;
				break;
			}

			update_counter_cache(counter, counter_index, data, cache);
		}
	}
}

static void mlx5_fc_free(struct mlx5_core_dev *dev, struct mlx5_fc *counter)
{
	mlx5_cmd_fc_free(dev, counter->id);
	kfree(counter);
}

static void mlx5_fc_release(struct mlx5_core_dev *dev, struct mlx5_fc *counter)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;

	if (counter->bulk)
		mlx5_fc_pool_release_counter(&fc_stats->fc_pool, counter);
	else
		mlx5_fc_free(dev, counter);
}

static void mlx5_fc_stats_bulk_query_size_increase(struct mlx5_core_dev *dev)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
	int max_bulk_len = get_max_bulk_query_len(dev);
	unsigned long now = jiffies;
	u32 *bulk_query_out_tmp;
	int max_out_len;

	if (fc_stats->bulk_query_alloc_failed &&
	    time_before(now, fc_stats->next_bulk_query_alloc))
		return;

	max_out_len = mlx5_cmd_fc_get_bulk_query_out_len(max_bulk_len);
	bulk_query_out_tmp = kzalloc(max_out_len, GFP_KERNEL);
	if (!bulk_query_out_tmp) {
		mlx5_core_warn_once(dev,
				    "Can't increase flow counters bulk query buffer size, insufficient memory, bulk_size(%d)\n",
				    max_bulk_len);
		fc_stats->bulk_query_alloc_failed = true;
		fc_stats->next_bulk_query_alloc =
			now + MLX5_FC_BULK_QUERY_ALLOC_PERIOD;
		return;
	}

	kfree(fc_stats->bulk_query_out);
	fc_stats->bulk_query_out = bulk_query_out_tmp;
	fc_stats->bulk_query_len = max_bulk_len;
	if (fc_stats->bulk_query_alloc_failed) {
		mlx5_core_info(dev,
			       "Flow counters bulk query buffer size increased, bulk_size(%d)\n",
			       max_bulk_len);
		fc_stats->bulk_query_alloc_failed = false;
	}
}

static void mlx5_fc_stats_work(struct work_struct *work)
{
	struct mlx5_core_dev *dev = container_of(work, struct mlx5_core_dev,
						 priv.fc_stats.work.work);
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
	/* Take dellist first to ensure that counters cannot be deleted before
	 * they are inserted.
	 */
	struct llist_node *dellist = llist_del_all(&fc_stats->dellist);
	struct llist_node *addlist = llist_del_all(&fc_stats->addlist);
	struct mlx5_fc *counter = NULL, *last = NULL, *tmp;
	unsigned long now = jiffies;

	if (addlist || !list_empty(&fc_stats->counters))
		queue_delayed_work(fc_stats->wq, &fc_stats->work,
				   fc_stats->sampling_interval);

	llist_for_each_entry(counter, addlist, addlist) {
		mlx5_fc_stats_insert(dev, counter);
		fc_stats->num_counters++;
	}

	llist_for_each_entry_safe(counter, tmp, dellist, dellist) {
		if (counter->dummy) {
			mlx5_fc_free_dummy_counter(counter);
			continue;
		}

		mlx5_fc_stats_remove(dev, counter);
		mlx5_fc_release(dev, counter);
		fc_stats->num_counters--;
	}

	if (fc_stats->bulk_query_len < get_max_bulk_query_len(dev) &&
	    fc_stats->num_counters > get_init_bulk_query_len(dev))
		mlx5_fc_stats_bulk_query_size_increase(dev);

	if (time_before(now, fc_stats->next_query) ||
	    list_empty(&fc_stats->counters))
		return;
	last = list_last_entry(&fc_stats->counters, struct mlx5_fc, list);

	counter = list_first_entry(&fc_stats->counters, struct mlx5_fc,
				   list);
	if (counter)
		mlx5_fc_stats_query_counter_range(dev, counter, last->id);

	fc_stats->next_query = now + fc_stats->sampling_interval;
}

static struct mlx5_fc *mlx5_fc_single_alloc(struct mlx5_core_dev *dev)
{
	struct mlx5_fc *counter;
	int err;

	counter = kzalloc(sizeof(*counter), GFP_KERNEL);
	if (!counter)
		return ERR_PTR(-ENOMEM);

	err = mlx5_cmd_fc_alloc(dev, &counter->id);
	if (err) {
		kfree(counter);
		return ERR_PTR(err);
	}

	return counter;
}

static struct mlx5_fc *mlx5_fc_acquire(struct mlx5_core_dev *dev, bool aging)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
	struct mlx5_fc *counter;

	if (aging && MLX5_CAP_GEN(dev, flow_counter_bulk_alloc) != 0) {
		counter = mlx5_fc_pool_acquire_counter(&fc_stats->fc_pool);
		if (!IS_ERR(counter))
			return counter;
	}

	return mlx5_fc_single_alloc(dev);
}

struct mlx5_fc *mlx5_fc_create(struct mlx5_core_dev *dev, bool aging)
{
	struct mlx5_fc *counter = mlx5_fc_acquire(dev, aging);
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
#ifdef USE_IDR
	int err;
#endif

	if (IS_ERR(counter))
		return counter;

#ifdef USE_IDR
	INIT_LIST_HEAD(&counter->list);
#endif
	counter->aging = aging;

	if (aging) {
#ifdef USE_IDR
       	u32 id = counter->id;
#endif

		counter->cache.lastuse = jiffies;
		counter->lastbytes = counter->cache.bytes;
		counter->lastpackets = counter->cache.packets;

#ifdef USE_IDR
		idr_preload(GFP_KERNEL);
		spin_lock(&fc_stats->counters_idr_lock);

		err = idr_alloc_u32(&fc_stats->counters_idr, counter, &id, id,
				    GFP_NOWAIT);

		spin_unlock(&fc_stats->counters_idr_lock);
		idr_preload_end();
		if (err)
			goto err_out_alloc;
#endif
		llist_add(&counter->addlist, &fc_stats->addlist);

		mod_delayed_work(fc_stats->wq, &fc_stats->work, 0);
	}

	return counter;

#ifdef USE_IDR
err_out_alloc:
	mlx5_fc_release(dev, counter);
	return ERR_PTR(err);
#endif
}
EXPORT_SYMBOL(mlx5_fc_create);

u32 mlx5_fc_id(struct mlx5_fc *counter)
{
	return counter->id;
}
EXPORT_SYMBOL(mlx5_fc_id);

void mlx5_fc_link_dummies(struct mlx5_fc *counter, struct mlx5_fc **dummies, int nr_dummies)
{
	BUG_ON(nr_dummies > MINIFLOW_MAX_FLOWS);
	memcpy(counter->dummies, dummies, sizeof(*dummies) * nr_dummies);
	atomic_set(&counter->nr_dummies, nr_dummies);
}

void mlx5_fc_unlink_dummies(struct mlx5_fc *counter)
{
	atomic_set(&counter->nr_dummies, 0);
}

struct mlx5_fc *mlx5_fc_alloc_dummy_counter(void)
{
	struct mlx5_fc *counter;

	counter = kzalloc(sizeof(*counter), GFP_ATOMIC);
	if (!counter)
		return NULL;

	counter->dummy = true;
	counter->cache.lastuse = jiffies;
	counter->aging = true;

	return counter;
}

void mlx5_fc_free_dummy_counter(struct mlx5_fc *counter)
{
	kfree(counter);
}

void mlx5_fc_destroy(struct mlx5_core_dev *dev, struct mlx5_fc *counter)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;

	if (!counter)
		return;

	if (counter->aging) {
		llist_add(&counter->dellist, &fc_stats->dellist);
		return;
	}

	mlx5_fc_release(dev, counter);
}
EXPORT_SYMBOL(mlx5_fc_destroy);

int mlx5_init_fc_stats(struct mlx5_core_dev *dev)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
	int init_bulk_len;
	int init_out_len;

#ifdef USE_IDR
	spin_lock_init(&fc_stats->counters_idr_lock);
	idr_init(&fc_stats->counters_idr);
#endif
	INIT_LIST_HEAD(&fc_stats->counters);
	init_llist_head(&fc_stats->addlist);
	init_llist_head(&fc_stats->dellist);

	init_bulk_len = get_init_bulk_query_len(dev);
	init_out_len = mlx5_cmd_fc_get_bulk_query_out_len(init_bulk_len);
	fc_stats->bulk_query_out = kzalloc(init_out_len, GFP_KERNEL);
	if (!fc_stats->bulk_query_out)
		return -ENOMEM;
	fc_stats->bulk_query_len = init_bulk_len;

	fc_stats->wq = create_singlethread_workqueue("mlx5_fc");
	if (!fc_stats->wq)
		goto err_wq_create;

	fc_stats->sampling_interval = MLX5_FC_STATS_PERIOD;
	INIT_DELAYED_WORK(&fc_stats->work, mlx5_fc_stats_work);

	mlx5_fc_pool_init(&fc_stats->fc_pool, dev);
	return 0;

err_wq_create:
	kfree(fc_stats->bulk_query_out);
	return -ENOMEM;
}

void mlx5_cleanup_fc_stats(struct mlx5_core_dev *dev)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;
	struct llist_node *tmplist;
	struct mlx5_fc *counter;
	struct mlx5_fc *tmp;

	cancel_delayed_work_sync(&dev->priv.fc_stats.work);
	destroy_workqueue(dev->priv.fc_stats.wq);
	dev->priv.fc_stats.wq = NULL;

	tmplist = llist_del_all(&fc_stats->addlist);
	llist_for_each_entry_safe(counter, tmp, tmplist, addlist)
		mlx5_fc_release(dev, counter);

	list_for_each_entry_safe(counter, tmp, &fc_stats->counters, list)
		mlx5_fc_release(dev, counter);

	mlx5_fc_pool_cleanup(&fc_stats->fc_pool);
#ifdef USE_IDR
	idr_destroy(&fc_stats->counters_idr);
#endif
	kfree(fc_stats->bulk_query_out);
}

int mlx5_fc_query(struct mlx5_core_dev *dev, struct mlx5_fc *counter,
		  u64 *packets, u64 *bytes)
{
	return mlx5_cmd_fc_query(dev, counter->id, packets, bytes);
}
EXPORT_SYMBOL(mlx5_fc_query);

void mlx5_fc_query_cached(struct mlx5_fc *counter,
			  u64 *bytes, u64 *packets, u64 *lastuse)
{
	struct mlx5_fc_cache c;

	c = counter->cache;

	*bytes = c.bytes - counter->lastbytes;
	*packets = c.packets - counter->lastpackets;
	*lastuse = c.lastuse;

	counter->lastbytes = c.bytes;
	counter->lastpackets = c.packets;
}

#ifdef HAVE_TCF_TUNNEL_INFO
void mlx5_fc_queue_stats_work(struct mlx5_core_dev *dev,
			      struct delayed_work *dwork,
			      unsigned long delay)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;

	queue_delayed_work(fc_stats->wq, dwork, delay);
}

void mlx5_fc_update_sampling_interval(struct mlx5_core_dev *dev,
				      unsigned long interval)
{
	struct mlx5_fc_stats *fc_stats = &dev->priv.fc_stats;

	fc_stats->sampling_interval = min_t(unsigned long, interval,
					    fc_stats->sampling_interval);
}
#endif
/* Flow counter bluks */

struct mlx5_fc_bulk {
	struct list_head pool_list;
	u32 base_id;
	int bulk_len;
	unsigned long *bitmask;
	struct mlx5_fc fcs[0];
};

static void mlx5_fc_init(struct mlx5_fc *counter, struct mlx5_fc_bulk *bulk,
			 u32 id)
{
	counter->bulk = bulk;
	counter->id = id;
}

static int mlx5_fc_bulk_get_free_fcs_amount(struct mlx5_fc_bulk *bulk)
{
	return bitmap_weight(bulk->bitmask, bulk->bulk_len);
}

static struct mlx5_fc_bulk *mlx5_fc_bulk_create(struct mlx5_core_dev *dev)
{
	enum mlx5_fc_bulk_alloc_bitmask alloc_bitmask;
	struct mlx5_fc_bulk *bulk;
	int err = -ENOMEM;
	int bulk_len;
	u32 base_id;
	int i;

	alloc_bitmask = MLX5_CAP_GEN(dev, flow_counter_bulk_alloc);
	bulk_len = alloc_bitmask > 0 ? MLX5_FC_BULK_NUM_FCS(alloc_bitmask) : 1;

	bulk = kzalloc(sizeof(*bulk) + bulk_len * sizeof(struct mlx5_fc),
		       GFP_KERNEL);
	if (!bulk)
		goto err_alloc_bulk;

	bulk->bitmask = kcalloc(BITS_TO_LONGS(bulk_len), sizeof(unsigned long),
				GFP_KERNEL);
	if (!bulk->bitmask)
		goto err_alloc_bitmask;

	err = mlx5_cmd_fc_bulk_alloc(dev, alloc_bitmask, &base_id);
	if (err)
		goto err_mlx5_cmd_bulk_alloc;

	bulk->base_id = base_id;
	bulk->bulk_len = bulk_len;
	for (i = 0; i < bulk_len; i++) {
		mlx5_fc_init(&bulk->fcs[i], bulk, base_id + i);
		set_bit(i, bulk->bitmask);
	}

	return bulk;

err_mlx5_cmd_bulk_alloc:
	kfree(bulk->bitmask);
err_alloc_bitmask:
	kfree(bulk);
err_alloc_bulk:
	return ERR_PTR(err);
}

static int
mlx5_fc_bulk_destroy(struct mlx5_core_dev *dev, struct mlx5_fc_bulk *bulk)
{
	if (mlx5_fc_bulk_get_free_fcs_amount(bulk) < bulk->bulk_len) {
		mlx5_core_err(dev, "Freeing bulk before all counters were released\n");
		return -EBUSY;
	}

	mlx5_cmd_fc_free(dev, bulk->base_id);
	kfree(bulk->bitmask);
	kfree(bulk);

	return 0;
}

static struct mlx5_fc *mlx5_fc_bulk_acquire_fc(struct mlx5_fc_bulk *bulk)
{
	int free_fc_index = find_first_bit(bulk->bitmask, bulk->bulk_len);

	if (free_fc_index >= bulk->bulk_len)
		return ERR_PTR(-ENOSPC);

	clear_bit(free_fc_index, bulk->bitmask);
	return &bulk->fcs[free_fc_index];
}

static int mlx5_fc_bulk_release_fc(struct mlx5_fc_bulk *bulk, struct mlx5_fc *fc)
{
	int fc_index = fc->id - bulk->base_id;

	if (test_bit(fc_index, bulk->bitmask))
		return -EINVAL;

	set_bit(fc_index, bulk->bitmask);
	return 0;
}

/* Flow counters pool API */

static void mlx5_fc_pool_init(struct mlx5_fc_pool *fc_pool, struct mlx5_core_dev *dev)
{
	fc_pool->dev = dev;
	mutex_init(&fc_pool->pool_lock);
	INIT_LIST_HEAD(&fc_pool->fully_used);
	INIT_LIST_HEAD(&fc_pool->partially_used);
	INIT_LIST_HEAD(&fc_pool->unused);
	fc_pool->available_fcs = 0;
	fc_pool->used_fcs = 0;
	fc_pool->threshold = 0;
}

static void mlx5_fc_pool_cleanup(struct mlx5_fc_pool *fc_pool)
{
	struct mlx5_core_dev *dev = fc_pool->dev;
	struct mlx5_fc_bulk *bulk;
	struct mlx5_fc_bulk *tmp;

	list_for_each_entry_safe(bulk, tmp, &fc_pool->fully_used, pool_list)
		mlx5_fc_bulk_destroy(dev, bulk);
	list_for_each_entry_safe(bulk, tmp, &fc_pool->partially_used, pool_list)
		mlx5_fc_bulk_destroy(dev, bulk);
	list_for_each_entry_safe(bulk, tmp, &fc_pool->unused, pool_list)
		mlx5_fc_bulk_destroy(dev, bulk);
}

static void mlx5_fc_pool_update_threshold(struct mlx5_fc_pool *fc_pool)
{
	fc_pool->threshold = min_t(int, MLX5_FC_POOL_MAX_THRESHOLD,
				   fc_pool->used_fcs / MLX5_FC_POOL_USED_BUFF_RATIO);
}

static struct mlx5_fc_bulk *
mlx5_fc_pool_alloc_new_bulk(struct mlx5_fc_pool *fc_pool)
{
	struct mlx5_core_dev *dev = fc_pool->dev;
	struct mlx5_fc_bulk *new_bulk;

	new_bulk = mlx5_fc_bulk_create(dev);
	if (!IS_ERR(new_bulk))
		fc_pool->available_fcs += new_bulk->bulk_len;
	mlx5_fc_pool_update_threshold(fc_pool);
	return new_bulk;
}

static void
mlx5_fc_pool_free_bulk(struct mlx5_fc_pool *fc_pool, struct mlx5_fc_bulk *bulk)
{
	struct mlx5_core_dev *dev = fc_pool->dev;

	fc_pool->available_fcs -= bulk->bulk_len;
	mlx5_fc_bulk_destroy(dev, bulk);
	mlx5_fc_pool_update_threshold(fc_pool);
}

static struct mlx5_fc *
mlx5_fc_pool_acquire_from_list(struct list_head *src_list,
			       struct list_head *next_list,
			       bool move_non_full_bulk)
{
	struct mlx5_fc_bulk *bulk;
	struct mlx5_fc *fc;

	if (list_empty(src_list))
		return ERR_PTR(-ENODATA);

	bulk = list_first_entry(src_list, struct mlx5_fc_bulk, pool_list);
	fc = mlx5_fc_bulk_acquire_fc(bulk);
	if (move_non_full_bulk || mlx5_fc_bulk_get_free_fcs_amount(bulk) == 0)
		list_move(&bulk->pool_list, next_list);
	return fc;
}

static struct mlx5_fc *
mlx5_fc_pool_acquire_counter(struct mlx5_fc_pool *fc_pool)
{
	struct mlx5_fc_bulk *new_bulk;
	struct mlx5_fc *fc;

	mutex_lock(&fc_pool->pool_lock);

	fc = mlx5_fc_pool_acquire_from_list(&fc_pool->partially_used,
					    &fc_pool->fully_used, false);
	if (IS_ERR(fc))
		fc = mlx5_fc_pool_acquire_from_list(&fc_pool->unused,
						    &fc_pool->partially_used,
						    true);
	if (IS_ERR(fc)) {
		new_bulk = mlx5_fc_pool_alloc_new_bulk(fc_pool);
		if (IS_ERR(new_bulk)) {
			fc = ERR_CAST(new_bulk);
			goto out;
		}
		fc = mlx5_fc_bulk_acquire_fc(new_bulk);
		list_add(&new_bulk->pool_list, &fc_pool->partially_used);
	}
	fc_pool->available_fcs--;
	fc_pool->used_fcs++;

out:
	mutex_unlock(&fc_pool->pool_lock);
	return fc;
}

static void
mlx5_fc_pool_release_counter(struct mlx5_fc_pool *fc_pool, struct mlx5_fc *fc)
{
	struct mlx5_core_dev *dev = fc_pool->dev;
	struct mlx5_fc_bulk *bulk = fc->bulk;
	int bulk_free_fcs_amount;

	mutex_lock(&fc_pool->pool_lock);

	if (mlx5_fc_bulk_release_fc(bulk, fc)) {
		mlx5_core_warn(dev, "Attempted to release a counter which is not acquired\n");
		goto unlock;
	}

	fc_pool->available_fcs++;
	fc_pool->used_fcs--;

	bulk_free_fcs_amount = mlx5_fc_bulk_get_free_fcs_amount(bulk);
	if (bulk_free_fcs_amount == 1)
		list_move_tail(&bulk->pool_list, &fc_pool->partially_used);
	if (bulk_free_fcs_amount == bulk->bulk_len) {
		list_del(&bulk->pool_list);
		if (fc_pool->available_fcs > fc_pool->threshold)
			mlx5_fc_pool_free_bulk(fc_pool, bulk);
		else
			list_add(&bulk->pool_list, &fc_pool->unused);
	}

unlock:
	mutex_unlock(&fc_pool->pool_lock);
}
