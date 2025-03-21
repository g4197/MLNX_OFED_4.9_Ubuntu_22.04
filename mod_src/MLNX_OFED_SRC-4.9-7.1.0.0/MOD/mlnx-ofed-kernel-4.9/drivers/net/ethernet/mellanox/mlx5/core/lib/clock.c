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

#include <linux/clocksource.h>
#include <linux/highmem.h>
#include <rdma/mlx5-abi.h>
#include "lib/eq.h"
#include "en.h"
#include "clock.h"

#ifndef smp_store_mb
#define smp_store_mb set_mb
#endif

enum {
	MLX5_CYCLES_SHIFT	= 23
};

#ifdef HAVE_PTP_CLOCK_INFO_N_PINS
enum {
	MLX5_PIN_MODE_IN		= 0x0,
	MLX5_PIN_MODE_OUT		= 0x1,
};

enum {
	MLX5_OUT_PATTERN_PULSE		= 0x0,
	MLX5_OUT_PATTERN_PERIODIC	= 0x1,
};

enum {
	MLX5_EVENT_MODE_DISABLE	= 0x0,
	MLX5_EVENT_MODE_REPETETIVE	= 0x1,
	MLX5_EVENT_MODE_ONCE_TILL_ARM	= 0x2,
};

enum {
	MLX5_MTPPS_FS_ENABLE			= BIT(0x0),
	MLX5_MTPPS_FS_PATTERN			= BIT(0x2),
	MLX5_MTPPS_FS_PIN_MODE			= BIT(0x3),
	MLX5_MTPPS_FS_TIME_STAMP		= BIT(0x4),
	MLX5_MTPPS_FS_OUT_PULSE_DURATION	= BIT(0x5),
	MLX5_MTPPS_FS_ENH_OUT_PER_ADJ		= BIT(0x7),
};
#endif

static u64 read_internal_timer(const struct cyclecounter *cc)
{
	struct mlx5_clock *clock = container_of(cc, struct mlx5_clock, cycles);
	struct mlx5_core_dev *mdev = container_of(clock, struct mlx5_core_dev,
						  clock);
#ifdef HAVE_GETTIMEX64
	return mlx5_read_internal_timer(mdev, NULL) & cc->mask;
#else
	return mlx5_read_internal_timer(mdev) & cc->mask;
#endif
}

static void mlx5_update_clock_info_page(struct mlx5_core_dev *mdev)
{
	struct mlx5_ib_clock_info *clock_info = mdev->clock_info;
	struct mlx5_clock *clock = &mdev->clock;
#ifdef HAVE_SMP_LOAD_ACQUIRE
	u32 sign;
#endif

	if (!clock_info)
		return;

#ifdef HAVE_SMP_LOAD_ACQUIRE
	sign = smp_load_acquire(&clock_info->sign);
	smp_store_mb(clock_info->sign,
		     sign | MLX5_IB_CLOCK_INFO_KERNEL_UPDATING);
#else
	++clock_info->sign;
	smp_wmb(); /* make sure signature change visible to user space */
#endif

	clock_info->cycles = clock->tc.cycle_last;
	clock_info->mult   = clock->cycles.mult;
	clock_info->nsec   = clock->tc.nsec;
#ifdef HAVE_CYCLECOUNTER_CYC2NS_4_PARAMS
	clock_info->frac   = clock->tc.frac;
#endif

#ifdef HAVE_SMP_LOAD_ACQUIRE
	smp_store_release(&clock_info->sign,
			  sign + MLX5_IB_CLOCK_INFO_KERNEL_UPDATING * 2);
#else
	smp_wmb(); /* sync all clock_info with userspace */
	++clock_info->sign;
#endif
}

