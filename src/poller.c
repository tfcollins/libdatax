// SPDX-License-Identifier: MIT
/*
 * The scheduler's poller thread (one per scheduler). Every poll_us it:
 *   1. reads the device time;
 *   2. drains Tick's one-entry completion window (seven completion_* reads,
 *      completion_ack, the device time) until it reads empty, matching each
 *      record to its command, and detects poller lag and lost reports;
 *   3. watches late_count and overflow_count and raises events on a rise;
 *   4. feeds PENDING commands to command_descriptor in timestamp order,
 *      keeping queue_level at most depth - 1, and completes LATE ("host
 *      could not keep the queue fed") any it can no longer write before its
 *      time minus the guard margin; before each write it checks for a late
 *      armed PPS edge (devtime.c);
 *   5. runs the poll hook, then delivers queued events with no lock held.
 * Rules: api-c-core "Scheduler semantics"; spec rulings 1 and 3.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scheduler.h"
#include "tick_ops.h"

/* Without work the poller reads the counters only every this many periods. */
#define IDLE_COUNTER_EVERY 100u

static const char *const comp_attrs[7] = {
	"completion_sequence_id", "completion_status", "completion_command_id",
	"completion_target_id", "completion_requested_timestamp_ticks",
	"completion_executed_timestamp_ticks", "completion_detail",
};
enum { R_SEQ, R_STATUS, R_CMD, R_TARGET, R_REQ, R_EXEC, R_DETAIL };

unsigned long long datax_mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static unsigned long long ns_to_ticks(unsigned long long ns, unsigned long long hz)
{
	return ns / 1000000000ULL * hz + ns % 1000000000ULL * hz / 1000000000ULL;
}

static int rd(struct datax_sched *s, const char *attr, unsigned long long *v)
{
	return datax_tick_read_u64(s->dev, attr, v);
}

static void note_time(struct datax_sched *s, unsigned long long t)
{
	pthread_mutex_lock(&s->lock);
	s->last_hw = t;
	s->last_hw_mono = datax_mono_ns();
	pthread_mutex_unlock(&s->lock);
}

int datax_poller_read_time(struct datax_sched *s)
{
	unsigned long long t;
	int ret = rd(s, "current_time_ticks", &t);

	if (!ret)
		note_time(s, t);
	return ret;
}

/*
 * The rest of one record after its seq: six completion_* reads, the ack,
 * then the device time (the drain's timestamp). The time is read after the
 * ack because the slot stays full until the ack lands: a reported command
 * due during the ack's round trip was held and must count as lagged. (A
 * command due just after the ack may be marked lagged too: a false LATE,
 * never a false OK.)
 */
static int read_record(struct datax_sched *s, unsigned long long v[7], unsigned long long *t)
{
	int ret = 0;

	for (int i = R_STATUS; i <= R_DETAIL && !ret; i++)
		ret = rd(s, comp_attrs[i], &v[i]);
	if (!ret)
		ret = datax_tick_write_str(s->dev, "completion_ack", "1");
	if (!ret)
		ret = rd(s, "current_time_ticks", t);
	return ret;
}

static int cmp_ull(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;

	return x < y ? -1 : x > y;
}

int datax_poller_measure(struct datax_sched *s)
{
	unsigned long long t[DATAX_SCHED_DRAIN_SAMPLES + 1], d[DATAX_SCHED_DRAIN_SAMPLES];
	unsigned long long v[7], med, raw;
	int ret;

	/* DRAIN_SAMPLES + 1 full drains (8 reads + ack, the slot normally
	 * empty: any stale record is acked and discarded). The time read of each
	 * is DRAIN_ACCESSES accesses after the previous one's, so each delta is
	 * one drain's cost in device ticks. */
	for (unsigned int i = 0; i <= DATAX_SCHED_DRAIN_SAMPLES; i++) {
		ret = rd(s, comp_attrs[R_SEQ], &v[R_SEQ]);
		if (!ret)
			ret = read_record(s, v, &t[i]);
		if (ret)
			return ret;
	}
	for (unsigned int i = 0; i < DATAX_SCHED_DRAIN_SAMPLES; i++)
		d[i] = t[i + 1] - t[i];
	qsort(d, DATAX_SCHED_DRAIN_SAMPLES, sizeof(d[0]), cmp_ull);
	med = d[DATAX_SCHED_DRAIN_SAMPLES / 2];
	s->access_ticks = med / DATAX_SCHED_DRAIN_ACCESSES;
	/* Ruling 1: spacing = 3 x the drain. Published in whole microseconds,
	 * rounded up, and enforced from that published value, so a caller
	 * spacing exactly REPORT_SPACING_US passes. */
	raw = 3 * med;
	s->spacing_us = (raw * 1000000ULL + s->tick_hz - 1) / s->tick_hz;
	if (!s->spacing_us)
		s->spacing_us = 1;
	s->spacing_ticks = s->spacing_us * s->tick_hz / 1000000ULL;
	return 0;
}

