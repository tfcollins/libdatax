// SPDX-License-Identifier: MIT
/*
 * Scheduler: builders, commit, flush, wait and completions (api-c-core
 * "Scheduler semantics"). The poller thread that feeds Tick and drains its
 * completion window lives in poller.c; the layout and lock rules are in
 * scheduler.h.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "error.h"
#include "fhm.h"
#include "scheduler.h"
#include "tick_descriptor.h"
#include "tick_ops.h"

#define SEQ_MAX 0x7fffffffu      /* seqs are positive longs, never 0 */

static int flush_outstanding(struct datax_sched *s, const char *detail);
#define MARKER_LINES 8u          /* RTL parameter MARKER_COUNT default */

/* ---- events ----------------------------------------------------------------- */

void datax_event_fire(struct datax_context *ctx, const struct datax_event *evt)
{
	datax_event_cb cb;
	void *user;

	if (!ctx || !evt)
		return;
	pthread_mutex_lock(&ctx->lock);
	cb = ctx->event_cb;
	user = ctx->event_user;
	pthread_mutex_unlock(&ctx->lock);
	if (cb)
		cb(evt, user);
}

static void emit_v(struct datax_sched *s, enum datax_event_type type,
		   unsigned long long hw_ticks, long seq, const char *fmt, va_list ap)
{
	struct sched_event *e;

	if (s->nevq == SCHED_EVQ_LEN) {
		/* summarised at delivery: "N events dropped" */
		s->evq_lost++;
		s->evq_lost_type = type;
		return;
	}
	e = &s->evq[s->nevq++];
	e->type = type;
	e->hw_ticks = hw_ticks;
	e->seq = seq;
	vsnprintf(e->detail, sizeof(e->detail), fmt, ap);
}

void datax_sched_emit_locked(struct datax_sched *s, enum datax_event_type type,
			     unsigned long long hw_ticks, long seq, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit_v(s, type, hw_ticks, seq, fmt, ap);
	va_end(ap);
}

void datax_sched_emit(struct datax_sched *s, enum datax_event_type type,
		      unsigned long long hw_ticks, long seq, const char *fmt, ...)
{
	va_list ap;

	if (!s)
		return;
	pthread_mutex_lock(&s->lock);
	va_start(ap, fmt);
	emit_v(s, type, hw_ticks, seq, fmt, ap);
	va_end(ap);
	s->kick = true;
	pthread_cond_signal(&s->wake);
	pthread_mutex_unlock(&s->lock);
}

/* ---- command table (s->lock held) -------------------------------------------- */

struct sched_cmd *datax_sched_find(struct datax_sched *s, long seq)
{
	for (size_t i = 0; i < s->ncmds; i++)
		if (s->cmds[i].seq == seq)
			return &s->cmds[i];
	return NULL;
}

void datax_sched_remove(struct datax_sched *s, struct sched_cmd *c)
{
	size_t i = (size_t)(c - s->cmds);

	if (i >= s->ncmds)
		return;
	s->cmds[i] = s->cmds[--s->ncmds];
}

static void publish(struct datax_sched *s, struct sched_cmd *c)
{
	if (!c->report || c->consume) {
		datax_sched_remove(s, c);
		return;
	}
	c->state = CMD_DONE;
	c->done_order = s->next_done++;
	pthread_cond_broadcast(&s->done);
}

void datax_sched_finish(struct datax_sched *s, struct sched_cmd *c,
			struct datax_sched_result *res)
{
	long seq = c->seq;
	datax_sched_complete_fn fn = c->on_complete;
	void *user = c->user;

	if (fn) {
		pthread_mutex_unlock(&s->lock);
		fn(s, seq, res, user);
		pthread_mutex_lock(&s->lock);
		c = datax_sched_find(s, seq);
		if (!c || c->state == CMD_DONE)
			return;
	}
	if (res->status == DATAX_COMPLETION_LATE)
		datax_sched_emit_locked(s, DATAX_EVT_LATE_COMMAND,
					datax_devtime_to_hw(s->dev, res->executed_ticks ?
							    res->executed_ticks :
							    res->requested_ticks),
					seq, "seq %ld: %s", seq, res->detail);
	/* A consumed result is never published, so any other non-OK outcome
	 * (DROPPED, FLUSHED) would vanish: report it the same way (controller
	 * ruling; there is no DROPPED event type). */
	else if (c->consume && res->status != DATAX_COMPLETION_OK)
		datax_sched_emit_locked(s, DATAX_EVT_LATE_COMMAND,
					datax_devtime_to_hw(s->dev, res->requested_ticks), seq,
					"seq %ld: %s: %s", seq,
					res->status == DATAX_COMPLETION_DROPPED ? "DROPPED" :
					res->status == DATAX_COMPLETION_FLUSHED ? "FLUSHED" : "?",
					res->detail);
	c->res = *res;
	publish(s, c);
}

