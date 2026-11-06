// SPDX-License-Identifier: MIT
/*
 * Talise FHM timed tunes (Task 4) against the fake Tick and the fake Talise
 * FHM: hop staging, queue_tune, the one-behind pipeline with its priming
 * edge, PLL readback, stage-late detection, sparse reporting of dense hops
 * and the timed set_frequency path.
 *
 * The fake Tick is bound to the emulator's adrv9009 pair (4R4T, no Tick, no
 * FHM attributes); the fake FHM is bound through a shim that records every
 * staging write with the fake time it landed at, and can stall one write
 * (advancing the fake clock inside it, like a slow round trip). Device time
 * moves only when the test advances it, edge by edge, waiting for the
 * poller's staging writes in between, so the run is deterministic.
 *
 * Usage: test_fhm [one <name>]
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
#include "internal.h"      /* dev->single_chip_tuned (Task 5 reads it) */
#include "scheduler.h"     /* last_hw: queue_tune's notion of now */
#include "tick_descriptor.h"

static void usleep_(unsigned int us)
{
	struct timespec ts = { us / 1000000, (long)(us % 1000000) * 1000L };

	nanosleep(&ts, NULL);
}

#define HZ        100000000ULL
#define MS        (HZ / 1000)
#define US        (HZ / 1000000)
/* Access cost while the scheduler measures: spacing = 3 x 9 x 100 us. */
#define COST      (100 * US)
#define SPACING_US 2700ULL
/* FHM access cost while stage_hop_table measures: one stage write plus the
 * device-time read (free) = 50 us. */
#define STAGE_COST (50 * US)
#define STAGE_US  50ULL
/* Past an edge by more than libdatax's edge margin (100 us). */
#define PAST      (150 * US)

#define F_A 2410000000ULL
#define F_B 2420000000ULL
#define F_C 2430000000ULL
#define F_INIT FAKE_FHM_INIT_HZ

/* ---- FHM shim: records staging writes, can stall one ------------------------ */

struct fshim {
	struct fake_fhm *m;
	struct fake_tick *f;
	pthread_mutex_t mu;
	struct { unsigned long long hz, tick; } st[512];
	size_t n;
	unsigned long long stall_hz;    /* one-shot: stall the write staging this */
	unsigned long long stall_ticks;
	bool enable_reads_zero;         /* the driver swallowed the enable */
	unsigned int sleep_us;          /* real latency per access (repro mode) */
	unsigned int enable_writes[2];  /* writes of 0 and 1 */
};

static int fshim_read(void *h, const char *attr, char *buf, size_t len)
{
	struct fshim *sh = h;

	if (sh->sleep_us)
		usleep_(sh->sleep_us);
	if (sh->enable_reads_zero && !strcmp(attr, "frequency_hopping_mode_enable"))
		return snprintf(buf, len, "0\n");
	return fake_fhm_read(sh->m, attr, buf, len);
}

static int fshim_write(void *h, const char *attr, const char *val)
{
	struct fshim *sh = h;
	unsigned long long v = strtoull(val, NULL, 10), stall = 0;
	int ret;

	if (sh->sleep_us)
		usleep_(sh->sleep_us);
	if (!strcmp(attr, "frequency_hopping_mode_enable")) {
		pthread_mutex_lock(&sh->mu);
		sh->enable_writes[v ? 1 : 0]++;
		pthread_mutex_unlock(&sh->mu);
	}
	if (strcmp(attr, "frequency_hopping_mode"))
		return fake_fhm_write(sh->m, attr, val);
	pthread_mutex_lock(&sh->mu);
	if (sh->stall_ticks && sh->stall_hz == v) {
		stall = sh->stall_ticks;
		sh->stall_ticks = 0;
	}
	pthread_mutex_unlock(&sh->mu);
	if (stall)
		fake_tick_advance(sh->f, stall);   /* edges fire during the stall */
	ret = fake_fhm_write(sh->m, attr, val);
	if (ret)
		return ret;
	pthread_mutex_lock(&sh->mu);
	if (sh->n < sizeof(sh->st) / sizeof(sh->st[0])) {
		sh->st[sh->n].hz = v;
		sh->st[sh->n].tick = fake_tick_now(sh->f);
		sh->n++;
	}
	pthread_mutex_unlock(&sh->mu);
	return 0;
}

static const struct datax_tick_ops fshim_ops = { fshim_read, fshim_write };

/* Tick shim: forwards to the fake; can fail the Nth descriptor write. */
struct tshim {
	struct fake_tick *f;
	pthread_mutex_t mu;
	unsigned int ndesc, fail_at;     /* 1-based; 0 never */
	unsigned int sleep_us;           /* real latency per access (repro mode) */
	/* repro mode: the longest host-time gap between two Tick accesses (the
	 * poller makes several per period while it has work) */
	unsigned long long last_ns, max_gap_ns;
	unsigned int stall_at;           /* before forwarding this one ... */
	unsigned long long stall_to;     /* ... advance the clock to here */
	/* once this many descriptors were written (1-based; 0 never),
	 * queue_level reads depth - 1: the feeder thinks the queue is full */
	unsigned int full_after, depth;
};

static void tshim_gap(struct tshim *sh)
{
	struct timespec ts;
	unsigned long long now;

	if (!sh->sleep_us)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
	pthread_mutex_lock(&sh->mu);
	if (sh->last_ns && now - sh->last_ns > sh->max_gap_ns)
		sh->max_gap_ns = now - sh->last_ns;
	sh->last_ns = now;
	pthread_mutex_unlock(&sh->mu);
}

static int tshim_read(void *h, const char *attr, char *buf, size_t len)
{
	struct tshim *sh = h;

	tshim_gap(sh);
	if (sh->sleep_us)
		usleep_(sh->sleep_us);
	if (!strcmp(attr, "queue_level")) {
		bool full;

		pthread_mutex_lock(&sh->mu);
		full = sh->full_after && sh->ndesc >= sh->full_after;
		pthread_mutex_unlock(&sh->mu);
		if (full)
			return snprintf(buf, len, "%u\n", sh->depth - 1);
	}
	return fake_tick_read(sh->f, attr, buf, len);
}

static int tshim_write(void *h, const char *attr, const char *val)
{
	struct tshim *sh = h;
	bool fail = false;
	unsigned long long to = 0;

	tshim_gap(sh);
	if (sh->sleep_us)
		usleep_(sh->sleep_us);
	if (!strcmp(attr, "command_descriptor")) {
		pthread_mutex_lock(&sh->mu);
		fail = ++sh->ndesc == sh->fail_at;
		if (sh->ndesc == sh->stall_at)
			to = sh->stall_to;
		pthread_mutex_unlock(&sh->mu);
	}
	if (to > fake_tick_now(sh->f))
		fake_tick_advance(sh->f, to - fake_tick_now(sh->f));   /* a slow write */
	return fail ? -EIO : fake_tick_write(sh->f, attr, val);
}

static const struct datax_tick_ops tshim_ops = { tshim_read, tshim_write };

static size_t stages(struct fshim *sh)
{
	size_t n;

	pthread_mutex_lock(&sh->mu);
	n = sh->n;
	pthread_mutex_unlock(&sh->mu);
	return n;
}

static unsigned long long stage_hz(struct fshim *sh, size_t i)
{
	unsigned long long v;

	pthread_mutex_lock(&sh->mu);
	CHECK(i < sh->n);
	v = sh->st[i].hz;
	pthread_mutex_unlock(&sh->mu);
	return v;
}

/* ---- events ----------------------------------------------------------------- */

struct evrec {
	pthread_mutex_t m;
	struct { enum datax_event_type type; long seq; char detail[128]; } e[256];
	size_t n;
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
}

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

static size_t count_events(struct evrec *r, enum datax_event_type type)
{
	size_t n = 0;

	pthread_mutex_lock(&r->m);
	for (size_t i = 0; i < r->n; i++)
		n += r->e[i].type == type;
	pthread_mutex_unlock(&r->m);
	return n;
}

/* ---- fixture ---------------------------------------------------------------- */

struct fx {
	struct datax_context *ctx;
	struct datax_device *dev;
	struct fake_tick *f;
	struct fake_fhm *m;
	struct fshim sh;
	struct tshim tsh;
	struct datax_sched *s;
	struct evrec ev;
	unsigned long ndesc;   /* descriptors committed so far (rise + fall per edge) */
	/* Exec-log entries and descriptor writes before the tunes (stage_hop_table
	 * parks the trigger low with one immediate write). */
	size_t ebase;
	unsigned long wbase;
};

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

#define WAIT_FOR(cond)                                                       \
	do {                                                                 \
		int w_ = 0;                                                  \
		while (!(cond) && w_++ < 3000)                               \
			msleep(1);                                           \
		CHECK(cond);                                                 \
	} while (0)

/* Tick queue depth for the next fx_open (0: tron's 16). */
static unsigned int g_depth;

