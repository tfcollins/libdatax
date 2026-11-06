// SPDX-License-Identifier: MIT
/*
 * Scheduler core and poller against the fake Tick (REQ-TEST-002/003).
 *
 * Every test binds the fake to a live emulator device (the fixture has no
 * Tick) through a recording shim, which forwards to the fake and logs each
 * command_descriptor write. Device time moves only when the test advances
 * the fake (the access cost is set only while the scheduler measures its
 * report spacing at create, then zeroed), so the poller's view is
 * deterministic; datax_internal_sched_pause freezes it at known points.
 *
 * Usage: test_sched all      (adrv9009 fixture: every test)
 *        test_sched ad936x   (ad936x fixture: per-family reserved lines)
 *        test_sched destroy  (lifetime only; also run under valgrind)
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "fake_tick.h"
#include "tick_descriptor.h"

#define HZ        100000000ULL           /* fake (tron) tick rate */
#define MS        (HZ / 1000)
#define GUARD     (10 * MS)              /* DATAX_LIMIT_GUARD_MARGIN_MS = 10 */
/* Access cost while the scheduler measures: 100 us per access, so one drain
 * (8 reads + ack) is 900 us, and the spacing is 3 x 900 us = 2700 us. */
#define COST      (100 * HZ / 1000000)
#define SPACING_US 2700ULL
#define SPACING   (SPACING_US * HZ / 1000000)

/* ---- recording shim over the fake ------------------------------------------ */

struct shim {
	struct fake_tick *f;
	pthread_mutex_t m;
	uint32_t seqs[1024];
	size_t nseq;
	bool track_level;
	unsigned int max_level;
	/* called before a descriptor write is forwarded */
	void (*before_desc)(struct shim *sh, uint32_t seq);
	uint32_t hook_seq;
	bool hook_done;
	/* one-shot: advance the fake this much just before forwarding the
	 * next completion_ack (a slow ack round trip) */
	uint64_t ack_delay;
	/* stall_and_fill also pauses this scheduler (from the poller thread) */
	struct datax_sched *pause_s;
	/* one-shot: the current_time_ticks read right after the next
	 * completion_ack fails (the record is acked, its drain time lost) */
	bool fail_time_after_ack, ack_seen;
};

static uint32_t csv_word(const char *csv, int idx)
{
	const char *p = csv;

	for (int i = 0; i < idx; i++) {
		p = strchr(p, ',');
		CHECK(p != NULL);
		p++;
	}
	return (uint32_t)strtoul(p, NULL, 0);
}

static int shim_read(void *h, const char *attr, char *buf, size_t len)
{
	struct shim *sh = h;
	bool fail = false;

	if (!strcmp(attr, "current_time_ticks")) {
		pthread_mutex_lock(&sh->m);
		fail = sh->ack_seen;
		sh->ack_seen = false;
		pthread_mutex_unlock(&sh->m);
	}
	return fail ? -EIO : fake_tick_read(sh->f, attr, buf, len);
}

static int shim_write(void *h, const char *attr, const char *val)
{
	struct shim *sh = h;
	int ret;

	if (!strcmp(attr, "completion_ack")) {
		uint64_t d;

		pthread_mutex_lock(&sh->m);
		d = sh->ack_delay;
		sh->ack_delay = 0;
		if (sh->fail_time_after_ack) {
			sh->fail_time_after_ack = false;
			sh->ack_seen = true;
		}
		pthread_mutex_unlock(&sh->m);
		if (d)
			fake_tick_advance(sh->f, d);
	}
	if (strcmp(attr, "command_descriptor"))
		return fake_tick_write(sh->f, attr, val);
	if (sh->before_desc)
		sh->before_desc(sh, csv_word(val, 5));
	ret = fake_tick_write(sh->f, attr, val);
	if (ret)
		return ret;
	pthread_mutex_lock(&sh->m);
	if (sh->nseq < sizeof(sh->seqs) / sizeof(sh->seqs[0]))
		sh->seqs[sh->nseq++] = csv_word(val, 5);
	if (sh->track_level) {
		char b[32];

		CHECK(fake_tick_read(sh->f, "queue_level", b, sizeof(b)) > 0);
		if ((unsigned)atoi(b) > sh->max_level)
			sh->max_level = (unsigned)atoi(b);
	}
	pthread_mutex_unlock(&sh->m);
	return 0;
}

static const struct datax_tick_ops shim_ops = { shim_read, shim_write };

static size_t shim_count(struct shim *sh)
{
	size_t n;

	pthread_mutex_lock(&sh->m);
	n = sh->nseq;
	pthread_mutex_unlock(&sh->m);
	return n;
}

static bool shim_wrote(struct shim *sh, long seq)
{
	bool hit = false;

	pthread_mutex_lock(&sh->m);
	for (size_t i = 0; i < sh->nseq; i++)
		hit = hit || sh->seqs[i] == (uint32_t)seq;
	pthread_mutex_unlock(&sh->m);
	return hit;
}

/* ---- events ---------------------------------------------------------------- */

struct evrec {
	pthread_mutex_t m;
	struct { enum datax_event_type type; long seq; char detail[128]; } e[256];
	size_t n;
	unsigned int sleep_us;      /* the callback dawdles (lifetime test) */
	/* the callback calls datax_sched_wait(wait_s, wait_seq, -1) once */
	struct datax_sched *wait_s;
	long wait_seq;
	int wait_ret;
	bool waited;
	char wait_err[256];
};

static void on_event(const struct datax_event *evt, void *user)
{
	struct evrec *r = user;

	pthread_mutex_lock(&r->m);
	if (r->n < sizeof(r->e) / sizeof(r->e[0])) {
		r->e[r->n].type = evt->type;
		r->e[r->n].seq = evt->seq;
		snprintf(r->e[r->n].detail, sizeof(r->e[r->n].detail), "%s",
			 evt->detail ? evt->detail : "");
		r->n++;
	}
	pthread_mutex_unlock(&r->m);
	if (r->wait_s && !r->waited) {
		struct datax_completion c;

		r->wait_ret = datax_sched_wait(r->wait_s, r->wait_seq, -1, &c);
		pthread_mutex_lock(&r->m);
		r->waited = true;
		pthread_mutex_unlock(&r->m);
	}
	if (r->sleep_us) {
		struct timespec ts = { 0, (long)r->sleep_us * 1000 };

		nanosleep(&ts, NULL);
	}
}