void datax_sched_map_status(uint32_t tick_status, struct datax_sched_result *r)
{
	r->tick_status = tick_status;
	r->detail[0] = '\0';
	switch (tick_status) {
	case DATAX_TICK_STATUS_EXECUTED:
		r->status = DATAX_COMPLETION_OK;
		return;
	case DATAX_TICK_STATUS_LATE_EXECUTED:
		r->status = DATAX_COMPLETION_LATE;
		snprintf(r->detail, sizeof(r->detail), "Tick executed it late");
		return;
	case DATAX_TICK_STATUS_LATE_DROPPED:
		r->status = DATAX_COMPLETION_LATE;
		snprintf(r->detail, sizeof(r->detail), "late: Tick dropped it");
		return;
	case DATAX_TICK_STATUS_OVERFLOW:
		r->status = DATAX_COMPLETION_DROPPED;
		snprintf(r->detail, sizeof(r->detail), "Tick queue overflow");
		return;
	case DATAX_TICK_STATUS_UNSUPPORTED:
		r->status = DATAX_COMPLETION_DROPPED;
		snprintf(r->detail, sizeof(r->detail), "Tick does not support this command");
		return;
	case DATAX_TICK_STATUS_BAD_DESCRIPTOR:
		r->status = DATAX_COMPLETION_DROPPED;
		snprintf(r->detail, sizeof(r->detail), "Tick rejected the descriptor");
		return;
	default:
		r->status = DATAX_COMPLETION_DROPPED;
		snprintf(r->detail, sizeof(r->detail), "Tick status %u", tick_status);
		return;
	}
}

