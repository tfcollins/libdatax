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

/* Tick ops the next fx_open binds (a test swaps in a slow link). */
static const struct datax_tick_ops *g_ops = &fake_tick_ops;

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
	CHECK(datax_internal_tick_bind_fake(x->dev, g_ops, x->f) == 0);
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
	WAIT_FOR(has_event(&x, "late PPS edge: device time jumped to epoch; 0 queued"));
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


/* ---- a slow link: every access costs real time ------------------------------- */

#define LINK_MS 20

static int slow_read(void *h, const char *attr, char *buf, size_t len)
{
	msleep(LINK_MS);
	return fake_tick_read(h, attr, buf, len);
}

static int slow_write(void *h, const char *attr, const char *val)
{
	msleep(LINK_MS);
	return fake_tick_write(h, attr, val);
}

static const struct datax_tick_ops slow_ops = { slow_read, slow_write };

static unsigned long long wall_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000 + (unsigned long long)t.tv_nsec / 1000000;
}

/* The 2 s wait is one deadline, not a count of iterations: over a link where
 * each access takes LINK_MS it still ends at about 2 s. */
static void test_pps_wait_is_two_seconds_wall(void)
{
	struct fx x;
	unsigned long long t0, dt;

	g_ops = &slow_ops;
	fx_open(&x, false);
	fake_tick_pps_edge(x.f);
	t0 = wall_ms();
	CHECK(datax_device_set_time_next_pps(x.dev, 5ULL * HZ) == 0);
	dt = wall_ms() - t0;
	CHECK(dt >= 1990);
	CHECK(dt <= 2000 + 6 * LINK_MS);      /* 2 s plus a few accesses */
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	fx_close(&x);
	g_ops = &fake_tick_ops;
	printf("test_pps_wait_is_two_seconds_wall: ok\n");
}

static unsigned long long fake_u(struct fake_tick *f, const char *attr)
{
	char b[64];

	CHECK(fake_tick_read(f, attr, b, sizeof(b)) > 0);
	return strtoull(b, NULL, 0);
}

/* After a timed-out hard alignment (soft fallback, Tick armed) a command is
 * written; then the edge lands and the counter jumps: it is dropped, Tick's
 * queue is emptied and an event says so. */