/* An event of type (and seq, unless seq == -2) whose detail contains text. */
static bool has_event(struct evrec *r, enum datax_event_type type, long seq, const char *text)
{
	bool hit = false;

	pthread_mutex_lock(&r->m);
	for (size_t i = 0; i < r->n; i++)
		if (r->e[i].type == type && (seq == -2 || r->e[i].seq == seq) &&
		    (!text || strstr(r->e[i].detail, text)))
			hit = true;
	pthread_mutex_unlock(&r->m);
	return hit;
}

/* ---- fixture ---------------------------------------------------------------- */

struct fx {
	struct datax_context *ctx;
	struct datax_device *dev;
	struct fake_tick *f;
	struct shim sh;
	struct datax_sched *s;
	struct evrec ev;
};

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void usleep_(unsigned int us)
{
	struct timespec ts = { us / 1000000, (long)(us % 1000000) * 1000L };

	nanosleep(&ts, NULL);
}

/* Poll cond for up to 3 s of real time. */
#define WAIT_FOR(cond)                                                       \
	do {                                                                 \
		int w_ = 0;                                                  \
		while (!(cond) && w_++ < 3000)                               \
			msleep(1);                                           \
		CHECK(cond);                                                 \
	} while (0)

static void fx_open(struct fx *x, unsigned int depth, bool create)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct fake_tick_config cfg = { .tick_hz = HZ, .queue_depth = depth };

	memset(x, 0, sizeof(*x));
	CHECK(uri != NULL);
	CHECK(pthread_mutex_init(&x->sh.m, NULL) == 0);
	CHECK(pthread_mutex_init(&x->ev.m, NULL) == 0);
	x->f = fake_tick_new(&cfg);
	CHECK(x->f != NULL);
	x->sh.f = x->f;
	CHECK(datax_context_create_from_uri(uri, &x->ctx) == 0);
	CHECK(datax_context_get_device(x->ctx, 0, &x->dev) == 0);
	CHECK(datax_internal_tick_bind_fake(x->dev, &shim_ops, &x->sh) == 0);
	CHECK(datax_context_set_event_callback(x->ctx, on_event, &x->ev) == 0);
	fake_tick_advance(x->f, 1000 * MS);   /* start well away from 0 */
	if (create) {
		fake_tick_set_access_cost(x->f, COST);
		CHECK(datax_sched_create(x->dev, &x->s) == 0);
		fake_tick_set_access_cost(x->f, 0);
	}
}

static void fx_close(struct fx *x)
{
	datax_sched_destroy(x->s);
	datax_context_destroy(x->ctx);
	fake_tick_free(x->f);
	pthread_mutex_destroy(&x->sh.m);
	pthread_mutex_destroy(&x->ev.m);
}

static const char *lasterr(struct fx *x)
{
	return datax_context_get_last_error(x->ctx);
}

