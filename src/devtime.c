// SPDX-License-Identifier: MIT
/*
 * Device time: the soft offset, PPS alignment and the internal-clock
 * fallback (spec decision 9).
 *
 * API time = hardware ticks + offset (mod 2^64). The offset is nonzero only
 * after a soft alignment; a hard (PPS) alignment loads the epoch into Tick
 * itself and clears it.
 *
 * PPS loss is not detectable on the current Tick: pps_present latches
 * "PPS seen" until the core is reset (tfcollins/tick#39). So the source
 * never becomes DATAX_TIME_PPS_HOLDOVER here; when Tick can report a live
 * PPS the poller watch below is where that transition belongs.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "error.h"
#include "scheduler.h"
#include "tick_ops.h"

#define PPS_WAIT_MS 2000

unsigned long long datax_devtime_to_hw(const struct datax_device *dev,
				       unsigned long long api_ticks)
{
	return api_ticks - atomic_load(&dev->time_off);
}

unsigned long long datax_devtime_from_hw(const struct datax_device *dev,
					 unsigned long long hw_ticks)
{
	return hw_ticks + atomic_load(&dev->time_off);
}

int datax_device_get_time_source(const struct datax_device *dev,
				 enum datax_time_source *src)
{
	if (!dev || !src)
		return dev ? datax_error_ret(dev->ctx, EINVAL, "time",
					     "get_time_source: NULL argument") : -EINVAL;
	if (!datax_tick_present(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "time",
				       "no Tick scheduler in this context");
	*src = (enum datax_time_source)atomic_load(&dev->time_source);
	return 0;
}

static void emit_pps(struct datax_device *dev, struct datax_sched *s,
		     unsigned long long hw, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

/* A PPS_LOCK_CHANGE event: through the scheduler's poller when there is
 * one, else directly on the caller's thread. */
static void emit_pps(struct datax_device *dev, struct datax_sched *s,
		     unsigned long long hw, const char *fmt, ...)
{
	char msg[DATAX_SCHED_DETAIL_LEN];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (s) {
		datax_sched_emit(s, DATAX_EVT_PPS_LOCK_CHANGE, hw, -1, "%s", msg);
	} else {
		struct datax_event evt = { .type = DATAX_EVT_PPS_LOCK_CHANGE,
					   .ticks = datax_devtime_from_hw(dev, hw),
					   .seq = -1, .detail = msg };

		datax_event_fire(dev->ctx, &evt);
	}
}

static void sleep_ms(unsigned int ms)
{
	struct timespec ts = { 0, (long)ms * 1000000L };

	nanosleep(&ts, NULL);
}

/* A queued command has been written (or is about to be): moving the clock
 * or the offset would change when it runs. s->lock is taken here. */
static bool sched_has_queued(struct datax_sched *s)
{
	bool busy = false;

	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->ncmds && !busy; i++)
		busy = s->cmds[i].state == CMD_PENDING || s->cmds[i].state == CMD_WRITTEN;
	pthread_mutex_unlock(&s->lock);
	return busy;
}

/* After the offset changes, commands not yet written keep the API time the
 * caller gave: recompute their hardware time. s->lock held. */
static void rebase_unwritten(struct datax_sched *s)
{
	for (size_t i = 0; i < s->ncmds; i++)
		if (s->cmds[i].state == CMD_BUILD || s->cmds[i].state == CMD_PENDING)
			s->cmds[i].hw_ts = datax_devtime_to_hw(s->dev, s->cmds[i].api_ts);
}

static void rebase_building(struct datax_sched *s)
{
	pthread_mutex_lock(&s->lock);
	rebase_unwritten(s);
	pthread_mutex_unlock(&s->lock);
}

static unsigned long long ns_now(void)
{
	return datax_mono_ns();
}

/* Soft alignment: the offset makes get_time read epoch at this call. The
 * accuracy is the round trip of the one device-time read. io is held. */