#if defined (HAVE_PTP_CLOCK_INFO_N_PINS) && defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
static void mlx5_pps_out(struct work_struct *work)
{
	struct mlx5_pps *pps_info = container_of(work, struct mlx5_pps,
						 out_work);
	struct mlx5_clock *clock = container_of(pps_info, struct mlx5_clock,
						pps_info);
	struct mlx5_core_dev *mdev = container_of(clock, struct mlx5_core_dev,
						  clock);
	u32 in[MLX5_ST_SZ_DW(mtpps_reg)] = {0};
	unsigned long flags;
	int i;

	for (i = 0; i < clock->ptp_info.n_pins; i++) {
		u64 tstart;

		write_seqlock_irqsave(&clock->lock, flags);
		tstart = clock->pps_info.start[i];
		clock->pps_info.start[i] = 0;
		write_sequnlock_irqrestore(&clock->lock, flags);
		if (!tstart)
			continue;

		MLX5_SET(mtpps_reg, in, pin, i);
		MLX5_SET64(mtpps_reg, in, time_stamp, tstart);
		MLX5_SET(mtpps_reg, in, field_select, MLX5_MTPPS_FS_TIME_STAMP);
		mlx5_set_mtpps(mdev, in, sizeof(in));
	}
}
#endif

static void mlx5_timestamp_overflow(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct mlx5_clock *clock = container_of(dwork, struct mlx5_clock,
						overflow_work);
	unsigned long flags;

	write_seqlock_irqsave(&clock->lock, flags);
	timecounter_read(&clock->tc);
	mlx5_update_clock_info_page(clock->mdev);
	write_sequnlock_irqrestore(&clock->lock, flags);
	schedule_delayed_work(&clock->overflow_work, clock->overflow_period);
}

#if defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
static int mlx5_ptp_settime(struct ptp_clock_info *ptp,
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
			    const struct timespec64 *ts)
#else
			    const struct timespec *ts)
#endif
{
	struct mlx5_clock *clock = container_of(ptp, struct mlx5_clock,
						 ptp_info);
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
	u64 ns = timespec64_to_ns(ts);
#else
	u64 ns = timespec_to_ns(ts);
#endif
	unsigned long flags;

	write_seqlock_irqsave(&clock->lock, flags);
	timecounter_init(&clock->tc, &clock->cycles, ns);
	mlx5_update_clock_info_page(clock->mdev);
	write_sequnlock_irqrestore(&clock->lock, flags);

	return 0;
}

#ifdef HAVE_GETTIMEX64
static int mlx5_ptp_gettimex(struct ptp_clock_info *ptp, struct timespec64 *ts,
			     struct ptp_system_timestamp *sts)
{
	struct mlx5_clock *clock = container_of(ptp, struct mlx5_clock,
						ptp_info);
	struct mlx5_core_dev *mdev = container_of(clock, struct mlx5_core_dev,
						  clock);
	unsigned long flags;
	u64 cycles, ns;

	write_seqlock_irqsave(&clock->lock, flags);
	cycles = mlx5_read_internal_timer(mdev, sts);
	ns = timecounter_cyc2time(&clock->tc, cycles);
	write_sequnlock_irqrestore(&clock->lock, flags);

	*ts = ns_to_timespec64(ns);

	return 0;
}
#else/*HAVE_GETTIMEX64*/
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
static int mlx5_ptp_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
#else
static int mlx5_ptp_gettime(struct ptp_clock_info *ptp, struct timespec *ts)
#endif
{
	struct mlx5_clock *clock = container_of(ptp, struct mlx5_clock,
			ptp_info);
	u64 ns;
	unsigned long flags;

	write_seqlock_irqsave(&clock->lock, flags);
	ns = timecounter_read(&clock->tc);
	write_sequnlock_irqrestore(&clock->lock, flags);

#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
	*ts = ns_to_timespec64(ns);
#else
	*ts = ns_to_timespec(ns);
#endif

	return 0;
}
#endif/*HAVE_GETTIMEX64*/

static int mlx5_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct mlx5_clock *clock = container_of(ptp, struct mlx5_clock,
						ptp_info);
	unsigned long flags;

	write_seqlock_irqsave(&clock->lock, flags);
	timecounter_adjtime(&clock->tc, delta);
	mlx5_update_clock_info_page(clock->mdev);
	write_sequnlock_irqrestore(&clock->lock, flags);

	return 0;
}