static void fx_open(struct fx *x, bool create)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct fake_tick_config cfg = { .tick_hz = HZ, .queue_depth = g_depth };

	memset(x, 0, sizeof(*x));
	CHECK(uri != NULL);
	CHECK(pthread_mutex_init(&x->sh.mu, NULL) == 0);
	CHECK(pthread_mutex_init(&x->tsh.mu, NULL) == 0);
	CHECK(pthread_mutex_init(&x->ev.m, NULL) == 0);
	x->f = fake_tick_new(&cfg);
	x->m = fake_fhm_new(0);
	CHECK(x->f && x->m);
	fake_fhm_attach(x->m, x->f);
	x->sh.m = x->m;
	x->sh.f = x->f;
	x->tsh.f = x->f;
	x->tsh.depth = g_depth ? g_depth : 16;
	CHECK(datax_context_create_from_uri(uri, &x->ctx) == 0);
	CHECK(datax_context_get_device(x->ctx, 0, &x->dev) == 0);
	CHECK(datax_internal_tick_bind_fake(x->dev, &tshim_ops, &x->tsh) == 0);
	CHECK(datax_internal_fhm_bind_fake(x->dev, &fshim_ops, &x->sh) == 0);
	CHECK(datax_context_set_event_callback(x->ctx, on_event, &x->ev) == 0);
	fake_tick_advance(x->f, 1000 * MS);
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
	fake_fhm_free(x->m);
	fake_tick_free(x->f);
	pthread_mutex_destroy(&x->sh.mu);
	pthread_mutex_destroy(&x->tsh.mu);
	pthread_mutex_destroy(&x->ev.m);
}

static const char *lasterr(struct fx *x)
{
	return datax_context_get_last_error(x->ctx);
}

/* Stage freqs with the measured stage cost; returns the staging writes the
 * measurement made. */
static size_t stage(struct fx *x, const unsigned long long *freqs, size_t n)
{
	fake_fhm_set_access_cost(x->m, STAGE_COST);
	CHECK(datax_device_stage_hop_table(x->dev, freqs, n) == 0);
	fake_fhm_set_access_cost(x->m, 0);
	x->ebase = fake_tick_exec_log(x->f, 0, NULL, 0);
	x->wbase = fake_tick_access_count(x->f, "command_descriptor", true);
	return stages(&x->sh);
}

/* Wait until the poller has written every committed descriptor that fits
 * (one queue entry spare) and drained the completion slot, so the test never
 * moves time past a command the feeder has not had a chance to write (it
 * would complete host-late) or past a reported command Tick holds behind an
 * unacked record (tick#38; the real clock does not wait for the poller). */
static void fed(struct fx *x)
{
	unsigned long want;

	for (int w = 0; w < 3000; w++) {
		size_t done = fake_tick_exec_log(x->f, 0, NULL, 0) - x->ebase;
		char b[32] = "1";

		want = done + 15 < x->ndesc ? (unsigned long)done + 15 : x->ndesc;
		(void)fake_tick_read(x->f, "completion_sequence_id", b, sizeof(b));
		if (fake_tick_access_count(x->f, "command_descriptor", true) - x->wbase >= want &&
		    !strtoull(b, NULL, 0))
			return;
		msleep(1);
	}
	CHECK(!"the poller did not feed the queue");
}

/* Advance the fake clock until the FHM has seen n committing edges (at
 * most limit ticks), then a little past the edge, and give the poller time. */
static void to_edge(struct fx *x, unsigned int n, unsigned long long limit)
{
	unsigned long long t0 = fake_tick_now(x->f);

	while (fake_fhm_edges(x->m) < n) {
		CHECK(fake_tick_now(x->f) - t0 < limit);
		fed(x);
		fake_tick_advance(x->f, 10 * US);
	}
	fake_tick_advance(x->f, PAST);
	msleep(3);
	fed(x);
}

static struct datax_completion wait_done(struct fx *x, long seq)
{
	struct datax_completion c;

	memset(&c, 0, sizeof(c));
	CHECK(datax_sched_wait(x->s, seq, 3000, &c) == 0);
	CHECK(c.seq == seq);
	return c;
}

static unsigned long long fake_attr_u(struct fake_tick *f, const char *attr)
{
	char b[64];

	CHECK(fake_tick_read(f, attr, b, sizeof(b)) > 0);
	return strtoull(b, NULL, 0);
}

static unsigned long long now(struct fx *x)
{
	return fake_tick_now(x->f);
}

/* Drive bit 0 high now, as a foreign Tick write would (descriptor packed by
 * the P1 packer, LATE_EXECUTE_NOW at timestamp 0: applied at the push). */
static void raise_bit0(struct fx *x)
{
	struct datax_tick_descriptor d = {
		.version = DATAX_TICK_DESC_VERSION, .size_bytes = DATAX_TICK_DESC_SIZE,
		.command_id = DATAX_TICK_COMMAND_ID_GPIO_WRITE, .target_id = 0,
		.timestamp_ticks = 0, .flags = DATAX_TICK_FLAG_LATE_EXECUTE_NOW,
		.sequence_id = 0x7ffffff0u, .args = { 0x1, 0x1, 0, 0 },
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
	CHECK(fake_tick_write(x->f, "command_descriptor", csv) == 0);
	CHECK(fake_tick_gpio(x->f) & 0x1);
}

/* ---- tests ------------------------------------------------------------------ */

static void test_stage_table_band_and_depth(void)
{
	struct fx x;
	const struct datax_caps *caps;
	unsigned long long v = 0, freqs[65], f = F_A;

	/* Without a Tick there are no timed hops to stage for. */
	{
		const char *uri = getenv("DATAX_TEST_URI");
		struct datax_context *ctx;
		struct datax_device *dev;

		CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
		CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 0);
		CHECK(datax_device_stage_hop_table(dev, &f, 1) == -ENOTSUP);
		CHECK(strstr(datax_context_get_last_error(ctx), "no Tick") != NULL);
		/* With a Tick but the real phy ops: the emulator's TRX LO has no
		 * FHM attributes, and the error names the one that failed. */
		{
			struct fake_tick *t = fake_tick_new(NULL);

			CHECK(datax_internal_tick_bind_fake(dev, &fake_tick_ops, t) == 0);
			CHECK(datax_device_stage_hop_table(dev, &f, 1) < 0);
			CHECK(strstr(datax_context_get_last_error(ctx),
				     "frequency_hopping_mode_enable") != NULL);
			datax_context_destroy(ctx);
			fake_tick_free(t);
		}
	}

	fx_open(&x, false);
	caps = datax_device_get_caps(x.dev);
	CHECK(datax_caps_get_limit(caps, DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 64);
	CHECK(datax_caps_get_limit(caps, DATAX_LIMIT_HOP_STAGE_US, &v) == -ENOTSUP);

	CHECK(datax_device_stage_hop_table(NULL, &f, 1) == -EINVAL);
	CHECK(datax_device_stage_hop_table(x.dev, NULL, 1) == -EINVAL);
	CHECK(datax_device_stage_hop_table(x.dev, &f, 0) == -EINVAL);
	/* the FHM band: driver default 2400-2500 MHz (hopcap talise.py) */
	f = 2399999999ULL;
	CHECK(datax_device_stage_hop_table(x.dev, &f, 1) == -EINVAL);
	CHECK(strstr(lasterr(&x), "outside the FHM band") != NULL);
	f = 2500000001ULL;
	CHECK(datax_device_stage_hop_table(x.dev, &f, 1) == -EINVAL);
	for (int i = 0; i < 65; i++)
		freqs[i] = 2400000000ULL + (unsigned long long)i * 1000000ULL;
	CHECK(datax_device_stage_hop_table(x.dev, freqs, 65) == -EINVAL);
	CHECK(strstr(lasterr(&x), "64") != NULL);
	CHECK(!fake_fhm_enabled(x.m));      /* nothing written on a refusal */
	CHECK(stages(&x.sh) == 0);

	/* 64 entries: accepted, FHM enabled on chip A, the stage round trip
	 * measured (median of 5 staging writes), less the device-time read
	 * (here 30 us per Tick access). */
	fake_fhm_set_access_cost(x.m, STAGE_COST);
	fake_tick_set_access_cost(x.f, 30 * US);
	CHECK(datax_device_stage_hop_table(x.dev, freqs, 64) == 0);
	fake_tick_set_access_cost(x.f, 0);
	fake_fhm_set_access_cost(x.m, 0);
	CHECK(fake_fhm_enabled(x.m));
	CHECK(stages(&x.sh) == 5);
	CHECK(datax_caps_get_limit(caps, DATAX_LIMIT_HOP_STAGE_US, &v) == 0);
	CHECK(v == STAGE_US);                 /* 80 us per sample less 30 us */
	/* The table is a set: duplicates fold. */
	{
		unsigned long long d[3] = { F_A, F_A, F_B };

		CHECK(datax_device_stage_hop_table(x.dev, d, 3) == 0);
	}

	/* Restaging while tunes are queued is refused. */
	fake_tick_set_access_cost(x.f, COST);
	CHECK(datax_sched_create(x.dev, &x.s) == 0);
	fake_tick_set_access_cost(x.f, 0);
	CHECK(datax_sched_queue_tune(x.s, now(&x) + 100 * MS, 0, F_A) > 0);
	CHECK(datax_device_stage_hop_table(x.dev, freqs, 2) == -EBUSY);
	CHECK(datax_sched_flush(x.s) == 0);
	CHECK(datax_device_stage_hop_table(x.dev, freqs, 2) == 0);

	/* The seam cannot be rebound under a scheduler; detached, staging is
	 * -ENOTSUP. */
	CHECK(datax_internal_fhm_bind_fake(x.dev, &fshim_ops, &x.sh) == -EBUSY);
	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(datax_internal_fhm_bind_fake(x.dev, NULL, NULL) == 0);
	CHECK(datax_device_stage_hop_table(x.dev, freqs, 2) == -ENOTSUP);
	fx_close(&x);
	printf("test_stage_table_band_and_depth: ok\n");
}

/* The driver can swallow a TALISE_setFhmMode failure: stage_hop_table reads
 * the enable back and fails -EIO. Either way, the context that enabled FHM
 * disables it at teardown (after its scheduler is flushed); one that never
 * enabled it writes nothing. */
static void test_enable_checked_and_disabled_at_teardown(void)
{
	struct fx x;
	unsigned long long f = F_A;

	fx_open(&x, true);
	x.sh.enable_reads_zero = true;
	CHECK(datax_device_stage_hop_table(x.dev, &f, 1) == -EIO);
	CHECK(strstr(lasterr(&x), "did not enable") != NULL);
	x.sh.enable_reads_zero = false;
	CHECK(datax_sched_queue_tune(x.s, now(&x) + 100 * MS, 0, F_A) == -EINVAL);
	(void)stage(&x, &f, 1);
	CHECK(datax_sched_queue_tune(x.s, now(&x) + 100 * MS, 0, F_A) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	CHECK(fake_fhm_enabled(x.m));
	CHECK(x.sh.enable_writes[0] == 0);
	datax_context_destroy(x.ctx);          /* destroys the scheduler first */
	x.ctx = NULL;
	x.s = NULL;
	CHECK(!fake_fhm_enabled(x.m));
	CHECK(x.sh.enable_writes[0] == 1);
	/* the tune was flushed and never ran; destroy parked bit 0 low, once
	 * per dropped fall (the priming edge's and the tune's) */
	CHECK(fake_tick_exec_log(x.f, 0, NULL, 0) == x.ebase + 2);
	CHECK(!(fake_tick_gpio(x.f) & 0x1));
	fx_close(&x);

	fx_open(&x, false);                    /* never staged: no write */
	datax_context_destroy(x.ctx);
	x.ctx = NULL;
	CHECK(x.sh.enable_writes[0] == 0 && x.sh.enable_writes[1] == 0);
	fx_close(&x);
	printf("test_enable_checked_and_disabled_at_teardown: ok\n");
}

static void test_tune_requires_staged(void)
{
	struct fx x;
	unsigned long long t, a = F_A;

	fx_open(&x, true);
	t = now(&x) + 100 * MS;
	CHECK(datax_sched_queue_tune(NULL, t, 0, F_A) == -EINVAL);
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) == -EINVAL);
	CHECK(strstr(lasterr(&x), "frequency not staged") != NULL);
	(void)stage(&x, &a, 1);
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_B) == -EINVAL);
	CHECK(strstr(lasterr(&x), "frequency not staged") != NULL);
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	/* in time order only */
	CHECK(datax_sched_queue_tune(x.s, t - 10 * MS, 0, F_A) == -EINVAL);
	CHECK(strstr(lasterr(&x), "time order") != NULL);
	CHECK(datax_sched_queue_tune(x.s, t + 10 * MS, 0, F_A) > 0);
	fx_close(&x);
	printf("test_tune_requires_staged: ok\n");
}