/* Push a descriptor straight into the fake, as another Tick user would. */
static void foreign_push(struct fake_tick *f, uint32_t seq, uint64_t ts, uint32_t flags)
{
	struct datax_tick_descriptor d = {
		.version = DATAX_TICK_DESC_VERSION, .size_bytes = DATAX_TICK_DESC_SIZE,
		.command_id = DATAX_TICK_COMMAND_ID_GPIO_WRITE, .target_id = 0,
		.timestamp_ticks = ts, .flags = flags, .sequence_id = seq,
		.args = { 0x2, 0x2, 0, 0 },
	};
	unsigned char b[48];
	char csv[256];
	size_t n = 0;

	CHECK(datax_internal_tick_descriptor_pack(&d, b) == 0);
	for (int i = 0; i < 12; i++) {
		uint32_t w = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 |
			     (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;

		n += (size_t)snprintf(csv + n, sizeof(csv) - n, "%s%u", i ? "," : "", w);
	}
	CHECK(fake_tick_write(f, "command_descriptor", csv) == 0);
}

static unsigned long long fake_attr(struct fake_tick *f, const char *attr)
{
	char b[64];

	CHECK(fake_tick_read(f, attr, b, sizeof(b)) > 0);
	return strtoull(b, NULL, 0);
}

static long gpio(struct fx *x, unsigned long long when, bool report)
{
	long seq = datax_sched_queue_gpio(x->s, when, 0xe, 0x2);

	CHECK(seq > 0);
	if (report)
		CHECK(datax_sched_report(x->s, seq) == 0);
	return seq;
}

static struct datax_completion wait_ok(struct fx *x, long seq)
{
	struct datax_completion c;

	memset(&c, 0, sizeof(c));
	CHECK(datax_sched_wait(x->s, seq, 3000, &c) == 0);
	CHECK(c.seq == seq);
	return c;
}

/* Advance fake time in small steps, giving the poller real time between. */
static void run_until(struct fx *x, unsigned long long until, unsigned long long step)
{
	while (fake_tick_now(x->f) < until) {
		fake_tick_advance(x->f, step);
		usleep_(2000);
	}
}

/* ---- tests ----------------------------------------------------------------- */

static void test_one_scheduler_per_device(void)
{
	struct fx x;
	struct datax_sched *s2 = (struct datax_sched *)&x;
	const struct datax_caps *caps;
	unsigned long long v = 0;

	fx_open(&x, 0, false);
	caps = datax_device_get_caps(x.dev);
	CHECK(datax_sched_create(NULL, &x.s) == -EINVAL);
	CHECK(datax_sched_create(x.dev, NULL) == -EINVAL);
	CHECK(datax_caps_get_limit(caps, DATAX_LIMIT_REPORT_SPACING_US, &v) == -ENOTSUP);

	fake_tick_set_access_cost(x.f, COST);
	CHECK(datax_sched_create(x.dev, &x.s) == 0 && x.s != NULL);
	fake_tick_set_access_cost(x.f, 0);
	/* 3 x the median timed drain (8 reads + ack at COST each) */
	CHECK(datax_caps_get_limit(caps, DATAX_LIMIT_REPORT_SPACING_US, &v) == 0);
	CHECK(v == SPACING_US);
	CHECK(datax_caps_get_limit(caps, DATAX_LIMIT_HOP_STAGE_US, &v) == -ENOTSUP);

	CHECK(datax_sched_create(x.dev, &s2) == -EBUSY);
	CHECK(s2 == NULL);
	CHECK(strstr(lasterr(&x), "already") != NULL);
	/* the seam cannot be rebound under a live poller */
	CHECK(datax_internal_tick_bind_fake(x.dev, &fake_tick_ops, x.f) == -EBUSY);
	CHECK(strstr(lasterr(&x), "scheduler") != NULL);

	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(datax_caps_get_limit(caps, DATAX_LIMIT_REPORT_SPACING_US, &v) == -ENOTSUP);
	CHECK(datax_sched_create(x.dev, &x.s) == 0);
	datax_sched_destroy(x.s);
	x.s = NULL;

	/* someone else's command in the queue: a warning, not a failure */
	foreign_push(x.f, 777, fake_tick_now(x.f) + 1000 * MS, DATAX_TICK_FLAG_LATE_DROP);
	CHECK(datax_sched_create(x.dev, &x.s) == 0);
	CHECK(strstr(lasterr(&x), "queue_level") != NULL);
	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(fake_tick_write(x.f, "flush_queue", "1") == 0);

	/* no Tick: -ENOTSUP */
	CHECK(datax_internal_tick_bind_fake(x.dev, NULL, NULL) == 0);
	CHECK(datax_sched_create(x.dev, &x.s) == -ENOTSUP);
	CHECK(strstr(lasterr(&x), "no Tick") != NULL);
	fx_close(&x);
	printf("test_one_scheduler_per_device: ok\n");
}

/* Destroy leaves Tick empty (ruling 3, Tick is exclusive): nothing a
 * destroyed scheduler queued runs unattended or holds the next one's
 * reported heads; stale records in the slot are never matched. */
static void test_destroy_leaves_tick_empty(void)
{
	struct fx x;
	unsigned long long t;
	long a, b, c, d;
	struct datax_completion cc;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	/* (1) written descriptors are flushed from Tick by destroy */
	a = gpio(&x, t + 50 * MS, true);
	(void)gpio(&x, t + 60 * MS, false);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 2);
	CHECK(fake_attr(x.f, "queue_level") == 2);
	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(fake_attr(x.f, "queue_level") == 0);
	fake_tick_advance(x.f, 100 * MS);
	CHECK(fake_tick_exec_log(x.f, 0, NULL, 0) == 0);     /* nothing ran unattended */

	/* (2) an executed, undrained report is acked by destroy */
	CHECK(datax_sched_create(x.dev, &x.s) == 0);
	t = fake_tick_now(x.f);
	b = gpio(&x, t + 20 * MS, true);
	CHECK(b != a);                                        /* ids never repeat */
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_wrote(&x.sh, b));
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_advance(x.f, 30 * MS);                      /* b ran; slot full */
	CHECK(fake_attr(x.f, "completion_sequence_id") == (unsigned long long)b);
	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(fake_attr(x.f, "completion_sequence_id") == 0);

	/* (3) another user's record in the slot at create is not ours */
	foreign_push(x.f, 4040, 1, DATAX_TICK_FLAG_LATE_DROP);
	CHECK(fake_attr(x.f, "completion_sequence_id") == 4040);
	CHECK(datax_sched_create(x.dev, &x.s) == 0);
	CHECK(fake_attr(x.f, "completion_sequence_id") == 0);  /* create acked it */
	t = fake_tick_now(x.f);
	c = gpio(&x, t + 20 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_wrote(&x.sh, c));
	/* (4) a stale record whose seq collides with a written command (same
	 * seq and command, another timestamp) is not matched to it */
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	foreign_push(x.f, (uint32_t)c, 1, DATAX_TICK_FLAG_LATE_DROP);
	CHECK(fake_attr(x.f, "completion_sequence_id") == (unsigned long long)c);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(fake_attr(x.f, "completion_sequence_id") == 0);
	CHECK(datax_sched_wait(x.s, c, 0, &cc) == -ETIMEDOUT);  /* still pending */
	run_until(&x, t + 25 * MS, MS);
	cc = wait_ok(&x, c);
	CHECK(cc.status == DATAX_COMPLETION_OK && cc.requested_ticks == t + 20 * MS &&
	      cc.executed_ticks == t + 20 * MS);
	/* (5) the context's destroy flushes too */
	d = gpio(&x, fake_tick_now(x.f) + 50 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_wrote(&x.sh, d));
	x.s = NULL;
	datax_context_destroy(x.ctx);
	x.ctx = NULL;
	CHECK(fake_attr(x.f, "queue_level") == 0);
	fx_close(&x);
	printf("test_destroy_leaves_tick_empty: ok\n");
}

static void test_builder_sorts(void)
{
	struct fx x;
	unsigned long long t;
	long s30, s10, s20a, s20b;
	struct fake_tick_exec log[8];

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	s30 = gpio(&x, t + 30 * MS, false);
	s10 = gpio(&x, t + 11 * MS, false);
	s20a = gpio(&x, t + 20 * MS, false);
	s20b = gpio(&x, t + 20 * MS, false);   /* equal time: queue order kept */
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 4);
	CHECK(x.sh.seqs[0] == (uint32_t)s10 && x.sh.seqs[1] == (uint32_t)s20a &&
	      x.sh.seqs[2] == (uint32_t)s20b && x.sh.seqs[3] == (uint32_t)s30);
	run_until(&x, t + 40 * MS, 5 * MS);
	CHECK(fake_tick_exec_log(x.f, 0, log, 8) == 4);
	CHECK(log[0].sequence_id == (uint32_t)s10 && log[3].sequence_id == (uint32_t)s30);
	for (int i = 0; i < 4; i++)
		CHECK(log[i].status == DATAX_TICK_STATUS_EXECUTED &&
		      log[i].executed_ticks == log[i].requested_ticks && !log[i].reported);
	/* an empty commit is a no-op */
	CHECK(datax_sched_commit(x.s) == 0);
	fx_close(&x);
	printf("test_builder_sorts: ok\n");
}

static void test_commit_guard_margin_late(void)
{
	struct fx x;
	unsigned long long t;
	long ok_seq, late_seq, edge;
	struct datax_completion c;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	ok_seq = gpio(&x, t + 50 * MS, true);
	late_seq = gpio(&x, t + GUARD - 1, false);
	CHECK(datax_sched_commit(x.s) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "guard") != NULL);
	msleep(20);
	CHECK(shim_count(&x.sh) == 0);
	/* the whole batch was discarded */
	CHECK(datax_sched_wait(x.s, ok_seq, 0, &c) == -EINVAL);
	CHECK(datax_sched_wait(x.s, late_seq, 0, &c) == -EINVAL);
	CHECK(datax_sched_commit(x.s) == 0);
	/* exactly now + guard is accepted by commit (the feeder may still find
	 * it inside the guard by the time it gets to it: then it completes LATE
	 * unwritten, never written late) */
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	edge = gpio(&x, t + GUARD, true);
	CHECK(datax_sched_commit(x.s) == 0);
	CHECK(datax_sched_wait(x.s, edge, 0, &c) == -ETIMEDOUT);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	if (datax_sched_wait(x.s, edge, 200, &c) == -ETIMEDOUT) {
		run_until(&x, t + GUARD + MS, MS);   /* it was written in time */
		CHECK(datax_sched_wait(x.s, edge, 3000, &c) == 0);
	}
	CHECK(c.status == DATAX_COMPLETION_LATE || c.status == DATAX_COMPLETION_OK);
	CHECK(c.status == DATAX_COMPLETION_OK || !shim_wrote(&x.sh, edge));
	/* comfortably outside the guard: written */
	edge = gpio(&x, t + GUARD + 20 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_wrote(&x.sh, edge));
	fx_close(&x);
	printf("test_commit_guard_margin_late: ok\n");
}