static int mlx5_ptp_adjfreq(struct ptp_clock_info *ptp, s32 delta)
{
	u64 adj;
	u32 diff;
	unsigned long flags;
	int neg_adj = 0;
	struct mlx5_clock *clock = container_of(ptp, struct mlx5_clock,
						ptp_info);

	if (delta < 0) {
		neg_adj = 1;
		delta = -delta;
	}

	adj = clock->nominal_c_mult;
	adj *= delta;
	diff = div_u64(adj, 1000000000ULL);

	write_seqlock_irqsave(&clock->lock, flags);
	timecounter_read(&clock->tc);
	clock->cycles.mult = neg_adj ? clock->nominal_c_mult - diff :
				       clock->nominal_c_mult + diff;
	mlx5_update_clock_info_page(clock->mdev);
	write_sequnlock_irqrestore(&clock->lock, flags);

	return 0;
}
#ifdef HAVE_PTP_CLOCK_INFO_N_PINS
#ifndef PTP_STRICT_FLAGS
#define PTP_STRICT_FLAGS   (1<<3)
#endif
#ifndef PTP_EXTTS_EDGES
#define PTP_EXTTS_EDGES    (PTP_RISING_EDGE | PTP_FALLING_EDGE)
#endif

#ifndef HAVE_PTP_FIND_PIN_UNLOCK
static int mlx5_ptp_find_pin(struct mlx5_clock *clock,
			     enum ptp_pin_function func,
			     unsigned int chan, int on)
{
	int i;

	if (on)
		return ptp_find_pin(clock->ptp, func, chan);

	for (i = 0; i < clock->ptp_info.n_pins; i++) {
		if (clock->ptp_info.pin_config[i].func == func &&
		    clock->ptp_info.pin_config[i].chan == chan)
			return i;
	}
	return -1;
}
#endif
static int mlx5_extts_configure(struct ptp_clock_info *ptp,
				struct ptp_clock_request *rq,
				int on)
{
	struct mlx5_clock *clock =
			container_of(ptp, struct mlx5_clock, ptp_info);
	struct mlx5_core_dev *mdev =
			container_of(clock, struct mlx5_core_dev, clock);
	u32 in[MLX5_ST_SZ_DW(mtpps_reg)] = {0};
	u32 field_select = 0;
	u8 pin_mode = 0;
	u8 pattern = 0;
	int pin = -1;
	int err = 0;

	if (!MLX5_PPS_CAP(mdev))
		return -EOPNOTSUPP;

	if (rq->extts.index >= clock->ptp_info.n_pins)
		return -EINVAL;

	if (on) {
#ifdef HAVE_PTP_FIND_PIN_UNLOCK
		pin = ptp_find_pin(clock->ptp, PTP_PF_EXTTS, rq->extts.index);
#else
		pin = mlx5_ptp_find_pin(clock, PTP_PF_EXTTS, rq->extts.index, on);
#endif
		if (pin < 0)
			return -EBUSY;
		pin_mode = MLX5_PIN_MODE_IN;
		pattern = !!(rq->extts.flags & PTP_FALLING_EDGE);
		field_select = MLX5_MTPPS_FS_PIN_MODE |
			       MLX5_MTPPS_FS_PATTERN |
			       MLX5_MTPPS_FS_ENABLE;
	} else {
		pin = rq->extts.index;
		field_select = MLX5_MTPPS_FS_ENABLE;
	}

	MLX5_SET(mtpps_reg, in, pin, pin);
	MLX5_SET(mtpps_reg, in, pin_mode, pin_mode);
	MLX5_SET(mtpps_reg, in, pattern, pattern);
	MLX5_SET(mtpps_reg, in, enable, on);
	MLX5_SET(mtpps_reg, in, field_select, field_select);

	err = mlx5_set_mtpps(mdev, in, sizeof(in));
	if (err)
		return err;

	return mlx5_set_mtppse(mdev, pin, 0,
			       MLX5_EVENT_MODE_REPETETIVE & on);
}