static int align_soft(struct datax_device *dev, struct datax_sched *s,
		      unsigned long long epoch, const char *why)
{
	unsigned long long t0, t1, hw, rtt_us;
	int ret;

	t0 = ns_now();
	ret = datax_tick_read_u64(dev, "current_time_ticks", &hw);
	t1 = ns_now();
	if (ret)
		return datax_error_ret(dev->ctx, -ret, "time",
				       "set_time_next_pps: Tick read failed");
	rtt_us = (t1 - t0) / 1000;
	atomic_store(&dev->time_off, epoch - hw);
	atomic_store(&dev->time_source, DATAX_TIME_SOFT_ALIGNED);
	if (s)
		rebase_building(s);
	emit_pps(dev, s, hw, "%s: aligned to the host clock (\xc2\xb1%llu \xc2\xb5s)", why,
		 rtt_us);
	return 0;
}

/*
 * A timed-out hard alignment leaves Tick armed, and a late edge then makes
 * the counter jump to the epoch. Adopt it: the soft offset goes, commands
 * not yet written are re-based, commands already written (they were timed
 * against the old counter) are dropped, and Tick's queue is emptied of
 * them. s may be NULL; otherwise io is held.
 */
bool datax_devtime_check_late_edge(struct datax_device *dev, struct datax_sched *s)
{
	unsigned long long v = 0, hw = 0;
	unsigned int dropped = 0;

	if (!atomic_load(&dev->time_armed))
		return false;
	if (datax_tick_read_u64(dev, "time_aligned", &v) || !v)
		return false;
	if (!atomic_exchange(&dev->time_armed, false))
		return false;                    /* another thread adopted it */
	if (s) {
		bool written = false;

		pthread_mutex_lock(&s->lock);
		for (size_t i = 0; i < s->ncmds; i++)
			written = written || s->cmds[i].state == CMD_WRITTEN;
		pthread_mutex_unlock(&s->lock);
		if (written) {          /* keep what ran, empty the queue, keep what ran since */
			(void)datax_poller_read_time(s);
			(void)datax_poller_drain(s);
			(void)datax_tick_write_str(dev, "flush_queue", "1");
			(void)datax_poller_drain(s);
		}
		pthread_mutex_lock(&s->lock);
		atomic_store(&dev->time_off, 0);
		atomic_store(&dev->time_source, DATAX_TIME_PPS_ALIGNED);
		rebase_unwritten(s);
		for (size_t i = 0; i < s->ncmds; i++) {
			long seq = s->cmds[i].state == CMD_WRITTEN ? s->cmds[i].seq : 0;
			struct sched_cmd *c;
			struct datax_sched_result res = {
				.status = DATAX_COMPLETION_DROPPED, .tick_status = UINT32_MAX,
			};

			if (!seq)
				continue;
			c = datax_sched_find(s, seq);
			if (!c || c->state == CMD_DONE)
				continue;
			res.requested_ticks = c->api_ts;
			snprintf(res.detail, sizeof(res.detail),
				 "device time jumped on a late PPS edge");
			datax_sched_finish(s, c, &res);   /* may drop s->lock for a hook */
			dropped++;
			i = (size_t)-1;                   /* the table may have moved: rescan */
		}
		s->kick = true;
		pthread_cond_signal(&s->wake);
		pthread_mutex_unlock(&s->lock);
	} else {
		atomic_store(&dev->time_off, 0);
		atomic_store(&dev->time_source, DATAX_TIME_PPS_ALIGNED);
	}
	(void)datax_tick_read_u64(dev, "current_time_ticks", &hw);
	emit_pps(dev, s, hw,
		 "late PPS edge: device time jumped to epoch; %u queued commands dropped",
		 dropped);
	return true;
}