/* ---- drain ---------------------------------------------------------------------- */

/* A reported command written and due before last_hw, with no record yet,
 * when the slot reads empty after last_hw. s->lock held. */
static struct sched_cmd *next_lost(struct datax_sched *s)
{
	for (size_t i = 0; i < s->ncmds; i++) {
		struct sched_cmd *c = &s->cmds[i];

		if (c->state == CMD_WRITTEN && c->report && c->hw_ts < s->last_hw)
			return c;
	}
	return NULL;
}

/*
 * The slot read empty after last_hw. Every reported command written and due
 * before then without a record has provably lost its report: a command held
 * behind a full slot refills it the cycle after the ack, so the slot cannot
 * read empty while one is pending. Ruling 1: a lost report shows only in
 * overflow_count; it completes DROPPED now, so wait(-1) and get_completions
 * see it. io held, s->lock not held.
 */
static int finish_lost(struct datax_sched *s)
{
	unsigned long long ovf = 0;
	struct sched_cmd *c;
	bool any;
	int ret;

	pthread_mutex_lock(&s->lock);
	any = next_lost(s) != NULL;
	pthread_mutex_unlock(&s->lock);
	if (!any)
		return 0;
	/* For the detail only; check_counters raises the overflow event. */
	ret = rd(s, "overflow_count", &ovf);
	if (ret)
		return ret;
	pthread_mutex_lock(&s->lock);
	while ((c = next_lost(s))) {
		struct datax_sched_result res = {
			.status = DATAX_COMPLETION_DROPPED,
			.requested_ticks = c->api_ts,
			.tick_status = UINT32_MAX,
		};

		snprintf(res.detail, sizeof(res.detail),
			 "no completion; see overflow_count (%llu)", ovf);
		datax_sched_finish(s, c, &res);   /* DONE: no longer WRITTEN */
	}
	pthread_mutex_unlock(&s->lock);
	return 0;
}

/* One drained record v, its drain read at hardware time t. s->lock held. */
static void process(struct datax_sched *s, const unsigned long long v[7], unsigned long long t)
{
	uint32_t rseq = (uint32_t)v[R_SEQ];
	struct sched_cmd *c = NULL;
	struct datax_sched_result res;

	/*
	 * Ruling 1, poller lag: the slot held this record until its ack, read
	 * back as t (after the ack). A reported
	 * command due by then that has not completed was held behind it (Tick
	 * then records it as on time, tick#38), so it completes LATE.
	 */
	for (size_t i = 0; i < s->ncmds; i++) {
		struct sched_cmd *o = &s->cmds[i];

		if (o->state == CMD_WRITTEN && o->report &&
		    (uint32_t)o->seq != rseq && o->hw_ts <= t)
			o->lagged = true;
	}
	for (size_t i = 0; i < s->ncmds && !c; i++)
		if (s->cmds[i].state == CMD_WRITTEN && s->cmds[i].report &&
		    (uint32_t)s->cmds[i].seq == rseq && s->cmds[i].command_id == v[R_CMD] &&
		    s->cmds[i].hw_ts == v[R_REQ])
			c = &s->cmds[i];
	/* Not ours (another Tick user, a stale record whose seq collides: it
	 * must also match the command and its timestamp) or an unreported late
	 * drop, which late_count already reports. */
	if (!c)
		return;
	memset(&res, 0, sizeof(res));
	datax_sched_map_status((uint32_t)v[R_STATUS], &res);
	res.requested_ticks = c->api_ts;
	res.executed_ticks = v[R_EXEC] ? datax_devtime_from_hw(s->dev, v[R_EXEC]) : 0;
	if (res.status == DATAX_COMPLETION_OK && c->lagged) {
		res.status = DATAX_COMPLETION_LATE;
		snprintf(res.detail, sizeof(res.detail),
			 "poller lagged; execution may have been held");
	}
	datax_sched_finish(s, c, &res);
}