void datax_sched_format_desc(const struct sched_cmd *c, char *buf, size_t len)
{
	struct datax_tick_descriptor d = {
		.version = DATAX_TICK_DESC_VERSION,
		.size_bytes = DATAX_TICK_DESC_SIZE,
		.command_id = c->command_id,
		.target_id = c->target_id,
		.timestamp_ticks = c->hw_ts,
		/* Drop-on-late always; a completion only when reported. Never
		 * REPEAT (later occurrences report another requested timestamp)
		 * nor LATE_EXECUTE_NOW: poller.c process() matches a record to its
		 * command by seq, command and requested == hw_ts, which holds only
		 * for these one-shot, drop-on-late descriptors. */
		.flags = DATAX_TICK_FLAG_LATE_DROP |
			 (c->report ? DATAX_TICK_FLAG_REPORT_COMPLETION : 0),
		.sequence_id = (uint32_t)c->seq,
		.args = { c->args[0], c->args[1], c->args[2], c->args[3] },
	};
	unsigned char b[DATAX_TICK_DESC_SIZE];
	size_t n = 0;

	datax_internal_tick_descriptor_pack(&d, b);
	/* The driver's CSV: twelve 32-bit words, little-endian from the packer. */
	for (unsigned int i = 0; i < DATAX_TICK_DESC_SIZE / 4 && n < len; i++) {
		uint32_t w = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 |
			     (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
		int k = snprintf(buf + n, len - n, "%s%u", i ? "," : "", w);

		if (k < 0)
			break;
		n += (size_t)k;
	}
}

/* ---- create / destroy ---------------------------------------------------------- */

struct datax_sched *datax_device_sched(const struct datax_device *dev)
{
	struct datax_sched *s;

	if (!dev)
		return NULL;
	pthread_mutex_lock(&dev->ctx->lock);
	s = dev->sched;
	pthread_mutex_unlock(&dev->ctx->lock);
	return s;
}

struct datax_sched *datax_device_sched_get(const struct datax_device *dev)
{
	struct datax_sched *s;

	if (!dev)
		return NULL;
	pthread_mutex_lock(&dev->ctx->lock);
	s = dev->sched;
	if (s)
		s->refs++;
	pthread_mutex_unlock(&dev->ctx->lock);
	return s;
}

void datax_sched_put(struct datax_sched *s)
{
	if (!s)
		return;
	pthread_mutex_lock(&s->ctx->lock);
	if (!--s->refs)
		pthread_cond_broadcast(&s->unref);
	pthread_mutex_unlock(&s->ctx->lock);
}

/* Per-thread stack of held io locks (C11 _Thread_local). */
static _Thread_local struct datax_io_hold *io_held;

void datax_sched_io_lock(struct datax_sched *s, struct datax_io_hold *h)
{
	pthread_mutex_lock(&s->io);
	h->s = s;
	h->prev = io_held;
	io_held = h;
}

void datax_sched_io_unlock(struct datax_sched *s, struct datax_io_hold *h)
{
	io_held = h->prev;
	pthread_mutex_unlock(&s->io);
}

bool datax_sched_holds_io(const struct datax_sched *s)
{
	for (const struct datax_io_hold *h = io_held; h; h = h->prev)
		if (h->s == s)
			return true;
	return false;
}

static int io_busy(struct datax_sched *s, const char *verb)
{
	return datax_error_ret(s->ctx, EDEADLK, "sched",
			       "%s from a scheduler hook (this thread holds the Tick I/O "
			       "lock) would deadlock", verb);
}

static int init_sync(struct datax_sched *s)
{
	pthread_condattr_t ca;
	int ret;

	ret = pthread_condattr_init(&ca);
	if (ret)
		return -ret;
	ret = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	if (!ret)
		ret = pthread_mutex_init(&s->io, NULL);
	if (!ret && (ret = pthread_mutex_init(&s->lock, NULL)))
		pthread_mutex_destroy(&s->io);
	if (!ret && (ret = pthread_cond_init(&s->wake, &ca))) {
		pthread_mutex_destroy(&s->lock);
		pthread_mutex_destroy(&s->io);
	}
	if (!ret && (ret = pthread_cond_init(&s->done, &ca))) {
		pthread_cond_destroy(&s->wake);
		pthread_mutex_destroy(&s->lock);
		pthread_mutex_destroy(&s->io);
	}
	if (!ret && (ret = pthread_cond_init(&s->unref, NULL))) {
		pthread_cond_destroy(&s->done);
		pthread_cond_destroy(&s->wake);
		pthread_mutex_destroy(&s->lock);
		pthread_mutex_destroy(&s->io);
	}
	pthread_condattr_destroy(&ca);
	return -ret;
}

static void free_sched(struct datax_sched *s, bool sync)
{
	if (sync) {
		pthread_cond_destroy(&s->unref);
		pthread_cond_destroy(&s->done);
		pthread_cond_destroy(&s->wake);
		pthread_mutex_destroy(&s->lock);
		pthread_mutex_destroy(&s->io);
	}
	free(s->cmds);
	free(s);
}

/* Unpublish s from dev, then wait until no datax_device_sched_get
 * reference remains (s NULL: create failed before anyone could see it). */
static void release_device(struct datax_device *dev, struct datax_sched *s)
{
	pthread_mutex_lock(&dev->ctx->lock);
	dev->sched = NULL;
	while (s && s->refs)
		pthread_cond_wait(&s->unref, &dev->ctx->lock);
	dev->caps.measured &= ~(1u << DATAX_LIMIT_REPORT_SPACING_US);
	dev->caps.limits[DATAX_LIMIT_REPORT_SPACING_US] = 0;
	pthread_mutex_unlock(&dev->ctx->lock);
}

static void seed_seq(struct datax_device *dev)
{
	struct timespec a, b;

	if (dev->next_seq)
		return;
	/* Different per process, so a run does not match records a previous
	 * process left in Tick. */
	clock_gettime(CLOCK_REALTIME, &a);
	clock_gettime(CLOCK_MONOTONIC, &b);
	dev->next_seq = (unsigned int)(((unsigned long long)a.tv_nsec ^
					(unsigned long long)b.tv_nsec ^
					(unsigned long long)a.tv_sec * 2654435761ULL) %
				       (SEQ_MAX / 2)) + 1u;
}

int datax_sched_create(struct datax_device *dev, struct datax_sched **out)
{
	struct datax_context *ctx;
	struct datax_sched *s;
	unsigned long long level = 0, v;
	bool sync = false;
	int ret;

	if (out)
		*out = NULL;
	if (!dev)
		return -EINVAL;
	ctx = dev->ctx;
	if (!out)
		return datax_error_ret(ctx, EINVAL, "sched", "sched_create: NULL output");
	if (!datax_tick_present(dev))
		return datax_error_ret(ctx, ENOTSUP, "sched", "no Tick scheduler in this context");

	s = calloc(1, sizeof(*s));
	if (!s)
		return datax_error_ret(ctx, ENOMEM, "sched", "out of memory");
	s->dev = dev;
	s->ctx = ctx;
	s->poll_us = DATAX_SCHED_DEFAULT_POLL_US;
	/* GPIO bit 0 drives TAL_GPIO_0, the FHM hop trigger, on the adrv9009
	 * (ZU11EG) image: only queue_tune may drive it. */
	s->reserved_mask = dev->family == DATAX_FAMILY_ADRV9009 ? 0x1u : 0u;
	s->next_order = 1;
	s->next_done = 1;
	/* Locks first: once published, get_time may take a reference and io. */
	ret = init_sync(s);
	if (ret) {
		free(s);
		return datax_error_ret(ctx, -ret, "sched", "sched_create: cannot initialise locks");
	}
	sync = true;
	pthread_mutex_lock(&ctx->lock);
	if (dev->sched) {
		pthread_mutex_unlock(&ctx->lock);
		free_sched(s, sync);
		return datax_error_ret(ctx, EBUSY, "sched",
				       "a scheduler already exists on this device");
	}
	dev->sched = s;   /* reserved: a concurrent create gets -EBUSY */
	pthread_mutex_unlock(&ctx->lock);

	ret = datax_tick_read_limits(dev, &s->tick_hz, &s->depth, NULL);
	if (ret) {
		datax_error_set(ctx, "sched", "sched_create: cannot read the Tick limits (%d)", ret);
		goto fail;
	}
	if (s->depth < 2 || !s->tick_hz) {
		ret = -ENOTSUP;
		datax_error_set(ctx, "sched", "sched_create: Tick queue depth %u, %llu Hz: unusable",
				s->depth, s->tick_hz);
		goto fail;
	}
	s->guard_ticks = dev->caps.limits[DATAX_LIMIT_GUARD_MARGIN_MS] * s->tick_hz / 1000;
	ret = datax_tick_read_u64(dev, "late_count", &s->late_seen);
	if (!ret)
		ret = datax_tick_read_u64(dev, "overflow_count", &s->overflow_seen);
	if (!ret)
		ret = datax_tick_read_u64(dev, "queue_level", &level);
	if (ret) {
		datax_error_set(ctx, "sched", "sched_create: cannot read the Tick counters (%d)", ret);
		goto fail;
	}
	ret = datax_poller_measure(s);
	if (ret) {
		datax_error_set(ctx, "sched", "sched_create: completion drain failed (%d)", ret);
		goto fail;
	}
	if (!datax_tick_read_u64(dev, "current_time_ticks", &v)) {
		s->last_hw = v;
		s->last_hw_mono = datax_mono_ns();
	}
	pthread_mutex_lock(&ctx->lock);
	seed_seq(dev);
	pthread_mutex_unlock(&ctx->lock);
	ret = datax_poller_start(s);
	if (ret) {
		datax_error_set(ctx, "sched", "sched_create: cannot start the poller (%d)", ret);
		goto fail;
	}
	pthread_mutex_lock(&ctx->lock);
	dev->caps.limits[DATAX_LIMIT_REPORT_SPACING_US] = s->spacing_us;
	dev->caps.measured |= 1u << DATAX_LIMIT_REPORT_SPACING_US;
	pthread_mutex_unlock(&ctx->lock);
	if (level)
		datax_error_set(ctx, "sched",
				"warning: Tick queue_level is %llu at scheduler create; another "
				"Tick user may be running (the scheduler is exclusive)", level);
	*out = s;
	return 0;
fail:
	release_device(dev, s);
	free_sched(s, sync);
	return ret;
}

void datax_sched_destroy(struct datax_sched *s)
{
	struct datax_io_hold h;

	if (!s)
		return;
	/* From an event callback (the poller thread) it cannot join itself;
	 * from a hook (io held) it would deadlock. */
	if (datax_poller_is_self(s) || datax_sched_holds_io(s))
		return;
	/*
	 * Leave Tick empty (it is exclusive, ruling 3). Descriptors left queued
	 * would run unattended: a reported one fills the slot, nobody acks it,
	 * and Tick holds every later reported head (RTL line 351) until the next
	 * scheduler's create acks it and releases them late, recorded on time.
	 * Under io, after the current iteration; stop first so the poller does
	 * no further I/O. Best effort: destroy cannot fail.
	 */
	datax_sched_io_lock(s, &h);
	pthread_mutex_lock(&s->lock);
	s->stop = true;
	pthread_cond_broadcast(&s->done);   /* waiters return -ECANCELED */
	pthread_mutex_unlock(&s->lock);
	(void)datax_tick_write_str(s->dev, "flush_queue", "1");
	(void)datax_poller_clear_slot(s);
	/* Every outstanding command ends FLUSHED through its hook (Task 4's
	 * tune hooks keep state); on -ENOMEM they are dropped unfinished. */
	pthread_mutex_lock(&s->lock);
	(void)flush_outstanding(s, "flushed: scheduler destroyed");
	pthread_mutex_unlock(&s->lock);
	datax_sched_io_unlock(s, &h);
	datax_poller_stop(s);
	/* Woken waiters still hold s->lock briefly: let the last one leave. */
	pthread_mutex_lock(&s->lock);
	while (s->nwaiters)
		pthread_cond_wait(&s->done, &s->lock);
	pthread_mutex_unlock(&s->lock);
	release_device(s->dev, s);
	free_sched(s, true);
}

/* ---- builders ------------------------------------------------------------------ */

long datax_sched_enqueue(struct datax_sched *s, const struct datax_sched_cmd_spec *spec)
{
	struct sched_cmd *c;
	long seq;

	if (!s || !spec)
		return s ? datax_error_ret(s->ctx, EINVAL, "sched", "enqueue: NULL spec") : -EINVAL;
	pthread_mutex_lock(&s->lock);
	if (s->ncmds == s->capcmds) {
		size_t cap = s->capcmds ? 2 * s->capcmds : 64;
		struct sched_cmd *n = realloc(s->cmds, cap * sizeof(*n));

		if (!n) {
			pthread_mutex_unlock(&s->lock);
			return datax_error_ret(s->ctx, ENOMEM, "sched", "out of memory");
		}
		s->cmds = n;
		s->capcmds = cap;
	}
	/* one scheduler per device: s->lock serialises next_seq */
	seq = (long)s->dev->next_seq;
	s->dev->next_seq = s->dev->next_seq >= SEQ_MAX ? 1u : s->dev->next_seq + 1u;
	c = &s->cmds[s->ncmds++];
	memset(c, 0, sizeof(*c));
	c->seq = seq;
	c->state = CMD_BUILD;
	c->order = s->next_order++;
	c->api_ts = spec->when;
	c->hw_ts = datax_devtime_to_hw(s->dev, spec->when);
	c->command_id = spec->command_id;
	c->target_id = spec->target_id;
	memcpy(c->args, spec->args, sizeof(c->args));
	c->report = spec->report;
	c->consume = spec->consume;
	c->on_complete = spec->on_complete;
	c->user = spec->user;
	pthread_mutex_unlock(&s->lock);
	return seq;
}

long datax_sched_queue_gpio(struct datax_sched *s, datax_time when,
			    unsigned int mask, unsigned int value)
{
	struct datax_sched_cmd_spec spec = {
		.when = when, .command_id = DATAX_TICK_COMMAND_ID_GPIO_WRITE,
		.target_id = DATAX_TICK_TARGET_FAMILY_GPIO,
		.args = { mask, value & mask, 0, 0 },
	};

	if (!s)
		return -EINVAL;
	if (!mask)
		return datax_error_ret(s->ctx, EINVAL, "sched", "queue_gpio: empty mask");
	if (mask & s->reserved_mask)
		return datax_error_ret(s->ctx, EINVAL, "sched",
				       "queue_gpio: mask 0x%x touches reserved GPIO line(s) 0x%x "
				       "(the hop trigger; only queue_tune drives it)",
				       mask, s->reserved_mask);
	return datax_sched_enqueue(s, &spec);
}

long datax_sched_queue_marker(struct datax_sched *s, datax_time when,
			      unsigned int line, unsigned int width_ticks)
{
	/* RTL: marker_out[arg0] pulses; the model takes max(width, 1). */
	struct datax_sched_cmd_spec spec = {
		.when = when, .command_id = DATAX_TICK_COMMAND_ID_MARKER_PULSE,
		.target_id = DATAX_TICK_TARGET_FAMILY_MARKER,
		.args = { line, width_ticks ? width_ticks : 1, 0, 0 },
	};

	if (!s)
		return -EINVAL;
	if (line >= MARKER_LINES)
		return datax_error_ret(s->ctx, EINVAL, "sched",
				       "queue_marker: line %u out of range (Tick has %u)",
				       line, MARKER_LINES);
	return datax_sched_enqueue(s, &spec);
}

long datax_sched_queue_tune(struct datax_sched *s, datax_time when,
			    unsigned int ch, unsigned long long hz)
{
	if (!s)
		return -EINVAL;
	return datax_fhm_queue_tune(s, when, ch, hz);   /* fhm.c */
}

long datax_sched_queue_stream_gate(struct datax_sched *s, datax_time when,
				   struct datax_stream *stream, bool enable)
{
	(void)when; (void)stream; (void)enable;
	if (s)
		return datax_error_ret(s->ctx, ENOTSUP, "sched",
				       "queue_stream_gate: no timed-stream hardware on this "
				       "platform yet (P3b)");
	return -ENOTSUP;
}

int datax_sched_report(struct datax_sched *s, long seq)
{
	struct sched_cmd *c;

	if (!s)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	c = datax_sched_find(s, seq);
	if (!c) {
		pthread_mutex_unlock(&s->lock);
		return datax_error_ret(s->ctx, EINVAL, "sched", "report: unknown seq %ld", seq);
	}
	if (c->state != CMD_BUILD) {
		pthread_mutex_unlock(&s->lock);
		return datax_error_ret(s->ctx, EINVAL, "sched",
				       "report: seq %ld is already committed (mark commands "
				       "before commit)", seq);
	}
	c->report = true;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

/* ---- commit ------------------------------------------------------------------- */

/*
 * A rejected commit discards every uncommitted command. Each one's hook
 * runs with r->discarded set (a terminal outcome; nothing is published), so
 * state kept beside the scheduler (the FHM tune pipeline) forgets it too.
 * s->lock held on entry and return, dropped around each hook; io held.
 * Snapshotted first: a command a hook enqueues stays queued.
 */
static void discard_batch(struct datax_sched *s)
{
	long *seqs = malloc((s->ncmds ? s->ncmds : 1) * sizeof(*seqs));
	size_t n = 0;

	if (!seqs) {      /* out of memory: discard without the hooks */
		for (size_t i = 0; i < s->ncmds;)
			if (s->cmds[i].state == CMD_BUILD)
				s->cmds[i] = s->cmds[--s->ncmds];
			else
				i++;
		return;
	}
	for (size_t i = 0; i < s->ncmds; i++)
		if (s->cmds[i].state == CMD_BUILD)
			seqs[n++] = s->cmds[i].seq;
	for (size_t i = 0; i < n; i++) {
		struct sched_cmd *c = datax_sched_find(s, seqs[i]);
		struct datax_sched_result res = {
			.status = DATAX_COMPLETION_FLUSHED,
			.tick_status = UINT32_MAX,
			.discarded = true,
		};
		datax_sched_complete_fn fn;
		void *user;

		if (!c || c->state != CMD_BUILD)
			continue;
		fn = c->on_complete;
		user = c->user;
		res.requested_ticks = c->api_ts;
		snprintf(res.detail, sizeof(res.detail), "discarded: the commit was rejected");
		datax_sched_remove(s, c);
		if (fn) {
			pthread_mutex_unlock(&s->lock);
			fn(s, seqs[i], &res, user);
			pthread_mutex_lock(&s->lock);
		}
	}
	free(seqs);
}

struct rep_ts {
	unsigned long long ts;
	long seq;
	bool fresh;
};

static int cmp_rep(const void *a, const void *b)
{
	const struct rep_ts *x = a, *y = b;

	return x->ts < y->ts ? -1 : x->ts > y->ts;
}

int datax_sched_commit(struct datax_sched *s)
{
	unsigned long long now;
	struct rep_ts *r = NULL;
	struct datax_io_hold h;
	size_t nb = 0, nr = 0;
	int ret;

	if (!s)
		return -EINVAL;
	if (datax_sched_holds_io(s))
		return io_busy(s, "commit");
	datax_sched_io_lock(s, &h);
	ret = datax_tick_read_u64(s->dev, "current_time_ticks", &now);
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->ncmds; i++)
		nb += s->cmds[i].state == CMD_BUILD;
	if (!nb)
		goto out;
	if (ret) {
		ret = datax_error_ret(s->ctx, -ret, "sched",
				      "commit: cannot read the device time (%d)", ret);
		goto out;
	}
	/* Guard margin: a command already inside it fails the whole batch,
	 * before anything is written. */
	for (size_t i = 0; i < s->ncmds; i++) {
		struct sched_cmd *c = &s->cmds[i];

		/* In API time: a past API time must not wrap into the far future
		 * when the soft offset is subtracted. */
		if (c->state == CMD_BUILD &&
		    (c->api_ts < datax_devtime_from_hw(s->dev, now) + s->guard_ticks ||
		     c->hw_ts < now + s->guard_ticks)) {
			datax_error_set(s->ctx, "sched",
					"commit: seq %ld at %llu is inside the guard margin "
					"(%llu ms) of now (%llu); batch discarded", c->seq,
					c->api_ts, s->dev->caps.limits[DATAX_LIMIT_GUARD_MARGIN_MS],
					datax_devtime_from_hw(s->dev, now));
			discard_batch(s);
			ret = DATAX_ERR_LATE;
			goto out;
		}
	}
	/* Report spacing (ruling 1): reported commands, in this batch or still
	 * queued, at least DATAX_LIMIT_REPORT_SPACING_US apart. */
	r = malloc(s->ncmds * sizeof(*r));
	if (!r) {
		ret = datax_error_ret(s->ctx, ENOMEM, "sched", "out of memory");
		goto out;
	}
	for (size_t i = 0; i < s->ncmds; i++) {
		struct sched_cmd *c = &s->cmds[i];

		if (c->report && c->state != CMD_DONE)
			r[nr++] = (struct rep_ts){ c->hw_ts, c->seq, c->state == CMD_BUILD };
	}
	qsort(r, nr, sizeof(*r), cmp_rep);
	for (size_t i = 1; i < nr; i++) {
		if ((r[i].fresh || r[i - 1].fresh) && r[i].ts - r[i - 1].ts < s->spacing_ticks) {
			datax_error_set(s->ctx, "sched",
					"commit: seq %ld and %ld are reported commands closer than "
					"the report spacing (%llu us); batch discarded",
					r[i - 1].seq, r[i].seq, s->spacing_us);
			discard_batch(s);
			ret = -EINVAL;
			goto out;
		}
	}
	for (size_t i = 0; i < s->ncmds; i++)
		if (s->cmds[i].state == CMD_BUILD)
			s->cmds[i].state = CMD_PENDING;
	ret = 0;
	s->kick = true;
	pthread_cond_signal(&s->wake);
out:
	pthread_mutex_unlock(&s->lock);
	datax_sched_io_unlock(s, &h);
	free(r);
	return nb ? ret : 0;
}

/* ---- flush -------------------------------------------------------------------- */

/* Finish every command not yet complete, as of now, FLUSHED through the
 * normal path (hooks run, waiters wake). A hook may enqueue more: those
 * stay queued. io and s->lock held. 0 or -ENOMEM. */
static int flush_outstanding(struct datax_sched *s, const char *detail)
{
	struct sched_cmd *c;
	long *seqs;
	size_t n = 0;

	seqs = malloc((s->ncmds ? s->ncmds : 1) * sizeof(*seqs));
	if (!seqs)
		return -ENOMEM;
	for (size_t i = 0; i < s->ncmds; i++)
		if (s->cmds[i].state != CMD_DONE)
			seqs[n++] = s->cmds[i].seq;
	for (size_t i = 0; i < n; i++) {
		struct datax_sched_result res = {
			.status = DATAX_COMPLETION_FLUSHED,
			.tick_status = UINT32_MAX,
		};

		c = datax_sched_find(s, seqs[i]);
		if (!c || c->state == CMD_DONE)
			continue;
		res.requested_ticks = c->api_ts;
		snprintf(res.detail, sizeof(res.detail), "%s", detail);
		datax_sched_finish(s, c, &res);
	}
	free(seqs);
	return 0;
}

int datax_sched_flush(struct datax_sched *s)
{
	struct datax_io_hold h;
	int ret;

	if (!s)
		return -EINVAL;
	if (datax_sched_holds_io(s))
		return io_busy(s, "flush");
	datax_sched_io_lock(s, &h);
	/* Keep what already ran, empty the queue, then keep what ran between
	 * the drain and the flush. */
	ret = datax_poller_read_time(s);
	if (!ret)
		ret = datax_poller_drain(s);
	if (!ret)
		ret = datax_tick_write_str(s->dev, "flush_queue", "1");
	if (!ret)
		ret = datax_poller_drain(s);
	if (ret) {
		datax_sched_io_unlock(s, &h);
		return datax_error_ret(s->ctx, -ret, "sched", "flush: Tick access failed (%d)", ret);
	}
	pthread_mutex_lock(&s->lock);
	ret = flush_outstanding(s, "flushed");
	s->kick = true;            /* deliver what the drains queued */
	pthread_cond_signal(&s->wake);
	pthread_mutex_unlock(&s->lock);
	datax_sched_io_unlock(s, &h);
	return ret ? datax_error_ret(s->ctx, -ret, "sched", "out of memory") : 0;
}

/* ---- results ------------------------------------------------------------------ */

static void fill(struct datax_completion *out, const struct sched_cmd *c)
{
	out->seq = c->seq;
	out->status = c->res.status;
	out->requested_ticks = c->res.requested_ticks;
	out->executed_ticks = c->res.executed_ticks;
}

static void deadline_after(struct timespec *ts, int ms)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

static bool passed(const struct timespec *dl)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec > dl->tv_sec || (now.tv_sec == dl->tv_sec && now.tv_nsec >= dl->tv_nsec);
}