static void test_tune_chip_b_enotsup(void)
{
	static const unsigned int chip_b[] = { 2, 3, 6, 7 };   /* RX 2-3, TX 2-3 */
	static const unsigned int chip_a[] = { 0, 1, 4, 5 };
	struct fx x;
	unsigned long long t, a = F_A;

	fx_open(&x, true);
	(void)stage(&x, &a, 1);
	t = now(&x) + 100 * MS;
	for (size_t i = 0; i < 4; i++) {
		CHECK(datax_sched_queue_tune(x.s, t, chip_b[i], F_A) == -ENOTSUP);
		CHECK(strstr(lasterr(&x), "chip B FHM trigger is not wired on this image"));
	}
	CHECK(datax_sched_queue_tune(x.s, t, 8, F_A) == -EINVAL);
	for (size_t i = 0; i < 4; i++)
		CHECK(datax_sched_queue_tune(x.s, t + (unsigned long long)i * 10 * MS,
					     chip_a[i], F_A) > 0);
	fx_close(&x);
	printf("test_tune_chip_b_enotsup: ok\n");
}

/* Plan [A, B, C] 20 ms apart: four edges (priming + three), each tune's
 * frequency staged one edge ahead, every reported tune OK with exec ==
 * requested, and the PLL on the plan after every edge. */
