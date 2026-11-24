// SPDX-License-Identifier: MIT
/*
 * Timed start (P3b; api-c-core "Timed start and stream gating"; spec
 * decision 4 and the G2/G3 rulings): datax_stream_start_at closes the
 * direction's DMA gate, arms the DMA and opens the gate at T through the
 * device scheduler. stop() cancels the stream's gate commands and forces the
 * gate open; an untimed start() forces it open first.
 *
 * Gate bits come from the Tick's stream_gate_map (dev->caps.gate_map): pass
 * = !mode | level, and reset (all 0) is forced open. Every write here is
 * execute-now (datax_tick_gate_now), so none can be dropped or reported.
 *
 * The opening command is STREAM_GATE(level = 1) at T, LATE_DROP, consumed,
 * with open_done as its hook and the stream as its user:
 *  - RX: reported, and lead (at an equal timestamp the feeder writes it
 *    before a TX opening). recv holds block 0 until the hook has seen the
 *    result: OK delivers, anything else fails the stream.
 *  - TX: unreported (controller ruling, Task 3): a reported TX opening at
 *    the same T as an RX one would be refused by the report-spacing rule, or
 *    held behind the RX one's completion record. It therefore has no
 *    completion, and its lateness is detected three ways, each through the
 *    hook: host-late (the feeder's guard rule); late on submit, which the
 *    feeder detects by sampling late_count around its push (late_probe);
 *    and running behind a held completion slot. A reported head held by a
 *    full slot blocks Tick's whole queue (RTL line 351), so a TX opening due
 *    from that head's time to the ack that released it runs late, yet Tick
 *    records it nowhere (no record, no late_count rise): when the poller
 *    classes the head LATE "poller lagged" it finishes those LATE too
 *    (poller.c held_behind). An on-time TX opening is never confirmed: it
 *    leaves the table once due.
 *
 * A TX preload whose opening has not run must never reach the DAC: stop()
 * and a failed start_at cancel the DMA (no drain) before the gate commands
 * are cancelled and the gate is forced open. "Has not run" is decided on a
 * fresh device-time read against T, not on the table (an unreported opening
 * stays WRITTEN after it ran, until the poller prunes it).
 *
 * Lifetime: a stream holds no scheduler reference across calls. stop()
 * finishes every gate command of its direction (datax_sched_cancel_gates)
 * before destroy frees the stream, and datax_sched_destroy finishes them
 * (FLUSHED) itself if it runs first, so no hook runs on a freed stream.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "error.h"
#include "scheduler.h"
#include "stream.h"
#include "tick_descriptor.h"

#define SUB "stream"

static bool is_rx(const struct datax_stream *s)
{
	return s->cfg.direction == DATAX_RX;
}

static const char *dname(const struct datax_stream *s)
{
	return is_rx(s) ? "RX" : "TX";
}

static int level_bit(const struct datax_stream *s)
{
	return is_rx(s) ? s->dev->caps.gate_map.rx_level : s->dev->caps.gate_map.tx_level;
}

static int mode_bit(const struct datax_stream *s)
{
	return is_rx(s) ? s->dev->caps.gate_map.rx_mode : s->dev->caps.gate_map.tx_mode;
}

/* The direction's RX_ARM / TX_ARM: a usable Tick with the STREAM_GATE
 * target and both gate roles in the map. */
static bool armable(const struct datax_stream *s)
{
	enum datax_cap c = is_rx(s) ? DATAX_CAP_RX_ARM : DATAX_CAP_TX_ARM;
	bool on;

	pthread_mutex_lock(&s->dev->ctx->lock);
	on = s->dev->caps.flags & (1u << c);
	pthread_mutex_unlock(&s->dev->ctx->lock);
	return on;
}

static int not_armable(struct datax_stream *s)
{
	const struct datax_gate_map *m = &s->dev->caps.gate_map;
	const char *lv = is_rx(s) ? "rx-dma" : "tx-dma", *md = is_rx(s) ? "rx-mode" : "tx-mode";
	bool nl = level_bit(s) < 0, nm = mode_bit(s) < 0;

	if (!m->present || (!nl && !nm))
		return datax_error_ret(s->dev->ctx, ENOTSUP, SUB,
				       "start_at: %s needs DATAX_CAP_%s_ARM: %s (map roles %s, %s)",
				       dname(s), dname(s),
				       !m->present ? "no Tick stream_gate_map" :
				       "no usable Tick with a STREAM_GATE target", lv, md);
	return datax_error_ret(s->dev->ctx, ENOTSUP, SUB,
			       "start_at: %s needs DATAX_CAP_%s_ARM: the Tick stream_gate_map lacks %s%s%s",
			       dname(s), dname(s), nl ? lv : "", nl && nm ? ", " : "", nm ? md : "");
}