/* Leave wait(): the last waiter out of a stopping scheduler lets destroy
 * free it. s->lock held; s must not be touched after the unlock. */
static void wait_leave(struct datax_sched *s)
{
	if (!--s->nwaiters && s->stop)
		pthread_cond_broadcast(&s->done);
	pthread_mutex_unlock(&s->lock);
}

/* Leave wait() with an error. The last error is written before wait_leave,
 * while s->lock is held: afterwards destroy may free s and the caller may
 * destroy the context. */
static int __attribute__((format(printf, 4, 5)))
wait_fail(struct datax_sched *s, struct datax_context *ctx, int err, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	datax_error_set(ctx, "sched", "%s", msg);
	wait_leave(s);
	return -err;
}

int datax_sched_wait(struct datax_sched *s, long seq, int timeout_ms,
		     struct datax_completion *out)
{
	struct datax_context *ctx;
	struct sched_cmd *c;
	struct timespec dl;
	char detail[DATAX_SCHED_DETAIL_LEN];
	enum sched_state st;

	if (!s)
		return -EINVAL;
	/* After wait_leave, s may already be freed by a concurrent destroy:
	 * only the context (which outlives its schedulers) is used. */
	ctx = s->ctx;
	if (!out)
		return datax_error_ret(ctx, EINVAL, "sched", "wait: NULL output");
	/* On the poller thread (an event callback) a wait would stall or
	 * deadlock the thread that produces completions; from a hook (this
	 * thread holds io) the poller could never drain. */
	if (datax_poller_is_self(s))
		return datax_error_ret(ctx, EDEADLK, "sched",
				       "wait from an event callback (the poller thread) would "
				       "block it; use get_completions");
	if (datax_sched_holds_io(s))
		return io_busy(s, "wait");
	if (timeout_ms >= 0)
		deadline_after(&dl, timeout_ms);
	pthread_mutex_lock(&s->lock);
	if (s->stop) {
		pthread_mutex_unlock(&s->lock);
		return datax_error_ret(ctx, ECANCELED, "sched",
				       "wait: the scheduler is being destroyed");
	}
	c = datax_sched_find(s, seq);
	if (!c) {
		pthread_mutex_unlock(&s->lock);
		return datax_error_ret(ctx, EINVAL, "sched",
				       "wait: seq %ld is unknown or already consumed", seq);
	}
	if (!c->report) {
		pthread_mutex_unlock(&s->lock);
		return datax_error_ret(ctx, EINVAL, "sched",
				       "wait: seq %ld was not reported (datax_sched_report)", seq);
	}
	s->nwaiters++;
	for (;;) {
		/* destroy first: it also finishes outstanding commands FLUSHED,
		 * but a waiter it interrupts reports the cancellation */
		if (s->stop) {
			return wait_fail(s, ctx, ECANCELED,
				    "wait: the scheduler is being destroyed");
		}
		c = datax_sched_find(s, seq);
		if (!c) {           /* consumed by get_completions meanwhile */
			return wait_fail(s, ctx, EINVAL,
				    "wait: seq %ld was consumed", seq);
		}
		if (c->state == CMD_DONE)
			break;
		/* A proven loss is already DONE (DROPPED, poller.c finish_lost);
		 * anything else at the deadline is not yet known (ruling). */
		if (timeout_ms >= 0 && passed(&dl)) {
			st = c->state;
			return wait_fail(s, ctx, ETIMEDOUT,
				    "wait: seq %ld has not completed in %d ms (%s)", seq,
				    timeout_ms, st == CMD_BUILD ? "not committed" :
				    st == CMD_PENDING ? "not yet written" :
				    "not yet due or not yet drained");
		}
		if (timeout_ms >= 0)
			pthread_cond_timedwait(&s->done, &s->lock, &dl);
		else
			pthread_cond_wait(&s->done, &s->lock);
	}
	fill(out, c);
	snprintf(detail, sizeof(detail), "%s", c->res.detail);
	datax_sched_remove(s, c);
	/* Ruling 4: a non-OK completion's detail travels in the last error.
	 * Written before wait_leave, still under s->lock (order s->lock then
	 * ctx->lock): once the last waiter leaves, destroy may free s and the
	 * caller may destroy the context. */
	if (out->status != DATAX_COMPLETION_OK)
		datax_error_set(ctx, "sched", "seq %ld: %s", seq, detail);
	wait_leave(s);
	return 0;
}

