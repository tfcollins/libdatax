// SPDX-License-Identifier: MIT
/*
 * Talise FHM timed tunes on the adrv9009 family (api-c-core "Hop staging";
 * spec decisions 6-7, "Spike results" and ruling 1).
 *
 * Hardware facts this follows:
 * - The adrv9009 driver (ADI Linux drivers/iio/adc/adrv9009.c, lnx-adi
 *   45e0a21af, adrv9009_phy_ext_lo_info lines 2050-2051) exposes FHM as two
 *   ext_info attributes of the TRX LO channel (output altvoltage0):
 *   frequency_hopping_mode_enable (TALISE_setFhmConfig/Mode) and
 *   frequency_hopping_mode (write: TALISE_setFhmHop stages the next
 *   frequency; read: TALISE_getFhmRfPllFrequency, the live RF PLL).
 * - The FHM band is not an attribute: it comes only from the device-tree
 *   properties adi,fhm-config-fhm-min-freq_mhz / -max-freq_mhz (driver lines
 *   4439-4442, defaults 2400 and 2500 MHz). libdatax therefore uses the
 *   default band, as hopcap src/hopcap/talise.py FhmStager does.
 * - The engine is one stage deep (hopcap docs/explanation/fhm-pipeline.md,
 *   tick STATUS.md): each rising edge on TAL_GPIO_0 (Tick GPIO bit 0 on the
 *   ZU11EG image) commits the frequency staged before the previous edge, and
 *   the first edge after enabling lands on fhmInitFrequency.
 * - Only chip A's FHM GPIO is wired to Tick on this image (decision 7).
 * - Spike: a reported falling edge is held by the full completion slot and
 *   blocks the next rise, so a tune reports its rising edge only (ruling 1).
 *
 * The pipeline. Tunes are queued in time order. Each is a rise/fall pulse on
 * bit 0 (rise reported when no other reported command is within the report
 * spacing, so with dense hops every Nth rise is reported); a priming edge
 * (unreported) precedes the first tune after enabling or after the
 * pipeline drained.
 *
 * Pulse width (controller ruling): min(10 ms, half the distance to the next
 * edge on bit 0). Only 10 ms is proven on hardware (tick
 * examples/adrv9009_fhm_hop_demo.py, hopcap HopPlan.pulse_ticks); an edge's
 * fall is placed 10 ms after its rise and pulled in when the next edge is
 * queued in the same uncommitted batch. Once committed it cannot move, so
 * the next edge must then be at least two pulses later. Narrower pulses
 * (down to FHM_PULSE_MIN_US) are unproven: the Task 8 hardware run must
 * verify them by PLL readback on every edge. For edges E0, E1, ... in time order, the frequency of the tune at
 * Ej must be staged after E(j-2) fired and before E(j-1) fires. The
 * scheduler's hooks do it: a reported edge's completion hook, or the poll
 * hook once an unreported edge's time plus a margin has passed, stages the
 * next eligible frequency, then checks the device time: a write landing
 * after E(j-1) makes tune j LATE "stage late". A reported tune's hook reads
 * the PLL back: a mismatch is DROPPED "PLL <got> Hz, expected <want> Hz".
 *
 * Locks: io (held by every hook caller), then fhm->lock, then s->lock, then
 * ctx->lock. queue_tune takes fhm->lock then s->lock, never io.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <iio/iio.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "error.h"
#include "fhm.h"
#include "platform/family.h"
#include "scheduler.h"
#include "tick_descriptor.h"
#include "tick_ops.h"

#define ATTR_ENABLE "frequency_hopping_mode_enable"
#define ATTR_HOP    "frequency_hopping_mode"

/* adrv9009 driver defaults (see the file comment); hopcap talise.py. */
#define FHM_BAND_MIN_HZ 2400000000ULL
#define FHM_BAND_MAX_HZ 2500000000ULL
/* Host-side set (decision 10): the engine needs only the next frequency. */
#define FHM_TABLE_DEPTH 64u
#define FHM_STAGE_SAMPLES 5u
/* Trigger pulse: the proven 10 ms, shrunk to half the distance to the next
 * edge (see the file comment), never below FHM_PULSE_MIN_US. Consecutive
 * edges are therefore at least 2 x the pulse actually used apart. */
#define FHM_PULSE_MAX_US 10000ULL
#define FHM_PULSE_MIN_US 50ULL
/* After an edge's time, before the next staging write may land (Tick
 * dispatch and the engine's latch). */
#define FHM_MARGIN_US 100ULL
#define FHM_MIN_LEAD_US 1000ULL
#define TRIGGER_MASK 0x1u          /* GPIO bit 0 -> TAL_GPIO_0 */

enum edge_state { E_LIVE, E_FIRED, E_NOEDGE };

struct fhm_edge {
	long seq;                  /* the rise's seq */
	datax_time api;            /* the rise, API ticks */
	unsigned long long hz;     /* 0: a priming edge */
	enum edge_state st;
	bool reported;             /* the rise reports a completion */
	bool staged;               /* hz written before the previous edge */
	bool late;                 /* hz could not be staged in time */
	bool fire_pending;         /* reconcile: fired, not yet processed */
	bool behind;               /* the previous edge did not fire */
	int stage_err;             /* the staging write failed */
	long fall_seq;             /* the fall ending this pulse */
	unsigned long long pulse;  /* rise to fall, API ticks */
	unsigned int num;          /* edge number since FHM was (re)enabled (trace) */
	bool hold_traced;          /* trace: its first staging hold was logged */
};

struct datax_fhm {
	pthread_mutex_t lock;
	unsigned long long table[FHM_TABLE_DEPTH];
	size_t ntable;
	unsigned long long stage_us;   /* DATAX_LIMIT_HOP_STAGE_US */
	bool fresh;                    /* FHM (re)enabled: the next tune primes */
	bool enabled;                  /* this context enabled FHM: disable at teardown */
	struct fhm_edge *e;            /* edges in time order */
	size_t n, cap;
	unsigned int next_num;         /* trace: E0 is the first edge after enabling */
};

/* ---- opt-in trace (DATAX_TRACE=fhm, a comma list) ------------------------------ */

/*
 * One stderr line per pipeline event, prefixed "datax-fhm:", for hardware
 * diagnosis. Off unless DATAX_TRACE names "fhm" (read once). Times are
 * hardware ticks; "mono" is host CLOCK_MONOTONIC in microseconds. The trace
 * does no extra Tick or phy I/O: "start" is the device-time estimate (last
 * read plus host time since), "end" the read that follows the write anyway.
 */