/* tx-start-fullmask-only (G2/G3 ruling 2): TX start_at only on a mask of
 * every TX channel. */
static bool tx_mask_ok(const struct datax_stream *s)
{
	unsigned int n = s->dev->tx_channels;
	unsigned int all = n >= 32 ? ~0u : (1u << n) - 1u;

	return !s->dev->caps.gate_map.tx_fullmask_only || s->cfg.channel_mask == all;
}

/* capture_tag_enable read-modify-write of the RX level bit (libdatax owns
 * the attribute, gap 2). Only with the tag attributes. */
static int tag_bit(struct datax_stream *s, bool on)
{
	unsigned long long v;
	unsigned int bit = (unsigned int)level_bit(s);
	int ret;

	if (!is_rx(s) || !s->dev->caps.tag_attrs)
		return 0;
	ret = datax_tick_read_u64(s->dev, "capture_tag_enable", &v);
	if (ret)
		return ret;
	v = on ? v | (1ull << bit) : v & ~(1ull << bit);
	return datax_tick_write_u64(s->dev, "capture_tag_enable", v);
}

/* gate(mode, mode_val) then gate(level, 0), then the tag bit (tag: 1 set,
 * -1 clear, 0 leave). Hold sch->io when sch is not NULL. */
static int gate_setup(struct datax_stream *s, struct datax_sched *sch, unsigned int mode_val,
		      int tag)
{
	int ret = datax_tick_gate_now(s->dev, sch, (unsigned int)mode_bit(s), mode_val);

	if (!ret)
		ret = datax_tick_gate_now(s->dev, sch, (unsigned int)level_bit(s), 0);
	if (!ret && tag)
		ret = tag_bit(s, tag > 0);
	return ret;
}

int datax_stream_force_open(struct datax_stream *s, bool clear_tag)
{
	struct datax_sched *sch;
	struct datax_io_hold h;
	int ret;

	if (!armable(s))
		return 0;                  /* no map roles: no Tick access */
	sch = datax_device_sched_get(s->dev);
	if (sch && datax_sched_holds_io(sch)) {
		datax_sched_put(sch);
		return datax_error_ret(s->dev->ctx, EDEADLK, SUB,
				       "forcing the %s gate open from a scheduler hook would deadlock",
				       dname(s));
	}
	if (sch)
		datax_sched_io_lock(sch, &h);
	ret = gate_setup(s, sch, 0, clear_tag ? -1 : 0);
	if (sch) {
		datax_sched_io_unlock(sch, &h);
		datax_sched_put(sch);
	}
	if (ret)
		return datax_error_ret(s->dev->ctx, -ret, SUB,
				       "forcing the %s stream gate open failed (%d)", dname(s), ret);
	return 0;
}

void datax_stream_timed_reset(struct datax_stream *s)
{
	pthread_mutex_lock(&s->tmu);
	s->timed = false;
	atomic_store(&s->stopping, false);
	atomic_store(&s->fail, 0);
	atomic_store(&s->open_state, 0);
	atomic_store(&s->open_seq, 0);
	s->blk0_held = false;
	s->anchored = false;
	s->unknown = false;
	s->blk = 0;
	s->blk_strip = 0;
	s->blk_ts = 0;
	s->blk_valid = 0;
	s->fail_detail[0] = '\0';
	pthread_mutex_unlock(&s->tmu);
}

int datax_stream_timed_failed(struct datax_stream *s, const char *verb, bool quiet)
{
	char detail[sizeof(s->fail_detail)];
	int f = atomic_load(&s->fail);

	if (!f)
		return 0;
	if (quiet)
		return f;
	pthread_mutex_lock(&s->tmu);
	snprintf(detail, sizeof(detail), "%s", s->fail_detail);
	pthread_mutex_unlock(&s->tmu);
	return datax_error_ret(s->dev->ctx, -f, SUB, "%s: the timed start of the %s stream %s: %s",
			       verb, dname(s), f == DATAX_ERR_LATE ? "was late" : "was cancelled",
			       detail);
}