int datax_sched_get_completions(struct datax_sched *s, struct datax_completion *out,
				size_t max, size_t *count)
{
	size_t n = 0;

	if (!s)
		return -EINVAL;
	if (!count || (!out && max))
		return datax_error_ret(s->ctx, EINVAL, "sched", "get_completions: NULL argument");
	pthread_mutex_lock(&s->lock);
	while (n < max) {
		struct sched_cmd *best = NULL;

		for (size_t i = 0; i < s->ncmds; i++)
			if (s->cmds[i].state == CMD_DONE &&
			    (!best || s->cmds[i].done_order < best->done_order))
				best = &s->cmds[i];
		if (!best)
			break;
		fill(&out[n++], best);
		datax_sched_remove(s, best);
	}
	pthread_mutex_unlock(&s->lock);
	*count = n;
	return 0;
}

/* ---- internal controls ------------------------------------------------------------ */

int datax_internal_sched_pause(struct datax_sched *s, bool pause)
{
	struct datax_io_hold h;
	bool self;

	if (!s)
		return -EINVAL;
	/* This thread holds io (a hook, or a seam access inside an iteration):
	 * no iteration of another thread is in progress, so just set the flag.
	 * The same from an event callback: the poller holds no lock and is
	 * between I/O. Otherwise wait for the current iteration to end. */
	self = datax_poller_is_self(s) || datax_sched_holds_io(s);
	if (!self)
		datax_sched_io_lock(s, &h);
	pthread_mutex_lock(&s->lock);
	s->paused = pause;
	if (!pause) {
		s->kick = true;
		pthread_cond_signal(&s->wake);
	}
	pthread_mutex_unlock(&s->lock);
	if (!self)
		datax_sched_io_unlock(s, &h);
	return 0;
}

int datax_sched_set_poll_hook(struct datax_sched *s, datax_sched_poll_fn fn, void *user)
{
	if (!s)
		return -EINVAL;
	pthread_mutex_lock(&s->lock);
	s->poll_fn = fn;
	s->poll_user = fn ? user : NULL;
	pthread_mutex_unlock(&s->lock);
	return 0;
}