static void test_fhm_one_behind_pipeline(void)
{
	static const unsigned long long plan[3] = { F_A, F_B, F_C };
	struct fx x;
	struct fake_tick_exec rec[16];
	unsigned long long t0;
	size_t base, n;
	long seq[3];

	fx_open(&x, true);
	base = stage(&x, plan, 3);
	t0 = now(&x) + 100 * MS;
	for (int i = 0; i < 3; i++) {
		seq[i] = datax_sched_queue_tune(x.s, t0 + (unsigned long long)i * 20 * MS, 0, plan[i]);
		CHECK(seq[i] > 0);
	}
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 8;
	/* h1 is staged before the priming edge fires */
	WAIT_FOR(stages(&x.sh) == base + 1);
	CHECK(stage_hz(&x.sh, base) == F_A);
	CHECK(fake_fhm_edges(x.m) == 0);

	to_edge(&x, 1, 200 * MS);                 /* priming edge: fhmInitFrequency */
	CHECK(now(&x) < t0);
	CHECK(fake_fhm_pll_hz(x.m) == F_INIT);
	CHECK(!x.dev->single_chip_tuned);         /* a priming edge is not a tune */
	WAIT_FOR(stages(&x.sh) == base + 2);      /* h2 after the priming edge */
	CHECK(stage_hz(&x.sh, base + 1) == F_B);

	to_edge(&x, 2, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_A);
	WAIT_FOR(x.dev->single_chip_tuned);       /* decision 7: PHASE_COHERENT off */
	WAIT_FOR(stages(&x.sh) == base + 3);
	CHECK(stage_hz(&x.sh, base + 2) == F_C);

	to_edge(&x, 3, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	to_edge(&x, 4, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_C);

	for (int i = 0; i < 3; i++) {
		struct datax_completion c = wait_done(&x, seq[i]);

		CHECK(c.status == DATAX_COMPLETION_OK);
		CHECK(c.requested_ticks == t0 + (unsigned long long)i * 20 * MS);
		CHECK(c.executed_ticks == c.requested_ticks);
	}
	fake_tick_advance(x.f, 11 * MS);           /* past the last 10 ms pulse */
	msleep(5);
	CHECK(stages(&x.sh) == base + 3);          /* nothing staged past the plan */
	CHECK(fake_fhm_edges(x.m) == 4);
	/* ground truth: 4 rises and 4 falls, the priming rise first, before t0;
	 * hops 20 ms apart keep the proven 10 ms pulse (the last one too), the
	 * priming pulse is half its lead */
	n = fake_tick_exec_log(x.f, x.ebase, rec, 16) - x.ebase;
	CHECK(n == 8);
	/* the priming lead: at least the guard margin (fix/p3a-stage-late) */
	CHECK(rec[0].requested_ticks == t0 - 10 * MS);
	CHECK(!rec[0].reported);
	CHECK(rec[1].requested_ticks - rec[0].requested_ticks == (t0 - rec[0].requested_ticks) / 2);
	for (int i = 1; i < 4; i++)
		CHECK(rec[2 * i + 1].requested_ticks - rec[2 * i].requested_ticks == 10 * MS);
	CHECK(count_events(&x.ev, DATAX_EVT_LATE_COMMAND) == 0);
	fx_close(&x);
	printf("test_fhm_one_behind_pipeline: ok\n");
}

static void test_fhm_readback_mismatch(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	struct datax_completion c;
	unsigned long long t0;
	char want[96];
	long s1, s2;

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t0 = now(&x) + 100 * MS;
	s1 = datax_sched_queue_tune(x.s, t0, 0, F_A);
	s2 = datax_sched_queue_tune(x.s, t0 + 20 * MS, 0, F_B);
	CHECK(s1 > 0 && s2 > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 6;
	to_edge(&x, 1, 200 * MS);
	fake_fhm_corrupt_next_readback(x.m, 2471000000ULL);
	to_edge(&x, 2, 50 * MS);
	c = wait_done(&x, s1);
	CHECK(c.status == DATAX_COMPLETION_DROPPED);
	snprintf(want, sizeof(want), "PLL 2471000000 Hz, expected %llu Hz", F_A);
	CHECK(strstr(lasterr(&x), want) != NULL);
	to_edge(&x, 3, 50 * MS);
	c = wait_done(&x, s2);
	CHECK(c.status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	fx_close(&x);
	printf("test_fhm_readback_mismatch: ok\n");
}

/* The write staging C (due between edges 1 and 2) stalls past edge 2: C's
 * tune completes LATE "stage late" with an event, and the PLL stays on B
 * (the engine armed B at edge 2). */
static void test_fhm_stage_late(void)
{
	static const unsigned long long plan[3] = { F_A, F_B, F_C };
	struct fx x;
	struct datax_completion c;
	unsigned long long t0;
	long seq[3];

	fx_open(&x, true);
	(void)stage(&x, plan, 3);
	t0 = now(&x) + 100 * MS;
	for (int i = 0; i < 3; i++)
		CHECK((seq[i] = datax_sched_queue_tune(x.s, t0 + (unsigned long long)i * 20 * MS,
						       0, plan[i])) > 0);
	pthread_mutex_lock(&x.sh.mu);
	x.sh.stall_hz = F_C;
	x.sh.stall_ticks = 25 * MS;
	pthread_mutex_unlock(&x.sh.mu);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 8;
	to_edge(&x, 1, 200 * MS);
	to_edge(&x, 2, 50 * MS);          /* staging C stalls past edge 3 (B's) */
	WAIT_FOR(fake_fhm_edges(x.m) >= 3);
	c = wait_done(&x, seq[0]);
	CHECK(c.status == DATAX_COMPLETION_OK);
	c = wait_done(&x, seq[1]);
	CHECK(c.status == DATAX_COMPLETION_OK);
	to_edge(&x, 4, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	c = wait_done(&x, seq[2]);
	CHECK(c.status == DATAX_COMPLETION_LATE);
	CHECK(strstr(lasterr(&x), "stage late") != NULL);
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, seq[2], "stage late"));
	fx_close(&x);
	printf("test_fhm_stage_late: ok\n");
}

/* Hops 2 ms apart, closer than the report spacing (2.7 ms): only every 2nd
 * rise is reported, so commit's spacing check holds; the unreported ones
 * still hop on time (staged after their edge's time plus the margin). */
static void test_dense_hops_report_every_nth(void)
{
	static const unsigned long long plan[3] = { F_A, F_B, F_C };
	enum { N = 8 };
	struct fx x;
	unsigned long long t0, v;
	size_t base;
	long seq[N];

	fx_open(&x, true);
	CHECK(datax_caps_get_limit(datax_device_get_caps(x.dev),
				   DATAX_LIMIT_REPORT_SPACING_US, &v) == 0 && v == SPACING_US);
	base = stage(&x, plan, 3);
	t0 = now(&x) + 100 * MS;
	for (int i = 0; i < N; i++)
		CHECK((seq[i] = datax_sched_queue_tune(x.s, t0 + (unsigned long long)i * 2 * MS,
						       0, plan[i % 3])) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 2 * (N + 1);
	/* odd tunes are 2 ms after a reported one: not reported */
	for (int i = 1; i < N; i += 2) {
		struct datax_completion c;

		CHECK(datax_sched_wait(x.s, seq[i], 0, &c) == -EINVAL);
		CHECK(strstr(lasterr(&x), "not reported") != NULL);
	}
	WAIT_FOR(stages(&x.sh) == base + 1);
	for (unsigned int k = 0; k <= N; k++) {
		to_edge(&x, k + 1, k ? 50 * MS : 200 * MS);
		/* after edge k, tune k+2's frequency is staged (if any) */
		WAIT_FOR(stages(&x.sh) == base + (k + 2 < N ? k + 2 : N));
		if (k)
			CHECK(fake_fhm_pll_hz(x.m) == plan[(k - 1) % 3]);
	}
	for (int i = 0; i < N; i++) {
		struct datax_completion c;

		CHECK(stage_hz(&x.sh, base + (size_t)i) == plan[i % 3]);
		if (i % 2) {
			/* executed and gone (unreported commands leave once due) */
			CHECK(datax_sched_wait(x.s, seq[i], 0, &c) == -EINVAL);
		} else {
			c = wait_done(&x, seq[i]);
			CHECK(c.status == DATAX_COMPLETION_OK);
			CHECK(c.executed_ticks == c.requested_ticks);
		}
	}
	CHECK(fake_fhm_edges(x.m) == N + 1);
	CHECK(count_events(&x.ev, DATAX_EVT_LATE_COMMAND) == 0);
	/* the pulse shrinks to half the 2 ms hop; the last keeps 10 ms */
	{
		struct fake_tick_exec rec[2 * (N + 1)];
		size_t k;

		fake_tick_advance(x.f, 11 * MS);
		msleep(3);
		k = fake_tick_exec_log(x.f, x.ebase, rec, 2 * (N + 1)) - x.ebase;
		CHECK(k == 2 * (N + 1));
		for (int i = 1; i < N; i++)
			CHECK(rec[2 * i + 1].requested_ticks - rec[2 * i].requested_ticks == 1 * MS);
		CHECK(rec[2 * N + 1].requested_ticks - rec[2 * N].requested_ticks == 10 * MS);
	}
	fx_close(&x);
	printf("test_dense_hops_report_every_nth: ok\n");
}

/* A batch rejected at commit takes its tune entries with it: the next tune
 * gets a fresh priming edge and lands on its frequency. */
static void test_rejected_batch_reprimes(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	struct datax_completion c;
	unsigned long long t;
	long s1;

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	/* A tune outside the guard margin whose priming edge (one lead, 10 ms,
	 * before it) would land inside it is refused by queue_tune itself,
	 * naming the tune (not by commit under the priming edge's seq). */
	CHECK(datax_sched_queue_tune(x.s, now(&x) + 15 * MS, 0, F_A) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "needs a priming edge") != NULL);
	CHECK(strstr(lasterr(&x), "guard") != NULL);
	/* A batch rejected at commit (a reported GPIO 1 ms from the tune breaks
	 * the report spacing) takes the tune and its priming edge with it. */
	t = now(&x) + 50 * MS;
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	s1 = datax_sched_queue_gpio(x.s, t + 1 * MS, 0x2, 0x2);
	CHECK(s1 > 0 && datax_sched_report(x.s, s1) == 0);
	CHECK(datax_sched_commit(x.s) == -EINVAL);
	t = now(&x) + 100 * MS;
	s1 = datax_sched_queue_tune(x.s, t, 0, F_B);
	CHECK(s1 > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 4;
	to_edge(&x, 1, 200 * MS);
	CHECK(now(&x) < t);                      /* the new priming edge */
	to_edge(&x, 2, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	c = wait_done(&x, s1);
	CHECK(c.status == DATAX_COMPLETION_OK);
	fx_close(&x);
	printf("test_rejected_batch_reprimes: ok\n");
}

/* set_frequency(when) creates the device's scheduler lazily and tunes
 * through it; a refused call creates nothing. */
static void test_timed_set_frequency_uses_device_sched(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	struct datax_sched *s2 = NULL;
	unsigned long long t;
	char want[96];

	fx_open(&x, false);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	CHECK(datax_device_set_frequency(x.dev, 2, F_A, t) == -ENOTSUP);
	CHECK(strstr(lasterr(&x), "chip B FHM trigger is not wired on this image"));
	CHECK(datax_device_set_frequency(x.dev, 0, 2470000000ULL, t) == -EINVAL);
	CHECK(strstr(lasterr(&x), "frequency not staged") != NULL);
	/* neither refusal left a scheduler behind */
	CHECK(datax_sched_create(x.dev, &s2) == 0);
	datax_sched_destroy(s2);
	s2 = NULL;

	CHECK(datax_device_set_frequency(x.dev, 0, F_B, t) == 0);
	x.ndesc = 4;
	CHECK(datax_sched_create(x.dev, &s2) == -EBUSY);   /* the device's own */
	to_edge(&x, 1, 200 * MS);
	/* Its result is not kept, so a DROPPED verdict (here a PLL readback
	 * mismatch) is raised as an event instead of vanishing. */
	fake_fhm_corrupt_next_readback(x.m, 2471000000ULL);
	to_edge(&x, 2, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	snprintf(want, sizeof(want), "DROPPED: PLL 2471000000 Hz, expected %llu Hz", F_B);
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, -2, want));
	CHECK(now(&x) >= t);
	/* A second timed tune reuses it (and re-primes: the pipeline drained). */
	t = now(&x) + 100 * MS;
	CHECK(datax_device_set_frequency(x.dev, 1, F_A, t) == 0);
	x.ndesc += 4;
	to_edge(&x, 3, 200 * MS);
	to_edge(&x, 4, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_A);
	/* Nobody holds this scheduler, so its tunes keep no completions (the
	 * hooks still verify them; test_sched_hooks consumed_result_not_kept). */
	CHECK(count_events(&x.ev, DATAX_EVT_LATE_COMMAND) == 1);
	fx_close(&x);                            /* context destroy frees it */
	printf("test_timed_set_frequency_uses_device_sched: ok\n");
}

static void test_reserved_bit_still_refused_for_gpio(void)
{
	struct fx x;
	unsigned long long t, a = F_A;
	long seq;

	fx_open(&x, true);
	(void)stage(&x, &a, 1);
	t = now(&x) + 100 * MS;
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	CHECK(datax_sched_queue_gpio(x.s, t + 5 * MS, 0x1, 0x1) == -EINVAL);
	CHECK(strstr(lasterr(&x), "reserved") != NULL);
	CHECK(datax_sched_queue_gpio(x.s, t + 5 * MS, 0x3, 0x2) == -EINVAL);
	seq = datax_sched_queue_gpio(x.s, t + 5 * MS, 0xe, 0x4);
	CHECK(seq > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 5;
	to_edge(&x, 1, 200 * MS);
	while (now(&x) < t)
		fake_tick_advance(x.f, 10 * US);
	CHECK(fake_tick_gpio(x.f) & 0x1);        /* the tune drives bit 0 high ... */
	fake_tick_advance(x.f, 6 * MS);
	CHECK((fake_tick_gpio(x.f) & 0xf) == 0x5);   /* bits 3:1 written, bit 0 held */
	fake_tick_advance(x.f, 5 * MS);
	CHECK(!(fake_tick_gpio(x.f) & 0x1));     /* ... and low after the 10 ms pulse */
	fx_close(&x);
	printf("test_reserved_bit_still_refused_for_gpio: ok\n");
}

/* Pulse = min(10 ms, half the distance to the next edge): a committed
 * edge keeps its pulse, so the next must be two pulses later; inside an
 * uncommitted batch the pulse shrinks (down to 50 us, so 100 us apart). */
static void test_pulse_gap_rule(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	unsigned long long t;

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	CHECK(datax_sched_queue_tune(x.s, t + 15 * MS, 0, F_B) == -EINVAL);
	CHECK(strstr(lasterr(&x), "at least 20000 us") != NULL);
	CHECK(datax_sched_queue_tune(x.s, t + 20 * MS, 0, F_B) > 0);   /* still building */
	CHECK(datax_sched_queue_tune(x.s, t + 20 * MS + 50 * US, 0, F_A) == -EINVAL);
	CHECK(strstr(lasterr(&x), "at least 100 us") != NULL);
	CHECK(datax_sched_queue_tune(x.s, t + 20 * MS + 100 * US, 0, F_A) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 8;                     /* no second prime: t was still ahead */
	to_edge(&x, 4, 200 * MS);
	fake_tick_advance(x.f, 11 * MS);
	msleep(3);
	{
		struct fake_tick_exec rec[8];

		CHECK(fake_tick_exec_log(x.f, x.ebase, rec, 8) - x.ebase == 8);
		/* prime, t (10 ms: committed alone), t+20 ms (50 us), t+20.1 ms */
		CHECK(rec[3].requested_ticks - rec[2].requested_ticks == 10 * MS);
		CHECK(rec[4].requested_ticks == t + 20 * MS);
		CHECK(rec[5].requested_ticks == t + 20 * MS + 50 * US);
		CHECK(rec[6].requested_ticks == t + 20 * MS + 100 * US);
		CHECK(rec[7].requested_ticks - rec[6].requested_ticks == 10 * MS);
	}
	CHECK(fake_fhm_pll_hz(x.m) == F_A);
	fx_close(&x);
	printf("test_pulse_gap_rule: ok\n");
}

/* The previous tune has fired, so a new one needs a priming edge; if that
 * edge would sit within two pulses of the previous one, queue_tune refuses. */
static void test_prime_too_close_refused(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	unsigned long long t;

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 4;
	to_edge(&x, 2, 200 * MS);
	fake_tick_advance(x.f, 12 * MS);
	msleep(3);
	CHECK(datax_sched_queue_tune(x.s, t + 20 * MS + 500 * US, 0, F_B) == -EINVAL);
	CHECK(strstr(lasterr(&x), "needs a priming edge") != NULL);
	CHECK(strstr(lasterr(&x), "too close to the previous edge") != NULL);
	/* well clear of the guard: queue_tune estimates device time from the
	 * host clock, which runs on while the fake clock waits for the test */
	CHECK(datax_sched_queue_tune(x.s, t + 300 * MS, 0, F_B) > 0);
	fx_close(&x);
	printf("test_prime_too_close_refused: ok\n");
}

/* An edge that did not fire for a host-known reason (here its descriptor
 * write failed) leaves the engine where it was, so the next tune lands one
 * behind: reported, it completes DROPPED; unreported, an event says so. */
static void test_previous_edge_not_fired_lands_behind(void)
{
	static const unsigned long long plan[3] = { F_A, F_B, F_C };
	struct fx x;
	struct datax_completion c;
	unsigned long long t;
	long s1, s2, s3;

	/* (1) the priming edge's write fails: tune A is DROPPED, B is fine */
	fx_open(&x, true);
	(void)stage(&x, plan, 3);
	t = now(&x) + 100 * MS;
	s1 = datax_sched_queue_tune(x.s, t, 0, F_A);
	s2 = datax_sched_queue_tune(x.s, t + 20 * MS, 0, F_B);
	CHECK(s1 > 0 && s2 > 0);
	x.tsh.fail_at = x.tsh.ndesc + 1;            /* (stage parked the line: one write) */
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 5;
	to_edge(&x, 1, 200 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_INIT);      /* A's edge committed init */
	c = wait_done(&x, s1);
	CHECK(c.status == DATAX_COMPLETION_DROPPED);
	CHECK(strstr(lasterr(&x), "previous edge did not fire; lands one behind") != NULL);
	to_edge(&x, 2, 50 * MS);
	c = wait_done(&x, s2);
	CHECK(c.status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	fx_close(&x);

	/* (2) dense: the reported tune A's rise fails; unreported B lands one
	 * behind (on the frequency armed for A) with an event; C is fine */
	fx_open(&x, true);
	(void)stage(&x, plan, 3);
	t = now(&x) + 100 * MS;
	s1 = datax_sched_queue_tune(x.s, t, 0, F_A);
	s2 = datax_sched_queue_tune(x.s, t + 2 * MS, 0, F_B);
	s3 = datax_sched_queue_tune(x.s, t + 4 * MS, 0, F_C);
	CHECK(s1 > 0 && s2 > 0 && s3 > 0);
	x.tsh.fail_at = x.tsh.ndesc + 3;            /* prime rise, prime fall, A rise */
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 7;
	to_edge(&x, 1, 200 * MS);
	c = wait_done(&x, s1);
	CHECK(c.status == DATAX_COMPLETION_DROPPED);
	to_edge(&x, 2, 50 * MS);
	CHECK(fake_fhm_pll_hz(x.m) == F_A);         /* B's edge: one behind */
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, s2,
			   "previous edge did not fire; lands one behind"));
	to_edge(&x, 3, 50 * MS);
	c = wait_done(&x, s3);
	CHECK(c.status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_C);
	fx_close(&x);
	printf("test_previous_edge_not_fired_lands_behind: ok\n");
}

/* An edge that did not fire still leaves its fall queued. (1) Tick
 * late-drops the rise (its write was slow) but the fall was written: later
 * edges keep two full pulses away from it. (2) The rise's write fails while
 * its fall is still unwritten: the fall moves to the minimum pulse. */
static void test_noedge_fall_still_counts(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	struct datax_completion c;
	struct fake_tick_exec rec[8];
	unsigned long long t;
	long s1;

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	s1 = datax_sched_queue_tune(x.s, t, 0, F_A);
	CHECK(s1 > 0);
	pthread_mutex_lock(&x.tsh.mu);
	x.tsh.stall_at = x.tsh.ndesc + 3;           /* the tune's rise */
	x.tsh.stall_to = t + 1 * MS;
	pthread_mutex_unlock(&x.tsh.mu);
	CHECK(datax_sched_commit(x.s) == 0);
	c = wait_done(&x, s1);
	CHECK(c.status == DATAX_COMPLETION_LATE);   /* "late: Tick dropped it" */
	CHECK(strstr(lasterr(&x), "Tick dropped it") != NULL);
	WAIT_FOR(fake_attr_u(x.f, "queue_level") == 1);   /* its fall, at t + 10 ms */
	CHECK(datax_sched_queue_tune(x.s, t + 15 * MS, 0, F_B) == -EINVAL);
	/* (it needs a priming edge, which would sit inside that gap) */
	CHECK(strstr(lasterr(&x), "too close to the previous edge") != NULL);
	/* its priming edge (one guard margin before it) clears the 20 ms */
	CHECK(datax_sched_queue_tune(x.s, t + 35 * MS, 0, F_B) > 0);
	fx_close(&x);

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	pthread_mutex_lock(&x.tsh.mu);
	x.tsh.fail_at = x.tsh.ndesc + 3;            /* the tune's rise */
	pthread_mutex_unlock(&x.tsh.mu);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 3;
	to_edge(&x, 1, 200 * MS);                   /* the priming edge */
	while (now(&x) < t + 1 * MS)
		fake_tick_advance(x.f, 10 * US);
	msleep(3);
	CHECK(fake_tick_exec_log(x.f, x.ebase, rec, 8) - x.ebase == 3);
	CHECK(rec[2].requested_ticks == t + 50 * US);   /* the fall, pulled in */
	fx_close(&x);
	printf("test_noedge_fall_still_counts: ok\n");
}

/* flush and destroy drop a queued fall: within 10 ms of a fired rise that
 * leaves bit 0 high, so libdatax parks it low (an undroppable immediate
 * write); stage_hop_table parks it low before enabling FHM. Afterwards a
 * tune still moves the PLL (its priming rise makes a real edge). */
static void test_trigger_parked_low(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	struct datax_completion c;
	unsigned long long t;
	long s1;

	for (int destroy = 0; destroy < 2; destroy++) {
		fx_open(&x, true);
		(void)stage(&x, plan, 2);
		t = now(&x) + 100 * MS;
		CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
		CHECK(datax_sched_commit(x.s) == 0);
		x.ndesc = 4;
		to_edge(&x, 2, 200 * MS);              /* the tune's rise fired */
		CHECK(fake_tick_gpio(x.f) & 0x1);      /* its 10 ms pulse is on */
		if (destroy) {
			datax_sched_destroy(x.s);
			fake_tick_set_access_cost(x.f, COST);
			CHECK(datax_sched_create(x.dev, &x.s) == 0);
			fake_tick_set_access_cost(x.f, 0);
		} else {
			CHECK(datax_sched_flush(x.s) == 0);
		}
		CHECK(!(fake_tick_gpio(x.f) & 0x1));   /* parked low */
		msleep(5);
		CHECK(!has_event(&x.ev, DATAX_EVT_LATE_COMMAND, -2, "late_count"));
		(void)stage(&x, plan, 2);
		t = now(&x) + 100 * MS;
		s1 = datax_sched_queue_tune(x.s, t, 0, F_B);
		CHECK(s1 > 0);
		CHECK(datax_sched_commit(x.s) == 0);
		x.ndesc = 4;
		to_edge(&x, 4, 200 * MS);              /* edges 3 and 4 (the fake counts on) */
		c = wait_done(&x, s1);
		CHECK(c.status == DATAX_COMPLETION_OK);   /* by PLL readback */
		CHECK(fake_fhm_pll_hz(x.m) == F_B);
		fx_close(&x);
	}

	/* stage_hop_table parks a line someone else left high */
	fx_open(&x, true);
	raise_bit0(&x);
	(void)stage(&x, plan, 2);
	CHECK(!(fake_tick_gpio(x.f) & 0x1));
	fx_close(&x);
	printf("test_trigger_parked_low: ok\n");
}

/* The rise has executed but the poller has not seen it yet (paused: its
 * record undrained, or, unreported, not yet pruned). Destroy, and flush,
 * finish it FLUSHED; its fall is dropped with the queue, so bit 0 must be
 * parked low anyway (round 3: a flushed fall always parks). */
static void test_park_when_rise_unseen(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	unsigned long long t;

	/* destroy, reported rise, record undrained */
	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 4;
	fed(&x);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	while (now(&x) < t + 150 * US)
		fake_tick_advance(x.f, 10 * US);
	CHECK(fake_tick_gpio(x.f) & 0x1);
	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(!(fake_tick_gpio(x.f) & 0x1));
	fx_close(&x);

	/* flush, unreported rise (a reported GPIO 0.5 ms before it takes the
	 * report spacing), not yet pruned */
	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	{
		long g = datax_sched_queue_gpio(x.s, t - 500 * US, 0x2, 0x2);

		CHECK(g > 0 && datax_sched_report(x.s, g) == 0);
	}
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 5;
	fed(&x);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	while (now(&x) < t + 150 * US)
		fake_tick_advance(x.f, 10 * US);
	CHECK(fake_tick_gpio(x.f) & 0x1);
	CHECK(datax_sched_flush(x.s) == 0);
	CHECK(!(fake_tick_gpio(x.f) & 0x1));
	fx_close(&x);
	printf("test_park_when_rise_unseen: ok\n");
}

/* Restaging while a fired tune's pulse is still on the line (its fall
 * queued) would cut the pulse with the park: -EBUSY until the fall ran. */
static void test_stage_busy_while_fall_pending(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	unsigned long long t;

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	CHECK(datax_sched_queue_tune(x.s, t, 0, F_A) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 4;
	to_edge(&x, 2, 200 * MS);
	CHECK(fake_tick_gpio(x.f) & 0x1);
	CHECK(datax_device_stage_hop_table(x.dev, plan, 2) == -EBUSY);
	CHECK(fake_tick_gpio(x.f) & 0x1);           /* the pulse was not cut */
	fake_tick_advance(x.f, 11 * MS);
	msleep(5);
	WAIT_FOR(datax_device_stage_hop_table(x.dev, plan, 2) == 0);
	fx_close(&x);
	printf("test_stage_busy_while_fall_pending: ok\n");
}

/* Rises completing host-late (the poller was held past them): their falls,
 * pulled in to the minimum pulse, are inside the guard too and are dropped
 * quietly instead of raising host-late events for seqs the caller never saw.
 * Only the two rises report host-late. */
static void test_shortened_fall_inside_guard_quiet(void)
{
	static const unsigned long long plan[1] = { F_A };
	struct fx x;
	unsigned long long t;
	size_t late = 0;
	long s1;

	fx_open(&x, true);
	(void)stage(&x, plan, 1);
	t = now(&x) + 100 * MS;
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	s1 = datax_sched_queue_tune(x.s, t, 0, F_A);
	CHECK(s1 > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	fake_tick_advance(x.f, 95 * MS);            /* everything inside the guard */
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, s1, "host could not keep"));
	msleep(10);
	pthread_mutex_lock(&x.ev.m);
	for (size_t i = 0; i < x.ev.n; i++)
		late += strstr(x.ev.e[i].detail, "host could not keep") != NULL;
	pthread_mutex_unlock(&x.ev.m);
	CHECK(late == 2);                            /* priming rise and tune rise */
	CHECK(fake_attr_u(x.f, "queue_level") == 0);
	fx_close(&x);
	printf("test_shortened_fall_inside_guard_quiet: ok\n");
}

/* queue_tune's notion of device time: the poller's last read plus host time
 * since (it can only grow between this read and queue_tune's). */
static unsigned long long sched_now_est(struct fx *x)
{
	struct timespec ts;
	unsigned long long mono, est;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	mono = (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
	pthread_mutex_lock(&x->s->lock);
	est = x->s->last_hw + (mono - x->s->last_hw_mono) * HZ / 1000000000ULL;
	pthread_mutex_unlock(&x->s->lock);
	return est;
}

/*
 * The first tune after staging needs when >= now + guard + lead: its priming
 * edge sits one lead (here the 10 ms guard; the fake's formula is about
 * 1.9 ms) before it and must itself clear the guard. One tick below that
 * (against queue_tune's own estimate of now) is DATAX_ERR_LATE from
 * queue_tune; 1 ms above (against the later of that estimate and the fake's
 * clock, which commit reads) is accepted and completes OK.
 */
static void test_first_tune_minimum_lead(void)
{
	static const unsigned long long plan[1] = { F_A };
	struct fx x;
	unsigned long long t;
	long s1;

	fx_open(&x, true);
	(void)stage(&x, plan, 1);
	CHECK(datax_sched_queue_tune(x.s, sched_now_est(&x) + 20 * MS - 1, 0, F_A) ==
	      DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "needs a priming edge 10000 us before it") != NULL);
	CHECK(strstr(lasterr(&x), "guard") != NULL);
	t = sched_now_est(&x);
	if (now(&x) > t)
		t = now(&x);
	t += 20 * MS + 1 * MS;
	s1 = datax_sched_queue_tune(x.s, t, 0, F_A);
	CHECK(s1 > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 4;
	to_edge(&x, 1, 50 * MS);
	CHECK(now(&x) < t);
	to_edge(&x, 2, 50 * MS);
	CHECK(wait_done(&x, s1).status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_A);
	fx_close(&x);
	printf("test_first_tune_minimum_lead: ok\n");
}

/* API device time (after an alignment it differs from the fake's counter). */
static unsigned long long api_now(struct fx *x)
{
	unsigned long long t, hz;

	CHECK(datax_device_get_time(x->dev, &t, &hz) == 0);
	return t;
}

/* After stage(): rebase the exec-log / write counters for a new batch. */
static void rebase_counts(struct fx *x, unsigned long ndesc)
{
	x->ebase = fake_tick_exec_log(x->f, 0, NULL, 0);
	x->wbase = fake_tick_access_count(x->f, "command_descriptor", true);
	x->ndesc = ndesc;
}

/*
 * Final review I1: a fall that ends without executing (other than by flush)
 * must not leave bit 0 high. (1) The fall's descriptor write fails: the park
 * is deferred until the rise has fired, then bit 0 is low after the pulse
 * and the next tune completes OK.
 */
static void test_fall_write_failure_parks(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	unsigned long long t;
	long sa, sb;

	size_t base;

	fx_open(&x, true);
	base = stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	sa = datax_sched_queue_tune(x.s, t, 0, F_A);
	sb = datax_sched_queue_tune(x.s, t + 30 * MS, 0, F_B);
	CHECK(sa > 0 && sb > 0);
	pthread_mutex_lock(&x.tsh.mu);
	x.tsh.fail_at = x.tsh.ndesc + 4;            /* prime rise, prime fall, A rise, A fall */
	pthread_mutex_unlock(&x.tsh.mu);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 5;                                /* the failed write does not count */
	to_edge(&x, 1, 200 * MS);                   /* the priming edge */
	WAIT_FOR(stages(&x.sh) == base + 2);        /* B staged before A */
	to_edge(&x, 2, 50 * MS);                    /* A's rise fired; its fall never will */
	CHECK(wait_done(&x, sa).status == DATAX_COMPLETION_OK);
	fake_tick_advance(x.f, 11 * MS);            /* past A's 10 ms pulse */
	WAIT_FOR(!(fake_tick_gpio(x.f) & 0x1));
	to_edge(&x, 3, 50 * MS);
	CHECK(wait_done(&x, sb).status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	fx_close(&x);
	printf("test_fall_write_failure_parks: ok\n");
}

/* (2) The fall completes host-late (the feeder saw the queue full until it
 * was due): deferred park, bit 0 low, the next tune OK. */
static void test_fall_host_late_parks(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	unsigned long long t;
	long sa, sb;

	size_t base;

	g_depth = 4;                                /* one pass writes 3: P rise/fall, A rise */
	fx_open(&x, true);
	g_depth = 0;
	base = stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	sa = datax_sched_queue_tune(x.s, t, 0, F_A);
	sb = datax_sched_queue_tune(x.s, t + 30 * MS, 0, F_B);
	CHECK(sa > 0 && sb > 0);
	pthread_mutex_lock(&x.tsh.mu);
	x.tsh.full_after = x.tsh.ndesc + 3;
	pthread_mutex_unlock(&x.tsh.mu);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 3;
	to_edge(&x, 1, 200 * MS);                   /* the priming edge */
	WAIT_FOR(stages(&x.sh) == base + 2);        /* B staged before A */
	to_edge(&x, 2, 50 * MS);                    /* A fired; its fall is now inside the guard */
	WAIT_FOR(has_event(&x.ev, DATAX_EVT_LATE_COMMAND, -2, "host could not keep"));
	CHECK(wait_done(&x, sa).status == DATAX_COMPLETION_OK);
	WAIT_FOR(!(fake_tick_gpio(x.f) & 0x1));     /* parked (the 10 ms pulse is not over) */
	pthread_mutex_lock(&x.tsh.mu);
	x.tsh.full_after = 0;                       /* the queue drains again */
	pthread_mutex_unlock(&x.tsh.mu);
	x.ndesc = 5;
	to_edge(&x, 3, 50 * MS);
	CHECK(wait_done(&x, sb).status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	fx_close(&x);
	printf("test_fall_host_late_parks: ok\n");
}

/* (3) A late PPS edge is adopted while A's pulse is high: the queue is
 * emptied (A's fall dropped), bit 0 is parked at once, and (I2) the FHM
 * history is reset, so a tune queued on the jumped-back clock primes and
 * completes OK. */
static void test_adoption_parks_and_reprimes(void)
{
	static const unsigned long long plan[3] = { F_A, F_B, F_C };
	struct fx x;
	struct datax_completion c;
	unsigned long long t, e1 = 500 * MS;
	enum datax_time_source src;
	long sa, sb, sc;

	fx_open(&x, true);
	(void)stage(&x, plan, 3);
	fake_tick_pps_edge(x.f);                    /* PPS present, but no edge in 2 s */
	CHECK(datax_device_set_time_next_pps(x.dev, e1) == 0);   /* soft, still armed */
	/* the 2 s wait held the poller off: let it read the (fake, unmoved)
	 * clock again, or queue_tune's host-time estimate runs 2 s ahead */
	msleep(20);
	t = api_now(&x) + 100 * MS;
	sa = datax_sched_queue_tune(x.s, t, 0, F_A);
	sb = datax_sched_queue_tune(x.s, t + 30 * MS, 0, F_B);
	CHECK(sa > 0 && sb > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 6;
	to_edge(&x, 2, 200 * MS);
	CHECK(wait_done(&x, sa).status == DATAX_COMPLETION_OK);
	CHECK(fake_tick_gpio(x.f) & 0x1);           /* A's pulse is on */
	fake_tick_pps_edge(x.f);                    /* the late edge: the counter jumps back */
	WAIT_FOR(!(fake_tick_gpio(x.f) & 0x1));
	CHECK(datax_device_get_time_source(x.dev, &src) == 0 && src == DATAX_TIME_PPS_ALIGNED);
	c = wait_done(&x, sb);
	CHECK(c.status == DATAX_COMPLETION_DROPPED);
	CHECK(strstr(lasterr(&x), "device time jumped") != NULL);
	t = api_now(&x) + 200 * MS;
	CHECK(t < 500 * MS + 300 * MS);             /* time did move back */
	sc = datax_sched_queue_tune(x.s, t, 0, F_C);
	CHECK(sc > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	rebase_counts(&x, 4);
	to_edge(&x, 4, 300 * MS);                   /* a fresh priming edge, then C */
	CHECK(wait_done(&x, sc).status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_C);
	fx_close(&x);
	printf("test_adoption_parks_and_reprimes: ok\n");
}

/*
 * Final review I2: stage, a tune OK, align (soft) to epoch 0 so device time
 * moves back, then queue_tune(now + 200 ms) is accepted (the history was
 * reset, so it primes) and completes OK.
 */
static void test_tune_after_time_moves_back(void)
{
	static const unsigned long long plan[2] = { F_A, F_B };
	struct fx x;
	unsigned long long t;
	enum datax_time_source src;
	long sa, sb;

	fx_open(&x, true);
	(void)stage(&x, plan, 2);
	t = now(&x) + 100 * MS;
	sa = datax_sched_queue_tune(x.s, t, 0, F_A);
	CHECK(sa > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	x.ndesc = 4;
	to_edge(&x, 2, 200 * MS);
	CHECK(wait_done(&x, sa).status == DATAX_COMPLETION_OK);
	fake_tick_advance(x.f, 11 * MS);            /* its fall ran: nothing queued */
	msleep(5);
	CHECK(datax_device_set_time_next_pps(x.dev, 0) == 0);
	CHECK(datax_device_get_time_source(x.dev, &src) == 0 && src == DATAX_TIME_SOFT_ALIGNED);
	t = api_now(&x);
	CHECK(t < 100 * MS);                        /* moved back to about 0 */
	sb = datax_sched_queue_tune(x.s, t + 200 * MS, 0, F_B);
	CHECK(sb > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	rebase_counts(&x, 4);
	to_edge(&x, 4, 300 * MS);
	CHECK(wait_done(&x, sb).status == DATAX_COMPLETION_OK);
	CHECK(fake_fhm_pll_hz(x.m) == F_B);
	fx_close(&x);
	printf("test_tune_after_time_moves_back: ok\n");
}

/* set_time_next_pps is -EBUSY while a tune is live, also one still in the
 * builder (planned on the old mapping); after a flush it aligns. */
static void test_align_busy_with_live_tune(void)
{
	static const unsigned long long plan[1] = { F_A };
	struct fx x;

	fx_open(&x, true);
	(void)stage(&x, plan, 1);
	CHECK(datax_sched_queue_tune(x.s, now(&x) + 100 * MS, 0, F_A) > 0);
	CHECK(datax_device_set_time_next_pps(x.dev, 0) == -EBUSY);
	CHECK(strstr(lasterr(&x), "timed tune") != NULL);
	CHECK(datax_sched_commit(x.s) == 0);
	CHECK(datax_device_set_time_next_pps(x.dev, 0) == -EBUSY);
	CHECK(datax_sched_flush(x.s) == 0);
	CHECK(datax_device_set_time_next_pps(x.dev, 0) == 0);
	CHECK(datax_sched_queue_tune(x.s, api_now(&x) + 100 * MS, 0, F_A) > 0);
	fx_close(&x);
	printf("test_align_busy_with_live_tune: ok\n");
}

/* A consumed (timed set_frequency) tune flushed by teardown raises no event:
 * nobody asked, and its delivery would race the teardown. */
static void test_teardown_flush_is_silent(void)
{
	static const unsigned long long plan[1] = { F_A };
	struct fx x;

	fx_open(&x, false);
	(void)stage(&x, plan, 1);
	CHECK(datax_device_set_frequency(x.dev, 0, F_A, now(&x) + 500 * MS) == 0);
	msleep(5);
	datax_context_destroy(x.ctx);
	x.ctx = NULL;
	msleep(5);
	CHECK(!has_event(&x.ev, DATAX_EVT_LATE_COMMAND, -2, "FLUSHED"));
	fx_close(&x);
	printf("test_teardown_flush_is_silent: ok\n");
}

/* ---- repro mode: tron-like timing (not part of the default run) -------------- */

/*
 * The fake clock normally moves only when a test advances it, so the poller
 * always has time between edges. Here a thread advances it at wall-clock
 * rate, and both shims sleep per access (the tron round trips: about 0.2 ms
 * per Tick attribute, about 0.46 ms per FHM staging write), so the poller's
 * real latency decides whether a staging write lands before its edge.
 */
struct rtclock {
	struct fake_tick *f;
	atomic_bool stop;
	pthread_t th;
	_Atomic unsigned long long max_gap_ns;   /* longest stall between ticks */
};

static void *rtclock_main(void *arg)
{
	struct rtclock *c = arg;
	unsigned long long t0 = 0, last;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	last = (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
	(void)t0;
	while (!atomic_load(&c->stop)) {
		unsigned long long now;

		usleep_(20);
		clock_gettime(CLOCK_MONOTONIC, &ts);
		now = (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
		fake_tick_advance(c->f, (now - last) * HZ / 1000000000ULL);
		if (now - last > atomic_load(&c->max_gap_ns))
			atomic_store(&c->max_gap_ns, now - last);
		last = now;
	}
	return NULL;
}

/* n reported tunes `apart` ticks apart, the first `first` ahead; returns the
 * number not completing OK (printing each such outcome; all with verbose).
 * *starved: the host could not keep this run real time (the clock thread
 * stalled over 3 ms, the poller went over 5 ms between Tick accesses, or
 * the scheduler itself reported "poller lagged"). */
static unsigned int repro_run(unsigned int n, unsigned long long first, unsigned long long apart,
			      bool verbose, bool *starved)
{
	static const unsigned long long plan[4] = { 2420000000ULL, 2440000000ULL,
						    2460000000ULL, 2480000000ULL };
	struct fx x;
	struct rtclock clk;
	unsigned long long t, v = 0;
	unsigned int late = 0;
	long seq[16];

	CHECK(n <= 16);
	fx_open(&x, false);
	x.tsh.sleep_us = 200;
	x.sh.sleep_us = 460;
	clk.f = x.f;
	atomic_init(&clk.stop, false);
	atomic_init(&clk.max_gap_ns, 0);
	*starved = false;
	CHECK(pthread_create(&clk.th, NULL, rtclock_main, &clk) == 0);
	CHECK(datax_sched_create(x.dev, &x.s) == 0);
	CHECK(datax_device_stage_hop_table(x.dev, plan, 4) == 0);
	CHECK(datax_caps_get_limit(datax_device_get_caps(x.dev), DATAX_LIMIT_HOP_STAGE_US, &v) == 0);
	if (verbose)
		printf("realtime: hop_stage_us %llu", v);
	CHECK(datax_caps_get_limit(datax_device_get_caps(x.dev), DATAX_LIMIT_REPORT_SPACING_US,
				   &v) == 0);
	if (verbose)
		printf(", report_spacing_us %llu\n", v);
	t = now(&x) + first;
	for (unsigned int i = 0; i < n; i++) {
		seq[i] = datax_sched_queue_tune(x.s, t + i * apart, 0, plan[i % 4]);
		CHECK(seq[i] > 0);
	}
	CHECK(datax_sched_commit(x.s) == 0);
	pthread_mutex_lock(&x.tsh.mu);
	x.tsh.max_gap_ns = 0;                    /* judge the schedule, not the setup */
	pthread_mutex_unlock(&x.tsh.mu);
	for (unsigned int i = 0; i < n; i++) {
		struct datax_completion c;
		int ret = datax_sched_wait(x.s, seq[i], 5000, &c);

		if (verbose || ret || c.status != DATAX_COMPLETION_OK)
			printf("realtime: tune %u seq %ld: ret %d status %d %s\n", i + 1, seq[i], ret,
			       ret ? -1 : (int)c.status,
			       ret || c.status == DATAX_COMPLETION_OK ? "" : lasterr(&x));
		late += ret || c.status != DATAX_COMPLETION_OK;
		if (!ret && c.status != DATAX_COMPLETION_OK && strstr(lasterr(&x), "poller lagged"))
			*starved = true;
	}
	atomic_store(&clk.stop, true);
	pthread_join(clk.th, NULL);
	if (atomic_load(&clk.max_gap_ns) > 3000000ULL)
		*starved = true;
	/* the poller went more than 5 ms (5 periods) without touching Tick */
	pthread_mutex_lock(&x.tsh.mu);
	if (x.tsh.max_gap_ns > 5000000ULL)
		*starved = true;
	pthread_mutex_unlock(&x.tsh.mu);
	fx_close(&x);
	return late;
}

/*
 * Regression for the tron stage-late tune (fix/p3a-stage-late): with tron's
 * latencies, tune 2's staging window is the priming lead, which used to be
 * about 2 ms and was missed by the poll hook every run. Every tune of both
 * hardware schedules must complete OK, RUNS times each.
 */
#define REALTIME_RUNS 10

/*
 * One schedule, retried (at most 3 attempts) while the host was starved.
 * Only unstarved runs are judged: a starved host produces honest late and
 * dropped outcomes (poller lagged, host-late, readback after the next edge)
 * that say nothing about the pipeline, while the pipeline fault this guards
 * against showed on every unstarved run (19 of 160 tunes, c38a845).
 * Returns the bad tunes of the judged run; *judged: whether one was.
 */
static unsigned int realtime_schedule(unsigned int n, unsigned long long apart, bool verbose,
				      unsigned int *retries, bool *judged)
{
	unsigned int bad = 0;
	bool starved = true;

	for (int a = 0; a < 3 && starved; a++) {
		if (a)
			(*retries)++;
		bad = repro_run(n, 50 * MS, apart, verbose && !a, &starved);
	}
	*judged = !starved;
	return starved ? 0 : bad;
}

static void test_realtime_no_stage_late(void)
{
	unsigned int bad = 0, retries = 0, judged[2] = { 0, 0 };

	for (int i = 0; i < REALTIME_RUNS; i++) {
		bool j;

		/* test_hop_schedule: 4 tunes 200 ms apart */
		bad += realtime_schedule(4, 200 * MS, i == 0, &retries, &j);
		judged[0] += j;
		/* test_dense_hops_library_readback_each_edge: 12 at ~2 x spacing */
		bad += realtime_schedule(12, 11 * MS, false, &retries, &j);
		judged[1] += j;
	}
	printf("realtime: %u tunes not OK in %u + %u judged runs of %d each (%u retries: host "
	       "starved)\n", bad, judged[0], judged[1], REALTIME_RUNS, retries);
	CHECK(bad == 0);
	/* a mostly starved host proves nothing: a majority must be judged */
	if (judged[0] < REALTIME_RUNS / 2 || judged[1] < REALTIME_RUNS / 2)
		fprintf(stderr, "test_realtime_no_stage_late: only %u + %u of %d runs per schedule "
			"ran unstarved (need %d each): host too loaded to judge\n", judged[0],
			judged[1], REALTIME_RUNS, REALTIME_RUNS / 2);
	CHECK(judged[0] >= REALTIME_RUNS / 2 && judged[1] >= REALTIME_RUNS / 2);
	printf("test_realtime_no_stage_late: ok\n");
}

int main(int argc, char **argv)
{
	static const struct { const char *name; void (*fn)(void); } t[] = {
		{ "stage_table_band_and_depth", test_stage_table_band_and_depth },
		{ "tune_requires_staged", test_tune_requires_staged },
		{ "tune_chip_b_enotsup", test_tune_chip_b_enotsup },
		{ "fhm_one_behind_pipeline", test_fhm_one_behind_pipeline },
		{ "fhm_readback_mismatch", test_fhm_readback_mismatch },
		{ "fhm_stage_late", test_fhm_stage_late },
		{ "dense_hops_report_every_nth", test_dense_hops_report_every_nth },
		{ "rejected_batch_reprimes", test_rejected_batch_reprimes },
		{ "timed_set_frequency_uses_device_sched",
		  test_timed_set_frequency_uses_device_sched },
		{ "reserved_bit_still_refused_for_gpio", test_reserved_bit_still_refused_for_gpio },
		{ "enable_checked_and_disabled_at_teardown",
		  test_enable_checked_and_disabled_at_teardown },
		{ "pulse_gap_rule", test_pulse_gap_rule },
		{ "prime_too_close_refused", test_prime_too_close_refused },
		{ "previous_edge_not_fired_lands_behind", test_previous_edge_not_fired_lands_behind },
		{ "noedge_fall_still_counts", test_noedge_fall_still_counts },
		{ "trigger_parked_low", test_trigger_parked_low },
		{ "teardown_flush_is_silent", test_teardown_flush_is_silent },
		{ "first_tune_minimum_lead", test_first_tune_minimum_lead },
		{ "fall_write_failure_parks", test_fall_write_failure_parks },
		{ "fall_host_late_parks", test_fall_host_late_parks },
		{ "adoption_parks_and_reprimes", test_adoption_parks_and_reprimes },
		{ "tune_after_time_moves_back", test_tune_after_time_moves_back },
		{ "align_busy_with_live_tune", test_align_busy_with_live_tune },
		{ "park_when_rise_unseen", test_park_when_rise_unseen },
		{ "stage_busy_while_fall_pending", test_stage_busy_while_fall_pending },
		{ "shortened_fall_inside_guard_quiet", test_shortened_fall_inside_guard_quiet },
	};

	/* Real-time mode (its own ctest, test_fhm_realtime): tron-like timing
	 * with a wall-clock fake and per-access latency. */
	if (argc > 1 && !strcmp(argv[1], "realtime")) {
		test_realtime_no_stage_late();
		return 0;
	}
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (argc > 2 && !strcmp(argv[1], "one") && strcmp(argv[2], t[i].name))
			continue;
		t[i].fn();
	}
	printf("test_fhm: all ok\n");
	return 0;
}