static void test_commit_streams_large_batch(void)
{
	struct fx x;
	unsigned long long t, t0;
	long seq[40];
	struct fake_tick_exec log[48];

	fx_open(&x, 16, true);
	x.sh.track_level = true;
	t = fake_tick_now(x.f);
	t0 = t + 20 * MS;
	/* 40 commands 1 ms apart, every 4th reported (4 ms > the 2.7 ms spacing) */
	for (int i = 0; i < 40; i++)
		seq[i] = gpio(&x, t0 + (unsigned long long)i * MS, i % 4 == 0);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 15);
	msleep(10);
	CHECK(shim_count(&x.sh) == 15);          /* depth - 1, one entry spare */
	run_until(&x, t0 + 41 * MS, MS / 4);
	WAIT_FOR(shim_count(&x.sh) == 40);
	CHECK(x.sh.max_level <= 15);
	for (int i = 0; i < 40; i++)
		CHECK(x.sh.seqs[i] == (uint32_t)seq[i]);
	CHECK(fake_tick_exec_log(x.f, 0, log, 48) == 40);
	for (int i = 0; i < 40; i++)
		CHECK(log[i].sequence_id == (uint32_t)seq[i] &&
		      log[i].status == DATAX_TICK_STATUS_EXECUTED &&
		      log[i].executed_ticks == log[i].requested_ticks);
	CHECK(fake_attr(x.f, "overflow_count") == 0 && fake_attr(x.f, "late_count") == 0);
	for (int i = 0; i < 40; i += 4) {
		struct datax_completion c = wait_ok(&x, seq[i]);

		CHECK(c.status == DATAX_COMPLETION_OK);
		CHECK(c.requested_ticks == t0 + (unsigned long long)i * MS);
		CHECK(c.executed_ticks == c.requested_ticks);
	}
	CHECK(!has_event(&x.ev, DATAX_EVT_LATE_COMMAND, -2, NULL));
	fx_close(&x);
	printf("test_commit_streams_large_batch: ok\n");
}