int datax_stream_wait_open(struct datax_stream *s, int timeout_ms, long long deadline_us)
{
	struct timespec dl;
	int st;

	if (timeout_ms > 0) {
		dl.tv_sec = (time_t)(deadline_us / 1000000LL);
		dl.tv_nsec = (long)(deadline_us % 1000000LL) * 1000L;
	}
	pthread_mutex_lock(&s->tmu);
	while (!(st = atomic_load(&s->open_state)) && timeout_ms) {
		if (timeout_ms > 0) {
			if (pthread_cond_timedwait(&s->tcv, &s->tmu, &dl) == ETIMEDOUT)
				break;
		} else {
			pthread_cond_wait(&s->tcv, &s->tmu);
		}
	}
	st = atomic_load(&s->open_state);
	pthread_mutex_unlock(&s->tmu);
	if (st == 1)
		return 0;
	if (!st)
		return -ETIMEDOUT;
	if (!atomic_load(&s->fail))
		atomic_store(&s->fail, -ECANCELED);   /* resolved without a cause */
	return atomic_load(&s->fail);
}

/*
 * The opening command's hook (scheduler.h rules: io held, s->lock not held,
 * on the thread doing the Tick I/O). OK: opened on time. LATE (host-late,
 * Tick dropped it, or a held completion slot, "poller lagged"): the stream
 * fails DATAX_ERR_LATE; datax_sched_finish raises the event. FLUSHED or
 * DROPPED (flush, destroy, PPS adoption, a write failure): -ECANCELED,
 * unless stop() is cancelling it. A rejected commit (discarded) is undone by
 * start_at itself.
 */
static void open_done(struct datax_sched *sch, long seq, struct datax_sched_result *r,
		      void *user)
{
	struct datax_stream *s = user;
	int fail = 0, st;

	(void)sch;
	if (r->discarded || seq != atomic_load(&s->open_seq))
		return;
	if (r->status == DATAX_COMPLETION_OK) {
		st = 1;
	} else {
		st = -1;
		if (r->status == DATAX_COMPLETION_LATE)
			fail = DATAX_ERR_LATE;
		else if (!atomic_load(&s->stopping))
			fail = -ECANCELED;
	}
	pthread_mutex_lock(&s->tmu);
	if (fail && !atomic_load(&s->fail)) {
		snprintf(s->fail_detail, sizeof(s->fail_detail), "seq %ld: %s", seq,
			 r->detail[0] ? r->detail : "not executed");
		atomic_store(&s->fail, fail);
	}
	atomic_store(&s->open_state, st);
	pthread_cond_broadcast(&s->tcv);
	pthread_mutex_unlock(&s->tmu);
}

unsigned int datax_stream_held_count(const struct datax_stream *s)
{
	unsigned int n = 0;

	for (unsigned int i = 0; s->held && i < s->nblocks; i++)
		n += s->held[i];
	return n;
}

int datax_stream_enqueue_held(struct datax_stream *s, const char *verb)
{
	char what[64];

	/* Ring order: the oldest held block is the first held one from cur. */
	for (unsigned int j = 0; s->held && j < s->nblocks; j++) {
		unsigned int i = (s->cur + j) % s->nblocks;
		int ret;

		if (!s->held[i])
			continue;
		ret = datax_stream_enqueue(s, i);
		if (ret) {
			snprintf(what, sizeof(what), "%s: enqueueing a preloaded block", verb);
			return datax_stream_fail_iio(s, ret, what);
		}
		s->held[i] = false;
		s->stats.blocks_done++;
	}
	return 0;
}

/* Arm the DMA behind the closed gate (step 6). */
static int arm(struct datax_stream *s)
{
	int ret;

	if (s->cfg.cyclic) {
		ret = datax_stream_cyclic_arm(s, "start_at");
		if (!ret)
			s->held[0] = false;
		return ret;
	}
	if (!s->hw) {
		ret = datax_stream_hw_open(s);
		if (ret)
			return ret;
	}
	if (is_rx(s)) {
		for (unsigned int i = 0; i < s->nblocks; i++) {
			ret = s->queued[i] ? 0 : datax_stream_enqueue(s, i);
			if (ret)
				return datax_stream_fail_iio(s, ret, "start_at: enqueueing a block");
		}
		s->cur = 0;
		s->cur_off = 0;
	}
	ret = s->src->start(s->src_h);
	if (ret)
		return datax_stream_fail_iio(s, ret, "start_at: enabling the buffer");
	s->enabled = true;
	if (is_rx(s)) {
		/* The sticky idle overflow of forced-open mode (ruling 7). */
		datax_stream_status_clear(s);
		return 0;
	}
	return datax_stream_enqueue_held(s, "start_at");
}

/* Undo a start_at past step 5: nothing left armed, then forced open again.
 * The caller's error stays the last error. */
