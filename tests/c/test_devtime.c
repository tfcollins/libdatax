// SPDX-License-Identifier: MIT
/*
 * Device time, PPS alignment and the internal-clock fallback against the
 * fake Tick (spec decision 9). The fake is bound straight to a live
 * emulator device (the fixture has no Tick); device time moves only when
 * the test advances the fake.
 *
 * What stays out: PPS loss. The current Tick latches pps_present until the
 * core is reset (tfcollins/tick#39), so a loss cannot be observed and the
 * source never becomes DATAX_TIME_PPS_HOLDOVER; the lab-side check belongs
 * to the hardware tests once Tick reports a live PPS.
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

#define HZ 100000000ULL
#define MS (HZ / 1000)

struct ev {
	pthread_mutex_t m;
	struct { enum datax_event_type type; unsigned long long ticks; char detail[160]; } e[64];
	size_t n;
};

static void on_event(const struct datax_event *evt, void *user)
{
	struct ev *r = user;

	pthread_mutex_lock(&r->m);
	if (r->n < sizeof(r->e) / sizeof(r->e[0])) {
		r->e[r->n].type = evt->type;
		r->e[r->n].ticks = evt->ticks;
		snprintf(r->e[r->n].detail, sizeof(r->e[r->n].detail), "%s", evt->detail);
		r->n++;
	}
	pthread_mutex_unlock(&r->m);
}

struct fx {
	struct datax_context *ctx;
	struct datax_device *dev;
	struct fake_tick *f;
	struct datax_sched *s;
	struct ev ev;
};

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

#define WAIT_FOR(cond)                                           \
	do {                                                     \
		int w_ = 0;                                      \
		while (!(cond) && w_++ < 3000)                   \
			msleep(1);                               \
		CHECK(cond);                                     \
	} while (0)

static void fx_open(struct fx *x, bool sched)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct fake_tick_config cfg = { .tick_hz = HZ };

	memset(x, 0, sizeof(*x));
	CHECK(uri != NULL);
	CHECK(pthread_mutex_init(&x->ev.m, NULL) == 0);
	x->f = fake_tick_new(&cfg);
	CHECK(x->f != NULL);
	CHECK(datax_context_create_from_uri(uri, &x->ctx) == 0);
	CHECK(datax_context_get_device(x->ctx, 0, &x->dev) == 0);
	CHECK(datax_internal_tick_bind_fake(x->dev, &fake_tick_ops, x->f) == 0);
	CHECK(datax_context_set_event_callback(x->ctx, on_event, &x->ev) == 0);
	fake_tick_advance(x->f, 1000 * MS);
	if (sched)
		CHECK(datax_sched_create(x->dev, &x->s) == 0);
}

static void fx_close(struct fx *x)
{
	datax_sched_destroy(x->s);
	datax_context_destroy(x->ctx);
	fake_tick_free(x->f);
	pthread_mutex_destroy(&x->ev.m);
}

static bool has_event(struct fx *x, const char *text)
{
	bool hit = false;

	pthread_mutex_lock(&x->ev.m);
	for (size_t i = 0; i < x->ev.n; i++)
		hit = hit || (x->ev.e[i].type == DATAX_EVT_PPS_LOCK_CHANGE &&
			      strstr(x->ev.e[i].detail, text));
	pthread_mutex_unlock(&x->ev.m);
	return hit;
}

static size_t nevents(struct fx *x)
{
	size_t n;

	pthread_mutex_lock(&x->ev.m);
	n = x->ev.n;
	pthread_mutex_unlock(&x->ev.m);
	return n;
}

static unsigned long long api_time(struct fx *x)
{
	unsigned long long t, hz;

	CHECK(datax_device_get_time(x->dev, &t, &hz) == 0);
	CHECK(hz == HZ);
	return t;
}

static enum datax_time_source source(struct fx *x)
{
	enum datax_time_source src;

	CHECK(datax_device_get_time_source(x->dev, &src) == 0);
	return src;
}

static const char *lasterr(struct fx *x)
{
	return datax_context_get_last_error(x->ctx);
}

/* set_time_next_pps on its own thread, so the test can pulse the PPS. */
struct align_arg {
	struct datax_device *dev;
	unsigned long long epoch;
	int ret;
};

static void *align_main(void *arg)
{
	struct align_arg *a = arg;

	a->ret = datax_device_set_time_next_pps(a->dev, a->epoch);
	return NULL;
}

/* Run a hard alignment: wait for the call to arm Tick, then deliver the edge. */
static void hard_align(struct fx *x, unsigned long long epoch)
{
	struct align_arg a = { x->dev, epoch, 1 };
	unsigned long armed = fake_tick_access_count(x->f, "arm_time_next_pps", true);
	pthread_t th;

	CHECK(pthread_create(&th, NULL, align_main, &a) == 0);
	WAIT_FOR(fake_tick_access_count(x->f, "arm_time_next_pps", true) > armed);
	fake_tick_pps_edge(x->f);
	CHECK(pthread_join(th, NULL) == 0);
	CHECK(a.ret == 0);
}