static void test_feeder_starved_marks_late(void)
{
	struct fx x;
	unsigned long long t;
	long a, b, far;
	struct datax_completion c;

	/* (1) the poller cannot write in time (paused past the guard) */
	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	a = gpio(&x, t + GUARD + MS, true);
	b = gpio(&x, t + GUARD + MS + SPACING, true);
	far = gpio(&x, t + 100 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	msleep(10);
	CHECK(shim_count(&x.sh) == 0);           /* paused: nothing written */
	fake_tick_advance(x.f, 2 * MS);          /* a is now inside the guard */
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	CHECK(datax_sched_wait(x.s, a, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_LATE && c.executed_ticks == 0);
	CHECK(c.requested_ticks == t + GUARD + MS);
	CHECK(strstr(lasterr(&x), "host could not keep the queue fed") != NULL);
	CHECK(!shim_wrote(&x.sh, a));
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, a, "queue fed"));
	WAIT_FOR(shim_wrote(&x.sh, b) && shim_wrote(&x.sh, far));
	run_until(&x, t + 110 * MS, 5 * MS);
	CHECK(wait_ok(&x, b).status == DATAX_COMPLETION_OK);
	CHECK(wait_ok(&x, far).status == DATAX_COMPLETION_OK);
	fx_close(&x);

	/* (2) the queue is full (depth 4: 3 in hardware) and the rest fall due
	 * before there is room: they complete LATE and are never written */
	fx_open(&x, 4, true);
	x.sh.track_level = true;
	t = fake_tick_now(x.f);
	{
		long s[8];

		for (int i = 0; i < 8; i++)
			s[i] = gpio(&x, t + 20 * MS + (unsigned long long)i, i == 7);
		CHECK(datax_sched_commit(x.s) == 0);
		WAIT_FOR(shim_count(&x.sh) == 3);
		msleep(10);
		CHECK(shim_count(&x.sh) == 3);
		fake_tick_advance(x.f, 25 * MS);
		CHECK(datax_sched_wait(x.s, s[7], 3000, &c) == 0);
		CHECK(c.status == DATAX_COMPLETION_LATE);
		CHECK(strstr(lasterr(&x), "host could not keep the queue fed") != NULL);
		CHECK(shim_count(&x.sh) == 3 && x.sh.max_level <= 3);
		CHECK(fake_tick_exec_log(x.f, 0, NULL, 0) == 3);
	}
	fx_close(&x);

	/* (3) the queue is full of later commands: an earlier one committed
	 * after them cannot be written, and the detail says why */
	fx_open(&x, 4, true);
	t = fake_tick_now(x.f);
	for (int i = 0; i < 3; i++)
		(void)gpio(&x, t + 100 * MS + (unsigned long long)i, false);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 3);
	a = gpio(&x, t + 20 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	msleep(10);
	CHECK(!shim_wrote(&x.sh, a));
	fake_tick_advance(x.f, 15 * MS);
	CHECK(datax_sched_wait(x.s, a, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_LATE);
	CHECK(strstr(lasterr(&x), "queue held depth-1 (3) commands, 3 of them later") != NULL);
	fx_close(&x);
	printf("test_feeder_starved_marks_late: ok\n");
}

static void test_report_spacing_refused(void)
{
	struct fx x;
	unsigned long long t, ta;
	long a, b, c2, d;
	struct datax_completion c;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	ta = t + 50 * MS;
	/* one tick under the published spacing: refused, nothing written */
	a = gpio(&x, ta, true);
	b = gpio(&x, ta + SPACING - 1, true);
	CHECK(datax_sched_commit(x.s) == -EINVAL);
	CHECK(strstr(lasterr(&x), "report spacing") != NULL);
	CHECK(datax_sched_wait(x.s, a, 0, &c) == -EINVAL);
	/* unreported commands are not subject to it */
	(void)gpio(&x, ta + 1, false);
	(void)gpio(&x, ta + 2, false);
	/* exactly the spacing is accepted */
	a = gpio(&x, ta, true);
	b = gpio(&x, ta + SPACING, true);
	CHECK(datax_sched_commit(x.s) == 0);
	/* against commands already committed, too */
	c2 = gpio(&x, ta + SPACING + SPACING / 2, true);
	CHECK(datax_sched_commit(x.s) == -EINVAL);
	CHECK(datax_sched_wait(x.s, c2, 0, &c) == -EINVAL);
	d = gpio(&x, ta + SPACING + 1, false);
	CHECK(datax_sched_commit(x.s) == 0);
	/* report() only marks commands not yet committed */
	CHECK(datax_sched_report(x.s, d) == -EINVAL);
	CHECK(strstr(lasterr(&x), "committed") != NULL);
	CHECK(datax_sched_report(x.s, 999999) == -EINVAL);
	CHECK(datax_sched_report(NULL, a) == -EINVAL);
	WAIT_FOR(shim_count(&x.sh) == 5);
	CHECK(!shim_wrote(&x.sh, c2));
	run_until(&x, ta + 2 * SPACING, MS / 4);   /* drain between a and b */
	CHECK(wait_ok(&x, a).status == DATAX_COMPLETION_OK);
	CHECK(wait_ok(&x, b).status == DATAX_COMPLETION_OK);
	fx_close(&x);
	printf("test_report_spacing_refused: ok\n");
}

static void test_wait_unreported_einval(void)
{
	struct fx x;
	unsigned long long t;
	long u, r;
	struct datax_completion c;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	u = gpio(&x, t + 50 * MS, false);
	r = gpio(&x, t + 1000 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	CHECK(datax_sched_wait(x.s, u, 100, &c) == -EINVAL);
	CHECK(strstr(lasterr(&x), "not reported") != NULL);
	CHECK(datax_sched_wait(x.s, 123456789, 0, &c) == -EINVAL);
	CHECK(datax_sched_wait(x.s, r, 0, NULL) == -EINVAL);
	CHECK(datax_sched_wait(NULL, r, 0, &c) == -EINVAL);
	/* not yet due: a timeout, not a DROPPED */
	CHECK(datax_sched_wait(x.s, r, 20, &c) == -ETIMEDOUT);
	CHECK(datax_sched_wait(x.s, r, 0, &c) == -ETIMEDOUT);
	run_until(&x, t + 1010 * MS, 50 * MS);
	CHECK(wait_ok(&x, r).status == DATAX_COMPLETION_OK);
	/* consumed */
	CHECK(datax_sched_wait(x.s, r, 0, &c) == -EINVAL);
	fx_close(&x);
	printf("test_wait_unreported_einval: ok\n");
}

/* Ruling 1: Tick holds a reported head while its one completion slot is
 * unacked, then records it as on time. A drain after the next reported
 * command's time must mark that command LATE, never OK. */
static void test_poller_lag_marks_late(void)
{
	struct fx x;
	unsigned long long t, ta, tb;
	long a, b;
	struct datax_completion c;
	struct fake_tick_exec log[4];

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	ta = t + 20 * MS;
	tb = ta + SPACING;
	a = gpio(&x, ta, true);
	b = gpio(&x, tb, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 2);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_advance(x.f, tb + MS - fake_tick_now(x.f));  /* a ran; b is held */
	CHECK(fake_tick_exec_log(x.f, 0, log, 4) == 1);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);

	c = wait_ok(&x, a);
	CHECK(c.status == DATAX_COMPLETION_OK && c.executed_ticks == ta);
	CHECK(datax_sched_wait(x.s, b, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_LATE);
	CHECK(strstr(lasterr(&x), "poller lagged") != NULL);
	CHECK(c.requested_ticks == tb);
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, b, "poller lagged"));
	/* Tick's own record claims b ran on time: exactly what must not show */
	CHECK(fake_tick_exec_log(x.f, 0, log, 4) == 2);
	CHECK(log[1].sequence_id == (uint32_t)b && log[1].status == DATAX_TICK_STATUS_EXECUTED &&
	      log[1].executed_ticks == tb);
	fx_close(&x);
	printf("test_poller_lag_marks_late: ok\n");
}

/* The slot stays full until the ack lands. A reported command that comes
 * due during the ack's round trip is held, then released by the ack and
 * recorded on time: it must still complete LATE, never OK. */
static void test_slow_ack_marks_late(void)
{
	struct fx x;
	unsigned long long t, ta, tb;
	long a, b;
	struct datax_completion c;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	ta = t + 20 * MS;
	tb = ta + SPACING;
	a = gpio(&x, ta, true);
	b = gpio(&x, tb, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 2);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_advance(x.f, tb - MS - fake_tick_now(x.f));  /* a ran; b not yet due */
	pthread_mutex_lock(&x.sh.m);
	x.sh.ack_delay = 2 * MS;                               /* b falls due mid-ack */
	pthread_mutex_unlock(&x.sh.m);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	c = wait_ok(&x, a);
	CHECK(c.status == DATAX_COMPLETION_OK);
	CHECK(datax_sched_wait(x.s, b, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_LATE);
	CHECK(strstr(lasterr(&x), "poller lagged") != NULL);
	fx_close(&x);
	printf("test_slow_ack_marks_late: ok\n");
}

/* The write of seq is stalled past its time while another user's record
 * fills the slot: Tick drops it late with nowhere to report (RTL 846-861). */
static void stall_and_fill(struct shim *sh, uint32_t seq)
{
	if (sh->hook_done || seq != sh->hook_seq)
		return;
	sh->hook_done = true;
	foreign_push(sh->f, 4242, 1, DATAX_TICK_FLAG_LATE_DROP);  /* late: fills the slot */
	fake_tick_advance(sh->f, 30 * MS);
	if (sh->pause_s)   /* on the poller thread, mid-feed */
		CHECK(datax_internal_sched_pause(sh->pause_s, true) == 0);
}

enum lost_mode { LOST_WAIT_FOREVER, LOST_GET_COMPLETIONS, LOST_FLUSH };

/* A proven loss completes DROPPED as soon as it is proven: wait(-1)
 * returns it, get_completions surfaces it, and flush reports it DROPPED
 * rather than FLUSHED. */
static void lost_case(enum lost_mode mode)
{
	struct fx x;
	unsigned long long t;
	long a;
	struct datax_completion c;
	struct fake_tick_exec log[4];
	size_t n = 0;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	a = gpio(&x, t + 20 * MS, true);
	x.sh.hook_seq = (uint32_t)a;
	x.sh.before_desc = stall_and_fill;
	if (mode == LOST_FLUSH)
		x.sh.pause_s = x.s;
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_wrote(&x.sh, a));
	CHECK(fake_tick_exec_log(x.f, 0, log, 4) == 2);
	CHECK(log[1].sequence_id == (uint32_t)a && !log[1].reported &&
	      log[1].status == DATAX_TICK_STATUS_LATE_DROPPED);
	CHECK(fake_attr(x.f, "overflow_count") == 1);
	switch (mode) {
	case LOST_WAIT_FOREVER:
		CHECK(datax_sched_wait(x.s, a, -1, &c) == 0);
		CHECK(strstr(lasterr(&x), "no completion; see overflow_count (1)") != NULL);
		break;
	case LOST_GET_COMPLETIONS:
		for (int i = 0; i < 3000 && n == 0; i++) {
			CHECK(datax_sched_get_completions(x.s, &c, 1, &n) == 0);
			if (!n)
				msleep(1);
		}
		CHECK(n == 1 && c.seq == a);
		break;
	case LOST_FLUSH:
		msleep(5);
		CHECK(fake_attr(x.f, "completion_sequence_id") == 4242);  /* undrained */
		CHECK(datax_sched_flush(x.s) == 0);
		CHECK(datax_sched_get_completions(x.s, &c, 1, &n) == 0 && n == 1);
		CHECK(c.seq == a);
		CHECK(datax_internal_sched_pause(x.s, false) == 0);
		break;
	}
	CHECK(c.status == DATAX_COMPLETION_DROPPED && c.executed_ticks == 0);
	CHECK(c.requested_ticks == t + 20 * MS);
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_QUEUE_OVERFLOW, -1, "overflow_count"));
	/* consumed */
	CHECK(datax_sched_wait(x.s, a, 0, &c) == -EINVAL);
	fx_close(&x);
}