static _Atomic int trace_state = -1;   /* first callers may race: same answer */

static bool tracing(void)
{
	if (atomic_load(&trace_state) < 0) {
		const char *v = getenv("DATAX_TRACE");
		int on = 0;

		if (v) {
			const char *p = strstr(v, "fhm");

			on = p && (p == v || p[-1] == ',') && (p[3] == '\0' || p[3] == ',');
		}
		atomic_store(&trace_state, on);
	}
	return atomic_load(&trace_state) == 1;
}

static void __attribute__((format(printf, 1, 2))) trace(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n;

	if (!tracing())
		return;
	n = snprintf(line, sizeof(line), "datax-fhm: mono=%llu ", datax_mono_ns() / 1000ULL);
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", line);
}

static const char *st_name(int st)
{
	return st == 0 ? "live" : st == 1 ? "fired" : "noedge";
}

/* ---- the phy seam ------------------------------------------------------------- */

static int iio_fhm_read(void *h, const char *attr, char *buf, size_t len)
{
	const struct iio_attr *a;
	ssize_t n;

	if (!h || !attr || !buf || !len)
		return -EINVAL;
	a = iio_channel_find_attr(h, attr);
	if (!a)
		return -ENOENT;
	n = iio_attr_read_raw(a, buf, len - 1);
	if (n < 0)
		return (int)n;
	if ((size_t)n > len - 1)
		n = (ssize_t)(len - 1);
	buf[n] = '\0';
	return (int)n;
}

static int iio_fhm_write(void *h, const char *attr, const char *val)
{
	const struct iio_attr *a;
	ssize_t n;

	if (!h || !attr || !val)
		return -EINVAL;
	a = iio_channel_find_attr(h, attr);
	if (!a)
		return -ENOENT;
	n = iio_attr_write_string(a, val);
	return n < 0 ? (int)n : 0;
}

const struct datax_tick_ops datax_fhm_iio_ops = { iio_fhm_read, iio_fhm_write };

int datax_fhm_bind(struct datax_device *dev, const struct datax_tick_ops *ops, void *h)
{
	bool busy;

	if (!dev || (ops && (!ops->read || !ops->write)))
		return -EINVAL;
	/* The hooks use fhm_ops/fhm_h unlocked: never rebind under a poller. */
	pthread_mutex_lock(&dev->ctx->lock);
	busy = dev->sched != NULL;
	pthread_mutex_unlock(&dev->ctx->lock);
	if (busy)
		return -EBUSY;
	dev->fhm_ops = ops;
	dev->fhm_h = ops ? h : NULL;
	datax_caps_fhm_rebound(dev);
	return 0;
}

int datax_internal_fhm_bind_fake(struct datax_device *dev,
				 const struct datax_tick_ops *ops, void *h)
{
	int ret = datax_fhm_bind(dev, ops, h);

	if (ret == -EBUSY)
		return datax_error_ret(dev->ctx, EBUSY, "fhm",
				       "fhm_bind_fake: the device has a scheduler; destroy it first");
	if (ret && dev)
		return datax_error_ret(dev->ctx, -ret, "fhm", "fhm_bind_fake: ops need read and write");
	return ret;
}

static int phy_write_u64(const struct datax_device *dev, const char *attr,
			 unsigned long long v)
{
	char buf[32];

	if (!dev->fhm_ops)
		return -ENODEV;
	snprintf(buf, sizeof(buf), "%llu", v);
	return dev->fhm_ops->write(dev->fhm_h, attr, buf);
}

static int phy_read_u64(const struct datax_device *dev, const char *attr,
			unsigned long long *v)
{
	char buf[64], *end;
	int n;

	if (!dev->fhm_ops)
		return -ENODEV;
	n = dev->fhm_ops->read(dev->fhm_h, attr, buf, sizeof(buf));
	if (n < 0)
		return n;
	errno = 0;
	*v = strtoull(buf, &end, 10);
	if (errno || end == buf || buf[0] == '-')
		return -EINVAL;
	return 0;
}

/* ---- state -------------------------------------------------------------------- */

static struct datax_fhm *fhm_get(struct datax_device *dev, bool create)
{
	struct datax_fhm *m;

	pthread_mutex_lock(&dev->ctx->lock);
	m = dev->fhm;
	if (!m && create) {
		m = calloc(1, sizeof(*m));
		if (m && pthread_mutex_init(&m->lock, NULL)) {
			free(m);
			m = NULL;
		}
		dev->fhm = m;
	}
	pthread_mutex_unlock(&dev->ctx->lock);
	return m;
}

void datax_fhm_free(struct datax_device *dev)
{
	if (!dev || !dev->fhm)
		return;
	/* Leave the chip as found: FHM off, after the scheduler was destroyed
	 * (its queue flushed). Best effort. */
	if (dev->fhm->enabled)
		(void)phy_write_u64(dev, ATTR_ENABLE, 0);
	pthread_mutex_destroy(&dev->fhm->lock);
	free(dev->fhm->e);
	free(dev->fhm);
	dev->fhm = NULL;
}

static bool in_table(const struct datax_fhm *m, unsigned long long hz)
{
	for (size_t i = 0; i < m->ntable; i++)
		if (m->table[i] == hz)
			return true;
	return false;
}

static unsigned long long us_ticks(unsigned long long us, unsigned long long hz)
{
	return us * hz / 1000000ULL;
}

static unsigned long long ns_ticks(unsigned long long ns, unsigned long long hz)
{
	return ns / 1000000000ULL * hz + ns % 1000000000ULL * hz / 1000000000ULL;
}

static unsigned long long edge_hw(const struct datax_sched *s, const struct fhm_edge *e)
{
	return datax_devtime_to_hw(s->dev, e->api);
}

static struct fhm_edge *find_edge(struct datax_fhm *m, long seq)
{
	for (size_t i = 0; i < m->n; i++)
		if (m->e[i].seq == seq)
			return &m->e[i];
	return NULL;
}

static void remove_edge(struct datax_fhm *m, struct fhm_edge *e)
{
	size_t i = (size_t)(e - m->e);

	memmove(&m->e[i], &m->e[i + 1], (m->n - i - 1) * sizeof(*e));
	m->n--;
}

static bool outstanding(const struct datax_fhm *m)
{
	for (size_t i = 0; i < m->n; i++)
		if (m->e[i].st == E_LIVE)
			return true;
	return false;
}