static void test_free_run_default(void)
{
	struct fx x;
	unsigned long long t0;

	fx_open(&x, false);
	CHECK(source(&x) == DATAX_TIME_FREE_RUN);
	t0 = api_time(&x);
	CHECK(t0 == fake_tick_now(x.f));
	fake_tick_advance(x.f, 5 * MS);
	CHECK(api_time(&x) == t0 + 5 * MS);
	CHECK(source(&x) == DATAX_TIME_FREE_RUN);
	CHECK(datax_device_set_time_next_pps(NULL, 0) == -EINVAL);
	fx_close(&x);
	printf("test_free_run_default: ok\n");
}

static void test_pps_hard_align(void)
{
	struct fx x;
	unsigned long long epoch = 42ULL * HZ, now;

	fx_open(&x, true);
	/* A soft alignment first (no PPS yet); the hard one must clear it. */
	CHECK(datax_device_set_time_next_pps(x.dev, 7ULL * HZ) == 0);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	CHECK(api_time(&x) == 7ULL * HZ);
	fake_tick_pps_edge(x.f);            /* PPS now present (and sticky) */
	hard_align(&x, epoch);
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	CHECK(fake_tick_now(x.f) == epoch);        /* Tick itself holds the epoch */
	now = api_time(&x);
	CHECK(now == epoch);                       /* no soft offset left */
	fake_tick_advance(x.f, 3 * MS);
	CHECK(api_time(&x) == epoch + 3 * MS);
	WAIT_FOR(has_event(&x, "aligned on a fabric PPS edge"));
	fx_close(&x);
	printf("test_pps_hard_align: ok\n");
}