static void test_lost_report_dropped(void)
{
	lost_case(LOST_WAIT_FOREVER);
	lost_case(LOST_GET_COMPLETIONS);
	lost_case(LOST_FLUSH);
	printf("test_lost_report_dropped: ok\n");
}

/* An event callback runs on the poller thread: a wait there would stall or
 * deadlock the poller, so it is refused, and the poller keeps going. */
static void test_wait_in_callback_refused(void)
{
	struct fx x;
	unsigned long long t;
	long r;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	r = gpio(&x, t + 50 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_wrote(&x.sh, r));
	x.ev.wait_seq = r;
	x.ev.wait_ret = 1;
	x.ev.wait_s = x.s;
	foreign_push(x.f, 6060, 1, DATAX_TICK_FLAG_LATE_DROP);   /* late_count rises */
	{
		int i = 0;
		bool w = false;

		while (!w && i++ < 3000) {
			pthread_mutex_lock(&x.ev.m);
			w = x.ev.waited;
			pthread_mutex_unlock(&x.ev.m);
			if (!w)
				msleep(1);
		}
		CHECK(w);
	}
	CHECK(x.ev.wait_ret == -EDEADLK);
	CHECK(strstr(lasterr(&x), "event callback") != NULL);
	run_until(&x, t + 55 * MS, MS);
	CHECK(wait_ok(&x, r).status == DATAX_COMPLETION_OK);   /* the poller lives */
	fx_close(&x);                                          /* and joins */
	printf("test_wait_in_callback_refused: ok\n");
}

/* More events in one iteration than the queue holds: one summary. */
static void test_event_overflow_summarised(void)
{
	struct fx x;
	unsigned long long t;
	size_t late = 0;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	for (int i = 0; i < 40; i++)
		(void)gpio(&x, t + GUARD + MS + (unsigned long long)i, false);
	CHECK(datax_sched_commit(x.s) == 0);
	fake_tick_advance(x.f, 2 * MS);     /* all 40 host-late in one feed pass */
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, -1, "8 events dropped"));
	pthread_mutex_lock(&x.ev.m);
	for (size_t i = 0; i < x.ev.n; i++)
		late += x.ev.e[i].type == DATAX_EVT_LATE_COMMAND && x.ev.e[i].seq > 0;
	pthread_mutex_unlock(&x.ev.m);
	CHECK(late == 32);
	CHECK(shim_count(&x.sh) == 0);
	fx_close(&x);
	printf("test_event_overflow_summarised: ok\n");
}

static void test_events_late_and_overflow(void)
{
	struct fx x;

	fx_open(&x, 0, true);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	/* another Tick user: two late descriptors; the second finds the slot
	 * full, so its report is lost (late_count 2, overflow_count 1) */
	foreign_push(x.f, 9001, 1, DATAX_TICK_FLAG_LATE_DROP);
	foreign_push(x.f, 9002, 2, DATAX_TICK_FLAG_LATE_DROP);
	CHECK(fake_attr(x.f, "late_count") == 2 && fake_attr(x.f, "overflow_count") == 1);
	msleep(5);
	CHECK(x.ev.n == 0);                       /* paused: nothing observed */
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, -1, "late_count 2"));
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_QUEUE_OVERFLOW, -1, "overflow_count 1"));
	/* a rise is reported once */
	msleep(20);
	{
		size_t late = 0;

		pthread_mutex_lock(&x.ev.m);
		for (size_t i = 0; i < x.ev.n; i++)
			late += x.ev.e[i].type == DATAX_EVT_LATE_COMMAND;
		pthread_mutex_unlock(&x.ev.m);
		CHECK(late == 1);
	}
	CHECK(datax_internal_sched_pause(NULL, true) == -EINVAL);
	fx_close(&x);
	printf("test_events_late_and_overflow: ok\n");
}

