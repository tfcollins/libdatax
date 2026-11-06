// SPDX-License-Identifier: MIT
/*
 * The scheduler's internal hook contract (src/scheduler.h), which Task 4's
 * tunes build on. Linked against the library sources directly (the hooks
 * and datax_sched_enqueue are not exported), with the fake Tick bound to an
 * emulator device without Tick.
 *
 *   - destroy finishes every outstanding command FLUSHED through its hook;
 *   - a hook running on flush's or destroy's caller (this thread holds the
 *     Tick I/O lock) gets -EDEADLK from wait/commit/flush instead of
 *     deadlocking, pause just sets the flag, and datax_device_get_time
 *     works;
 *   - the same from the poll hook, on the poller thread.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "fake_tick.h"
#include "scheduler.h"
#include "tick_descriptor.h"

#define HZ   100000000ULL
#define MS   (HZ / 1000)
#define COST (100 * HZ / 1000000)      /* spacing 2700 us at create */

struct call {
	long seq;
	enum datax_completion_status status;
	char detail[DATAX_SCHED_DETAIL_LEN];
	pthread_t thread;
	int r_wait, r_commit, r_flush, r_pause, r_time, r_stage;
};

struct rec {
	pthread_mutex_t m;
	struct call c[32];
	size_t n;
	bool probe;            /* hooks try the blocking calls */
	long other_seq;        /* what a probing hook waits for */
	struct datax_sched *s;
	struct datax_device *dev;
};

static void probe_calls(struct rec *r, struct call *c)
{
	struct datax_completion out;
	unsigned long long ticks = 0, hz = 0;

	c->r_wait = datax_sched_wait(r->s, r->other_seq, -1, &out);
	c->r_commit = datax_sched_commit(r->s);
	c->r_flush = datax_sched_flush(r->s);
	c->r_pause = datax_internal_sched_pause(r->s, true);
	c->r_time = datax_device_get_time(r->dev, &ticks, &hz);
	{
		unsigned long long f = 2450000000ULL;

		c->r_stage = datax_device_stage_hop_table(r->dev, &f, 1);
	}
}

static void on_complete(struct datax_sched *s, long seq, struct datax_sched_result *res,
			void *user)
{
	struct rec *r = user;
	struct call c;

	(void)s;
	memset(&c, 0, sizeof(c));
	c.seq = seq;
	c.status = res->status;
	snprintf(c.detail, sizeof(c.detail), "%s", res->detail);
	c.thread = pthread_self();
	if (r->probe)
		probe_calls(r, &c);
	pthread_mutex_lock(&r->m);
	if (r->n < sizeof(r->c) / sizeof(r->c[0]))
		r->c[r->n++] = c;
	pthread_mutex_unlock(&r->m);
}

static const struct call *find_call(struct rec *r, long seq)
{
	for (size_t i = 0; i < r->n; i++)
		if (r->c[i].seq == seq)
			return &r->c[i];
	return NULL;
}

struct fx {
	struct datax_context *ctx;
	struct datax_device *dev;
	struct fake_tick *f;
	struct datax_sched *s;
	struct rec r;
};

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static unsigned long long fake_attr(struct fake_tick *f, const char *attr)
{
	char b[64];

	CHECK(fake_tick_read(f, attr, b, sizeof(b)) > 0);
	return strtoull(b, NULL, 0);
}

#define WAIT_FOR(cond)                                                       \
	do {                                                                 \
		int w_ = 0;                                                  \
		while (!(cond) && w_++ < 3000)                               \
			msleep(1);                                           \
		CHECK(cond);                                                 \
	} while (0)

static void fx_open(struct fx *x)
{
	const char *uri = getenv("DATAX_TEST_URI");

	memset(x, 0, sizeof(*x));
	CHECK(uri != NULL);
	CHECK(pthread_mutex_init(&x->r.m, NULL) == 0);
	x->f = fake_tick_new(NULL);
	CHECK(x->f != NULL);
	CHECK(datax_context_create_from_uri(uri, &x->ctx) == 0);
	CHECK(datax_context_get_device(x->ctx, 0, &x->dev) == 0);
	CHECK(datax_internal_tick_bind_fake(x->dev, &fake_tick_ops, x->f) == 0);
	fake_tick_advance(x->f, 1000 * MS);
	fake_tick_set_access_cost(x->f, COST);
	CHECK(datax_sched_create(x->dev, &x->s) == 0);
	fake_tick_set_access_cost(x->f, 0);
	x->r.s = x->s;
	x->r.dev = x->dev;
}