static void test_pps_timeout_soft_fallback(void)
{
	struct fx x;
	unsigned long long epoch = 9ULL * HZ, t;
	struct timespec a, b;

	/* (1) no PPS at all: soft at once */
	fx_open(&x, true);
	CHECK(source(&x) == DATAX_TIME_FREE_RUN);
	CHECK(datax_device_set_time_next_pps(x.dev, epoch) == 0);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	CHECK(api_time(&x) == epoch);                  /* reads epoch at the call */
	fake_tick_advance(x.f, 2 * MS);
	CHECK(api_time(&x) == epoch + 2 * MS);
	CHECK(fake_tick_access_count(x.f, "arm_time_next_pps", true) == 0);
	WAIT_FOR(has_event(&x, "no PPS: aligned to the host clock (\xc2\xb1"));
	WAIT_FOR(has_event(&x, " \xc2\xb5s)"));
	fx_close(&x);

	/* (2) PPS present but no edge: waits 2 s, then falls back */
	fx_open(&x, true);
	fake_tick_pps_edge(x.f);
	clock_gettime(CLOCK_MONOTONIC, &a);
	CHECK(datax_device_set_time_next_pps(x.dev, epoch) == 0);
	clock_gettime(CLOCK_MONOTONIC, &b);
	t = (unsigned long long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
	CHECK(t >= 1900 && t < 3500);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	CHECK(api_time(&x) == epoch);
	WAIT_FOR(has_event(&x, "aligned to the host clock"));

	/* The armed edge lands late: Tick loads the epoch, so the source is
	 * hard-aligned and the soft offset is dropped. */
	fake_tick_advance(x.f, 50 * MS);
	fake_tick_pps_edge(x.f);
	WAIT_FOR(source(&x) == DATAX_TIME_PPS_ALIGNED);
	CHECK(api_time(&x) == epoch);
	CHECK(fake_tick_now(x.f) == epoch);
	WAIT_FOR(has_event(&x, "late fabric PPS edge"));
	fx_close(&x);
	printf("test_pps_timeout_soft_fallback: ok\n");
}

/* Timed commands see hardware time; the API sees offset time. Two epochs:
 * one ahead of the hardware clock and one behind it (a negative offset). */
static void test_soft_offset_round_trips(void)
{
	unsigned long long epochs[2] = { 500ULL * HZ, 100 };

	for (int i = 0; i < 2; i++) {
		struct fx x;
		struct fake_tick_exec rec[4];
		struct datax_completion c;
		unsigned long long hw0, due;
		long seq;

		fx_open(&x, true);
		hw0 = fake_tick_now(x.f);
		CHECK(datax_device_set_time_next_pps(x.dev, epochs[i]) == 0);
		CHECK(api_time(&x) == epochs[i]);
		due = epochs[i] + 80 * MS;                 /* API time */
		seq = datax_sched_queue_gpio(x.s, due, 0xe, 0x2);
		CHECK(seq > 0);
		CHECK(datax_sched_report(x.s, seq) == 0);
		CHECK(datax_sched_commit(x.s) == 0);
		for (int k = 0; k < 20; k++) {
			fake_tick_advance(x.f, 5 * MS);
			msleep(3);
		}
		memset(&c, 0, sizeof(c));
		CHECK(datax_sched_wait(x.s, seq, 3000, &c) == 0);
		CHECK(c.status == DATAX_COMPLETION_OK);
		CHECK(c.requested_ticks == due);           /* echoed in API time */
		CHECK(c.executed_ticks == due);
		/* ground truth: the hardware ran it at due - offset */
		CHECK(fake_tick_exec_log(x.f, 0, rec, 4) == 1);
		CHECK(rec[0].requested_ticks == hw0 + 80 * MS);
		CHECK(rec[0].executed_ticks == hw0 + 80 * MS);
		fx_close(&x);
	}
	printf("test_soft_offset_round_trips: ok\n");
}

static void test_busy_with_queued_commands(void)
{
	struct fx x;
	struct align_arg a;
	pthread_t th;
	long seq;

	fx_open(&x, true);
	seq = datax_sched_queue_gpio(x.s, api_time(&x) + 500 * MS, 0xe, 0x2);
	CHECK(seq > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	CHECK(datax_device_set_time_next_pps(x.dev, 10ULL * HZ) == -EBUSY);
	CHECK(strstr(lasterr(&x), "scheduler has queued commands; flush first") != NULL);
	CHECK(source(&x) == DATAX_TIME_FREE_RUN);
	CHECK(api_time(&x) == fake_tick_now(x.f));
	/* the same with a live PPS: Tick is not touched */
	fake_tick_pps_edge(x.f);
	CHECK(datax_device_set_time_next_pps(x.dev, 10ULL * HZ) == -EBUSY);
	CHECK(fake_tick_access_count(x.f, "arm_time_next_pps", true) == 0);
	CHECK(datax_sched_flush(x.s) == 0);
	a.dev = x.dev;
	a.epoch = 10ULL * HZ;
	CHECK(pthread_create(&th, NULL, align_main, &a) == 0);
	WAIT_FOR(fake_tick_access_count(x.f, "arm_time_next_pps", true) == 1);
	fake_tick_pps_edge(x.f);
	CHECK(pthread_join(th, NULL) == 0);
	CHECK(a.ret == 0);
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	fx_close(&x);
	printf("test_busy_with_queued_commands: ok\n");
}

/* Losing the PPS edges after a hard alignment changes nothing: Tick latches
 * pps_present until reset, so loss is invisible (tfcollins/tick#39) and the
 * source is never PPS_HOLDOVER. */
static void test_pps_edges_stop_no_holdover(void)
{
	struct fx x;
	size_t n;

	fx_open(&x, true);
	fake_tick_pps_edge(x.f);
	hard_align(&x, 20ULL * HZ);
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	WAIT_FOR(has_event(&x, "aligned on a fabric PPS edge"));
	n = nevents(&x);
	for (int i = 0; i < 5; i++) {      /* edges, then silence */
		fake_tick_advance(x.f, 100 * MS);
		fake_tick_pps_edge(x.f);
	}
	fake_tick_advance(x.f, 3000 * MS);
	msleep(400);                       /* several watch periods */
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	CHECK(nevents(&x) == n);
	fx_close(&x);
	printf("test_pps_edges_stop_no_holdover: ok\n");
}

static void test_pps_appears_no_jump(void)
{
	struct fx x;
	unsigned long long before, hw;

	fx_open(&x, true);
	msleep(250);                       /* the watch has seen "no PPS" */
	CHECK(datax_device_set_time_next_pps(x.dev, 3ULL * HZ) == 0);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	before = api_time(&x);
	hw = fake_tick_now(x.f);
	fake_tick_pps_edge(x.f);           /* PPS appears; nothing was armed */
	WAIT_FOR(has_event(&x, "PPS appeared"));
	CHECK(fake_tick_now(x.f) == hw);
	CHECK(api_time(&x) == before);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	fx_close(&x);
	printf("test_pps_appears_no_jump: ok\n");
}

static void test_no_tick(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct datax_context *ctx;
	struct datax_device *dev;

	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
	CHECK(datax_device_set_time_next_pps(dev, 1) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(ctx), "no Tick scheduler") != NULL);
	datax_context_destroy(ctx);
	printf("test_no_tick: ok\n");
}

int main(void)
{
	test_no_tick();
	test_free_run_default();
	test_pps_hard_align();
	test_pps_timeout_soft_fallback();
	test_soft_offset_round_trips();
	test_busy_with_queued_commands();
	test_pps_edges_stop_no_holdover();
	test_pps_appears_no_jump();
	printf("test_devtime: all ok\n");
	return 0;
}