int datax_poller_clear_slot(struct datax_sched *s)
{
	for (unsigned int n = 0; n <= s->depth + 1; n++) {
		unsigned long long seq;
		int ret = rd(s, comp_attrs[R_SEQ], &seq);

		if (ret || !seq)
			return ret;
		ret = datax_tick_write_str(s->dev, "completion_ack", "1");
		if (ret)
			return ret;
	}
	return 0;
}

int datax_poller_drain(struct datax_sched *s)
{
	for (unsigned int n = 0; n <= s->depth + 1; n++) {
		unsigned long long v[7], t;
		int ret = rd(s, comp_attrs[R_SEQ], &v[R_SEQ]);

		if (ret)
			return ret;
		if (!v[R_SEQ]) {
			/* Empty: seq 0 is never issued (no valid bit, tick#39). */
			return finish_lost(s);
		}
		ret = read_record(s, v, &t);
		if (ret)
			return ret;
		pthread_mutex_lock(&s->lock);
		s->last_hw = t;
		s->last_hw_mono = datax_mono_ns();
		process(s, v, t);
		pthread_mutex_unlock(&s->lock);
	}
	return 0;
}

/* ---- counters --------------------------------------------------------------------- */

static int check_counters(struct datax_sched *s)
{
	unsigned long long late, ovf;
	int ret = rd(s, "late_count", &late);

	if (!ret)
		ret = rd(s, "overflow_count", &ovf);
	if (ret)
		return ret;
	pthread_mutex_lock(&s->lock);
	if (late > s->late_seen)
		datax_sched_emit_locked(s, DATAX_EVT_LATE_COMMAND, s->last_hw, -1,
					"late_count %llu (was %llu)", late, s->late_seen);
	if (ovf > s->overflow_seen)
		datax_sched_emit_locked(s, DATAX_EVT_QUEUE_OVERFLOW, s->last_hw, -1,
					"overflow_count %llu (was %llu): a report or command "
					"may have been lost", ovf, s->overflow_seen);
	/* Always re-baseline, so a counter that went backwards (Tick reset or
	 * reloaded) raises the next event on its next rise. */
	s->late_seen = late;
	s->overflow_seen = ovf;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

/* ---- feed ------------------------------------------------------------------------- */

static struct sched_cmd *earliest_pending(struct datax_sched *s)
{
	struct sched_cmd *best = NULL;

	for (size_t i = 0; i < s->ncmds; i++) {
		struct sched_cmd *c = &s->cmds[i];

		if (c->state == CMD_PENDING &&
		    (!best || c->hw_ts < best->hw_ts ||
		     (c->hw_ts == best->hw_ts && c->order < best->order)))
			best = c;
	}
	return best;
}

static int feed(struct datax_sched *s)
{
	unsigned long long level, t0, h0, accesses;
	char buf[160];
	int ret;

restart:
	accesses = 1;
	pthread_mutex_lock(&s->lock);
	if (!earliest_pending(s)) {
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	pthread_mutex_unlock(&s->lock);
	ret = rd(s, "queue_level", &level);
	if (ret)
		return ret;
	pthread_mutex_lock(&s->lock);
	t0 = s->last_hw;
	h0 = s->last_hw_mono;
	for (;;) {
		struct sched_cmd *c = earliest_pending(s);
		unsigned long long host, acc, now;
		long seq;

		if (!c)
			break;
		/* Device time now: the last read plus the larger of the host time
		 * since and the accesses since at the measured per-access cost
		 * (no extra round trips; deterministic with the fake). */
		host = ns_to_ticks(datax_mono_ns() - h0, s->tick_hz);
		acc = accesses * s->access_ticks;
		now = t0 + (host > acc ? host : acc);
		if (c->hw_ts < now + s->guard_ticks) {
			struct datax_sched_result res = {
				.status = DATAX_COMPLETION_LATE,
				.requested_ticks = c->api_ts,
				.tick_status = UINT32_MAX,
			};

			if (level + 1 >= s->depth) {
				unsigned int later = 0;

				for (size_t i = 0; i < s->ncmds; i++)
					later += s->cmds[i].state == CMD_WRITTEN &&
						 s->cmds[i].hw_ts > c->hw_ts;
				snprintf(res.detail, sizeof(res.detail),
					 "host could not keep the queue fed: the Tick queue held "
					 "depth-1 (%llu) commands, %u of them later", level, later);
			} else {
				snprintf(res.detail, sizeof(res.detail),
					 "host could not keep the queue fed");
			}
			s->host_late++;
			/* finish raises DATAX_EVT_LATE_COMMAND, reported or not */
			datax_sched_finish(s, c, &res);
			continue;
		}
		if (level + 1 >= s->depth)   /* at most depth - 1: one entry spare */
			break;
		/*
		 * Before every write: has a late armed PPS edge moved the counter
		 * (Task 3 review)? Adopting it flushes the queue, drops what was
		 * written and re-bases PENDING, so level, t0 and c are stale: read
		 * the time and start the pass again. Without an armed alignment
		 * this is one atomic load. An edge landing between this check and
		 * the write is the documented residual window.
		 */
		seq = c->seq;
		pthread_mutex_unlock(&s->lock);
		if (datax_devtime_check_late_edge(s->dev, s)) {
			ret = datax_poller_read_time(s);
			if (ret)
				return ret;
			goto restart;
		}
		pthread_mutex_lock(&s->lock);
		c = datax_sched_find(s, seq);
		if (!c || c->state != CMD_PENDING)
			continue;
		datax_sched_format_desc(c, buf, sizeof(buf));
		c->state = CMD_WRITTEN;
		seq = c->seq;
		pthread_mutex_unlock(&s->lock);
		ret = datax_tick_write_str(s->dev, "command_descriptor", buf);
		accesses++;
		pthread_mutex_lock(&s->lock);
		if (ret) {
			c = datax_sched_find(s, seq);
			if (c && c->state == CMD_WRITTEN) {
				struct datax_sched_result res = {
					.status = DATAX_COMPLETION_DROPPED,
					.requested_ticks = c->api_ts,
					.tick_status = UINT32_MAX,
				};

				snprintf(res.detail, sizeof(res.detail),
					 "descriptor write failed (%d)", ret);
				datax_sched_finish(s, c, &res);
			}
			break;
		}
		level++;
	}
	pthread_mutex_unlock(&s->lock);
	return ret;
}

/* ---- the loop ---------------------------------------------------------------------- */

/* s->lock held */
static bool has_work(const struct datax_sched *s)
{
	for (size_t i = 0; i < s->ncmds; i++) {
		const struct sched_cmd *c = &s->cmds[i];

		if (c->state == CMD_PENDING || c->state == CMD_WRITTEN)
			return true;
	}
	return false;
}

/* Unreported commands leave the table once due. s->lock held. */
static void prune(struct datax_sched *s)
{
	for (size_t i = 0; i < s->ncmds;) {
		struct sched_cmd *c = &s->cmds[i];

		if (c->state == CMD_WRITTEN && !c->report && c->hw_ts < s->last_hw)
			datax_sched_remove(s, c);
		else
			i++;
	}
}

void datax_poller_deliver(struct datax_sched *s)
{
	struct sched_event q[SCHED_EVQ_LEN];
	enum datax_event_type lost_type;
	unsigned long lost;
	unsigned int n;

	pthread_mutex_lock(&s->lock);
	n = s->nevq;
	memcpy(q, s->evq, n * sizeof(q[0]));
	s->nevq = 0;
	lost = s->evq_lost;
	lost_type = s->evq_lost_type;
	s->evq_lost = 0;
	pthread_mutex_unlock(&s->lock);
	for (unsigned int i = 0; i < n; i++) {
		struct datax_event evt = {
			.type = q[i].type,
			.ticks = q[i].hw_ticks ? datax_devtime_from_hw(s->dev, q[i].hw_ticks) : 0,
			.seq = q[i].seq,
			.detail = q[i].detail,
		};

		datax_event_fire(s->ctx, &evt);
	}
	if (lost) {
		char detail[DATAX_SCHED_DETAIL_LEN];
		struct datax_event evt = { .type = lost_type, .ticks = 0, .seq = -1,
					   .detail = detail };

		snprintf(detail, sizeof(detail),
			 "%lu events dropped (more than %u in one poller iteration)",
			 lost, SCHED_EVQ_LEN);
		datax_event_fire(s->ctx, &evt);
	}
}

static void iterate(struct datax_sched *s)
{
	datax_sched_poll_fn fn = NULL;
	void *user = NULL;
	unsigned long long now = 0;
	bool active, counters;
	struct datax_io_hold h;

	datax_sched_io_lock(s, &h);
	pthread_mutex_lock(&s->lock);
	if (s->paused || s->stop) {
		pthread_mutex_unlock(&s->lock);
		datax_sched_io_unlock(s, &h);
		datax_poller_deliver(s);
		return;
	}
	active = has_work(s);
	counters = active || s->idle_iters++ % IDLE_COUNTER_EVERY == 0;
	pthread_mutex_unlock(&s->lock);
	/* A Tick error skips the rest of this iteration; the next retries. */
	if ((active || counters) && !datax_poller_read_time(s) &&
	    (!active || !datax_poller_drain(s)) &&
	    (!counters || !check_counters(s)) &&
	    (!active || !feed(s))) {
		pthread_mutex_lock(&s->lock);
		prune(s);
		fn = s->poll_fn;
		user = s->poll_user;
		now = s->last_hw;
		pthread_mutex_unlock(&s->lock);
		if (counters)
			datax_devtime_poll(s);   /* PPS watch (devtime.c) */
		if (fn)
			fn(s, now, user);
	}
	datax_sched_io_unlock(s, &h);
	datax_poller_deliver(s);
}

static void *poller_main(void *arg)
{
	struct datax_sched *s = arg;

	pthread_mutex_lock(&s->lock);   /* waits for create to finish starting us */
	while (!s->stop) {
		pthread_mutex_unlock(&s->lock);
		iterate(s);
		pthread_mutex_lock(&s->lock);
		if (s->stop)
			break;
		if (!s->kick) {
			unsigned long long now = datax_mono_ns(), until = now + s->poll_us * 1000ULL;
			struct timespec dl;

			/* A targeted wake (datax_sched_wake_at) earlier than the
			 * period: device time to host time from the last read. */
			if (s->wake_hw) {
				unsigned long long d = s->wake_hw > s->last_hw ?
						       s->wake_hw - s->last_hw : 0;
				unsigned long long at = s->last_hw_mono + d / s->tick_hz * 1000000000ULL +
							d % s->tick_hz * 1000000000ULL / s->tick_hz;

				if (at <= now) {
					s->wake_hw = 0;    /* due: run now */
					until = now;
				} else if (at < until) {
					until = at;
				}
			}
			if (until > now) {
				dl.tv_sec = (time_t)(until / 1000000000ULL);
				dl.tv_nsec = (long)(until % 1000000000ULL);
				pthread_cond_timedwait(&s->wake, &s->lock, &dl);
			}
		}
		s->kick = false;
	}
	pthread_mutex_unlock(&s->lock);
	return NULL;
}

int datax_poller_start(struct datax_sched *s)
{
	int ret;

	pthread_mutex_lock(&s->lock);
	ret = pthread_create(&s->thread, NULL, poller_main, s);
	s->thread_started = ret == 0;
	pthread_mutex_unlock(&s->lock);
	return -ret;
}

void datax_poller_stop(struct datax_sched *s)
{
	bool started;

	pthread_mutex_lock(&s->lock);
	s->stop = true;
	started = s->thread_started;
	pthread_cond_signal(&s->wake);
	pthread_cond_broadcast(&s->done);   /* waiters return -ECANCELED */
	pthread_mutex_unlock(&s->lock);
	if (started)
		pthread_join(s->thread, NULL);
	pthread_mutex_lock(&s->lock);
	s->thread_started = false;
	pthread_mutex_unlock(&s->lock);
}

void datax_sched_wake_at(struct datax_sched *s, unsigned long long hw_ticks)
{
	if (!s || !hw_ticks)
		return;
	pthread_mutex_lock(&s->lock);
	if (!s->wake_hw || hw_ticks < s->wake_hw) {
		s->wake_hw = hw_ticks;
		if (!datax_poller_is_self(s))
			pthread_cond_signal(&s->wake);   /* re-plan its sleep */
	}
	pthread_mutex_unlock(&s->lock);
}

bool datax_poller_is_self(const struct datax_sched *s)
{
	return s && s->thread_started && pthread_equal(pthread_self(), s->thread);
}