static int mlx5_perout_configure(struct ptp_clock_info *ptp,
				 struct ptp_clock_request *rq,
				 int on)
{
	struct mlx5_clock *clock =
			container_of(ptp, struct mlx5_clock, ptp_info);
	struct mlx5_core_dev *mdev =
			container_of(clock, struct mlx5_core_dev, clock);
	u32 in[MLX5_ST_SZ_DW(mtpps_reg)] = {0};
	u64 nsec_now, nsec_delta, time_stamp = 0;
	u64 cycles_now, cycles_delta;
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
	struct timespec64 ts;
#else
	struct timespec ts;
#endif
	unsigned long flags;
	u32 field_select = 0;
	u8 pin_mode = 0;
	u8 pattern = 0;
	int pin = -1;
	int err = 0;
	s64 ns;

	if (!MLX5_PPS_CAP(mdev))
		return -EOPNOTSUPP;

	if (rq->perout.index >= clock->ptp_info.n_pins)
		return -EINVAL;

	if (on) {
#ifdef HAVE_PTP_FIND_PIN_UNLOCK
		pin = ptp_find_pin(clock->ptp, PTP_PF_PEROUT,
				   rq->perout.index);
#else
		pin = mlx5_ptp_find_pin(clock, PTP_PF_PEROUT,
					rq->perout.index, on);
#endif
		if (pin < 0)
			return -EBUSY;

		pin_mode = MLX5_PIN_MODE_OUT;
		pattern = MLX5_OUT_PATTERN_PERIODIC;
		ts.tv_sec = rq->perout.period.sec;
		ts.tv_nsec = rq->perout.period.nsec;
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
		ns = timespec64_to_ns(&ts);
#else
		ns = timespec_to_ns(&ts);
#endif

		if ((ns >> 1) != 500000000LL)
			return -EINVAL;

		ts.tv_sec = rq->perout.start.sec;
		ts.tv_nsec = rq->perout.start.nsec;
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
		ns = timespec64_to_ns(&ts);
#else
		ns = timespec_to_ns(&ts);
#endif
#ifdef HAVE_GETTIMEX64
		cycles_now = mlx5_read_internal_timer(mdev, NULL);
#else
		cycles_now = mlx5_read_internal_timer(mdev);
#endif
		write_seqlock_irqsave(&clock->lock, flags);
		nsec_now = timecounter_cyc2time(&clock->tc, cycles_now);
		nsec_delta = ns - nsec_now;
		cycles_delta = div64_u64(nsec_delta << clock->cycles.shift,
					 clock->cycles.mult);
		write_sequnlock_irqrestore(&clock->lock, flags);
		time_stamp = cycles_now + cycles_delta;
		field_select = MLX5_MTPPS_FS_PIN_MODE |
			       MLX5_MTPPS_FS_PATTERN |
			       MLX5_MTPPS_FS_ENABLE |
			       MLX5_MTPPS_FS_TIME_STAMP;
	} else {
		pin = rq->perout.index;
		field_select = MLX5_MTPPS_FS_ENABLE;
	}

	MLX5_SET(mtpps_reg, in, pin, pin);
	MLX5_SET(mtpps_reg, in, pin_mode, pin_mode);
	MLX5_SET(mtpps_reg, in, pattern, pattern);
	MLX5_SET(mtpps_reg, in, enable, on);
	MLX5_SET64(mtpps_reg, in, time_stamp, time_stamp);
	MLX5_SET(mtpps_reg, in, field_select, field_select);

	err = mlx5_set_mtpps(mdev, in, sizeof(in));
	if (err)
		return err;

	return mlx5_set_mtppse(mdev, pin, 0,
			       MLX5_EVENT_MODE_REPETETIVE & on);
}

static int mlx5_pps_configure(struct ptp_clock_info *ptp,
			      struct ptp_clock_request *rq,
			      int on)
{
	struct mlx5_clock *clock =
			container_of(ptp, struct mlx5_clock, ptp_info);

	clock->pps_info.enabled = !!on;
	return 0;
}