/* s->lock not held */
static unsigned long long last_hw(struct datax_sched *s)
{
	unsigned long long t;

	pthread_mutex_lock(&s->lock);
	t = s->last_hw;
	pthread_mutex_unlock(&s->lock);
	return t;
}

/* ---- the pipeline (fhm->lock held, s->lock not held, io held) ---------------- */

static void set_late(struct datax_sched *s, struct fhm_edge *e)
{
	e->late = true;
	trace("late E%u seq=%ld hw=%llu reported=%d", e->num, e->seq, edge_hw(s, e), e->reported);
	/* A reported tune's LATE result raises the event in datax_sched_finish;
	 * an unreported one never reaches it. */
	if (!e->reported)
		datax_sched_emit(s, DATAX_EVT_LATE_COMMAND, edge_hw(s, e), e->seq,
				 "seq %ld: stage late", e->seq);
}

/* An unreported tune that cannot land on its frequency. */
static void emit_unreported(struct datax_sched *s, const struct fhm_edge *e, const char *what)
{
	if (!e->reported)
		datax_sched_emit(s, DATAX_EVT_LATE_COMMAND, edge_hw(s, e), e->seq,
				 "seq %ld: %s", e->seq, what);
}

static void on_fired(struct datax_sched *s, struct fhm_edge *e, const char *via)
{
	struct datax_device *dev = s->dev;

	e->st = E_FIRED;
	e->fire_pending = false;
	trace("fired E%u seq=%ld hw=%llu last_hw=%llu via=%s staged=%d", e->num, e->seq,
	      edge_hw(s, e), last_hw(s), via, e->staged);
	if (!e->hz)
		return;
	/* Decision 7: only chip A moved, so the chips' LO phases no longer
	 * match (Task 5 owns PHASE_COHERENT; moot where it is false). */
	pthread_mutex_lock(&dev->ctx->lock);
	dev->single_chip_tuned = true;
	dev->caps.flags &= ~(1u << DATAX_CAP_PHASE_COHERENT);
	pthread_mutex_unlock(&dev->ctx->lock);
	if (e->behind)
		emit_unreported(s, e, "previous edge did not fire; lands one behind");
	/* Its frequency had to be staged before the previous edge, now past. */
	else if (!e->staged && !e->late && !e->stage_err)
		set_late(s, e);
}

/* The fall ending e's pulse is still in the scheduler (it may yet execute:
 * BUILD, PENDING or WRITTEN). s->lock not held. */
static bool fall_alive(struct datax_sched *s, const struct fhm_edge *e)
{
	bool alive;

	pthread_mutex_lock(&s->lock);
	alive = e->fall_seq > 0 && datax_sched_find(s, e->fall_seq) != NULL;
	pthread_mutex_unlock(&s->lock);
	return alive;
}

/* e's rise did not fire: if its fall is not written yet, move it to the
 * minimum pulse after the rise's time, so the line (low anyway) is not held
 * against a later edge. If that time is already inside the guard margin the
 * feeder would complete the fall host-late, with an event for a seq the
 * caller never saw: drop it quietly instead (the line is low; nothing is
 * lost). A written fall cannot move; the gap rule then keeps later edges two
 * full pulses away from it. */
static void shorten_fall(struct datax_sched *s, struct fhm_edge *e)
{
	unsigned long long p = us_ticks(FHM_PULSE_MIN_US, s->tick_hz), now;
	struct sched_cmd *c;

	pthread_mutex_lock(&s->lock);
	c = datax_sched_find(s, e->fall_seq);
	if (c && c->state == CMD_PENDING) {
		if (p < e->pulse) {
			c->api_ts = e->api + p;
			c->hw_ts = datax_devtime_to_hw(s->dev, c->api_ts);
			e->pulse = p;
		}
		now = s->last_hw + ns_ticks(datax_mono_ns() - s->last_hw_mono, s->tick_hz);
		if (c->hw_ts < now + s->guard_ticks)
			datax_sched_remove(s, c);    /* unwritten, unreported: no hook */
	}
	pthread_mutex_unlock(&s->lock);
}

/* Edge i did not fire for a host-known reason: the engine did not move, so
 * the next tune commits the frequency armed for this one (one behind). */
static void mark_next_behind(struct datax_fhm *m, size_t i)
{
	if (i + 1 < m->n && m->e[i + 1].hz && m->e[i + 1].st == E_LIVE)
		m->e[i + 1].behind = true;
}

/*
 * Stage every frequency that is due, in order: tune j once E(j-2) has fired
 * (and its time plus the margin has passed) or never fired, while E(j-1) has
 * not. The device time read after the write decides on time vs late (a
 * false LATE is possible, a false OK is not).
 */
static void advance(struct datax_fhm *m, struct datax_sched *s, const char *via)
{
	unsigned long long margin = us_ticks(FHM_MARGIN_US, s->tick_hz);

	for (size_t j = 0; j < m->n; j++) {
		struct fhm_edge *e = &m->e[j];
		const struct fhm_edge *prev = j ? &m->e[j - 1] : NULL;
		const struct fhm_edge *pp = j > 1 ? &m->e[j - 2] : NULL;
		unsigned long long now = last_hw(s);
		int ret;

		if (!e->hz || e->staged || e->late || e->stage_err || e->st == E_NOEDGE)
			continue;
		if (prev && prev->st != E_NOEDGE &&
		    (prev->st == E_FIRED || now + margin >= edge_hw(s, prev))) {
			trace("stage-skip E%u seq=%ld hz=%llu via=%s now=%llu prev=E%u prev_st=%s "
			      "prev_hw=%llu", e->num, e->seq, e->hz, via, now, prev->num,
			      st_name(prev->st), edge_hw(s, prev));
			set_late(s, e);             /* already too late: do not write */
			continue;
		}
		if (pp && (pp->st == E_LIVE ||
			   (pp->st == E_FIRED && now < edge_hw(s, pp) + margin))) {
			if (!e->hold_traced)
				trace("stage-hold E%u seq=%ld via=%s now=%llu pp=E%u pp_st=%s pp_hw=%llu "
				      "prev_hw=%llu window_us=%llu", e->num, e->seq, via, now, pp->num,
				      st_name(pp->st), edge_hw(s, pp), prev ? edge_hw(s, prev) : 0,
				      prev ? (edge_hw(s, prev) - edge_hw(s, pp)) * 1000000ULL /
					     s->tick_hz : 0);
			e->hold_traced = true;
			break;                      /* E(j-2) still arms what it should */
		}
		{
			unsigned long long t0 = now, h0, mono0 = datax_mono_ns();

			pthread_mutex_lock(&s->lock);
			h0 = s->last_hw_mono;
			pthread_mutex_unlock(&s->lock);
			t0 = now + ns_ticks(mono0 > h0 ? mono0 - h0 : 0, s->tick_hz);
			ret = phy_write_u64(s->dev, ATTR_HOP, e->hz);
			if (ret) {
				trace("stage E%u seq=%ld hz=%llu via=%s start~%llu err=%d", e->num,
				      e->seq, e->hz, via, t0, ret);
				e->stage_err = ret;
				continue;
			}
			ret = datax_poller_read_time(s);
			if (ret ||
			    (prev && prev->st != E_NOEDGE && last_hw(s) + margin >= edge_hw(s, prev)))
				set_late(s, e);
			else
				e->staged = true;
			trace("stage E%u seq=%ld hz=%llu via=%s start~%llu end=%llu write_us=%llu "
			      "prev=E%u prev_hw=%llu pp_hw=%llu margin=%llu result=%s", e->num, e->seq,
			      e->hz, via, t0, last_hw(s), (datax_mono_ns() - mono0) / 1000ULL,
			      prev ? prev->num : 0, prev ? edge_hw(s, prev) : 0,
			      pp ? edge_hw(s, pp) : 0, margin, e->staged ? "ok" : "late");
		}
	}
}