static void fx_close(struct fx *x)
{
	datax_sched_destroy(x->s);
	datax_context_destroy(x->ctx);
	fake_tick_free(x->f);
	pthread_mutex_destroy(&x->r.m);
}

static long enq(struct fx *x, unsigned long long when, bool report)
{
	struct datax_sched_cmd_spec spec = {
		.when = when, .command_id = DATAX_TICK_COMMAND_ID_GPIO_WRITE,
		.target_id = DATAX_TICK_TARGET_FAMILY_GPIO,
		.args = { 0x2, 0x2, 0, 0 }, .report = report,
		.on_complete = on_complete, .user = &x->r,
	};
	long seq = datax_sched_enqueue(x->s, &spec);

	CHECK(seq > 0);
	return seq;
}

static void test_destroy_finishes_hooks(void)
{
	struct fx x;
	unsigned long long t;
	long s[4];

	fx_open(&x);
	t = fake_tick_now(x.f);
	s[0] = enq(&x, t + 50 * MS, true);
	s[1] = enq(&x, t + 60 * MS, true);
	s[2] = enq(&x, t + 70 * MS, false);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(fake_attr(x.f, "queue_level") == 3);   /* written */
	s[3] = enq(&x, t + 80 * MS, true);              /* uncommitted */
	x.r.probe = true;                               /* hooks on destroy's caller */
	x.r.other_seq = s[0];
	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(x.r.n == 4);
	for (int i = 0; i < 4; i++) {
		const struct call *c = find_call(&x.r, s[i]);

		CHECK(c != NULL);
		CHECK(c->status == DATAX_COMPLETION_FLUSHED);
		CHECK(strstr(c->detail, "destroyed") != NULL);
		CHECK(pthread_equal(c->thread, pthread_self()));
		/* io is held by this thread: refuse, never deadlock */
		CHECK(c->r_wait == -EDEADLK || c->r_wait == -ECANCELED);
		CHECK(c->r_commit == -EDEADLK && c->r_flush == -EDEADLK);
		CHECK(c->r_pause == 0);          /* only sets the flag */
		CHECK(c->r_time == 0);
	}
	CHECK(fake_attr(x.f, "queue_level") == 0);
	fx_close(&x);
	printf("test_destroy_finishes_hooks: ok\n");
}