static void undo(struct datax_stream *s)
{
	char keep[256];

	snprintf(keep, sizeof(keep), "%s", datax_context_get_last_error(s->dev->ctx));
	/* The DMA first (cancel, no drain): forcing the gate open with a TX
	 * preload armed would play it. */
	datax_stream_hw_close(s);
	(void)datax_stream_force_open(s, true);
	datax_stream_timed_reset(s);
	datax_error_set(s->dev->ctx, SUB, "%s", keep);
}

/* For an RX opening: the TX opening of the same hardware time is already in
 * Tick, so it would run first. */
static bool tx_open_written(struct datax_sched *sch, const struct datax_stream *s,
			    unsigned long long hw)
{
	int tx = s->dev->caps.gate_map.tx_level;
	bool hit = false;

	if (tx < 0)
		return false;
	pthread_mutex_lock(&sch->lock);
	for (size_t i = 0; i < sch->ncmds && !hit; i++) {
		const struct sched_cmd *c = &sch->cmds[i];

		hit = c->state == CMD_WRITTEN && c->hw_ts == hw &&
		      c->command_id == DATAX_TICK_COMMAND_ID_STREAM_GATE &&
		      c->target_id == (DATAX_TICK_TARGET_FAMILY_STREAM_GATE | (unsigned int)tx) &&
		      c->args[1] == 1;
	}
	pthread_mutex_unlock(&sch->lock);
	return hit;
}

int datax_stream_start_at(struct datax_stream *s, datax_time when)
{
	struct datax_context *ctx;
	struct datax_sched *sch, *made = NULL;
	struct datax_sched_cmd_spec spec;
	struct datax_io_hold h;
	unsigned long long now, hw, fs = 0;
	unsigned int lv;
	size_t nb = 0;
	long seq;
	int ret;

	if (!s)
		return -EINVAL;
	ctx = s->dev->ctx;
	/* 1. Checks, before any Tick access. */
	if (s->started)
		return datax_error_ret(ctx, EALREADY, SUB, "start_at: already started");
	if (!armable(s))
		return not_armable(s);
	if (!is_rx(s) && !tx_mask_ok(s))
		return datax_error_ret(ctx, ENOTSUP, SUB,
				       "start_at: TX start_at needs every TX channel on this image "
				       "(tx-start-fullmask-only): mask 0x%x of %u channels",
				       s->cfg.channel_mask, s->dev->tx_channels);
	if (!is_rx(s) && !datax_stream_held_count(s))
		return datax_error_ret(ctx, EINVAL, SUB,
				       "start_at: no full TX block is held: preload with send() first");
	/* 2. The sample rate, for the block timestamps (0: unknown). */
	if (datax_device_get_sample_rate(s->dev, &fs))
		fs = 0;
	/* 3. The device's scheduler, created lazily (ruling 3). */
	sch = datax_device_sched_get(s->dev);
	if (!sch) {
		ret = datax_sched_create(s->dev, &made);
		if (ret && ret != -EBUSY)
			return ret;
		sch = datax_device_sched_get(s->dev);
		if (!sch)
			return datax_error_ret(ctx, EAGAIN, SUB,
					       "start_at: the device's scheduler went away");
	}
	if (datax_sched_holds_io(sch)) {
		datax_sched_put(sch);
		return datax_error_ret(ctx, EDEADLK, SUB,
				       "start_at from a scheduler hook would deadlock");
	}
	/* Committing would also commit (or discard) the caller's batch. */
	pthread_mutex_lock(&sch->lock);
	for (size_t i = 0; i < sch->ncmds; i++)
		nb += sch->cmds[i].state == CMD_BUILD;
	pthread_mutex_unlock(&sch->lock);
	if (nb) {
		datax_sched_put(sch);
		return datax_error_ret(ctx, EBUSY, SUB,
				       "start_at: the device's scheduler has %zu uncommitted commands; "
				       "commit them first", nb);
	}
	hw = datax_devtime_to_hw(s->dev, when);
	lv = (unsigned int)level_bit(s);
	/* 4 and 5, under io: the guard margin (nothing written when late),
	 * then close: execute-now mode = 1, level = 0, and the RX tag bit. */
	datax_sched_io_lock(sch, &h);
	ret = datax_tick_read_u64(s->dev, "current_time_ticks", &now);
	if (ret) {
		datax_sched_io_unlock(sch, &h);
		datax_sched_put(sch);
		return datax_error_ret(ctx, -ret, SUB, "start_at: cannot read the device time (%d)",
				       ret);
	}
	if (when < datax_devtime_from_hw(s->dev, now) + sch->guard_ticks ||
	    hw < now + sch->guard_ticks) {
		datax_sched_io_unlock(sch, &h);
		datax_error_set(ctx, SUB, "start_at: T %llu is inside the guard margin (%llu ms) of "
				"now (%llu)", when,
				s->dev->caps.limits[DATAX_LIMIT_GUARD_MARGIN_MS],
				datax_devtime_from_hw(s->dev, now));
		datax_sched_put(sch);
		return DATAX_ERR_LATE;
	}
	ret = gate_setup(s, sch, 1, 1);
	/* Arming makes many libiio round trips: do not starve the poller. */
	datax_sched_io_unlock(sch, &h);
	if (ret) {
		datax_error_set(ctx, SUB, "start_at: closing the %s gate failed (%d)", dname(s), ret);
		goto undo;
	}
	datax_stream_timed_reset(s);
	s->fs = fs;
	s->tick_hz = sch->tick_hz;
	s->strip_frames = 0;
	if (is_rx(s) && (s->dev->caps.flags & (1u << DATAX_CAP_TIMESTAMPS)) && s->dev->caps.tag_bytes)
		s->strip_frames = (unsigned int)((s->dev->caps.tag_bytes + s->frame_bytes - 1) /
						 s->frame_bytes);
	s->t_open = when;
	/* 6. Arm behind the closed gate. */
	ret = arm(s);
	if (ret)
		goto undo;
	/* 7. The opening at T. The TX one is unreported (see the top of the
	 * file): it never completes, so an on-time TX open is not confirmed,
	 * and its lateness comes from the guard rule and late_probe. */
	memset(&spec, 0, sizeof(spec));
	spec.when = when;
	spec.command_id = DATAX_TICK_COMMAND_ID_STREAM_GATE;
	spec.target_id = (uint16_t)(DATAX_TICK_TARGET_FAMILY_STREAM_GATE | lv);
	spec.args[0] = lv;
	spec.args[1] = 1;
	spec.report = is_rx(s);
	spec.consume = true;
	spec.lead = is_rx(s);
	spec.late_probe = !is_rx(s);
	spec.on_complete = open_done;
	spec.user = s;
	seq = datax_sched_enqueue(sch, &spec);
	if (seq < 0) {
		ret = (int)seq;
		goto undo;
	}
	atomic_store(&s->open_seq, seq);   /* before the commit: the hook checks it */
	s->timed = true;
	s->started = true;
	ret = datax_sched_commit(sch);
	if (ret) {
		s->started = false;
		goto undo;
	}
	/* Same-T order (ruling 6): an RX opening must reach Tick before a TX
	 * opening already written for the same time. */
	if (is_rx(s) && tx_open_written(sch, s, hw)) {
		ret = datax_sched_refeed(sch);
		if (ret) {
			/* The opening is committed: stop() cancels it. */
			datax_sched_put(sch);
			return ret;
		}
	}
	datax_sched_put(sch);
	return 0;
undo:
	datax_sched_put(sch);
	undo(s);
	return ret;
}