/* Unreported edges complete without a hook: fired once written and due
 * (or pruned, which needs both). */
static void reconcile(struct datax_fhm *m, struct datax_sched *s)
{
	bool any = false;

	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < m->n; i++) {
		struct fhm_edge *e = &m->e[i];
		struct sched_cmd *c;

		if (e->st != E_LIVE)
			continue;
		c = datax_sched_find(s, e->seq);
		if (c) {
			if (c->report)
				e->reported = true;   /* datax_sched_report on a tune */
			else if (c->state == CMD_WRITTEN && c->hw_ts <= s->last_hw)
				e->fire_pending = any = true;
		} else if (!e->reported) {
			/* Every other way out runs the hook (scheduler.h): pruned. */
			e->fire_pending = any = true;
		}
	}
	pthread_mutex_unlock(&s->lock);
	for (size_t i = 0; any && i < m->n; i++)
		if (m->e[i].fire_pending)
			on_fired(s, &m->e[i], "poll");
}

static bool settled(const struct fhm_edge *e)
{
	return !e->hz || e->staged || e->late || e->stage_err || e->st == E_NOEDGE;
}

/* The poll hook alone notices an unreported edge fire (reconcile), and the
 * next staging may wait on it: wake the poller just after the earliest one
 * (its time plus the margin) rather than at its next period. */
static void request_wake(struct datax_fhm *m, struct datax_sched *s)
{
	for (size_t i = 0; i < m->n; i++) {
		const struct fhm_edge *e = &m->e[i];

		if (e->st == E_LIVE && !e->reported) {
			datax_sched_wake_at(s, edge_hw(s, e) + us_ticks(FHM_MARGIN_US, s->tick_hz));
			return;
		}
	}
}

/* Drop edges nobody needs any more (their pulse is over too); keep the
 * last two for ordering. */
static void trim(struct datax_fhm *m, struct datax_sched *s)
{
	unsigned long long now = last_hw(s);

	while (m->n > 2 && m->e[0].st != E_LIVE && settled(&m->e[1]) && settled(&m->e[2]) &&
	       edge_hw(s, &m->e[0]) + m->e[0].pulse < now)
		remove_edge(m, &m->e[0]);
}

/* ---- hooks -------------------------------------------------------------------- */

static void fhm_poll(struct datax_sched *s, unsigned long long hw_now, void *user)
{
	struct datax_fhm *m = user;

	(void)hw_now;   /* s->last_hw is re-read as staging writes take time */
	pthread_mutex_lock(&m->lock);
	reconcile(m, s);
	advance(m, s, "poll");
	trim(m, s);
	request_wake(m, s);
	pthread_mutex_unlock(&m->lock);
}

/* Set r's status and put what, ahead of any detail it already carries
 * (such as "poller lagged; ..."). */
static void __attribute__((format(printf, 3, 4)))
set_result(struct datax_sched_result *r, enum datax_completion_status st, const char *fmt, ...)
{
	char what[DATAX_SCHED_DETAIL_LEN], old[DATAX_SCHED_DETAIL_LEN];
	size_t n;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	snprintf(old, sizeof(old), "%s", r->detail);
	r->status = st;
	n = strlen(what);
	memcpy(r->detail, what, n + 1);
	if (old[0] && n + 2 < sizeof(r->detail)) {
		size_t k = strlen(old), room = sizeof(r->detail) - n - 3;

		memcpy(r->detail + n, "; ", 2);
		if (k > room)
			k = room;
		memcpy(r->detail + n + 2, old, k);
		r->detail[n + 2 + k] = '\0';
	}
}

/* Reported tune: one behind, stage late, stage failure or the PLL readback. */
static void verdict(struct datax_sched *s, const struct fhm_edge *e,
		    struct datax_sched_result *r)
{
	unsigned long long got = 0;
	int ret;

	int in = (int)r->status;

	if (e->behind) {
		set_result(r, DATAX_COMPLETION_DROPPED, "previous edge did not fire; lands one behind");
	} else if (e->late) {
		set_result(r, DATAX_COMPLETION_LATE, "stage late");
	} else if (e->stage_err) {
		set_result(r, DATAX_COMPLETION_DROPPED, "stage write failed (%d)", e->stage_err);
	} else {
		ret = phy_read_u64(s->dev, ATTR_HOP, &got);
		if (ret)
			set_result(r, DATAX_COMPLETION_DROPPED, "PLL readback failed (%d)", ret);
		else if (got != e->hz)
			set_result(r, DATAX_COMPLETION_DROPPED, "PLL %llu Hz, expected %llu Hz", got,
				   e->hz);
	}
	trace("verdict E%u seq=%ld hw=%llu exec=%llu last_hw=%llu behind=%d late=%d staged=%d "
	      "stage_err=%d readback=%llu want=%llu status_in=%d status_out=%d detail=\"%s\"",
	      e->num, e->seq, edge_hw(s, e), datax_devtime_to_hw(s->dev, r->executed_ticks),
	      last_hw(s), e->behind, e->late, e->staged, e->stage_err, got, e->hz, in,
	      (int)r->status, r->detail);
}