static int mlx5_ptp_enable(struct ptp_clock_info *ptp,
			   struct ptp_clock_request *rq,
			   int on)
{
	switch (rq->type) {
	case PTP_CLK_REQ_EXTTS:
		return mlx5_extts_configure(ptp, rq, on);
	case PTP_CLK_REQ_PEROUT:
		return mlx5_perout_configure(ptp, rq, on);
	case PTP_CLK_REQ_PPS:
		return mlx5_pps_configure(ptp, rq, on);
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int mlx5_ptp_verify(struct ptp_clock_info *ptp, unsigned int pin,
			   enum ptp_pin_function func, unsigned int chan)
{
	return (func == PTP_PF_PHYSYNC) ? -EOPNOTSUPP : 0;
}
#endif /* HAVE_PTP_CLOCK_INFO_N_PINS */

static const struct ptp_clock_info mlx5_ptp_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "mlx5_p2p",
	.max_adj	= 100000000,
	.n_alarm	= 0,
	.n_ext_ts	= 0,
	.n_per_out	= 0,
#ifdef HAVE_PTP_CLOCK_INFO_N_PINS
       .n_pins		= 0,
#endif
       .pps		= 0,
       .adjfreq	= mlx5_ptp_adjfreq,
       .adjtime	= mlx5_ptp_adjtime,
#ifdef HAVE_GETTIMEX64
       .gettimex64	= mlx5_ptp_gettimex,
       .settime64	= mlx5_ptp_settime,
#else /*HAVE_GETTIMEX64*/
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
	.gettime64      = mlx5_ptp_gettime,
	.settime64      = mlx5_ptp_settime,
#else
	.gettime        = mlx5_ptp_gettime,
	.settime        = mlx5_ptp_settime,
#endif
#endif /*HAVE_GETTIMEX64*/


       .enable		= NULL,
#ifdef HAVE_PTP_CLOCK_INFO_N_PINS
       .verify		= NULL,
#endif
};

#ifdef HAVE_PTP_CLOCK_INFO_N_PINS
static int mlx5_init_pin_config(struct mlx5_clock *clock)
{
	int i;

	clock->ptp_info.pin_config =
			kcalloc(clock->ptp_info.n_pins,
				sizeof(*clock->ptp_info.pin_config),
				GFP_KERNEL);
	if (!clock->ptp_info.pin_config)
		return -ENOMEM;
	clock->ptp_info.enable = mlx5_ptp_enable;
	clock->ptp_info.verify = mlx5_ptp_verify;
	clock->ptp_info.pps = 1;

	for (i = 0; i < clock->ptp_info.n_pins; i++) {
		snprintf(clock->ptp_info.pin_config[i].name,
			 sizeof(clock->ptp_info.pin_config[i].name),
			 "mlx5_pps%d", i);
		clock->ptp_info.pin_config[i].index = i;
		clock->ptp_info.pin_config[i].func = PTP_PF_NONE;
		clock->ptp_info.pin_config[i].chan = i;
	}

	return 0;
}

static void mlx5_get_pps_caps(struct mlx5_core_dev *mdev)
{
	struct mlx5_clock *clock = &mdev->clock;
	u32 out[MLX5_ST_SZ_DW(mtpps_reg)] = {0};

	mlx5_query_mtpps(mdev, out, sizeof(out));

	clock->ptp_info.n_pins = MLX5_GET(mtpps_reg, out,
					  cap_number_of_pps_pins);
	clock->ptp_info.n_ext_ts = MLX5_GET(mtpps_reg, out,
					    cap_max_num_of_pps_in_pins);
	clock->ptp_info.n_per_out = MLX5_GET(mtpps_reg, out,
					     cap_max_num_of_pps_out_pins);

	clock->pps_info.pin_caps[0] = MLX5_GET(mtpps_reg, out, cap_pin_0_mode);
	clock->pps_info.pin_caps[1] = MLX5_GET(mtpps_reg, out, cap_pin_1_mode);
	clock->pps_info.pin_caps[2] = MLX5_GET(mtpps_reg, out, cap_pin_2_mode);
	clock->pps_info.pin_caps[3] = MLX5_GET(mtpps_reg, out, cap_pin_3_mode);
	clock->pps_info.pin_caps[4] = MLX5_GET(mtpps_reg, out, cap_pin_4_mode);
	clock->pps_info.pin_caps[5] = MLX5_GET(mtpps_reg, out, cap_pin_5_mode);
	clock->pps_info.pin_caps[6] = MLX5_GET(mtpps_reg, out, cap_pin_6_mode);
	clock->pps_info.pin_caps[7] = MLX5_GET(mtpps_reg, out, cap_pin_7_mode);
}

static int mlx5_pps_event(struct notifier_block *nb,
			  unsigned long type, void *data)
{
	struct mlx5_clock *clock = mlx5_nb_cof(nb, struct mlx5_clock, pps_nb);
	struct mlx5_core_dev *mdev = clock->mdev;
	struct ptp_clock_event ptp_event;
	u64 cycles_now, cycles_delta;
	u64 nsec_now, nsec_delta, ns;
	struct mlx5_eqe *eqe = data;
	int pin = eqe->data.pps.pin;
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
	struct timespec64 ts;
#else
	struct timespec ts;
#endif
	unsigned long flags;

	switch (clock->ptp_info.pin_config[pin].func) {
	case PTP_PF_EXTTS:
		ptp_event.index = pin;
		ptp_event.timestamp = timecounter_cyc2time(&clock->tc,
					be64_to_cpu(eqe->data.pps.time_stamp));
		if (clock->pps_info.enabled) {
			ptp_event.type = PTP_CLOCK_PPSUSR;
			ptp_event.pps_times.ts_real =
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
					ns_to_timespec64(ptp_event.timestamp);
#else
					ns_to_timespec(ptp_event.timestamp);
#endif
		} else {
			ptp_event.type = PTP_CLOCK_EXTTS;
		}
		/* TODOL clock->ptp can be NULL if ptp_clock_register failes */
		ptp_clock_event(clock->ptp, &ptp_event);
		break;
	case PTP_PF_PEROUT:
#ifdef HAVE_GETTIMEX64
		mlx5_ptp_gettimex(&clock->ptp_info, &ts, NULL);
		cycles_now = mlx5_read_internal_timer(mdev, NULL);
#else
		mlx5_ptp_gettime(&clock->ptp_info, &ts);
		cycles_now = mlx5_read_internal_timer(mdev);
#endif
		ts.tv_sec += 1;
		ts.tv_nsec = 0;
#ifndef HAVE_PTP_CLOCK_INFO_GETTIME_32BIT
		ns = timespec64_to_ns(&ts);
#else
		ns = timespec_to_ns(&ts);
#endif
		write_seqlock_irqsave(&clock->lock, flags);
		nsec_now = timecounter_cyc2time(&clock->tc, cycles_now);
		nsec_delta = ns - nsec_now;
		cycles_delta = div64_u64(nsec_delta << clock->cycles.shift,
					 clock->cycles.mult);
		clock->pps_info.start[pin] = cycles_now + cycles_delta;
		schedule_work(&clock->pps_info.out_work);
		write_sequnlock_irqrestore(&clock->lock, flags);
		break;
	default:
		mlx5_core_err(mdev, " Unhandled clock PPS event, func %d\n",
			      clock->ptp_info.pin_config[pin].func);
	}

	return NOTIFY_OK;
}
#endif /* HAVE_PTP_CLOCK_INFO_N_PINS */
#endif /* HAVE_PTP_CLOCK_INFO && (CONFIG_PTP_1588_CLOCK || CONFIG_PTP_1588_CLOCK_MODULE) */

void mlx5_init_clock(struct mlx5_core_dev *mdev)
{
	struct mlx5_clock *clock = &mdev->clock;
	u64 overflow_cycles;
	u64 ns;
#ifdef HAVE_CYCLECOUNTER_CYC2NS_4_PARAMS
	u64 frac = 0;
#endif
	u32 dev_freq;

	dev_freq = MLX5_CAP_GEN(mdev, device_frequency_khz);
	if (!dev_freq) {
		mlx5_core_warn(mdev, "invalid device_frequency_khz, aborting HW clock init\n");
		return;
	}
	seqlock_init(&clock->lock);
	clock->cycles.read = read_internal_timer;
	clock->cycles.shift = MLX5_CYCLES_SHIFT;
	clock->cycles.mult = clocksource_khz2mult(dev_freq,
						  clock->cycles.shift);
	clock->nominal_c_mult = clock->cycles.mult;
	clock->cycles.mask = CLOCKSOURCE_MASK(41);
	clock->mdev = mdev;

	timecounter_init(&clock->tc, &clock->cycles,
			 ktime_to_ns(ktime_get_real()));

	/* Calculate period in seconds to call the overflow watchdog - to make
	 * sure counter is checked at least twice every wrap around.
	 * The period is calculated as the minimum between max HW cycles count
	 * (The clock source mask) and max amount of cycles that can be
	 * multiplied by clock multiplier where the result doesn't exceed
	 * 64bits.
	 */
	overflow_cycles = div64_u64(~0ULL >> 1, clock->cycles.mult);
	overflow_cycles = min(overflow_cycles, div_u64(clock->cycles.mask, 3));

#ifdef HAVE_CYCLECOUNTER_CYC2NS_4_PARAMS
	ns = cyclecounter_cyc2ns(&clock->cycles, overflow_cycles,
				 frac, &frac);
#else
	ns = cyclecounter_cyc2ns(&clock->cycles, overflow_cycles);
#endif
	do_div(ns, NSEC_PER_SEC / HZ);
	clock->overflow_period = ns;

	mdev->clock_info =
		(struct mlx5_ib_clock_info *)get_zeroed_page(GFP_KERNEL);
	if (mdev->clock_info) {
		mdev->clock_info->nsec = clock->tc.nsec;
		mdev->clock_info->cycles = clock->tc.cycle_last;
		mdev->clock_info->mask = clock->cycles.mask;
		mdev->clock_info->mult = clock->nominal_c_mult;
		mdev->clock_info->shift = clock->cycles.shift;
#ifdef HAVE_CYCLECOUNTER_CYC2NS_4_PARAMS
       	mdev->clock_info->frac = clock->tc.frac;
#endif
		mdev->clock_info->overflow_period = clock->overflow_period;
	}

#if defined (HAVE_PTP_CLOCK_INFO_N_PINS) && defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
	INIT_WORK(&clock->pps_info.out_work, mlx5_pps_out);
#endif
	INIT_DELAYED_WORK(&clock->overflow_work, mlx5_timestamp_overflow);
	if (clock->overflow_period)
		schedule_delayed_work(&clock->overflow_work, 0);
	else
		mlx5_core_warn(mdev, "invalid overflow period, overflow_work is not scheduled\n");

#if defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
	/* Configure the PHC */
	clock->ptp_info = mlx5_ptp_clock_info;

#ifdef HAVE_PTP_CLOCK_INFO_N_PINS
	/* Initialize 1PPS data structures */
	if (MLX5_PPS_CAP(mdev))
		mlx5_get_pps_caps(mdev);
	if (clock->ptp_info.n_pins)
		mlx5_init_pin_config(clock);
#endif
	clock->ptp = ptp_clock_register(&clock->ptp_info,
					&mdev->pdev->dev);
	if (IS_ERR(clock->ptp)) {
		mlx5_core_warn(mdev, "ptp_clock_register failed %ld\n",
			       PTR_ERR(clock->ptp));
		clock->ptp = NULL;
	}
#endif
#if defined (HAVE_PTP_CLOCK_INFO_N_PINS) && defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
	MLX5_NB_INIT(&clock->pps_nb, mlx5_pps_event, PPS_EVENT);
	mlx5_eq_notifier_register(mdev, &clock->pps_nb);
#endif
}

void mlx5_cleanup_clock(struct mlx5_core_dev *mdev)
{
	struct mlx5_clock *clock = &mdev->clock;

	if (!MLX5_CAP_GEN(mdev, device_frequency_khz))
		return;

	mlx5_eq_notifier_unregister(mdev, &clock->pps_nb);
#if defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
	if (clock->ptp) {
		ptp_clock_unregister(clock->ptp);
		clock->ptp = NULL;
	}
#endif

#if defined (HAVE_PTP_CLOCK_INFO_N_PINS) && defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
	cancel_work_sync(&clock->pps_info.out_work);
#endif
	cancel_delayed_work_sync(&clock->overflow_work);

	if (mdev->clock_info) {
		free_page((unsigned long)mdev->clock_info);
		mdev->clock_info = NULL;
	}

#if defined (HAVE_PTP_CLOCK_INFO_N_PINS) && defined (HAVE_PTP_CLOCK_INFO) && (defined (CONFIG_PTP_1588_CLOCK) || defined(CONFIG_PTP_1588_CLOCK_MODULE))
	kfree(clock->ptp_info.pin_config);
#endif
}