static void test_reserved_gpio_bit(bool adrv9009)
{
	struct fx x;
	unsigned long long t;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f) + 100 * MS;
	if (adrv9009) {
		/* bit 0 drives TAL_GPIO_0 (the FHM trigger): queue_tune only */
		CHECK(datax_sched_queue_gpio(x.s, t, 0x1, 0x1) == -EINVAL);
		CHECK(strstr(lasterr(&x), "reserved") != NULL);
		CHECK(datax_sched_queue_gpio(x.s, t, 0xf, 0x0) == -EINVAL);
	} else {
		CHECK(datax_sched_queue_gpio(x.s, t, 0x1, 0x1) > 0);
	}
	CHECK(datax_sched_queue_gpio(x.s, t, 0xe, 0x4) > 0);    /* bits 3:1 */
	CHECK(datax_sched_queue_gpio(x.s, t, 0, 0) == -EINVAL);
	CHECK(datax_sched_queue_gpio(NULL, t, 0xe, 0) == -EINVAL);
	CHECK(datax_sched_queue_marker(x.s, t, 3, 10) > 0);
	CHECK(datax_sched_queue_marker(x.s, t, 8, 10) == -EINVAL);
	CHECK(datax_sched_queue_marker(NULL, t, 0, 1) == -EINVAL);
	/* FHM tunes need a staged table (adrv9009); ad936x fastlock is P4.
	 * Stream gates are P3b. */
	if (adrv9009) {
		CHECK(datax_sched_queue_tune(x.s, t, 0, 2450000000ULL) == -EINVAL);
		CHECK(strstr(lasterr(&x), "frequency not staged") != NULL);
	} else {
		CHECK(datax_sched_queue_tune(x.s, t, 0, 2450000000ULL) == -ENOTSUP);
	}
	CHECK(datax_sched_queue_stream_gate(x.s, t, NULL, true) == -ENOTSUP);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == (adrv9009 ? 2u : 3u));
	run_until(&x, t + MS, 50 * MS);
	CHECK(fake_tick_marker(x.f) == 1u << 3);
	fx_close(&x);
	printf("test_reserved_gpio_bit(%s): ok\n", adrv9009 ? "adrv9009" : "ad936x");
}

/* Final review M1: the time read after an ack fails. The record has left
 * the slot, so it is processed anyway with an estimated time, as lagged:
 * LATE "poller lagged", never DROPPED "no completion". */