/*
 * A fall's hook: only a flush or destroy matters. flush_queue dropped this
 * fall; if its rise executed bit 0 is left high, and the next priming rise
 * would write 1 onto a high line: no edge, reported EXECUTED, every later
 * tune one behind. Park the line low
 * through the undroppable immediate write (datax_tick_gpio_now): the
 * scheduler's builder would need a commit a hook cannot make, and is lost
 * on destroy. io held (flush's or destroy's caller); the queue is empty.
 */
static void fhm_on_fall(struct datax_sched *s, long seq, struct datax_sched_result *r,
			void *user)
{
	(void)user;
	if (r->discarded || r->status != DATAX_COMPLETION_FLUSHED)
		return;
	trace("park after flushed fall seq=%ld", seq);
	/* Always (round 3 ruling): the queue is empty, so whatever pulse is on
	 * the line is orphaned. Deciding from the rise's state misses a rise
	 * that executed before the poller saw it (record undrained, or not yet
	 * pruned): destroy does not drain first. */
	(void)datax_tick_gpio_now(s->dev, s, TRIGGER_MASK, 0);
}

static void fhm_on_edge(struct datax_sched *s, long seq, struct datax_sched_result *r,
			void *user)
{
	struct datax_fhm *m = user;
	struct fhm_edge *e;
	bool record;

	pthread_mutex_lock(&m->lock);
	e = find_edge(m, seq);
	if (!e) {
		pthread_mutex_unlock(&m->lock);
		return;
	}
	if (r->discarded) {               /* a rejected commit: it never existed */
		remove_edge(m, e);
		pthread_mutex_unlock(&m->lock);
		return;
	}
	record = r->tick_status != UINT32_MAX;
	if (e->st == E_LIVE) {
		if (record && (r->tick_status == DATAX_TICK_STATUS_EXECUTED ||
			       r->tick_status == DATAX_TICK_STATUS_LATE_EXECUTED)) {
			on_fired(s, e, "record");
		} else {
			e->st = E_NOEDGE;     /* dropped, flushed, never written */
			trace("noedge E%u seq=%ld hw=%llu status=%d tick_status=%u detail=\"%s\"",
			      e->num, e->seq, edge_hw(s, e), (int)r->status, r->tick_status, r->detail);
			shorten_fall(s, e);
			/* Host-known no-fire (host-late, write failure, late-PPS
			 * drop, Tick drop record): the next tune lands one behind.
			 * Not on flush/destroy (it goes too), nor a lost report
			 * (the edge most likely fired). */
			if (r->status != DATAX_COMPLETION_FLUSHED &&
			    strncmp(r->detail, "no completion", 13))
				mark_next_behind(m, (size_t)(e - m->e));
		}
	}
	if (record && e->st == E_FIRED) {
		/* Read the PLL back before staging: a staging write can take long
		 * enough for the next edge to fire, and a readback after it would
		 * see the next frequency. That costs the staging one round trip. */
		if (e->hz && r->status != DATAX_COMPLETION_DROPPED)
			verdict(s, e, r);
		advance(m, s, "record");
	}
	pthread_mutex_unlock(&m->lock);
}

/* ---- stage_hop_table ---------------------------------------------------------- */

static int cmp_ull(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;

	return x < y ? -1 : x > y;
}

/* The staging round trip: FHM_STAGE_SAMPLES writes, each between two
 * device-time reads, less the median cost of a back-to-back time read (so
 * each sample is one write). io held if a scheduler exists. */
static int measure_stage(struct datax_device *dev, unsigned long long hz,
			 unsigned long long *us)
{
	unsigned long long t[FHM_STAGE_SAMPLES + 1], d[FHM_STAGE_SAMPLES], tick_hz, rc;
	int ret = datax_tick_read_limits(dev, &tick_hz, NULL, NULL);

	if (ret)
		return ret;
	for (unsigned int i = 0; i <= 3; i++) {
		ret = datax_tick_read_u64(dev, "current_time_ticks", &t[i]);
		if (ret)
			return ret;
	}
	for (unsigned int i = 0; i < 3; i++)
		d[i] = t[i + 1] - t[i];
	qsort(d, 3, sizeof(d[0]), cmp_ull);
	rc = d[1];
	for (unsigned int i = 0; i <= FHM_STAGE_SAMPLES; i++) {
		ret = datax_tick_read_u64(dev, "current_time_ticks", &t[i]);
		if (!ret && i < FHM_STAGE_SAMPLES)
			ret = phy_write_u64(dev, ATTR_HOP, hz);
		if (ret)
			return ret;
	}
	for (unsigned int i = 0; i < FHM_STAGE_SAMPLES; i++)
		d[i] = t[i + 1] - t[i] > rc ? t[i + 1] - t[i] - rc : 0;
	qsort(d, FHM_STAGE_SAMPLES, sizeof(d[0]), cmp_ull);
	*us = (d[FHM_STAGE_SAMPLES / 2] * 1000000ULL + tick_hz - 1) / tick_hz;
	if (!*us)
		*us = 1;
	return 0;
}