static void test_hook_on_flush_refuses_blocking(void)
{
	struct fx x;
	unsigned long long t;
	long a, b;
	const struct call *c;
	struct datax_completion out;

	fx_open(&x);
	t = fake_tick_now(x.f);
	a = enq(&x, t + 50 * MS, true);
	b = enq(&x, t + 60 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(fake_attr(x.f, "queue_level") == 2);
	x.r.probe = true;
	x.r.other_seq = b;
	CHECK(datax_sched_flush(x.s) == 0);             /* hooks on this thread */
	x.r.probe = false;
	c = find_call(&x.r, a);
	CHECK(c != NULL && c->status == DATAX_COMPLETION_FLUSHED);
	CHECK(pthread_equal(c->thread, pthread_self()));
	CHECK(c->r_wait == -EDEADLK && c->r_commit == -EDEADLK &&
	      c->r_flush == -EDEADLK);
	CHECK(c->r_pause == 0);                         /* only sets the flag */
	CHECK(c->r_time == 0);
	CHECK(c->r_stage == -EDEADLK);                  /* stage_hop_table takes io */
	CHECK(strstr(datax_context_get_last_error(x.ctx), "deadlock") != NULL ||
	      strstr(datax_context_get_last_error(x.ctx), "seq") != NULL);
	CHECK(datax_sched_wait(x.s, a, 0, &out) == 0 && out.status == DATAX_COMPLETION_FLUSHED);
	CHECK(datax_sched_wait(x.s, b, 0, &out) == 0 && out.status == DATAX_COMPLETION_FLUSHED);
	/* the hook's pause took effect; resume and the scheduler feeds again */
	t = fake_tick_now(x.f);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	a = enq(&x, t + 20 * MS, true);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(fake_attr(x.f, "queue_level") == 1);
	fx_close(&x);
	printf("test_hook_on_flush_refuses_blocking: ok\n");
}

/* The poll hook runs on the poller thread with io held. */
struct poll_rec {
	struct datax_device *dev;
	struct datax_sched *s;
	unsigned long calls;
	int r_time, r_wait, r_commit;
	pthread_mutex_t m;
};

static void on_poll(struct datax_sched *s, unsigned long long hw_now, void *user)
{
	struct poll_rec *p = user;
	unsigned long long ticks = 0, hz = 0;
	struct datax_completion out;
	int rt = datax_device_get_time(p->dev, &ticks, &hz);
	int rw = datax_sched_wait(s, 1, -1, &out);
	int rc = datax_sched_commit(s);

	(void)hw_now;
	pthread_mutex_lock(&p->m);
	p->calls++;
	p->r_time = rt;
	p->r_wait = rw;
	p->r_commit = rc;
	pthread_mutex_unlock(&p->m);
}

static void test_poll_hook_contract(void)
{
	struct fx x;
	struct poll_rec p;
	unsigned long calls = 0;

	fx_open(&x);
	memset(&p, 0, sizeof(p));
	CHECK(pthread_mutex_init(&p.m, NULL) == 0);
	p.dev = x.dev;
	p.s = x.s;
	CHECK(datax_sched_set_poll_hook(x.s, on_poll, &p) == 0);
	(void)enq(&x, fake_tick_now(x.f) + 500 * MS, true);   /* keep it active */
	CHECK(datax_sched_commit(x.s) == 0);
	for (int i = 0; i < 3000 && calls < 3; i++) {
		pthread_mutex_lock(&p.m);
		calls = p.calls;
		pthread_mutex_unlock(&p.m);
		msleep(1);
	}
	CHECK(calls >= 3);
	CHECK(datax_sched_set_poll_hook(x.s, NULL, NULL) == 0);
	msleep(5);
	pthread_mutex_lock(&p.m);
	CHECK(p.r_time == 0 && p.r_wait == -EDEADLK && p.r_commit == -EDEADLK);
	pthread_mutex_unlock(&p.m);
	fx_close(&x);
	pthread_mutex_destroy(&p.m);
	printf("test_poll_hook_contract: ok\n");
}

/* A reported command marked consume (timed set_frequency's tune: nobody
 * holds a handle to wait on it) runs its hook with the real record, then
 * leaves the table instead of staying DONE forever. */
static void test_consumed_result_not_kept(void)
{
	struct fx x;
	struct datax_completion out[4];
	struct datax_sched_cmd_spec spec = {
		.command_id = DATAX_TICK_COMMAND_ID_GPIO_WRITE,
		.target_id = DATAX_TICK_TARGET_FAMILY_GPIO,
		.args = { 0x2, 0x2, 0, 0 }, .report = true, .consume = true,
		.on_complete = on_complete,
	};
	unsigned long long t;
	const struct call *c = NULL;
	enum datax_completion_status st = DATAX_COMPLETION_DROPPED;
	size_t n = 99, left;
	long seq;

	fx_open(&x);
	spec.user = &x.r;
	t = fake_tick_now(x.f);
	spec.when = t + 20 * MS;
	seq = datax_sched_enqueue(x.s, &spec);
	CHECK(seq > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(fake_attr(x.f, "queue_level") == 1);
	fake_tick_advance(x.f, 30 * MS);
	for (int w = 0; w < 3000; w++) {
		pthread_mutex_lock(&x.r.m);
		c = find_call(&x.r, seq);
		if (c)
			st = c->status;
		pthread_mutex_unlock(&x.r.m);
		if (c)
			break;
		msleep(1);
	}
	CHECK(c != NULL);
	CHECK(st == DATAX_COMPLETION_OK);              /* the hook saw the record */
	msleep(5);
	CHECK(datax_sched_get_completions(x.s, out, 4, &n) == 0 && n == 0);
	pthread_mutex_lock(&x.s->lock);
	left = x.s->ncmds;
	pthread_mutex_unlock(&x.s->lock);
	CHECK(left == 0);
	CHECK(datax_sched_wait(x.s, seq, 0, out) == -EINVAL);
	fx_close(&x);
	printf("test_consumed_result_not_kept: ok\n");
}

/* Events queued (not yet delivered) whose detail contains text. */
static size_t queued_events(struct datax_sched *s, const char *text)
{
	size_t n = 0;

	pthread_mutex_lock(&s->lock);
	for (unsigned int i = 0; i < s->nevq; i++)
		n += strstr(s->evq[i].detail, text) != NULL;
	pthread_mutex_unlock(&s->lock);
	return n;
}

/* A consumed command flushed by the user raises "seq N: FLUSHED: ..."
 * (its result is never published); flushed while the scheduler is being
 * destroyed (stop set) it raises nothing: nobody asked, and the delivery
 * would race the teardown. */
static void test_consumed_flush_events(void)
{
	struct datax_sched_cmd_spec spec = {
		.command_id = DATAX_TICK_COMMAND_ID_GPIO_WRITE,
		.target_id = DATAX_TICK_TARGET_FAMILY_GPIO,
		.args = { 0x2, 0x2, 0, 0 }, .report = true, .consume = true,
	};
	struct fx x;

	fx_open(&x);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);   /* events stay queued */
	spec.when = fake_tick_now(x.f) + 50 * MS;
	CHECK(datax_sched_enqueue(x.s, &spec) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	CHECK(datax_sched_flush(x.s) == 0);
	CHECK(queued_events(x.s, "FLUSHED: flushed") == 1);
	/* the same, as destroy sees it */
	pthread_mutex_lock(&x.s->lock);
	x.s->nevq = 0;
	x.s->stop = true;
	pthread_mutex_unlock(&x.s->lock);
	spec.when = fake_tick_now(x.f) + 60 * MS;
	CHECK(datax_sched_enqueue(x.s, &spec) > 0);
	CHECK(datax_sched_flush(x.s) == 0);                  /* finishes the BUILD one */
	CHECK(queued_events(x.s, "FLUSHED") == 0);
	fx_close(&x);
	printf("test_consumed_flush_events: ok\n");
}

static unsigned long long wake_of(struct datax_sched *s)
{
	unsigned long long v;

	pthread_mutex_lock(&s->lock);
	v = s->wake_hw;
	pthread_mutex_unlock(&s->lock);
	return v;
}

/* A spent wake target (device time has reached it) is cleared by the next
 * iteration, and never blocks a later request; an earlier pending request
 * still wins over a later one. */
static void test_wake_target_spent(void)
{
	struct fx x;
	unsigned long long t, hw;

	fx_open(&x);
	t = fake_tick_now(x.f);
	(void)enq(&x, t + 500 * MS, false);          /* keeps the poller active */
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(fake_attr(x.f, "queue_level") == 1);
	pthread_mutex_lock(&x.s->lock);
	hw = x.s->last_hw;
	pthread_mutex_unlock(&x.s->lock);
	/* spent before any iteration: a later request replaces it */
	datax_sched_wake_at(x.s, hw - 10);
	datax_sched_wake_at(x.s, t + 300 * MS);
	CHECK(wake_of(x.s) == t + 300 * MS);
	datax_sched_wake_at(x.s, t + 200 * MS);       /* earlier pending wins */
	CHECK(wake_of(x.s) == t + 200 * MS);
	datax_sched_wake_at(x.s, t + 250 * MS);
	CHECK(wake_of(x.s) == t + 200 * MS);
	/* reached: the next iteration clears it */
	fake_tick_advance(x.f, 210 * MS);
	WAIT_FOR(wake_of(x.s) == 0);
	fx_close(&x);
	printf("test_wake_target_spent: ok\n");
}

int main(void)
{
	test_destroy_finishes_hooks();
	test_hook_on_flush_refuses_blocking();
	test_poll_hook_contract();
	test_consumed_result_not_kept();
	test_consumed_flush_events();
	test_wake_target_spent();
	printf("test_sched_hooks: all ok\n");
	return 0;
}