static void test_acked_record_kept_on_time_read_failure(void)
{
	struct fx x;
	struct datax_completion c;
	unsigned long long t;
	long seq;

	fx_open(&x, 0, true);
	t = fake_tick_now(x.f) + 50 * MS;
	seq = gpio(&x, t, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 1);
	pthread_mutex_lock(&x.sh.m);
	x.sh.fail_time_after_ack = true;
	pthread_mutex_unlock(&x.sh.m);
	run_until(&x, t + MS, 1 * MS);
	memset(&c, 0, sizeof(c));
	CHECK(datax_sched_wait(x.s, seq, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_LATE);
	CHECK(strstr(lasterr(&x), "poller lagged") != NULL);
	CHECK(c.executed_ticks == t);                /* Tick's own record, kept */
	CHECK(!x.sh.fail_time_after_ack && !x.sh.ack_seen);   /* the failure was hit */
	fx_close(&x);
	printf("test_acked_record_kept_on_time_read_failure: ok\n");
}

static void test_flush_completes_flushed(void)
{
	struct fx x;
	unsigned long long t;
	long s[6];
	struct datax_completion out[8];
	size_t n = 99;

	fx_open(&x, 4, true);
	t = fake_tick_now(x.f);
	for (int i = 0; i < 6; i++)
		s[i] = gpio(&x, t + 20 * MS + (unsigned long long)i * SPACING, true);
	(void)gpio(&x, t + 21 * MS, false);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(shim_count(&x.sh) == 3);         /* 3 written, 4 waiting */
	CHECK(datax_sched_get_completions(x.s, out, 8, &n) == 0 && n == 0);
	CHECK(datax_sched_flush(x.s) == 0);
	CHECK(fake_attr(x.f, "queue_level") == 0);
	CHECK(datax_sched_get_completions(x.s, out, 4, &n) == 0 && n == 4);
	for (size_t i = 0; i < 4; i++)
		CHECK(out[i].status == DATAX_COMPLETION_FLUSHED && out[i].executed_ticks == 0);
	CHECK(datax_sched_get_completions(x.s, out, 8, &n) == 0 && n == 2);
	for (size_t i = 0; i < 2; i++)
		CHECK(out[i].status == DATAX_COMPLETION_FLUSHED);
	CHECK(datax_sched_get_completions(x.s, out, 8, &n) == 0 && n == 0);
	CHECK(datax_sched_get_completions(x.s, NULL, 8, &n) == -EINVAL);
	CHECK(datax_sched_get_completions(NULL, out, 8, &n) == -EINVAL);
	run_until(&x, t + 50 * MS, 5 * MS);
	CHECK(fake_tick_exec_log(x.f, 0, NULL, 0) == 0);   /* nothing ran */
	CHECK(shim_count(&x.sh) == 3);
	CHECK(datax_sched_wait(x.s, s[0], 0, out) == -EINVAL);  /* consumed */
	/* the scheduler is usable after a flush */
	t = fake_tick_now(x.f);
	s[0] = gpio(&x, t + 20 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	run_until(&x, t + 25 * MS, 5 * MS);
	CHECK(wait_ok(&x, s[0]).status == DATAX_COMPLETION_OK);
	CHECK(datax_sched_flush(NULL) == -EINVAL);
	fx_close(&x);
	printf("test_flush_completes_flushed: ok\n");
}

/* A thread blocked in wait() while another destroys the scheduler: the
 * waiter returns -ECANCELED and destroy frees only after it has left
 * (ASan/TSan). Looped to shake out the race. */
struct waiter {
	struct datax_sched *s;
	long seq;
	int timeout_ms;
	int ret;
};

static void *waiter_main(void *arg)
{
	struct waiter *w = arg;
	struct datax_completion c;

	w->ret = datax_sched_wait(w->s, w->seq, w->timeout_ms, &c);
	return NULL;
}

#define NWAITERS 16

static void test_destroy_cancels_waiters(void)
{
	for (int i = 0; i < 50; i++) {
		struct fx x;
		struct waiter w[NWAITERS];
		pthread_t th[NWAITERS];
		unsigned long long t;
		long r;

		fx_open(&x, 0, true);
		t = fake_tick_now(x.f);
		r = gpio(&x, t + 1000 * MS, true);
		CHECK(datax_sched_commit(x.s) == 0);
		/* many waiters (one seq may have several), finite and infinite, so
		 * some re-take the lock only after destroy would have freed it */
		for (int k = 0; k < NWAITERS; k++) {
			w[k].s = x.s;
			w[k].seq = r;
			w[k].timeout_ms = k % 4 == 0 ? 10000 : -1;
			w[k].ret = 1;
			CHECK(pthread_create(&th[k], NULL, waiter_main, &w[k]) == 0);
		}
		msleep(20);                       /* all blocked in wait() */
		datax_sched_destroy(x.s);
		x.s = NULL;
		for (int k = 0; k < NWAITERS; k++) {
			CHECK(pthread_join(th[k], NULL) == 0);
			CHECK(w[k].ret == -ECANCELED);
		}
		fx_close(&x);
	}
	printf("test_destroy_cancels_waiters: ok\n");
}

/* The same, but the caller destroys the whole context while waiters are
 * blocked, and only then joins them: a waiter must write its last error
 * before it leaves (under the scheduler lock), not after the context is
 * gone. ASan reports the late write as a heap-use-after-free. */
static void test_context_destroy_cancels_waiters(void)
{
	for (int i = 0; i < 50; i++) {
		struct fx x;
		struct waiter w[NWAITERS];
		pthread_t th[NWAITERS];
		long r;

		fx_open(&x, 0, true);
		r = gpio(&x, fake_tick_now(x.f) + 1000 * MS, true);
		CHECK(datax_sched_commit(x.s) == 0);
		for (int k = 0; k < NWAITERS; k++) {
			w[k].s = x.s;
			w[k].seq = r;
			w[k].timeout_ms = k % 2 ? 10000 : -1;
			w[k].ret = 1;
			CHECK(pthread_create(&th[k], NULL, waiter_main, &w[k]) == 0);
		}
		msleep(10);
		x.s = NULL;                      /* the context destroys it */
		datax_context_destroy(x.ctx);
		x.ctx = NULL;
		for (int k = 0; k < NWAITERS; k++) {
			CHECK(pthread_join(th[k], NULL) == 0);
			CHECK(w[k].ret == -ECANCELED);
		}
		fake_tick_free(x.f);
		pthread_mutex_destroy(&x.sh.m);
		pthread_mutex_destroy(&x.ev.m);
	}
	printf("test_context_destroy_cancels_waiters: ok\n");
}

/* get_time racing scheduler create/destroy: it holds a reference while it
 * takes the scheduler's io lock, so destroy cannot free it underneath. */
struct timer_arg {
	struct datax_device *dev;
	atomic_bool stop;
	unsigned long n;
};

static void *timer_main(void *arg)
{
	struct timer_arg *a = arg;

	while (!atomic_load(&a->stop)) {
		unsigned long long ticks, hz;

		CHECK(datax_device_get_time(a->dev, &ticks, &hz) == 0);
		a->n++;
	}
	return NULL;
}

static void test_get_time_races_destroy(void)
{
	struct fx x;
	struct timer_arg a;
	pthread_t th;

	fx_open(&x, 0, false);
	a.dev = x.dev;
	atomic_init(&a.stop, false);
	a.n = 0;
	CHECK(pthread_create(&th, NULL, timer_main, &a) == 0);
	for (int i = 0; i < 50; i++) {
		CHECK(datax_sched_create(x.dev, &x.s) == 0);
		(void)gpio(&x, fake_tick_now(x.f) + 100 * MS, i % 2);
		CHECK(datax_sched_commit(x.s) == 0);
		usleep_((unsigned)(i % 5) * 300);
		datax_sched_destroy(x.s);
		x.s = NULL;
	}
	atomic_store(&a.stop, true);
	CHECK(pthread_join(th, NULL) == 0);
	CHECK(a.n > 0);
	fx_close(&x);
	printf("test_get_time_races_destroy: ok\n");
}

/* Destroying the scheduler or the whole context while the poller is mid-feed
 * or mid-drain joins the thread before anything is freed (ASan/valgrind). */
static void test_destroy_while_polling(void)
{
	for (int i = 0; i < 12; i++) {
		struct fx x;
		unsigned long long t;

		fx_open(&x, 0, true);
		x.ev.sleep_us = 200;
		/* the poller now spends real time per access: mid-I/O on destroy */
		fake_tick_set_access_cost(x.f, MS / 10);
		t = fake_tick_now(x.f);
		for (int k = 0; k < 40; k++)
			(void)gpio(&x, t + 15 * MS + (unsigned long long)k * MS, k % 8 == 0);
		CHECK(datax_sched_commit(x.s) == 0);
		foreign_push(x.f, 5000 + (uint32_t)i, 1, DATAX_TICK_FLAG_LATE_DROP);
		usleep_((unsigned)(i % 4) * 700);
		if (i % 2) {
			datax_sched_destroy(x.s);
		}
		x.s = NULL;                /* even i: the context destroys it */
		datax_context_destroy(x.ctx);
		x.ctx = NULL;
		fake_tick_free(x.f);
		pthread_mutex_destroy(&x.sh.m);
		pthread_mutex_destroy(&x.ev.m);
	}
	printf("test_destroy_while_polling: ok\n");
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "all";

	if (!strcmp(mode, "destroy")) {
		test_destroy_while_polling();
		test_destroy_cancels_waiters();
		test_context_destroy_cancels_waiters();
		test_get_time_races_destroy();
		return 0;
	}
	if (!strcmp(mode, "ad936x")) {
		test_reserved_gpio_bit(false);
		return 0;
	}
	if (!strcmp(mode, "one") && argc > 2) {
		static const struct { const char *name; void (*fn)(void); } t[] = {
			{ "destroy_leaves_tick_empty", test_destroy_leaves_tick_empty },
			{ "feeder_starved", test_feeder_starved_marks_late },
			{ "lost_report_dropped", test_lost_report_dropped },
			{ "wait_in_callback_refused", test_wait_in_callback_refused },
			{ "event_overflow_summarised", test_event_overflow_summarised },
			{ "destroy_cancels_waiters", test_destroy_cancels_waiters },
			{ "get_time_races_destroy", test_get_time_races_destroy },
			{ "context_destroy_cancels_waiters", test_context_destroy_cancels_waiters },
			{ "acked_record_kept_on_time_read_failure",
			  test_acked_record_kept_on_time_read_failure },
		};

		for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
			if (!strcmp(argv[2], t[i].name)) {
				t[i].fn();
				return 0;
			}
		return 2;
	}
	if (strcmp(mode, "all")) {
		fprintf(stderr, "usage: %s all | ad936x | destroy | one <test>\n", argv[0]);
		return 2;
	}
	test_one_scheduler_per_device();
	test_destroy_leaves_tick_empty();
	test_builder_sorts();
	test_commit_guard_margin_late();
	test_commit_streams_large_batch();
	test_feeder_starved_marks_late();
	test_report_spacing_refused();
	test_wait_unreported_einval();
	test_poller_lag_marks_late();
	test_slow_ack_marks_late();
	test_lost_report_dropped();
	test_wait_in_callback_refused();
	test_event_overflow_summarised();
	test_events_late_and_overflow();
	test_reserved_gpio_bit(true);
	test_flush_completes_flushed();
	test_acked_record_kept_on_time_read_failure();
	test_destroy_while_polling();
	test_destroy_cancels_waiters();
	test_context_destroy_cancels_waiters();
	test_get_time_races_destroy();
	printf("test_sched: all ok\n");
	return 0;
}