int datax_device_stage_hop_table(struct datax_device *dev,
				 const unsigned long long *freqs_hz, size_t count)
{
	unsigned long long uniq[FHM_TABLE_DEPTH], us = 0;
	struct datax_sched *s;
	struct datax_io_hold h;
	struct datax_fhm *m;
	size_t n = 0;
	int ret;

	if (!dev)
		return -EINVAL;
	if (dev->family == DATAX_FAMILY_AD936X)
		return datax_error_ret(dev->ctx, ENOTSUP, "fhm",
				       "stage_hop_table: ad936x fastlock hop staging arrives in P4");
	if (dev->family != DATAX_FAMILY_ADRV9009)
		return datax_error_ret(dev->ctx, ENOTSUP, "fhm",
				       "stage_hop_table: no hop staging for this device family");
	if (!freqs_hz || !count)
		return datax_error_ret(dev->ctx, EINVAL, "fhm", "stage_hop_table: empty table");
	if (count > FHM_TABLE_DEPTH)
		return datax_error_ret(dev->ctx, EINVAL, "fhm",
				       "stage_hop_table: %zu entries exceed the hop table depth (%u)",
				       count, FHM_TABLE_DEPTH);
	for (size_t i = 0; i < count; i++) {
		bool dup = false;

		if (freqs_hz[i] < FHM_BAND_MIN_HZ || freqs_hz[i] > FHM_BAND_MAX_HZ)
			return datax_error_ret(dev->ctx, EINVAL, "fhm",
					       "stage_hop_table: %llu Hz is outside the FHM band "
					       "%llu..%llu Hz (adrv9009 driver default)", freqs_hz[i],
					       FHM_BAND_MIN_HZ, FHM_BAND_MAX_HZ);
		for (size_t k = 0; k < n && !dup; k++)
			dup = uniq[k] == freqs_hz[i];
		if (!dup)
			uniq[n++] = freqs_hz[i];
	}
	qsort(uniq, n, sizeof(uniq[0]), cmp_ull);
	if (!datax_tick_present(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "fhm",
				       "no Tick scheduler in this context (it triggers the hops)");
	if (!dev->fhm_ops)
		return datax_error_ret(dev->ctx, ENOTSUP, "fhm",
				       "stage_hop_table: adrv9009-phy has no TRX LO channel "
				       "(altvoltage0) for FHM");
	m = fhm_get(dev, true);
	if (!m)
		return datax_error_ret(dev->ctx, ENOMEM, "fhm", "out of memory");

	s = datax_device_sched_get(dev);
	if (s && datax_sched_holds_io(s)) {
		datax_sched_put(s);
		return datax_error_ret(dev->ctx, EDEADLK, "fhm",
				       "stage_hop_table from a scheduler hook would deadlock");
	}
	if (s)
		datax_sched_io_lock(s, &h);
	pthread_mutex_lock(&m->lock);
	if (outstanding(m)) {
		ret = datax_error_ret(dev->ctx, EBUSY, "fhm",
				      "stage_hop_table: timed tunes are queued; flush them first");
		goto out;
	}
	/* A fired tune's pulse still on the line: the park below would cut
	 * it. Wait for its fall (or flush, which parks the line itself). */
	for (size_t i = 0; s && i < m->n; i++)
		if (fall_alive(s, &m->e[i])) {
			ret = datax_error_ret(dev->ctx, EBUSY, "fhm",
					      "stage_hop_table: a tune's trigger pulse is still "
					      "on the line; wait for it or flush");
			goto out;
		}
	/* Park the trigger low first, as hopcap (TickSession.park_trigger_low)
	 * and the Tick demo do before enabling FHM: the priming rise needs a
	 * low line to make an edge. */
	ret = datax_tick_gpio_now(dev, s, TRIGGER_MASK, 0);
	if (ret) {
		ret = datax_error_ret(dev->ctx, -ret, "fhm",
				      "stage_hop_table: parking the FHM trigger low failed (%d)", ret);
		goto out;
	}
	/* Enabling (re)primes the engine: the next edge lands on
	 * fhmInitFrequency (fhm-pipeline.md). Chip A only (decision 7). */
	ret = phy_write_u64(dev, ATTR_ENABLE, 1);
	if (ret) {
		ret = datax_error_ret(dev->ctx, -ret, "fhm",
				      "stage_hop_table: writing %s failed (%d)", ATTR_ENABLE, ret);
		goto out;
	}
	m->enabled = true;
	/* The driver can swallow a TALISE_setFhmMode failure (adrv9009.c
	 * FHM_ENABLE: its return is not checked): read it back. */
	{
		unsigned long long on = 0;

		ret = phy_read_u64(dev, ATTR_ENABLE, &on);
		if (ret || on != 1) {
			ret = datax_error_ret(dev->ctx, EIO, "fhm",
					      "stage_hop_table: FHM did not enable (%s reads %llu, "
					      "err %d)", ATTR_ENABLE, on, ret);
			goto out;
		}
	}
	ret = measure_stage(dev, uniq[0], &us);
	if (ret) {
		ret = datax_error_ret(dev->ctx, -ret, "fhm",
				      "stage_hop_table: timing the %s staging write failed (%d)",
				      ATTR_HOP, ret);
		goto out;
	}
	memcpy(m->table, uniq, n * sizeof(uniq[0]));
	m->ntable = n;
	m->stage_us = us;
	m->fresh = true;
	m->next_num = 0;
	trace("staged table n=%zu stage_us=%llu (enable, park and measure done)", n, us);
	m->n = 0;                          /* nothing outstanding: forget the past */
	pthread_mutex_lock(&dev->ctx->lock);
	dev->caps.limits[DATAX_LIMIT_HOP_STAGE_US] = us;
	dev->caps.measured |= 1u << DATAX_LIMIT_HOP_STAGE_US;
	pthread_mutex_unlock(&dev->ctx->lock);
	ret = 0;
out:
	pthread_mutex_unlock(&m->lock);
	if (s)
		datax_sched_io_unlock(s, &h);
	datax_sched_put(s);
	return ret;
}

/* ---- queue_tune --------------------------------------------------------------- */

/* Family, channel (chip A only) and staged frequency. */
static int check_tune(struct datax_device *dev, unsigned int ch, unsigned long long hz,
		      const char *verb)
{
	const struct datax_family_ops *ops = datax_family_ops(dev->family);
	struct datax_fhm *m;
	unsigned int n;
	bool staged = false;

	if (dev->family != DATAX_FAMILY_ADRV9009 || !ops)
		return datax_error_ret(dev->ctx, ENOTSUP, "fhm",
				       "%s: no timed tunes for this device family (ad936x "
				       "fastlock hops arrive in P4)", verb);
	if (ch >= dev->rx_channels + dev->tx_channels)
		return datax_error_ret(dev->ctx, EINVAL, "fhm",
				       "%s: channel %u out of range (device has %u)", verb, ch,
				       dev->rx_channels + dev->tx_channels);
	n = ch < dev->rx_channels ? ch : ch - dev->rx_channels;
	if (n / ops->rx_channels_per_phy)
		return datax_error_ret(dev->ctx, ENOTSUP, "fhm",
				       "%s: chip B FHM trigger is not wired on this image "
				       "(channel %u)", verb, ch);
	m = fhm_get(dev, false);
	if (m) {
		pthread_mutex_lock(&m->lock);
		staged = in_table(m, hz);
		pthread_mutex_unlock(&m->lock);
	}
	if (!staged)
		return datax_error_ret(dev->ctx, EINVAL, "fhm",
				       "%s: frequency not staged: %llu Hz is not in the hop table "
				       "(stage_hop_table first)", verb, hz);
	return 0;
}

static long enqueue_edge(struct datax_sched *s, datax_time when, bool rise, bool report,
			 bool consume, struct datax_fhm *m)
{
	struct datax_sched_cmd_spec spec = {
		.when = when,
		.command_id = DATAX_TICK_COMMAND_ID_GPIO_WRITE,
		.target_id = DATAX_TICK_TARGET_FAMILY_GPIO,
		.args = { TRIGGER_MASK, rise ? TRIGGER_MASK : 0u, 0, 0 },
		.report = report,
		.consume = consume,
		.on_complete = rise ? fhm_on_edge : fhm_on_fall,
		.user = m,
	};

	return datax_sched_enqueue(s, &spec);
}

static void unqueue(struct datax_sched *s, const long *seqs, size_t n)
{
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < n; i++) {
		struct sched_cmd *c = seqs[i] > 0 ? datax_sched_find(s, seqs[i]) : NULL;

		if (c && c->state == CMD_BUILD)
			datax_sched_remove(s, c);
	}
	pthread_mutex_unlock(&s->lock);
}