/* TX: the opening has not run (or failed): device time is before T. A
 * failed time read counts as not run (cancel: never play a preload late). */
static bool tx_not_opened(struct datax_stream *s)
{
	unsigned long long now, hz;

	if (atomic_load(&s->fail))
		return true;
	return datax_device_get_time(s->dev, &now, &hz) || now < s->t_open;
}

int datax_stream_on_poller(struct datax_stream *s, const char *verb)
{
	struct datax_sched *sch = datax_device_sched_get(s->dev);
	bool self = sch && (datax_poller_is_self(sch) || datax_sched_holds_io(sch));

	datax_sched_put(sch);
	if (!self)
		return 0;
	return datax_error_ret(s->dev->ctx, EDEADLK, SUB,
			       "stream_%s from the scheduler's poller thread (an event callback "
			       "or hook) would deadlock or free a stream its commands still "
			       "reference; call it from another thread", verb);
}

int datax_stream_timed_stop(struct datax_stream *s)
{
	struct datax_sched *sch;
	int ret = 0, fret;

	atomic_store(&s->stopping, true);
	/* A TX preload whose opening has not run: cancel the DMA before the
	 * opening is cancelled (it stays in Tick until the flush) and before
	 * the gate is forced open, so no sample reaches the DAC. stop()'s P2
	 * teardown then finds nothing open: no flush of the partial block, no
	 * drain. */
	if (!is_rx(s) && s->hw && tx_not_opened(s))
		datax_stream_hw_close(s);
	sch = datax_device_sched_get(s->dev);
	if (sch) {
		ret = datax_sched_cancel_gates(sch, 1u << (unsigned int)level_bit(s),
					       "cancelled: the stream was stopped");
		datax_sched_put(sch);
	}
	fret = datax_stream_force_open(s, true);
	return ret ? ret : fret;
}