static void test_late_edge_drops_written(void)
{
	struct fx x;
	struct datax_completion c;
	struct fake_tick_exec rec[2];
	/* the epoch is behind the counter: the jump leaves the command in the future */
	unsigned long long e1 = 500 * MS;
	long seq;

	fx_open(&x, true);
	fake_tick_pps_edge(x.f);
	CHECK(datax_device_set_time_next_pps(x.dev, e1) == 0);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	seq = datax_sched_queue_gpio(x.s, e1 + 800 * MS, 0xe, 0x2);
	CHECK(seq > 0);
	CHECK(datax_sched_report(x.s, seq) == 0);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(fake_u(x.f, "queue_level") == 1);     /* written under the soft offset */
	fake_tick_advance(x.f, 50 * MS);
	fake_tick_pps_edge(x.f);                        /* the armed edge, late */
	memset(&c, 0, sizeof(c));
	CHECK(datax_sched_wait(x.s, seq, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_DROPPED);
	CHECK(strstr(lasterr(&x), "device time jumped on a late PPS edge") != NULL);
	WAIT_FOR(has_event(&x, "late PPS edge: device time jumped to epoch; 1 queued commands dropped"));
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	CHECK(fake_u(x.f, "queue_level") == 0);
	CHECK(fake_tick_now(x.f) == e1);
	CHECK(fake_tick_exec_log(x.f, 0, rec, 2) == 0);   /* it never ran */
	fx_close(&x);
	printf("test_late_edge_drops_written: ok\n");
}

/* A committed but unwritten command is re-based when the counter jumps, so it
 * runs at its API time on the new clock. The scheduler is paused (no feeding,
 * no polling); get_time adopts the edge. */
static void test_late_edge_rebases_pending(void)
{
	struct fx x;
	struct fake_tick_exec rec[2];
	struct datax_completion c;
	unsigned long long e1 = 9ULL * HZ, due = e1 + 1000 * MS;
	long seq;

	fx_open(&x, true);
	fake_tick_pps_edge(x.f);
	CHECK(datax_device_set_time_next_pps(x.dev, e1) == 0);   /* soft, offset > 0 */
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	seq = datax_sched_queue_gpio(x.s, due, 0xe, 0x2);
	CHECK(seq > 0);
	CHECK(datax_sched_report(x.s, seq) == 0);
	CHECK(datax_sched_commit(x.s) == 0);
	fake_tick_advance(x.f, 50 * MS);
	fake_tick_pps_edge(x.f);
	CHECK(api_time(&x) == e1);                 /* adopted by get_time */
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	for (int k = 0; k < 30; k++) {
		fake_tick_advance(x.f, 50 * MS);
		msleep(3);
	}
	memset(&c, 0, sizeof(c));
	CHECK(datax_sched_wait(x.s, seq, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_OK);
	CHECK(fake_tick_exec_log(x.f, 0, rec, 2) == 1);
	CHECK(rec[0].requested_ticks == due);      /* hardware time == API time now */
	fx_close(&x);
	printf("test_late_edge_rebases_pending: ok\n");
}

/* A second call while still armed rewrites the epoch: the latest wins. */
static void test_second_epoch_wins(void)
{
	struct fx x;
	struct align_arg a = { 0 };
	unsigned long long e1 = 9ULL * HZ, e2 = 77ULL * HZ;
	unsigned long armed;
	pthread_t th;

	fx_open(&x, true);
	fake_tick_pps_edge(x.f);
	CHECK(datax_device_set_time_next_pps(x.dev, e1) == 0);   /* times out, armed */
	armed = fake_tick_access_count(x.f, "arm_time_next_pps", true);
	a.dev = x.dev;
	a.epoch = e2;
	CHECK(pthread_create(&th, NULL, align_main, &a) == 0);
	WAIT_FOR(fake_tick_access_count(x.f, "arm_time_next_pps", true) > armed);
	fake_tick_pps_edge(x.f);
	CHECK(pthread_join(th, NULL) == 0);
	CHECK(a.ret == 0);
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	CHECK(fake_tick_now(x.f) == e2);
	CHECK(api_time(&x) == e2);
	fx_close(&x);
	printf("test_second_epoch_wins: ok\n");
}

/* No scheduler, so nothing polls: get_time notices the landed edge. */
static void test_late_edge_adopted_by_get_time(void)
{
	struct fx x;
	unsigned long long e1 = 9ULL * HZ;

	fx_open(&x, false);
	fake_tick_pps_edge(x.f);
	CHECK(datax_device_set_time_next_pps(x.dev, e1) == 0);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	fake_tick_advance(x.f, 50 * MS);
	fake_tick_pps_edge(x.f);
	CHECK(api_time(&x) == e1);
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	CHECK(has_event(&x, "late PPS edge: device time jumped to epoch; 0 queued"));
	fx_close(&x);
	printf("test_late_edge_adopted_by_get_time: ok\n");
}

/* Tick ops that log each descriptor's timestamp and deliver a PPS edge
 * just before forwarding descriptor number fire_at (1-based; 0 never). */
static struct {
	pthread_mutex_t m;
	unsigned int fire_at, n;
	unsigned long long ts[8];
} g_desc = { PTHREAD_MUTEX_INITIALIZER, 0, 0, { 0 } };

static unsigned long long csv_u32(const char *csv, int idx)
{
	const char *p = csv;

	for (int i = 0; i < idx; i++) {
		p = strchr(p, ',');
		CHECK(p != NULL);
		p++;
	}
	return strtoull(p, NULL, 0);
}

static int desc_read(void *h, const char *attr, char *buf, size_t len)
{
	return fake_tick_read(h, attr, buf, len);
}

static int desc_write(void *h, const char *attr, const char *val)
{
	bool fire = false;

	if (!strcmp(attr, "command_descriptor")) {
		pthread_mutex_lock(&g_desc.m);
		if (g_desc.n < 8)
			g_desc.ts[g_desc.n] = csv_u32(val, 2) | csv_u32(val, 3) << 32;
		fire = ++g_desc.n == g_desc.fire_at;
		pthread_mutex_unlock(&g_desc.m);
	}
	if (fire)
		fake_tick_pps_edge(h);     /* lands after the check, before the write */
	return fake_tick_write(h, attr, val);
}

static const struct datax_tick_ops desc_ops = { desc_read, desc_write };

static unsigned int desc_count(void)
{
	unsigned int n;

	pthread_mutex_lock(&g_desc.m);
	n = g_desc.n;
	pthread_mutex_unlock(&g_desc.m);
	return n;
}

/* Carried from review: the poller checks for a late armed PPS edge before
 * every descriptor write, so once the counter has jumped no further
 * descriptor is written under the old offset. The edge lands inside the
 * first write (the documented residual window): that command is dropped,
 * and the second is written at its API time on the new clock. */
static void test_late_edge_checked_before_each_write(void)
{
	struct fx x;
	struct datax_completion c;
	unsigned long long e1 = 500 * MS, d1 = e1 + 800 * MS, d2 = e1 + 900 * MS;
	long s1, s2;

	g_ops = &desc_ops;
	fx_open(&x, true);
	g_ops = &fake_tick_ops;
	fake_tick_pps_edge(x.f);
	CHECK(datax_device_set_time_next_pps(x.dev, e1) == 0);   /* soft, still armed */
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	pthread_mutex_lock(&g_desc.m);
	g_desc.n = 0;
	g_desc.fire_at = 1;
	pthread_mutex_unlock(&g_desc.m);
	s1 = datax_sched_queue_gpio(x.s, d1, 0xe, 0x2);
	s2 = datax_sched_queue_gpio(x.s, d2, 0xe, 0x4);
	CHECK(s1 > 0 && s2 > 0);
	CHECK(datax_sched_report(x.s, s1) == 0 && datax_sched_report(x.s, s2) == 0);
	CHECK(datax_sched_commit(x.s) == 0);
	WAIT_FOR(desc_count() >= 2);
	msleep(5);
	CHECK(desc_count() == 2);
	CHECK(g_desc.ts[0] != d1);              /* written under the soft offset */
	CHECK(g_desc.ts[1] == d2);              /* the edge was adopted first */
	memset(&c, 0, sizeof(c));
	CHECK(datax_sched_wait(x.s, s1, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_DROPPED);
	CHECK(source(&x) == DATAX_TIME_PPS_ALIGNED);
	for (int k = 0; k < 20 && fake_tick_now(x.f) <= d2; k++) {
		fake_tick_advance(x.f, 50 * MS);
		msleep(3);
	}
	memset(&c, 0, sizeof(c));
	CHECK(datax_sched_wait(x.s, s2, 3000, &c) == 0);
	CHECK(c.status == DATAX_COMPLETION_OK);
	CHECK(c.executed_ticks == d2);
	fx_close(&x);
	printf("test_late_edge_checked_before_each_write: ok\n");
}

/* A past API time must not wrap into the far future once the soft offset
 * (here positive) is subtracted. */
static void test_past_api_time_rejected(void)
{
	struct fx x;
	long seq;

	fx_open(&x, true);
	CHECK(datax_device_set_time_next_pps(x.dev, 500ULL * HZ) == 0);
	seq = datax_sched_queue_gpio(x.s, 100ULL * HZ, 0xe, 0x2);   /* before 500 s */
	CHECK(seq > 0);
	CHECK(datax_sched_commit(x.s) == DATAX_ERR_LATE);
	CHECK(fake_u(x.f, "queue_level") == 0);
	fx_close(&x);
	printf("test_past_api_time_rejected: ok\n");
}

/* A failed epoch/arm write is reported as such, not as "no PPS". */
static void test_arm_write_failure_labelled(void)
{
	struct fx x;

	fx_open(&x, true);
	fake_tick_pps_edge(x.f);
	fake_tick_inject_error(x.f, "arm_time_next_pps", -EIO);
	CHECK(datax_device_set_time_next_pps(x.dev, 3ULL * HZ) == 0);
	CHECK(source(&x) == DATAX_TIME_SOFT_ALIGNED);
	WAIT_FOR(has_event(&x, "arming the PPS alignment failed"));
	CHECK(!has_event(&x, "no PPS:"));
	fx_close(&x);
	printf("test_arm_write_failure_labelled: ok\n");
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

int main(int argc, char **argv)
{
	static const struct { const char *name; void (*fn)(void); } t[] = {
		{ "no_tick", test_no_tick },
		{ "free_run_default", test_free_run_default },
		{ "pps_hard_align", test_pps_hard_align },
		{ "pps_timeout_soft_fallback", test_pps_timeout_soft_fallback },
		{ "soft_offset_round_trips", test_soft_offset_round_trips },
		{ "busy_with_queued_commands", test_busy_with_queued_commands },
		{ "pps_edges_stop_no_holdover", test_pps_edges_stop_no_holdover },
		{ "pps_appears_no_jump", test_pps_appears_no_jump },
		{ "pps_wait_is_two_seconds_wall", test_pps_wait_is_two_seconds_wall },
		{ "late_edge_drops_written", test_late_edge_drops_written },
		{ "late_edge_rebases_pending", test_late_edge_rebases_pending },
		{ "second_epoch_wins", test_second_epoch_wins },
		{ "late_edge_adopted_by_get_time", test_late_edge_adopted_by_get_time },
		{ "past_api_time_rejected", test_past_api_time_rejected },
		{ "arm_write_failure_labelled", test_arm_write_failure_labelled },
		{ "late_edge_checked_before_each_write",
		  test_late_edge_checked_before_each_write },
	};

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (argc > 2 && !strcmp(argv[1], "one") && strcmp(argv[2], t[i].name))
			continue;
		t[i].fn();
	}
	printf("test_devtime: all ok\n");
	return 0;
}