/* The fall ending live's pulse can still move (its batch is uncommitted). */
static bool fall_movable(struct datax_sched *s, const struct fhm_edge *live)
{
	const struct sched_cmd *c;
	bool movable;

	pthread_mutex_lock(&s->lock);
	c = datax_sched_find(s, live->fall_seq);
	movable = c && c->state == CMD_BUILD;
	pthread_mutex_unlock(&s->lock);
	return movable;
}

/* Least distance after live for the next edge: two pulses, the pulse
 * shrunk to the minimum while its fall can still move. */
static unsigned long long min_after(struct datax_sched *s, const struct fhm_edge *live)
{
	return 2 * (fall_movable(s, live) ? us_ticks(FHM_PULSE_MIN_US, s->tick_hz) : live->pulse);
}

/* Pull live's fall in to half the distance to the next edge at `next`
 * (never longer than it is). */
static void fit_pulse(struct datax_sched *s, struct fhm_edge *live, datax_time next)
{
	unsigned long long p = (next - live->api) / 2;
	struct sched_cmd *c;

	if (p >= live->pulse)
		return;
	pthread_mutex_lock(&s->lock);
	c = datax_sched_find(s, live->fall_seq);
	if (c && c->state == CMD_BUILD) {
		c->api_ts = live->api + p;
		c->hw_ts = datax_devtime_to_hw(s->dev, c->api_ts);
		live->pulse = p;
	}
	pthread_mutex_unlock(&s->lock);
}

/* consume: the tune's result is not kept (timed set_frequency: the caller
 * has no seq to wait on). Its hook still verifies it. */