int datax_device_set_time_next_pps(struct datax_device *dev, unsigned long long epoch_ticks)
{
	struct datax_sched *s;
	struct datax_io_hold h;
	bool locked;
	unsigned long long present = 0, aligned = 0, hw = 0, deadline, now;
	char why[96] = "no PPS";
	int ret, wret = 0;

	if (!dev)
		return -EINVAL;
	if (!datax_tick_present(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "time",
				       "no Tick scheduler in this context");
	s = datax_device_sched_get(dev);
	locked = s && !datax_sched_holds_io(s);
	if (locked)
		datax_sched_io_lock(s, &h);
	/* an edge that landed since the last look moved the counter already */
	(void)datax_devtime_check_late_edge(dev, s);
	if (s && sched_has_queued(s)) {
		ret = datax_error_ret(dev->ctx, EBUSY, "time",
				      "scheduler has queued commands; flush first");
		goto out;
	}
	/* A missing or unreadable pps_present is the same as no PPS. */
	if (datax_tick_read_u64(dev, "pps_present", &present))
		present = 0;
	if (present) {
		deadline = datax_mono_ns() + (unsigned long long)PPS_WAIT_MS * 1000000ULL;
		/* Tick reads the epoch register at the edge: an alignment still
		 * armed from an earlier call takes this epoch instead. */
		wret = datax_tick_write_u64(dev, "time_epoch_ticks", epoch_ticks);
		if (!wret)
			wret = datax_tick_write_u64(dev, "arm_time_next_pps", 1);
		if (wret) {
			snprintf(why, sizeof(why), "arming the PPS alignment failed (%d)", wret);
		} else {
			/* one monotonic deadline; the last read is at or after it */
			for (;;) {
				now = datax_mono_ns();
				if (!datax_tick_read_u64(dev, "time_aligned", &aligned) && aligned)
					break;
				if (now >= deadline) {
					/* Tick stays armed: a late edge is adopted */
					atomic_store(&dev->time_armed, true);
					break;
				}
				sleep_ms(1);
			}
		}
		if (aligned) {
			atomic_store(&dev->time_armed, false);
			atomic_store(&dev->time_off, 0);
			atomic_store(&dev->time_source, DATAX_TIME_PPS_ALIGNED);
			if (s)
				rebase_building(s);
			(void)datax_tick_read_u64(dev, "current_time_ticks", &hw);
			emit_pps(dev, s, hw, "aligned on a fabric PPS edge");
			ret = 0;
			goto out;
		}
		if (!wret)
			snprintf(why, sizeof(why), "no PPS edge within 2 s");
	}
	ret = align_soft(dev, s, epoch_ticks, why);
out:
	if (locked)
		datax_sched_io_unlock(s, &h);
	datax_sched_put(s);
	return ret;
}

/*
 * PPS watch, on the poller thread with io held. Only transitions are
 * reported:
 * - pps_present appearing (0 -> 1 after a first observation) while the clock
 *   is not PPS-aligned: an event saying a hard alignment is now possible.
 *   Time does not jump.
 * - a late armed edge: see datax_devtime_check_late_edge.
 * No loss detection (see the file comment).
 */
void datax_devtime_poll(struct datax_sched *s)
{
	struct datax_device *dev = s->dev;
	unsigned long long v = 0;
	int now_state;

	(void)datax_devtime_check_late_edge(dev, s);
	if (datax_tick_read_u64(dev, "pps_present", &v))
		return;
	now_state = v ? 2 : 1;
	if (s->pps_seen_state == 1 && now_state == 2 &&
	    atomic_load(&dev->time_source) != DATAX_TIME_PPS_ALIGNED)
		datax_sched_emit(s, DATAX_EVT_PPS_LOCK_CHANGE, s->last_hw, -1,
				 "PPS appeared: a hard alignment is now possible "
				 "(time did not change)");
	s->pps_seen_state = now_state;
}

/* Diagnostics (doctor): whether the Tick reports a fabric PPS (pps_present).
 * 1, 0, or -errno (-ENODEV without a Tick). */
int datax_internal_pps_present(const struct datax_device *dev)
{
	unsigned long long v;
	int ret;

	if (!dev)
		return -EINVAL;
	ret = datax_tick_read_u64(dev, "pps_present", &v);
	return ret ? ret : v != 0;
}