static long queue_tune(struct datax_sched *s, datax_time when, unsigned int ch,
		       unsigned long long hz, bool consume)
{
	struct datax_device *dev = s->dev;
	struct datax_fhm *m;
	struct fhm_edge *last = NULL, *live = NULL;
	unsigned long long hzt = s->tick_hz, pmax, ppulse, window, lead, now, when_hw;
	datax_time first;
	bool prime, report = true;
	long seqs[4] = { 0, 0, 0, 0 };
	long ret;

	ret = check_tune(dev, ch, hz, "queue_tune");
	if (ret)
		return ret;
	m = fhm_get(dev, false);
	pthread_mutex_lock(&m->lock);
	pmax = us_ticks(FHM_PULSE_MAX_US, hzt);
	/*
	 * The priming lead is tune 2's whole staging window: its frequency is
	 * staged after the (unreported) priming edge fires and before tune 1.
	 * Only the poll hook sees the priming edge fire, so the lead must cover
	 * (tron finding, fix/p3a-stage-late):
	 *   window: a poll period (the poller is also woken at the edge's time
	 *           plus the margin, datax_sched_wake_at), the staging round
	 *           trip twice (it varies under load) and the margin;
	 *   + one poller iteration before the hook runs: six Tick round trips
	 *     (time, completion seq, late_count, overflow_count, queue_level,
	 *     one descriptor write) at the access cost measured at create;
	 *   + two minimum pulses;
	 * and never less than the guard margin (DATAX_LIMIT_GUARD_MARGIN_MS,
	 * 10 ms), which dominates on tron (the sum is about 3.5 ms there).
	 * The first tune after staging or after the hops drained therefore needs
	 * when >= now + guard + lead (20 ms on tron).
	 */
	window = us_ticks(2 * m->stage_us + s->poll_us + FHM_MARGIN_US, hzt);
	lead = window + 6 * s->access_ticks + 2 * us_ticks(FHM_PULSE_MIN_US, hzt);
	if (lead < s->guard_ticks)
		lead = s->guard_ticks;
	if (lead < us_ticks(FHM_MIN_LEAD_US, hzt))
		lead = us_ticks(FHM_MIN_LEAD_US, hzt);
	ppulse = lead / 2 < pmax ? lead / 2 : pmax;
	pthread_mutex_lock(&s->lock);
	now = s->last_hw + ns_ticks(datax_mono_ns() - s->last_hw_mono, hzt);
	pthread_mutex_unlock(&s->lock);
	if (m->n)
		last = &m->e[m->n - 1];
	/* The gap reference: the latest edge whose pulse can still be on the
	 * line, i.e. any edge that is not NOEDGE, or a NOEDGE one whose fall is
	 * still queued (it executes, and would cut a later pulse short). */
	for (size_t i = m->n; i-- > 0 && !live;)
		if (m->e[i].st != E_NOEDGE || fall_alive(s, &m->e[i]))
			live = &m->e[i];
	/* A priming edge when the engine was just enabled, or when the last
	 * edge has fired (or is about to) so nothing is left to commit this
	 * tune's frequency: the next edge would commit whatever was armed. */
	prime = m->fresh || !last || last->st == E_NOEDGE ||
		edge_hw(s, last) <= now + window;
	if (prime && when < lead) {
		ret = datax_error_ret(s->ctx, EINVAL, "fhm",
				      "queue_tune: the tune at %llu needs a priming edge %llu us "
				      "before it", when, lead * 1000000ULL / hzt);
		goto out;
	}
	first = prime ? when - lead : when;
	/* In time order, rise-fall-rise well formed: at least two pulses
	 * after the previous edge (the pulse it keeps). */
	if (live && (first <= live->api || first - live->api < min_after(s, live))) {
		if (prime && when > live->api)
			ret = datax_error_ret(s->ctx, EINVAL, "fhm",
					      "queue_tune: the tune at %llu needs a priming edge %llu "
					      "us before it, too close to the previous edge (at %llu)",
					      when, lead * 1000000ULL / hzt, live->api);
		else
			ret = datax_error_ret(s->ctx, EINVAL, "fhm",
					      "queue_tune: tunes must be queued in time order, at "
					      "least %llu us after the previous edge (at %llu)",
					      min_after(s, live) * 1000000ULL / hzt, live->api);
		goto out;
	}
	/* A priming edge inside the guard margin would fail the commit under
	 * its own seq: refuse here, naming the tune. */
	if (prime && datax_devtime_to_hw(dev, first) < now + s->guard_ticks) {
		ret = datax_error_ret(s->ctx, ETIME, "fhm",
				      "queue_tune: the tune at %llu needs a priming edge %llu us "
				      "before it, inside the guard margin of now (%llu)", when,
				      lead * 1000000ULL / hzt, datax_devtime_from_hw(dev, now));
		goto out;
	}
	/* Ruling 1: report the rise only when no reported command is closer
	 * than the report spacing (the same test commit applies), so dense hops
	 * report every Nth rise. */
	when_hw = datax_devtime_to_hw(dev, when);
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->ncmds && report; i++) {
		const struct sched_cmd *c = &s->cmds[i];
		unsigned long long d;

		if (!c->report || c->state == CMD_DONE)
			continue;
		d = c->hw_ts > when_hw ? c->hw_ts - when_hw : when_hw - c->hw_ts;
		report = d >= s->spacing_ticks;
	}
	pthread_mutex_unlock(&s->lock);
	if (m->n + 2 > m->cap) {
		size_t cap = m->cap ? 2 * m->cap : 16;
		struct fhm_edge *e = realloc(m->e, cap * sizeof(*e));

		if (!e) {
			ret = datax_error_ret(s->ctx, ENOMEM, "fhm", "out of memory");
			goto out;
		}
		if (live)
			live = e + (live - m->e);
		m->e = e;
		m->cap = cap;
	}
	if (prime) {
		seqs[0] = enqueue_edge(s, first, true, false, false, m);
		if (seqs[0] > 0)
			seqs[1] = enqueue_edge(s, first + ppulse, false, false, false, m);
	}
	if (!prime || seqs[1] > 0) {
		seqs[2] = enqueue_edge(s, when, true, report, consume, m);
		if (seqs[2] > 0)
			seqs[3] = enqueue_edge(s, when + pmax, false, false, false, m);
	}
	if (seqs[3] <= 0) {
		for (int i = 0; i < 4; i++)
			if (seqs[i] < 0)
				ret = seqs[i];
		unqueue(s, seqs, 4);
		goto out;
	}
	if (live)
		fit_pulse(s, live, first);
	if (prime) {
		trace("queue E%u prime seq=%ld fall=%ld hw=%llu pulse=%llu lead_us=%llu window_us=%llu "
		      "now~%llu", m->next_num, seqs[0], seqs[1], datax_devtime_to_hw(dev, first),
		      ppulse, lead * 1000000ULL / hzt, window * 1000000ULL / hzt, now);
		m->e[m->n++] = (struct fhm_edge){ .seq = seqs[0], .api = first,
						  .fall_seq = seqs[1], .pulse = ppulse,
						  .num = m->next_num++ };
	}
	trace("queue E%u tune seq=%ld fall=%ld hw=%llu hz=%llu reported=%d consume=%d now~%llu",
	      m->next_num, seqs[2], seqs[3], when_hw, hz, report, consume, now);
	m->e[m->n++] = (struct fhm_edge){ .num = m->next_num++, .seq = seqs[2], .api = when, .hz = hz,
					  .reported = report, .fall_seq = seqs[3],
					  .pulse = pmax };
	m->fresh = false;
	(void)datax_sched_set_poll_hook(s, fhm_poll, m);
	request_wake(m, s);
	ret = seqs[2];
out:
	pthread_mutex_unlock(&m->lock);
	return ret;
}

long datax_fhm_queue_tune(struct datax_sched *s, datax_time when, unsigned int ch,
			  unsigned long long hz)
{
	return queue_tune(s, when, ch, hz, false);
}

/* ---- timed set_frequency ------------------------------------------------------ */

int datax_fhm_timed_set_frequency(struct datax_device *dev, unsigned int ch,
				  unsigned long long hz, datax_time when)
{
	struct datax_sched *s, *made = NULL;
	size_t nb = 0;
	long seq;
	int ret;

	/* Validate first: a refused call must not leave a scheduler behind. */
	ret = check_tune(dev, ch, hz, "set_frequency");
	if (ret)
		return ret;
	s = datax_device_sched_get(dev);
	if (!s) {
		/* Ruling 3: the device's scheduler, created lazily and destroyed
		 * with the device (context destroy). */
		ret = datax_sched_create(dev, &made);
		if (ret && ret != -EBUSY)
			return ret;
		s = datax_device_sched_get(dev);
		if (!s)
			return datax_error_ret(dev->ctx, EAGAIN, "fhm",
					       "set_frequency: the device's scheduler went away");
	}
	if (datax_sched_holds_io(s)) {
		datax_sched_put(s);
		return datax_error_ret(dev->ctx, EDEADLK, "fhm",
				       "set_frequency from a scheduler hook would deadlock");
	}
	/* Committing would also commit (or, on a rejection, discard) the
	 * caller's own uncommitted batch on this scheduler: refuse instead. */
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->ncmds; i++)
		nb += s->cmds[i].state == CMD_BUILD;
	pthread_mutex_unlock(&s->lock);
	if (nb) {
		datax_sched_put(s);
		return datax_error_ret(dev->ctx, EBUSY, "fhm",
				       "set_frequency: the device's scheduler has %zu uncommitted "
				       "commands; use datax_sched_queue_tune and commit", nb);
	}
	/* The caller gets no seq, so the result is not kept (it would pile up
	 * on a scheduler nobody holds). The hook still stages and verifies it;
	 * any non-OK result raises DATAX_EVT_LATE_COMMAND (datax_sched_finish). */
	seq = queue_tune(s, when, ch, hz, true);
	ret = seq < 0 ? (int)seq : datax_sched_commit(s);
	datax_sched_put(s);
	return ret;
}
