// SPDX-License-Identifier: MIT
/*
 * Timed streams (P3b Task 3): datax_stream_start_at for RX and TX, the
 * targeted cancel and re-feed in the scheduler, stop() cancelling the
 * stream's gate commands and forcing the gate open, the untimed start()
 * forcing open, and TX preload.
 *
 * The fake Tick (bitmap 0xdf, map "rx-dma,tx-dma,rx-mode,tx-mode") is bound
 * through a counting shim to the emulator's adrv9009 pair; the fake DMA, at
 * 1 MSPS (100 ticks per frame), is the block source of both directions and
 * gates on the fake Tick's levels, with the bit positions the device parsed
 * from the map. Device time moves only when a test advances it, except where
 * a test runs a clock thread. Compiled from the library sources: some tests
 * look at the scheduler's table and enqueue raw commands.
 *
 * Usage: test_stream_timed <name>   (one test; "list" prints the names)
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include <iio/iio.h>

#include "check.h"
#include "fake_dma.h"
#include "fake_tick.h"
#include "internal.h"
#include "scheduler.h"
#include "stream.h"
#include "tick_descriptor.h"

#define HZ  100000000ULL
#define MS  (HZ / 1000)
#define US  (HZ / 1000000)
#define COST (100 * US)          /* while the scheduler measures: spacing 2.7 ms */
#define FS  1000000ULL           /* 100 ticks per frame */
#define FS_3072 30720000ULL      /* 3.2552 ticks per frame: rounds */
#define FT  (HZ / FS)
#define BS  256u                 /* frames per block */
#define NB  4u
#define MAP "rx-dma,tx-dma,rx-mode,tx-mode"
#define ST_EXECUTED 0u
#define ST_LATE_EXECUTED 1u
#define ST_LATE_DROPPED 2u

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static unsigned long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

#define WAIT_FOR(cond)                                                       \
	do {                                                                 \
		int w_ = 0;                                                  \
		while (!(cond) && w_++ < 3000)                               \
			msleep(1);                                           \
		CHECK(cond);                                                 \
	} while (0)

/* ---- Tick shim: counts accesses, notes what the DMA had done at each push -- */

struct tshim {
	struct fake_tick *f;
	struct fake_dma *d;
	pthread_mutex_t mu;
	unsigned long accesses, desc, flushes;
	size_t dma_at[256];               /* DMA log length at descriptor write k */
	unsigned long long tag_write_at, flush_at;
};

static int tshim_read(void *h, const char *attr, char *buf, size_t len)
{
	struct tshim *sh = h;

	pthread_mutex_lock(&sh->mu);
	sh->accesses++;
	pthread_mutex_unlock(&sh->mu);
	return fake_tick_read(sh->f, attr, buf, len);
}

static int tshim_write(void *h, const char *attr, const char *val)
{
	struct tshim *sh = h;
	int ret;

	pthread_mutex_lock(&sh->mu);
	sh->accesses++;
	if (!strcmp(attr, "command_descriptor")) {
		if (sh->desc < 256)
			sh->dma_at[sh->desc] = sh->d ? fake_dma_log(sh->d, 0, NULL, 0) : 0;
		sh->desc++;
	}
	pthread_mutex_unlock(&sh->mu);
	ret = fake_tick_write(sh->f, attr, val);
	pthread_mutex_lock(&sh->mu);
	if (!strcmp(attr, "capture_tag_enable"))
		sh->tag_write_at = fake_tick_now(sh->f);
	if (!strcmp(attr, "flush_queue")) {
		sh->flush_at = fake_tick_now(sh->f);
		sh->flushes++;
	}
	pthread_mutex_unlock(&sh->mu);
	return ret;
}

static const struct datax_tick_ops tshim_ops = { tshim_read, tshim_write };

static unsigned long shim_get(struct tshim *sh, const unsigned long *field)
{
	unsigned long v;

	pthread_mutex_lock(&sh->mu);
	v = *field;
	pthread_mutex_unlock(&sh->mu);
	return v;
}

/* ---- events ----------------------------------------------------------------- */

struct evrec {
	pthread_mutex_t m;
	struct { enum datax_event_type type; long seq; char detail[160]; } e[256];
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

/* Events of type whose detail contains text (NULL: any). */
static size_t events(struct evrec *r, enum datax_event_type type, const char *text)
{
	size_t n = 0;

	pthread_mutex_lock(&r->m);
	for (size_t i = 0; i < r->n; i++)
		n += r->e[i].type == type && (!text || strstr(r->e[i].detail, text));
	pthread_mutex_unlock(&r->m);
	return n;
}

static void dump_events(struct evrec *r)
{
	pthread_mutex_lock(&r->m);
	for (size_t i = 0; i < r->n; i++)
		fprintf(stderr, "  event %d seq %ld: %s\n", (int)r->e[i].type, r->e[i].seq,
			r->e[i].detail);
	pthread_mutex_unlock(&r->m);
}

/* ---- fixture ---------------------------------------------------------------- */

struct fx {
	struct datax_context *ctx;
	struct datax_device *dev;
	struct fake_tick *f;
	struct fake_dma *d;
	struct fake_fhm *m;           /* with_fhm only */
	struct tshim sh;
	struct datax_sched *s;
	struct evrec ev;
	unsigned int rxl, rxm, txl, txm;
};

/* map NULL: no stream_gate_map attribute. fhm: bind the fake Talise FHM. */
static unsigned long long g_fs = FS;     /* the fake DMA's and the device's sample rate */
static uint32_t g_bitmap = 0xdf;         /* the fake Tick's capability bitmap */

/* Set the emulated phy's sampling_frequency (what start_at reads). */
static void set_device_fs(struct fx *x, unsigned long long fs)
{
	struct iio_device *ph = iio_context_find_device(x->ctx->iio, "adrv9009-phy");
	struct iio_channel *ch;
	const struct iio_attr *a;

	CHECK(ph != NULL);
	ch = iio_device_find_channel(ph, "voltage0", false);
	CHECK(ch != NULL);
	a = iio_channel_find_attr(ch, "sampling_frequency");
	CHECK(a != NULL);
	CHECK(iio_attr_write_longlong(a, (long long)fs) == 0);
}

static void fx_open_ex(struct fx *x, const char *map, bool create, bool fhm)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct fake_tick_config cfg = { .tick_hz = HZ, .queue_depth = 16,
					.capability_bitmap = g_bitmap };
	struct fake_dma_config dc;

	memset(x, 0, sizeof(*x));
	CHECK(uri != NULL);
	CHECK(pthread_mutex_init(&x->sh.mu, NULL) == 0);
	CHECK(pthread_mutex_init(&x->ev.m, NULL) == 0);
	x->f = fake_tick_new(&cfg);
	CHECK(x->f != NULL);
	fake_tick_set_gate_map(x->f, map);
	x->sh.f = x->f;
	CHECK(datax_context_create_from_uri(uri, &x->ctx) == 0);
	CHECK(datax_context_get_device(x->ctx, 0, &x->dev) == 0);
	CHECK(datax_internal_tick_bind_fake(x->dev, &tshim_ops, &x->sh) == 0);
	if (fhm) {
		x->m = fake_fhm_new(0);
		CHECK(x->m != NULL);
		fake_fhm_attach(x->m, x->f);
		CHECK(datax_internal_fhm_bind_fake(x->dev, &fake_fhm_ops, x->m) == 0);
	}
	CHECK(datax_context_set_event_callback(x->ctx, on_event, &x->ev) == 0);
	fake_tick_advance(x->f, 1000 * MS);
	/* Controller ruling (b): the fake's bit positions come from the map
	 * the device parsed, never from literals. */
	set_device_fs(x, g_fs);
	dc = fake_dma_config_from_map(g_fs, &x->dev->caps.gate_map);
	x->rxl = dc.rx_level;
	x->rxm = dc.rx_mode;
	x->txl = dc.tx_level;
	x->txm = dc.tx_mode;
	x->d = fake_dma_new(x->f, &dc);
	CHECK(x->d != NULL);
	pthread_mutex_lock(&x->sh.mu);
	x->sh.d = x->d;
	pthread_mutex_unlock(&x->sh.mu);
	CHECK(datax_internal_stream_bind_source(x->dev, DATAX_RX, &fake_dma_rx_ops, x->d) == 0);
	CHECK(datax_internal_stream_bind_source(x->dev, DATAX_TX, &fake_dma_tx_ops, x->d) == 0);
	if (create) {
		fake_tick_set_access_cost(x->f, COST);
		CHECK(datax_sched_create(x->dev, &x->s) == 0);
		fake_tick_set_access_cost(x->f, 0);
	}
}

static void fx_open(struct fx *x, bool create)
{
	fx_open_ex(x, MAP, create, false);
}

static void fx_close(struct fx *x)
{
	datax_sched_destroy(x->s);
	datax_context_destroy(x->ctx);      /* destroys the streams */
	fake_dma_free(x->d);
	fake_fhm_free(x->m);
	fake_tick_free(x->f);
	pthread_mutex_destroy(&x->sh.mu);
	pthread_mutex_destroy(&x->ev.m);
}

static const char *lasterr(struct fx *x)
{
	return datax_context_get_last_error(x->ctx);
}

static unsigned long long now(struct fx *x)
{
	return fake_tick_now(x->f);
}

/* A frame-aligned time ms from now. */
static unsigned long long at(struct fx *x, unsigned long long ms)
{
	unsigned long long t = now(x) + ms * MS;

	return t - t % FT;
}

static unsigned long long attr_u(struct fx *x, const char *attr)
{
	char b[64];

	CHECK(fake_tick_read(x->f, attr, b, sizeof(b)) > 0);
	return strtoull(b, NULL, 0);
}

static struct datax_stream *stream(struct fx *x, enum datax_direction dir, unsigned int mask,
				   bool cyclic)
{
	struct datax_stream_config c = { .direction = dir, .channel_mask = mask,
					 .block_samples = BS, .block_count = NB, .cyclic = cyclic };
	struct datax_stream *s = NULL;

	CHECK(datax_stream_create(x->dev, &c, &s) == 0);
	return s;
}

/* Preload n samples of a counting pattern on a TX stream. */
static int preload(struct datax_stream *s, size_t n)
{
	static int16_t buf[NB * BS * 2 * 4 + 64];

	for (size_t i = 0; i < sizeof(buf) / sizeof(buf[0]); i++)
		buf[i] = (int16_t)(i + 1);
	return datax_stream_send(s, buf, n, 0);
}

static size_t sched_count(struct fx *x, enum sched_state st)
{
	struct datax_sched *s = datax_device_sched(x->dev);
	size_t n = 0;

	if (!s)
		return 0;
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->ncmds; i++)
		n += s->cmds[i].state == st;
	pthread_mutex_unlock(&s->lock);
	return n;
}

/* Wait until the poller has written everything pending and drained the
 * completion slot, then let it run anything due by now. */
static void settle(struct fx *x)
{
	struct datax_sched *s = datax_device_sched(x->dev);

	for (int w = 0; w < 3000; w++) {
		if (!sched_count(x, CMD_PENDING) && !attr_u(x, "completion_sequence_id")) {
			if (s)
				CHECK(datax_internal_sched_catch_up(s, now(x)) >= 0);
			return;
		}
		msleep(1);
	}
	CHECK(!"the poller did not feed the queue");
}

/* Advance the clock to t in steps, settling in between. */
static void step_to(struct fx *x, unsigned long long t, unsigned long long step)
{
	while (now(x) < t) {
		settle(x);
		fake_tick_advance(x->f, t - now(x) < step ? t - now(x) : step);
	}
	settle(x);
	msleep(3);
	settle(x);
}

/* Exec-log entries (from first) of STREAM_GATE on bit at requested ticks
 * req; *idx gets the index of the first. */
static size_t gate_execs(struct fx *x, size_t first, unsigned int bit, unsigned long long req,
			 size_t *idx, struct fake_tick_exec *out)
{
	struct fake_tick_exec e[512];
	size_t n = fake_tick_exec_log(x->f, first, e, 512), k = 0;

	for (size_t i = 0; first + i < n && i < 512; i++)
		if (e[i].command_id == DATAX_TICK_COMMAND_ID_STREAM_GATE &&
		    e[i].target_id == (DATAX_TICK_TARGET_FAMILY_STREAM_GATE | bit) &&
		    e[i].requested_ticks == req) {
			if (!k && idx)
				*idx = first + i;
			if (!k && out)
				*out = e[i];
			k++;
		}
	return k;
}

static bool gate_bit(struct fx *x, unsigned int bit)
{
	return fake_tick_stream_gate(x->f) >> bit & 1u;
}

/* Write an execute-now STREAM_GATE straight to the fake (a foreign Tick
 * user, or a process that crashed with the gate closed). */
static void foreign_gate(struct fx *x, unsigned int bit, unsigned int level)
{
	struct datax_tick_descriptor d = {
		.version = DATAX_TICK_DESC_VERSION, .size_bytes = DATAX_TICK_DESC_SIZE,
		.command_id = DATAX_TICK_COMMAND_ID_STREAM_GATE,
		.target_id = (uint16_t)(DATAX_TICK_TARGET_FAMILY_STREAM_GATE | bit),
		.timestamp_ticks = 0, .flags = DATAX_TICK_FLAG_LATE_EXECUTE_NOW,
		.sequence_id = 0x7ffffff0u, .args = { bit, level, 0, 0 },
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
}

/* One RX block of 1 channel by recv. tagged: block 0 of a start_at, whose
 * 8 tag frames are stripped, so it delivers BS - 8 frames; either way the
 * frames are consecutive indices. Returns the index of the block's first
 * stored frame (what the stripped frames would have been, for tagged). */
static uint64_t recv_block(struct fx *x, struct datax_stream *s, struct datax_block_info *info,
			   bool tagged)
{
	static int16_t buf[BS * 2];
	unsigned int skip = tagged ? (32 + 3) / 4 : 0;   /* ceil(32 / frame_bytes) */
	uint64_t first;

	(void)x;
	CHECK(datax_stream_recv(s, buf, BS - skip, info, 3000) == (int)(BS - skip));
	first = fake_dma_frame_index(&buf[0]);
	for (unsigned int k = 0; k < BS - skip; k++)
		CHECK(fake_dma_frame_index(&buf[2 * k]) == first + k);
	return first - skip;
}

/* No TX frame was played: every segment is muted or an underflow. */
static void nothing_played(struct fx *x)
{
	struct fake_dma_tx_segment seg[64];
	size_t n = fake_dma_tx_segments(x->d, seg, 64);

	for (size_t i = 0; i < n && i < 64; i++) {
		if (!seg[i].muted && !seg[i].underflow)
			fprintf(stderr, "played: [%llu, %llu) from frame %llu\n",
				(unsigned long long)seg[i].t_start,
				(unsigned long long)seg[i].t_end,
				(unsigned long long)seg[i].first_frame);
		CHECK(seg[i].muted || seg[i].underflow);
	}
}

/* ---- tests ------------------------------------------------------------------ */

static void test_start_at_guard_margin_late(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long desc;
	uint32_t gate;
	size_t dlog;
	int16_t b[8];

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	desc = shim_get(&x.sh, &x.sh.desc);
	gate = fake_tick_stream_gate(x.f);
	dlog = fake_dma_log(x.d, 0, NULL, 0);
	CHECK(datax_stream_start_at(s, at(&x, 5)) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "guard margin") != NULL);
	CHECK(shim_get(&x.sh, &x.sh.desc) == desc);   /* no descriptor written */
	CHECK(fake_tick_stream_gate(x.f) == gate);
	CHECK(fake_dma_log(x.d, 0, NULL, 0) == dlog);  /* nothing armed */
	CHECK(datax_stream_recv(s, b, 1, NULL, 0) == -EINVAL);   /* not started */
	fx_close(&x);
}

static void test_start_at_order(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_tick_exec e[8], o;
	struct fake_dma_op op[16];
	unsigned long long T, late0;
	unsigned long d0;
	size_t e0, dl0, n, idx = 0;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	settle(&x);
	msleep(20);
	late0 = attr_u(&x, "late_count");
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	d0 = shim_get(&x.sh, &x.sh.desc);
	dl0 = fake_dma_log(x.d, 0, NULL, 0);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	/* close first: execute-now mode = 1, then level = 0 */
	n = fake_tick_exec_log(x.f, e0, e, 8) - e0;
	CHECK(n == 2);
	CHECK(e[0].target_id == (DATAX_TICK_TARGET_FAMILY_STREAM_GATE | x.rxm));
	CHECK(e[0].requested_ticks == 0 && e[0].status == ST_LATE_EXECUTED && !e[0].reported);
	CHECK(e[1].target_id == (DATAX_TICK_TARGET_FAMILY_STREAM_GATE | x.rxl));
	CHECK(e[1].requested_ticks == 0 && !e[1].reported);
	CHECK(gate_bit(&x, x.rxm) && !gate_bit(&x, x.rxl));
	CHECK(attr_u(&x, "capture_tag_enable") >> x.rxl & 1u);
	CHECK(attr_u(&x, "late_count") == late0 + 2);
	/* then arm: every block enqueued, then start */
	n = fake_dma_log(x.d, dl0, op, 16) - dl0;
	CHECK(n == NB + 1);
	for (unsigned int i = 0; i < NB; i++)
		CHECK(op[i].kind == FAKE_DMA_ENQUEUE && !op[i].tx && op[i].block == i);
	CHECK(op[NB].kind == FAKE_DMA_START && !op[NB].tx);
	/* the execute-now writes came before the arming; the open after it */
	WAIT_FOR(shim_get(&x.sh, &x.sh.desc) >= d0 + 3);
	pthread_mutex_lock(&x.sh.mu);
	CHECK(x.sh.dma_at[d0] == dl0 && x.sh.dma_at[d0 + 1] == dl0);
	CHECK(x.sh.dma_at[d0 + 2] == dl0 + NB + 1);
	pthread_mutex_unlock(&x.sh.mu);
	/* then the open at T, on time */
	step_to(&x, T + 10 * FT, 1 * MS);
	CHECK(gate_execs(&x, e0, x.rxl, T, &idx, &o) == 1);
	CHECK(o.executed_ticks == T && o.status == ST_EXECUTED && o.reported);
	CHECK(gate_bit(&x, x.rxl));
	msleep(20);
	settle(&x);
	if (events(&x.ev, DATAX_EVT_LATE_COMMAND, NULL))
		dump_events(&x.ev);
	CHECK(events(&x.ev, DATAX_EVT_LATE_COMMAND, NULL) == 0);
	fx_close(&x);
}

static void test_start_at_rx_first_frame_at_T(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 2 * BS * FT, 1 * MS);
	/* frame T/FT opens the gate; util_cpack2 drops it, so stored frame 0
	 * is T/FT + 1 (G2/G3 ruling 6); the first 8 frames are the header */
	CHECK(recv_block(&x, s, &info, true) == T / FT + 1);
	CHECK(!info.discontinuous);
	CHECK(recv_block(&x, s, &info, false) == T / FT + 1 + BS);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_start_at_late_at_dispatch_fails(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_tick_exec o;
	unsigned long long T;
	int16_t b[BS * 2];
	size_t e0;

	/* host-late: the poller cannot write the open before T - guard */
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 30);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(s, T) == 0);
	fake_tick_advance(x.f, T - 5 * MS - now(&x));
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "host could not keep the queue fed"));
	step_to(&x, T + 2 * BS * FT, 1 * MS);
	CHECK(datax_stream_recv(s, b, BS, NULL, 2000) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "host could not keep the queue fed") != NULL);
	CHECK(gate_execs(&x, e0, x.rxl, T, NULL, NULL) == 0);   /* never pushed */
	CHECK(!gate_bit(&x, x.rxl));                             /* never opened */
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);

	/* Tick late on submit: the push lands after T. Raised access cost
	 * after create, so feed()'s estimate (the last time read plus one
	 * access at the cost measured at create) still passes the guard. */
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 20);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(s, T) == 0);
	{
		/* after unpause: time read (C), slot (C), counters (2C),
		 * queue_level (C): the guard check sees about now + C + 100 us;
		 * the push is the 6th access */
		unsigned long long d = T - now(&x), c = (d - 10 * MS) / 2;

		CHECK(now(&x) + c + 10 * MS + 200 * US < T && now(&x) + 6 * c > T);
		fake_tick_set_access_cost(x.f, c);
	}
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(gate_execs(&x, e0, x.rxl, T, NULL, NULL) == 1);
	fake_tick_set_access_cost(x.f, 0);
	CHECK(gate_execs(&x, e0, x.rxl, T, NULL, &o) == 1);
	CHECK(o.status == ST_LATE_DROPPED && o.reported);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "Tick dropped it"));
	CHECK(datax_stream_recv(s, b, BS, NULL, 2000) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "Tick dropped it") != NULL);
	CHECK(!gate_bit(&x, x.rxl));
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_start_at_tx_late_on_submit(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_tick_exec o;
	unsigned long long T;
	size_t e0;

	/* The TX open is unreported: late on submit shows only as a
	 * late_count rise around its push (late_probe). */
	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, NB * BS) == (int)(NB * BS));
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 20);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(s, T) == 0);
	{
		/* time read, slot, 2 counters, queue_level, late_count: the
		 * push is the 7th access */
		unsigned long long d = T - now(&x), c = (d - 10 * MS) / 2;

		CHECK(now(&x) + c + 10 * MS + 200 * US < T && now(&x) + 7 * c > T);
		fake_tick_set_access_cost(x.f, c);
	}
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(gate_execs(&x, e0, x.txl, T, NULL, NULL) == 1);
	fake_tick_set_access_cost(x.f, 0);
	CHECK(gate_execs(&x, e0, x.txl, T, NULL, &o) == 1);
	/* a drop records a completion even unreported (RTL); libdatax ignores it */
	CHECK(o.status == ST_LATE_DROPPED);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "late_count rose at its push"));
	CHECK(preload(s, 1) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "late_count rose") != NULL);
	/* (the drop's record stays in the slot until the poller next has
	 * work: nothing to settle) */
	msleep(30);
	CHECK(datax_internal_sched_catch_up(x.s, now(&x) + 10 * MS) >= 0);
	/* the rise is reported once, not again by the late_count watch */
	CHECK(events(&x.ev, DATAX_EVT_LATE_COMMAND, "late_count ") == 1);
	CHECK(!gate_bit(&x, x.txl));
	(void)datax_stream_stop(s);
	fx_close(&x);
}

static void test_start_at_tx_host_late(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long T;
	size_t e0;

	/* The unreported TX open's other late path: the feeder's guard rule
	 * finishes it LATE through its hook. */
	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, NB * BS) == (int)(NB * BS));
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 30);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(s, T) == 0);
	fake_tick_advance(x.f, T - 5 * MS - now(&x));
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "host could not keep the queue fed"));
	CHECK(preload(s, 1) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "host could not keep the queue fed") != NULL);
	step_to(&x, T + 10 * FT, 1 * MS);
	CHECK(gate_execs(&x, e0, x.txl, T, NULL, NULL) == 0);   /* never pushed */
	CHECK(!gate_bit(&x, x.txl));
	(void)datax_stream_stop(s);
	fx_close(&x);
}

static void test_held_slot_open_never_delivers_shifted(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_tick_exec o;
	struct fake_tick_tag tag;
	unsigned long long T;
	int16_t b[BS * 2];
	unsigned char *blk0;
	unsigned long d0;
	size_t e0;
	long g;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 30);
	/* a reported GPIO 5 ms before T, then the open */
	g = datax_sched_queue_gpio(x.s, T - 5 * MS, 0x2, 0x2);
	CHECK(g > 0 && datax_sched_report(x.s, g) == 0 && datax_sched_commit(x.s) == 0);
	d0 = shim_get(&x.sh, &x.sh.desc);
	CHECK(datax_stream_start_at(s, T) == 0);
	WAIT_FOR(shim_get(&x.sh, &x.sh.desc) >= d0 + 3 && !sched_count(&x, CMD_PENDING));
	/* nobody acks the GPIO's record: Tick holds the open behind it */
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_advance(x.f, T + 5 * MS - now(&x));
	CHECK(!gate_bit(&x, x.rxl));
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "poller lagged"));
	/* Tick records it on time, the gate opened late */
	CHECK(gate_execs(&x, e0, x.rxl, T, NULL, &o) == 1);
	CHECK(o.executed_ticks == T && o.status == ST_EXECUTED);
	CHECK(fake_tick_gate_tag(x.f, x.rxl, &tag) == 0 && tag.ticks == T);
	CHECK(gate_bit(&x, x.rxl));
	step_to(&x, now(&x) + 2 * BS * FT, 1 * MS);
	memset(b, 0x5a, sizeof(b));
	CHECK(datax_stream_recv(s, b, BS, NULL, 2000) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "poller lagged") != NULL);
	for (size_t i = 0; i < sizeof(b) / sizeof(b[0]); i++)
		CHECK(b[i] == 0x5a5a);                 /* nothing delivered */
	/* the block it kept back is the shifted one */
	(void)fake_dma_tx_segments(x.d, NULL, 0);   /* the fake catches up lazily */
	blk0 = fake_dma_rx_ops.data(x.d, 0);
	CHECK(blk0 && fake_dma_frame_index(blk0 + 32) > (T + 5 * MS) / FT);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void stage_one(struct fx *x)
{
	const unsigned long long f = 2410000000ULL;

	fake_fhm_set_access_cost(x->m, 50 * US);
	CHECK(datax_device_stage_hop_table(x->dev, &f, 1) == 0);
	fake_fhm_set_access_cost(x->m, 0);
}

/* The GPIO write executions (from first) at requested ticks req on mask. */
static size_t gpio_execs(struct fx *x, size_t first, unsigned long long req, bool *on_time)
{
	struct fake_tick_exec e[512];
	size_t n = fake_tick_exec_log(x->f, first, e, 512), k = 0;

	for (size_t i = 0; first + i < n && i < 512; i++)
		if (e[i].command_id == DATAX_TICK_COMMAND_ID_GPIO_WRITE &&
		    e[i].requested_ticks == req) {
			k++;
			if (on_time)
				*on_time = e[i].executed_ticks == req;
		}
	return k;
}

static void test_refeed_keeps_near_edge(void)
{
	struct fx x;
	struct datax_stream *rx, *tx;
	unsigned long long t0, T, fall, tag_at, flush_at;
	size_t e0, ir = 0, it = 0;
	unsigned long fl0;
	bool ok = false;

	fx_open_ex(&x, MAP, true, true);
	stage_one(&x);
	rx = stream(&x, DATAX_RX, 0x1, false);
	tx = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(tx, NB * BS) == (int)(NB * BS));
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	t0 = at(&x, 100);
	fall = t0 + 10 * MS;               /* the tune's (unreported) fall */
	T = fall + 19 * MS;
	CHECK(datax_stream_start_at(tx, T) == 0);
	CHECK(datax_sched_queue_tune(x.s, t0, 0, 2410000000ULL) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	step_to(&x, fall - 1500 * US, 500 * US);
	CHECK(sched_count(&x, CMD_WRITTEN) >= 2);      /* the TX open and the fall */
	/* RX start_at's own accesses (six) and the poller's move the clock:
	 * 100 us each, so the fall is about 1 ms ahead at the refeed */
	fl0 = shim_get(&x.sh, &x.sh.flushes);
	fake_tick_set_access_cost(x.f, 100 * US);
	CHECK(datax_stream_start_at(rx, T) == 0);
	fake_tick_set_access_cost(x.f, 0);
	CHECK(!strstr(lasterr(&x), "refeed skipped"));
	pthread_mutex_lock(&x.sh.mu);
	tag_at = x.sh.tag_write_at;
	flush_at = x.sh.flush_at;
	pthread_mutex_unlock(&x.sh.mu);
	CHECK(shim_get(&x.sh, &x.sh.flushes) == fl0 + 1);
	CHECK(tag_at < fall);                  /* ahead when start_at began */
	CHECK(flush_at > fall);                /* the refeed waited for it */
	step_to(&x, T + 10 * FT, 1 * MS);
	CHECK(gpio_execs(&x, e0, fall, &ok) == 1 && ok);   /* exactly once, on time */
	CHECK(events(&x.ev, DATAX_EVT_LATE_COMMAND, "flushed by") == 0);
	/* and the reorder took: RX's gate before TX's at T */
	CHECK(gate_execs(&x, e0, x.rxl, T, &ir, NULL) == 1);
	CHECK(gate_execs(&x, e0, x.txl, T, &it, NULL) == 1);
	CHECK(ir < it);
	CHECK(datax_stream_stop(rx) == 0);
	(void)datax_stream_stop(tx);
	fx_close(&x);
}

static void test_stop_cancels_open(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long T;
	unsigned long fl0;
	size_t e0;

	/* WRITTEN: the Tick queue is emptied */
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	settle(&x);
	CHECK(sched_count(&x, CMD_WRITTEN) == 1);
	fl0 = shim_get(&x.sh, &x.sh.flushes);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(shim_get(&x.sh, &x.sh.flushes) == fl0 + 1);
	CHECK(datax_device_sched(x.dev)->ncmds == 0);
	/* forced open (reset state), tag bit cleared */
	CHECK(!gate_bit(&x, x.rxm) && !gate_bit(&x, x.rxl));
	CHECK(!(attr_u(&x, "capture_tag_enable") >> x.rxl & 1u));
	step_to(&x, T + 10 * FT, 1 * MS);
	CHECK(gate_execs(&x, e0, x.rxl, T, NULL, NULL) == 0);   /* never ran */
	msleep(10);
	CHECK(events(&x.ev, DATAX_EVT_LATE_COMMAND, NULL) == 0);  /* its own cancel: quiet */
	fx_close(&x);

	/* PENDING: no Tick I/O for the cancel */
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 30);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(s, T) == 0);
	CHECK(sched_count(&x, CMD_PENDING) == 1);
	fl0 = shim_get(&x.sh, &x.sh.flushes);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(shim_get(&x.sh, &x.sh.flushes) == fl0);
	CHECK(sched_count(&x, CMD_PENDING) == 0);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	CHECK(!gate_bit(&x, x.rxm));
	fx_close(&x);
}

static int build_hook_calls;
static enum datax_completion_status build_hook_status;

static void build_hook(struct datax_sched *s, long seq, struct datax_sched_result *r, void *user)
{
	(void)s; (void)seq; (void)user;
	build_hook_calls++;
	build_hook_status = r->status;
}

static void test_cancel_keeps_others(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_sched_cmd_spec spec;
	unsigned long long T, tg, tu;
	size_t e0;
	long g, gs;
	bool ok = false;

	fx_open_ex(&x, MAP, true, true);
	stage_one(&x);
	s = stream(&x, DATAX_RX, 0x1, false);
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 40);
	tg = T + 5 * MS;                 /* an unrelated GPIO */
	tu = T + 40 * MS;                /* a tune: priming edge at tu - 10 ms */
	CHECK(datax_stream_start_at(s, T) == 0);
	g = datax_sched_queue_gpio(x.s, tg, 0x2, 0x2);
	CHECK(g > 0);
	CHECK(datax_sched_queue_tune(x.s, tu, 0, 2410000000ULL) > 0);
	CHECK(datax_sched_commit(x.s) == 0);
	settle(&x);
	CHECK(sched_count(&x, CMD_WRITTEN) >= 6);   /* open, GPIO, 2 rises, 2 falls */
	/* a gate command of the stream still in the builder (Task 5's
	 * queue_stream_gate path) */
	memset(&spec, 0, sizeof(spec));
	spec.when = T + 20 * MS;
	spec.command_id = DATAX_TICK_COMMAND_ID_STREAM_GATE;
	spec.target_id = (uint16_t)(DATAX_TICK_TARGET_FAMILY_STREAM_GATE | x.rxl);
	spec.args[0] = x.rxl;
	spec.on_complete = build_hook;
	gs = datax_sched_enqueue(x.s, &spec);
	CHECK(gs > 0);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(build_hook_calls == 1 && build_hook_status == DATAX_COMPLETION_FLUSHED);
	CHECK(sched_count(&x, CMD_BUILD) == 0);
	CHECK(datax_sched_commit(x.s) == 0);        /* nothing left to commit */
	/* the GPIO and the tune survive and run once, on time */
	step_to(&x, tu + 11 * MS, 500 * US);
	CHECK(gpio_execs(&x, e0, tg, &ok) == 1 && ok);
	CHECK(gpio_execs(&x, e0, tu, &ok) == 1 && ok);
	CHECK(gpio_execs(&x, e0, tu - 10 * MS, &ok) == 1 && ok);   /* priming rise */
	CHECK(gpio_execs(&x, e0, tu + 10 * MS, &ok) == 1 && ok);   /* tune fall */
	CHECK(fake_fhm_edges(x.m) == 2);
	CHECK(fake_fhm_pll_hz(x.m) == 2410000000ULL);
	CHECK(gate_execs(&x, e0, x.rxl, T, NULL, NULL) == 0);
	if (events(&x.ev, DATAX_EVT_LATE_COMMAND, NULL))
		dump_events(&x.ev);
	CHECK(events(&x.ev, DATAX_EVT_LATE_COMMAND, NULL) == 0);
	fx_close(&x);
}

struct clockthr {
	struct fake_tick *f;
	pthread_t t;
	volatile int stop;
};

static void *clock_main(void *arg)
{
	struct clockthr *c = arg;
	const struct timespec step = { 0, 100000L };

	while (!c->stop) {
		fake_tick_advance(c->f, 100 * US);   /* about real time */
		nanosleep(&step, NULL);
	}
	return NULL;
}

static void test_stop_armed_rx_bounded(void)
{
	struct fx x;
	struct datax_stream *s;
	struct clockthr c;
	unsigned long long t0;
	size_t dl;

	fx_open(&x, true);
	/* the fake's blocking dequeue waits like a 5 s context timeout */
	fake_dma_set_wait_ms(x.d, 5000);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start_at(s, at(&x, 2000)) == 0);
	settle(&x);
	c.f = x.f;
	c.stop = 0;
	CHECK(pthread_create(&c.t, NULL, clock_main, &c) == 0);
	msleep(20);
	/* armed, never opened: cancel, force open, then drain */
	t0 = mono_ms();
	CHECK(datax_stream_stop(s) == 0);
	CHECK(mono_ms() - t0 < 1000);
	c.stop = 1;
	pthread_join(c.t, NULL);
	/* the drain completed, so the buffer stays open: no reopen */
	dl = fake_dma_log(x.d, 0, NULL, 0);
	CHECK(datax_stream_start(s) == 0);
	{
		struct fake_dma_op op[16];
		size_t n = fake_dma_log(x.d, dl, op, 16) - dl;

		CHECK(n == NB + 1 && op[0].kind == FAKE_DMA_ENQUEUE);
	}
	fx_close(&x);
}

static void test_start_after_aborted_start_at_open(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T, t1;
	int16_t b[8];

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	/* the open completes host-late: the stream fails */
	T = at(&x, 30);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(s, T) == 0);
	fake_tick_advance(x.f, T - 2 * MS - now(&x));
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "host could not"));
	CHECK(datax_stream_recv(s, b, 1, NULL, 1000) == DATAX_ERR_LATE);
	CHECK(datax_stream_stop(s) == 0);
	/* the next untimed start streams: forced open, failure forgotten */
	CHECK(datax_stream_start(s) == 0);
	CHECK(!gate_bit(&x, x.rxm));
	t1 = now(&x);
	step_to(&x, t1 + 2 * BS * FT, 1 * MS);
	CHECK(recv_block(&x, s, &info, false) > t1 / FT);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_start_forces_open(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long t1;

	fx_open(&x, false);                /* no scheduler: s == NULL path */
	/* a crashed process left the RX gate closed */
	foreign_gate(&x, x.rxm, 1);
	foreign_gate(&x, x.rxl, 0);
	CHECK(gate_bit(&x, x.rxm));
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start(s) == 0);
	CHECK(!gate_bit(&x, x.rxm) && !gate_bit(&x, x.rxl));
	t1 = now(&x);
	fake_tick_advance(x.f, 2 * BS * FT);
	CHECK(recv_block(&x, s, &info, false) > t1 / FT);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_device_sched(x.dev) == NULL);
	fx_close(&x);
}

static void test_start_no_map_no_tick_writes(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long acc;

	fx_open_ex(&x, NULL, false, false);
	CHECK(!datax_caps_has(datax_device_get_caps(x.dev), DATAX_CAP_RX_ARM));
	s = stream(&x, DATAX_RX, 0x1, false);
	acc = shim_get(&x.sh, &x.sh.accesses);
	CHECK(datax_stream_start(s) == 0);
	fake_tick_advance(x.f, 2 * BS * FT);
	(void)recv_block(&x, s, &info, false);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(shim_get(&x.sh, &x.sh.accesses) == acc);   /* no Tick access at all */
	fx_close(&x);
}

static void test_start_at_enotsup_without_map(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long acc;

	fx_open_ex(&x, NULL, false, false);
	s = stream(&x, DATAX_RX, 0x1, false);
	acc = shim_get(&x.sh, &x.sh.accesses);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -ENOTSUP);
	CHECK(strstr(lasterr(&x), "DATAX_CAP_RX_ARM") && strstr(lasterr(&x), "rx-dma") &&
	      strstr(lasterr(&x), "rx-mode"));
	CHECK(shim_get(&x.sh, &x.sh.accesses) == acc);
	CHECK(datax_device_sched(x.dev) == NULL);        /* none created */
	fx_close(&x);

	fx_open_ex(&x, "rx-dma,tx-dma", false, false);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -ENOTSUP);
	CHECK(strstr(lasterr(&x), "lacks rx-mode") != NULL);
	fx_close(&x);
}

static void test_start_at_ealready(void)
{
	struct fx x;
	struct datax_stream *s;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -EALREADY);
	CHECK(strstr(lasterr(&x), "already started") != NULL);
	CHECK(datax_stream_start_at(NULL, 1) == -EINVAL);
	fx_close(&x);
}

static void test_start_at_refuses_uncommitted_batch(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long desc;
	uint32_t gate;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_sched_queue_gpio(x.s, at(&x, 50), 0x2, 0x2) > 0);
	desc = shim_get(&x.sh, &x.sh.desc);
	gate = fake_tick_stream_gate(x.f);
	CHECK(datax_stream_start_at(s, at(&x, 60)) == -EBUSY);
	CHECK(strstr(lasterr(&x), "uncommitted") != NULL);
	CHECK(shim_get(&x.sh, &x.sh.desc) == desc && fake_tick_stream_gate(x.f) == gate);
	CHECK(sched_count(&x, CMD_BUILD) == 1);       /* the caller's batch is intact */
	fx_close(&x);
}

static void test_start_at_report_spacing_restores_gate(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T, t1;
	int16_t b[8];
	long g;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 50);
	g = datax_sched_queue_gpio(x.s, T - 1 * MS, 0x2, 0x2);
	CHECK(g > 0 && datax_sched_report(x.s, g) == 0 && datax_sched_commit(x.s) == 0);
	CHECK(datax_stream_start_at(s, T) == -EINVAL);
	CHECK(strstr(lasterr(&x), "report spacing") != NULL);
	/* undone: forced open, tag bit clear, nothing armed, not started */
	CHECK(!gate_bit(&x, x.rxm) && !gate_bit(&x, x.rxl));
	CHECK(!(attr_u(&x, "capture_tag_enable") >> x.rxl & 1u));
	CHECK(datax_stream_recv(s, b, 1, NULL, 0) == -EINVAL);
	CHECK(sched_count(&x, CMD_BUILD) == 0);
	/* and the next untimed start streams */
	CHECK(datax_stream_start(s) == 0);
	t1 = now(&x);
	fake_tick_advance(x.f, 2 * BS * FT);
	CHECK(recv_block(&x, s, &info, false) > t1 / FT);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_spacing_ignores_unreported(void)
{
	struct fx x;
	unsigned long long T;
	long a, b;

	/* Only reported commands are spaced: an unreported command next to a
	 * reported one at the same time commits (the TX opening relies on it). */
	fx_open(&x, true);
	T = at(&x, 50);
	a = datax_sched_queue_gpio(x.s, T, 0x2, 0x2);
	b = datax_sched_queue_gpio(x.s, T, 0x4, 0x4);
	CHECK(a > 0 && b > 0 && datax_sched_report(x.s, a) == 0);
	CHECK(datax_sched_commit(x.s) == 0);
	a = datax_sched_queue_gpio(x.s, T + 1 * US, 0x2, 0);
	CHECK(a > 0 && datax_sched_commit(x.s) == 0);
	/* two reported ones still are */
	a = datax_sched_queue_gpio(x.s, T + 2 * US, 0x4, 0);
	CHECK(a > 0 && datax_sched_report(x.s, a) == 0);
	CHECK(datax_sched_commit(x.s) == -EINVAL);
	fx_close(&x);
}

static void test_start_at_tx_partial_mask_enotsup(void)
{
	struct fx x;
	struct datax_stream *s;

	fx_open_ex(&x, MAP ",tx-start-fullmask-only", true, false);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, BS) == (int)BS);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -ENOTSUP);
	CHECK(strstr(lasterr(&x), "TX start_at needs every TX channel on this image") != NULL);
	datax_stream_destroy(s);
	s = stream(&x, DATAX_TX, 0xf, false);              /* every TX channel */
	CHECK(preload(s, BS) == (int)BS);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == 0);
	/* stopped before T: the DMA is cancelled, nothing drains or plays */
	CHECK(datax_stream_stop(s) == 0);
	fake_tick_advance(x.f, 100 * MS);
	nothing_played(&x);
	fx_close(&x);

	fx_open(&x, true);                                  /* without the token */
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, BS) == (int)BS);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == 0);
	CHECK(datax_stream_stop(s) == 0);
	fake_tick_advance(x.f, 100 * MS);
	nothing_played(&x);
	fx_close(&x);
}

static void test_start_at_clears_idle_overflow(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	unsigned long long T, t1;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	/* an untimed run (it polls the status at start), then idle */
	CHECK(datax_stream_start(s) == 0);
	t1 = now(&x);
	fake_tick_advance(x.f, 2 * BS * FT);
	CHECK(recv_block(&x, s, &info, false) > t1 / FT);
	fake_tick_advance(x.f, (NB + 1) * BS * FT);       /* the drain completes */
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.overflow_detect);
	CHECK(st.overflows == 0);
	/* forced open with no DMA running: the sticky idle overflow */
	fake_tick_advance(x.f, 5 * MS);
	/* within 100 ms of the last poll */
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 2 * BS * FT + BS * FT / 2, 1 * MS);
	CHECK(recv_block(&x, s, &info, true) == T / FT + 1);
	CHECK(!info.discontinuous);
	msleep(150);                                       /* past the rate limit */
	CHECK(recv_block(&x, s, &info, false) == T / FT + 1 + BS);
	CHECK(!info.discontinuous);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.overflows == 0);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void same_t(bool written_first)
{
	struct fx x;
	struct datax_stream *rx, *tx;
	unsigned long long T;
	unsigned long fl0;
	size_t e0, ir = 0, it = 0;

	fx_open(&x, true);
	rx = stream(&x, DATAX_RX, 0x1, false);
	tx = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(tx, NB * BS) == (int)(NB * BS));
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 40);
	if (!written_first)
		CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(tx, T) == 0);
	if (written_first) {
		settle(&x);
		CHECK(sched_count(&x, CMD_WRITTEN) == 1);
	}
	fl0 = shim_get(&x.sh, &x.sh.flushes);
	/* no report-spacing conflict: the TX opening is unreported */
	CHECK(datax_stream_start_at(rx, T) == 0);
	CHECK(!strstr(lasterr(&x), "refeed skipped"));
	/* the refeed empties Tick's queue only when the TX open was in it */
	CHECK(shim_get(&x.sh, &x.sh.flushes) == fl0 + (written_first ? 1 : 0));
	if (!written_first)
		CHECK(datax_internal_sched_pause(x.s, false) == 0);
	step_to(&x, T + 10 * FT, 1 * MS);
	CHECK(gate_execs(&x, e0, x.rxl, T, &ir, NULL) == 1);
	CHECK(gate_execs(&x, e0, x.txl, T, &it, NULL) == 1);
	CHECK(ir < it);                    /* RX's gate reached Tick first */
	CHECK(events(&x.ev, DATAX_EVT_LATE_COMMAND, NULL) == 0);
	CHECK(datax_stream_stop(rx) == 0);
	(void)datax_stream_stop(tx);
	fx_close(&x);
}

static void test_same_t_rx_open_before_tx(void)
{
	same_t(true);
	same_t(false);
}

static void test_start_at_tx_cyclic(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_dma_tx_segment seg[16];
	struct fake_dma_op op[16];
	unsigned long long T;
	size_t n, dl;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, true);
	dl = fake_dma_log(x.d, 0, NULL, 0);
	CHECK(preload(s, BS) == (int)BS);
	CHECK(preload(s, BS) == -EBUSY);                 /* one cyclic buffer */
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	n = fake_dma_log(x.d, dl, op, 16) - dl;
	CHECK(n == 3 && op[0].kind == FAKE_DMA_OPEN && op[1].kind == FAKE_DMA_ENQUEUE &&
	      op[1].cyclic && op[2].kind == FAKE_DMA_START);
	step_to(&x, T + 3 * BS * FT, 1 * MS);
	n = fake_dma_tx_segments(x.d, seg, 16);
	CHECK(n >= 1);
	CHECK(seg[0].t_start == T && seg[0].first_frame == 0 && !seg[0].muted &&
	      !seg[0].underflow);
	CHECK(seg[n - 1].t_end >= T + 3 * BS * FT - FT);  /* still playing */
	for (size_t i = 0; i < n; i++)
		CHECK(seg[i].t_start >= T && !seg[i].underflow);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_start_at_tx_stream_preload(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_dma_tx_segment seg[16];
	struct datax_internal_stream_stats st;
	unsigned long long T;
	size_t n;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, NB * BS) == (int)(NB * BS));
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.blocks_done == NB);
	step_to(&x, T + (NB + 2) * BS * FT, 1 * MS);
	n = fake_dma_tx_segments(x.d, seg, 16);
	CHECK(n >= 1);
	CHECK(seg[0].t_start == T && seg[0].first_frame == 0 && !seg[0].muted &&
	      !seg[0].underflow);
	CHECK(seg[0].t_end == T + NB * BS * FT);          /* the whole preload played */
	/* recorded, not asserted: block streaming without a feed underflows */
	printf("tx stream preload: %zu segments, underflow after the preload: %s\n", n,
	       n > 1 && seg[1].underflow ? "yes" : "no");
	(void)datax_stream_stop(s);
	fx_close(&x);
}

static void test_tx_preload_rules(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_dma_op op[16];
	void *p = NULL;
	size_t dl, n;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, false);
	dl = fake_dma_log(x.d, 0, NULL, 0);
	/* fills block_count blocks, enqueues nothing; full: a partial count,
	 * then -EBUSY */
	CHECK(preload(s, NB * BS + 10) == (int)(NB * BS));
	CHECK(preload(s, 1) == -EBUSY);
	CHECK(strstr(lasterr(&x), "preload is full") != NULL);
	CHECK(fake_dma_log(x.d, dl, NULL, 0) == dl);
	/* zero-copy cannot preload */
	CHECK(datax_stream_get_block(s, &p, NULL, 0) == -EINVAL);
	/* an untimed start transmits it at once: enable, then the blocks */
	CHECK(datax_stream_start(s) == 0);
	n = fake_dma_log(x.d, dl, op, 16) - dl;
	CHECK(n == NB + 1 && op[0].kind == FAKE_DMA_START);
	for (unsigned int i = 0; i < NB; i++)
		CHECK(op[1 + i].kind == FAKE_DMA_ENQUEUE && op[1 + i].block == i);
	fake_tick_advance(x.f, (NB + 1) * BS * FT);
	CHECK(datax_stream_stop(s) == 0);
	datax_stream_destroy(s);

	/* a partial block only: nothing full to transmit at T */
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, 10) == 10);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -EINVAL);
	CHECK(strstr(lasterr(&x), "preload with send() first") != NULL);
	datax_stream_destroy(s);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -EINVAL);
	datax_stream_destroy(s);

	/* cyclic: one buffer; start() arms it (enqueue, then enable) */
	s = stream(&x, DATAX_TX, 0x1, true);
	dl = fake_dma_log(x.d, 0, NULL, 0);
	CHECK(preload(s, BS / 2) == (int)(BS / 2));
	CHECK(preload(s, 1) == -EBUSY);
	CHECK(datax_stream_start(s) == 0);
	n = fake_dma_log(x.d, dl, op, 16) - dl;
	CHECK(n == 3 && op[0].kind == FAKE_DMA_OPEN && op[0].block_bytes == BS / 2 * 4 &&
	      op[1].kind == FAKE_DMA_ENQUEUE && op[1].cyclic && op[2].kind == FAKE_DMA_START);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_sched_flush_fails_armed(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long t0;
	int16_t b[8];

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start_at(s, at(&x, 30)) == 0);
	settle(&x);
	CHECK(datax_sched_flush(x.s) == 0);
	t0 = mono_ms();
	CHECK(datax_stream_recv(s, b, 1, NULL, -1) == -ECANCELED);   /* no silent hang */
	CHECK(mono_ms() - t0 < 1000);
	CHECK(strstr(lasterr(&x), "cancelled") != NULL);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "FLUSHED"));
	CHECK(datax_stream_stop(s) == 0);
	CHECK(!gate_bit(&x, x.rxm));
	fx_close(&x);
}

static void test_sched_destroy_while_armed(void)
{
	struct fx x;
	struct datax_stream *s;
	int16_t b[8];

	/* the scheduler goes first: the open's hook runs on the live stream */
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start_at(s, at(&x, 30)) == 0);
	settle(&x);
	datax_sched_destroy(x.s);
	x.s = NULL;
	CHECK(datax_stream_recv(s, b, 1, NULL, -1) == -ECANCELED);
	CHECK(datax_stream_stop(s) == 0);                  /* no scheduler: still forced open */
	CHECK(!gate_bit(&x, x.rxm));
	datax_stream_destroy(s);
	fx_close(&x);

	/* the stream goes first: no hook may run on it afterwards */
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start_at(s, at(&x, 30)) == 0);
	settle(&x);
	datax_stream_destroy(s);
	datax_sched_destroy(x.s);
	x.s = NULL;
	fx_close(&x);

	/* a lazily created scheduler, and the context destroyed under an armed
	 * stream (it destroys the scheduler before the streams) */
	fx_open(&x, false);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start_at(s, at(&x, 30)) == 0);
	CHECK(datax_device_sched(x.dev) != NULL);
	settle(&x);
	fx_close(&x);
}


static void test_stop_armed_tx_bounded(void)
{
	struct fx x;
	struct datax_stream *s;
	struct clockthr c;
	unsigned long long t0;

	fx_open(&x, true);
	fake_dma_set_wait_ms(x.d, 5000);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, NB * BS + 10) == (int)(NB * BS));
	CHECK(datax_stream_start_at(s, at(&x, 2000)) == 0);
	settle(&x);
	c.f = x.f;
	c.stop = 0;
	CHECK(pthread_create(&c.t, NULL, clock_main, &c) == 0);
	msleep(20);
	/* armed, never opened: the DMA is cancelled before the gate opens */
	t0 = mono_ms();
	CHECK(datax_stream_stop(s) == 0);
	CHECK(mono_ms() - t0 < 1000);
	msleep(20);                        /* the forced-open gate, the clock running */
	c.stop = 1;
	pthread_join(c.t, NULL);
	CHECK(!gate_bit(&x, x.txm));
	nothing_played(&x);
	fx_close(&x);
}

static void test_start_at_tx_failed_commit_transmits_nothing(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, NB * BS) == (int)(NB * BS));
	/* the guard check passes at step 4; the commit's time read, three
	 * accesses later at 2 ms each, is inside the margin */
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	T = at(&x, 15);
	fake_tick_set_access_cost(x.f, 2 * MS);
	CHECK(datax_stream_start_at(s, T) == DATAX_ERR_LATE);
	fake_tick_set_access_cost(x.f, 0);
	CHECK(strstr(lasterr(&x), "guard margin") != NULL);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	CHECK(!gate_bit(&x, x.txm) && !gate_bit(&x, x.txl));   /* forced open again */
	step_to(&x, T + (NB + 2) * BS * FT, 1 * MS);
	nothing_played(&x);
	fx_close(&x);
}

static void test_held_slot_tx_open_fails(void)
{
	struct fx x;
	struct datax_stream *rx, *tx;
	unsigned long long T;
	int16_t b[BS * 2];
	unsigned long d0;
	long g;

	/* the same-T loopback setup behind an earlier unacked reported GPIO:
	 * the held RX opening blocks Tick's queue, so the unreported TX opening
	 * behind it runs late with no record and no late_count rise */
	fx_open(&x, true);
	rx = stream(&x, DATAX_RX, 0x1, false);
	tx = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(tx, NB * BS) == (int)(NB * BS));
	T = at(&x, 40);
	g = datax_sched_queue_gpio(x.s, T - 5 * MS, 0x2, 0x2);
	CHECK(g > 0 && datax_sched_report(x.s, g) == 0 && datax_sched_commit(x.s) == 0);
	d0 = shim_get(&x.sh, &x.sh.desc);
	CHECK(datax_stream_start_at(tx, T) == 0);
	CHECK(datax_stream_start_at(rx, T) == 0);
	WAIT_FOR(shim_get(&x.sh, &x.sh.desc) >= d0 + 7 && !sched_count(&x, CMD_PENDING));
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_advance(x.f, T + 5 * MS - now(&x));
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(events(&x.ev, DATAX_EVT_LATE_COMMAND, "held completion slot"));
	CHECK(preload(tx, 1) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "held completion slot") != NULL);
	CHECK(datax_stream_recv(rx, b, BS, NULL, 2000) == DATAX_ERR_LATE);
	CHECK(strstr(lasterr(&x), "poller lagged") != NULL);
	CHECK(datax_stream_stop(rx) == 0);
	CHECK(datax_stream_stop(tx) == 0);
	fx_close(&x);
}

static void test_stop_after_tx_open_ran_no_flush(void)
{
	struct fx x;
	struct datax_stream *s;
	struct clockthr c;
	unsigned long long T, tg;
	unsigned long fl0;
	size_t e0, it = 0;
	bool ok = false;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(s, NB * BS) == (int)(NB * BS));
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 30);
	tg = T + 20 * MS;
	CHECK(datax_stream_start_at(s, T) == 0);
	CHECK(datax_sched_queue_gpio(x.s, tg, 0x2, 0x2) > 0 && datax_sched_commit(x.s) == 0);
	settle(&x);
	/* the open runs; the poller is paused, so it is still WRITTEN */
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_advance(x.f, T + 1 * MS - now(&x));
	CHECK(gate_execs(&x, e0, x.txl, T, &it, NULL) == 1);
	CHECK(sched_count(&x, CMD_WRITTEN) == 2);
	fl0 = shim_get(&x.sh, &x.sh.flushes);
	c.f = x.f;
	c.stop = 0;
	CHECK(pthread_create(&c.t, NULL, clock_main, &c) == 0);
	CHECK(datax_stream_stop(s) == 0);          /* it ran: the preload drains */
	c.stop = 1;
	pthread_join(c.t, NULL);
	CHECK(shim_get(&x.sh, &x.sh.flushes) == fl0);   /* no flush for an open that ran */
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	step_to(&x, tg + 1 * MS, 1 * MS);
	CHECK(gpio_execs(&x, e0, tg, &ok) == 1 && ok);
	CHECK(events(&x.ev, DATAX_EVT_LATE_COMMAND, NULL) == 0);
	fx_close(&x);
}

struct cbrec {
	struct datax_stream *s;
	pthread_mutex_t m;
	int done, stop_rc;
	char stop_err[256], destroy_err[256];
	struct datax_context *ctx;
};
static struct cbrec g_cb;

static void destroy_cb(const struct datax_event *evt, void *user)
{
	struct cbrec *r = user;

	if (evt->type != DATAX_EVT_LATE_COMMAND)
		return;
	pthread_mutex_lock(&r->m);
	if (!r->done) {
		r->stop_rc = datax_stream_stop(r->s);
		snprintf(r->stop_err, sizeof(r->stop_err), "%s",
			 datax_context_get_last_error(r->ctx));
		datax_stream_destroy(r->s);
		snprintf(r->destroy_err, sizeof(r->destroy_err), "%s",
			 datax_context_get_last_error(r->ctx));
		r->done = 1;
	}
	pthread_mutex_unlock(&r->m);
}

static int cb_done(void)
{
	int d;

	pthread_mutex_lock(&g_cb.m);
	d = g_cb.done;
	pthread_mutex_unlock(&g_cb.m);
	return d;
}

static void test_destroy_from_event_callback_refused(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long T;
	int16_t b[8];

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	memset(&g_cb, 0, sizeof(g_cb));
	CHECK(pthread_mutex_init(&g_cb.m, NULL) == 0);
	g_cb.s = s;
	g_cb.ctx = x.ctx;
	CHECK(datax_context_set_event_callback(x.ctx, destroy_cb, &g_cb) == 0);
	/* a host-late open raises the event on the poller thread */
	T = at(&x, 30);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	CHECK(datax_stream_start_at(s, T) == 0);
	fake_tick_advance(x.f, T - 5 * MS - now(&x));
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	WAIT_FOR(cb_done());
	CHECK(g_cb.stop_rc == -EDEADLK);
	CHECK(strstr(g_cb.stop_err, "stream_stop from the scheduler's poller thread") != NULL);
	CHECK(strstr(g_cb.destroy_err, "stream_destroy from the scheduler's poller thread") !=
	      NULL);
	CHECK(datax_context_set_event_callback(x.ctx, on_event, &x.ev) == 0);
	/* the stream is still valid: it reports its failure and stops */
	CHECK(datax_stream_recv(s, b, 1, NULL, 1000) == DATAX_ERR_LATE);
	CHECK(datax_stream_stop(s) == 0);
	datax_stream_destroy(s);
	pthread_mutex_destroy(&g_cb.m);
	fx_close(&x);
}


/* ---- Task 4: RX block timestamps -------------------------------------------- */

#define VEC_PATH DATAX_TAG_VECTORS

static const char *vkey(const char *line, const char *key)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\": ", key);
	p = strstr(line, pat);
	CHECK(p != NULL);
	return p + strlen(pat);
}

static unsigned long long vnum(const char *line, const char *key)
{
	return strtoull(vkey(line, key), NULL, 10);
}

/* The golden headers of the vendored Tick codec: positives parse to their
 * inputs (flags carry gate << 4), negatives are -EINVAL, and a short buffer
 * too. */
static void test_tag_vectors(void)
{
	FILE *fp = fopen(VEC_PATH, "r");
	char line[1024];
	unsigned int pos = 0, neg = 0;

	CHECK(fp != NULL);
	while (fgets(line, sizeof(line), fp)) {
		const char *hex;
		uint8_t b[32];
		struct datax_tag t;
		bool positive = strstr(line, "\"positive\": true") != NULL;

		if (!positive && !strstr(line, "\"positive\": false"))
			continue;
		hex = vkey(line, "bytes");
		CHECK(*hex == '"');
		for (unsigned int i = 0; i < 32; i++) {
			unsigned int v;

			CHECK(sscanf(hex + 1 + 2 * i, "%2x", &v) == 1);
			b[i] = (uint8_t)v;
		}
		if (!positive) {
			CHECK(datax_tag_parse(b, 32, &t) == -EINVAL);
			CHECK(datax_internal_tag_parse(b, 32, &t) == -EINVAL);
			neg++;
			continue;
		}
		CHECK(datax_tag_parse(b, 32, &t) == 0);
		CHECK(t.magic == DATAX_TAG_MAGIC && t.version == 1 && t.header_words == 8);
		CHECK(t.capture_tick == vnum(line, "capture_tick"));
		CHECK(t.flags == vnum(line, "flags"));
		CHECK(DATAX_TAG_GATE(t.flags) == vnum(line, "gate"));
		CHECK(t.seq == vnum(line, "seq") && t.target == vnum(line, "target"));
		CHECK(t.command == vnum(line, "cmd") && t.tick_hz == vnum(line, "tick_hz"));
		CHECK(t.burst == vnum(line, "burst"));
		CHECK(datax_tag_parse(b, 31, &t) == -EINVAL);
		pos++;
	}
	fclose(fp);
	CHECK(pos >= 6 && neg >= 2);
	CHECK(datax_tag_parse(NULL, 32, &(struct datax_tag){0}) == -EINVAL);
}

/* The anchoring rule: gate 0's valid, fresh, on-time tag only. */
static void test_mode_bit_tag_ignored(void)
{
	struct datax_tag t = { .magic = DATAX_TAG_MAGIC, .version = 1, .header_words = 8,
			       .capture_tick = 1000, .tick_hz = HZ,
			       .flags = DATAX_TAG_VALID };
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T;

	CHECK(datax_tag_anchors(&t, 0, HZ));
	CHECK(!datax_tag_anchors(&t, 0, 0));             /* the Tick's hz must match */
	t.tick_hz = 0;
	CHECK(datax_tag_anchors(&t, 0, HZ));             /* hz 0: unspecified */
	t.tick_hz = HZ + 1;
	CHECK(!datax_tag_anchors(&t, 0, HZ));
	t.tick_hz = HZ;
	t.flags = DATAX_TAG_VALID | (2u << 4) | DATAX_TAG_LATE_EXECUTED;   /* the mode bit's */
	CHECK(!datax_tag_anchors(&t, 0, HZ));
	t.flags = DATAX_TAG_VALID | (2u << 4);                              /* gate 2 */
	CHECK(!datax_tag_anchors(&t, 0, HZ));
	CHECK(datax_tag_anchors(&t, 2, HZ));                                /* only by the map */
	t.flags = DATAX_TAG_VALID | DATAX_TAG_STALE;
	CHECK(!datax_tag_anchors(&t, 0, HZ));
	t.flags = DATAX_TAG_VALID | DATAX_TAG_LATE_EXECUTED;
	CHECK(!datax_tag_anchors(&t, 0, HZ));
	t.flags = 0;
	CHECK(!datax_tag_anchors(&t, 0, HZ));
	CHECK(!datax_tag_anchors(&t, -1, HZ));

	/* end to end: the time is the release's, not the mode-bit write's */
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 2 * BS * FT, 1 * MS);
	CHECK(datax_stream_recv(s, (int16_t[BS * 2]){0}, 1, &info, 3000) == 1);
	CHECK(info.timestamp_ticks == T + 9 * FT);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_frames_to_ticks_no_overflow(void)
{
	static const struct { unsigned long long fs, hz; } rates[] = {
		{ 1000000ULL, HZ }, { FS_3072, HZ }, { 245760000ULL, HZ },
		{ 61440000ULL, 245760000ULL },
	};
	static const unsigned long long ns[] = { 0, 1, 2, 3, 255, 4097, 1ULL << 32,
		(1ULL << 32) + 12345, 1ULL << 40, (1ULL << 50) + 7, 1ULL << 50 };

	for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
		for (size_t i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
			unsigned __int128 want = ((unsigned __int128)ns[i] * rates[r].hz +
						  rates[r].fs / 2) / rates[r].fs;

			CHECK(datax_frames_to_ticks(ns[i], rates[r].fs, rates[r].hz) ==
			      (unsigned long long)want);
			CHECK(datax_internal_frames_to_ticks(ns[i], rates[r].fs, rates[r].hz) ==
			      (unsigned long long)want);
		}
	CHECK(datax_frames_to_ticks(5, 1000000, HZ) == 500);       /* exact */
	CHECK(datax_frames_to_ticks(1, FS_3072, HZ) == 3);         /* 3.255 */
	CHECK(datax_frames_to_ticks(2, FS_3072, HZ) == 7);         /* 6.510 */
	CHECK(datax_frames_to_ticks(0, 0, HZ) == 0);               /* no division by zero */
}

/* Receive count frames of `nch` channels in one call; checks frames are
 * consecutive; returns the first frame's index. */
static uint64_t recv_n(struct datax_stream *s, size_t count, unsigned int nch,
		       struct datax_block_info *info)
{
	static int16_t buf[BS * 2 * 4];
	uint64_t first;

	CHECK(datax_stream_recv(s, buf, count, info, 3000) == (int)count);
	first = fake_dma_frame_index(&buf[0]);
	for (size_t k = 0; k < count; k++)
		CHECK(fake_dma_frame_index(&buf[2 * nch * k]) == first + k);
	return first;
}

static void test_block0_tag_parsed_stripped(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x3, false);               /* 2 channels: 4 frames */
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 3 * BS * FT, 1 * MS);
	/* one cpack drop plus the strip */
	CHECK(recv_n(s, BS - 4, 2, &info) == T / FT + 5);
	CHECK(info.timestamp_ticks == T + 5 * FT);
	CHECK(info.valid_samples == BS - 4);
	CHECK(!info.discontinuous);
	CHECK(recv_n(s, BS, 2, &info) == T / FT + 1 + BS);
	CHECK(info.timestamp_ticks == T + (1 + BS) * FT);
	CHECK(info.valid_samples == BS && !info.discontinuous);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.tag_errors == 0);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_strip_by_channel_count(void)
{
	static const unsigned int mask[] = { 0x1, 0x3, 0x7, 0xf };
	static const unsigned int want[] = { 8, 4, 3, 2 };

	for (unsigned int i = 0; i < 4; i++) {
		struct fx x;
		struct datax_stream *s;
		struct datax_block_info info;
		unsigned long long T;

		fx_open(&x, true);
		s = stream(&x, DATAX_RX, mask[i], false);
		T = at(&x, 20);
		CHECK(datax_stream_start_at(s, T) == 0);
		step_to(&x, T + 3 * BS * FT, 1 * MS);
		CHECK(recv_n(s, BS - want[i], i + 1, &info) == T / FT + 1 + want[i]);
		CHECK(info.valid_samples == BS - want[i]);
		CHECK(info.timestamp_ticks == T + (1 + want[i]) * FT);
		CHECK(datax_stream_stop(s) == 0);
		fx_close(&x);
	}
}

/* Blocks 0..3 of a start_at at the given rate: block b's first delivered
 * sample is released sample 1 + b * BS (+ strip for b = 0). */
static void derived(unsigned long long fs)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T, n;

	g_fs = fs;
	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20) + 7;                                  /* not frame aligned */
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 8 * BS * HZ / fs, 1 * MS);
	recv_n(s, BS - 8, 1, &info);
	n = 1 + 8;
	CHECK(info.timestamp_ticks == T + datax_internal_frames_to_ticks(n, fs, HZ));
	for (unsigned int b = 1; b < 4; b++) {
		recv_n(s, BS, 1, &info);
		n = 1 + b * BS;
		CHECK(info.valid_samples == BS && !info.discontinuous);
		CHECK(info.timestamp_ticks == T + datax_internal_frames_to_ticks(n, fs, HZ));
	}
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
	g_fs = FS;
}

static void test_derived_timestamps(void)
{
	derived(FS);                       /* exact: 100 ticks a frame */
	derived(FS_3072);                  /* rounded */
}

static void test_overflow_zeroes_timestamps(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 2 * BS * FT, 1 * MS);
	recv_n(s, BS - 8, 1, &info);
	CHECK(info.timestamp_ticks == T + 9 * FT && !info.discontinuous);
	fake_dma_inject_overflow(x.d, 2);
	step_to(&x, now(&x) + 4 * BS * FT, 1 * MS);
	msleep(150);                                  /* past the status rate limit */
	recv_n(s, BS, 1, &info);
	CHECK(info.discontinuous && info.timestamp_ticks == 0);
	recv_n(s, BS, 1, &info);                      /* sticky: no new overflow */
	CHECK(info.discontinuous && info.timestamp_ticks == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.overflows == 1);
	CHECK(datax_stream_stop(s) == 0);

	/* the next start_at anchors again */
	step_to(&x, now(&x) + 5 * MS, 1 * MS);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 3 * BS * FT, 1 * MS);
	recv_n(s, BS - 8, 1, &info);
	CHECK(!info.discontinuous && info.timestamp_ticks == T + 9 * FT);
	recv_n(s, BS, 1, &info);
	CHECK(!info.discontinuous && info.timestamp_ticks == T + (1 + BS) * FT);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

/* A tag that does not parse: not stripped, unanchored, counted, warned. */
static void test_tag_bad_magic_unanchored(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	fake_dma_corrupt_next_tag(x.d, FAKE_DMA_TAG_BAD_MAGIC);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 3 * BS * FT, 1 * MS);
	{
		static int16_t raw[BS * 2];

		/* the whole block, the tag still in its first 8 frames */
		CHECK(datax_stream_recv(s, raw, BS, &info, 3000) == (int)BS);
		CHECK(raw[2] == 1);                                   /* the tag's version word */
		CHECK(fake_dma_frame_index(&raw[2 * 8]) == T / FT + 9);
	}
	CHECK(info.timestamp_ticks == 0 && info.valid_samples == BS && !info.discontinuous);
	recv_n(s, BS, 1, &info);
	CHECK(info.timestamp_ticks == 0 && !info.discontinuous);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.tag_errors == 1);
	CHECK(strstr(lasterr(&x), "tag") != NULL);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

/* A tag that parses but is not VALID: stripped, unanchored, counted. */
static void test_tag_flags_rejected(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	fake_dma_corrupt_next_tag(x.d, FAKE_DMA_TAG_BAD_FLAGS);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 3 * BS * FT, 1 * MS);
	CHECK(recv_n(s, BS - 8, 1, &info) == T / FT + 9);
	CHECK(info.timestamp_ticks == 0 && info.valid_samples == BS - 8 && !info.discontinuous);
	recv_n(s, BS, 1, &info);
	CHECK(info.timestamp_ticks == 0 && !info.discontinuous);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.tag_errors == 1);
	CHECK(strstr(lasterr(&x), "tag") != NULL);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_untimed_ts_zero(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	void *p;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start(s) == 0);
	fake_tick_advance(x.f, 3 * BS * FT);
	recv_n(s, BS, 1, &info);
	CHECK(info.timestamp_ticks == 0 && info.valid_samples == BS);
	CHECK(datax_stream_stop(s) == 0);
	datax_stream_destroy(s);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_stream_start(s) == 0);
	fake_tick_advance(x.f, 3 * BS * FT);
	CHECK(datax_stream_get_block(s, &p, &info, 3000) == 0);
	CHECK(info.timestamp_ticks == 0 && info.valid_samples == BS);
	CHECK(datax_stream_put_block(s, p) == 0);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_zero_copy_block0(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T;
	void *p0, *p1;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 4 * BS * FT, 1 * MS);
	CHECK(datax_stream_get_block(s, &p0, &info, 3000) == 0);
	CHECK(fake_dma_frame_index(p0) == T / FT + 9);       /* past the tag */
	CHECK(info.valid_samples == BS - 8 && info.timestamp_ticks == T + 9 * FT);
	CHECK(!info.discontinuous);
	CHECK(datax_stream_get_block(s, &p1, &info, 3000) == 0);
	CHECK(fake_dma_frame_index(p1) == T / FT + 1 + BS);
	CHECK(info.valid_samples == BS && info.timestamp_ticks == T + (1 + BS) * FT);
	CHECK(datax_stream_put_block(s, p1) == -EINVAL);     /* lend order */
	CHECK(datax_stream_put_block(s, p0) == 0);           /* the stripped pointer */
	CHECK(datax_stream_put_block(s, p1) == 0);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_soft_offset_timestamps(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	const unsigned long long off = 7000000000ULL;
	unsigned long long T;

	fx_open(&x, true);
	atomic_store(&x.dev->time_off, off);                /* as a soft alignment leaves it */
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20) + off;                               /* API time */
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T - off + 3 * BS * FT, 1 * MS);
	recv_n(s, BS - 8, 1, &info);
	CHECK(info.timestamp_ticks == T + 9 * FT);
	recv_n(s, BS, 1, &info);
	CHECK(info.timestamp_ticks == T + (1 + BS) * FT);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

/* ---- queue_stream_gate (P3b Task 5, #15) ---------------------------------- */

static void gate_completion_ok(struct fx *x, long seq, unsigned long long req)
{
	struct datax_completion c;

	CHECK(datax_sched_wait(x->s, seq, 2000, &c) == 0);
	CHECK(c.seq == seq && c.status == DATAX_COMPLETION_OK);
	CHECK(c.requested_ticks == req && c.executed_ticks == req);
}

static void test_gate_tx_burst(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_dma_tx_segment seg[16];
	unsigned long long T;
	size_t n, k = 0;
	long off, on;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, true);
	CHECK(preload(s, BS) == (int)BS);
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	off = datax_sched_queue_stream_gate(x.s, T + 20 * MS, s, false);
	on = datax_sched_queue_stream_gate(x.s, T + 40 * MS, s, true);
	CHECK(off > 0 && on > 0);
	CHECK(datax_sched_report(x.s, off) == 0 && datax_sched_report(x.s, on) == 0);
	CHECK(datax_sched_commit(x.s) == 0);
	step_to(&x, T + 60 * MS, 1 * MS);
	gate_completion_ok(&x, off, T + 20 * MS);
	gate_completion_ok(&x, on, T + 40 * MS);
	n = fake_dma_tx_segments(x.d, seg, 16);
	CHECK(n >= 3);
	CHECK(seg[0].t_start == T && !seg[0].muted && seg[0].first_frame == 0);
	CHECK(seg[0].t_end == T + 20 * MS);
	CHECK(seg[1].muted && seg[1].t_start == T + 20 * MS && seg[1].t_end == T + 40 * MS);
	/* muted, not paused: 40 ms of playback after T, modulo the cyclic length */
	CHECK(!seg[2].muted && !seg[2].underflow && seg[2].t_start == T + 40 * MS);
	CHECK(seg[2].first_frame % BS == (40 * MS / FT) % BS);
	for (size_t i = 0; i < n; i++)
		k += seg[i].underflow;
	CHECK(k == 0);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_gate_rx_enotsup(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long T;
	size_t nc;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	CHECK(datax_sched_queue_stream_gate(x.s, T + 20 * MS, s, false) == -ENOTSUP);
	CHECK(strstr(lasterr(&x), "RX gating needs a DMA with end-of-burst (tlast) on this image")
	      != NULL);
	nc = datax_device_sched(x.dev)->ncmds;
	CHECK(sched_count(&x, CMD_BUILD) == 0 && nc >= 1);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_gate_untimed_einval(void)
{
	struct fx x;
	struct datax_stream *s;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, true);
	CHECK(datax_sched_queue_stream_gate(x.s, at(&x, 50), s, false) == -EINVAL);
	CHECK(strstr(lasterr(&x), "stream is not in timed mode; use start_at") != NULL);
	CHECK(preload(s, BS) == (int)BS);
	CHECK(datax_stream_start(s) == 0);                    /* started, but untimed */
	CHECK(datax_sched_queue_stream_gate(x.s, at(&x, 50), s, false) == -EINVAL);
	CHECK(strstr(lasterr(&x), "stream is not in timed mode; use start_at") != NULL);
	CHECK(datax_sched_queue_stream_gate(x.s, at(&x, 50), NULL, false) == -EINVAL);
	CHECK(datax_sched_queue_stream_gate(NULL, at(&x, 50), s, false) == -EINVAL);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(sched_count(&x, CMD_BUILD) == 0);
	fx_close(&x);
}

static void test_gate_before_open_einval(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, true);
	CHECK(preload(s, BS) == (int)BS);
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	CHECK(datax_sched_queue_stream_gate(x.s, T - 1, s, false) == -EINVAL);
	CHECK(lasterr(&x) != NULL && strlen(lasterr(&x)) > 0);
	CHECK(sched_count(&x, CMD_BUILD) == 0);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_gate_other_device_einval(void)
{
	struct fx x;
	struct datax_context *c2 = NULL;
	struct datax_device *d2 = NULL;
	struct datax_stream *s2 = NULL;
	struct datax_stream_config c = { .direction = DATAX_TX, .channel_mask = 0x1,
					 .block_samples = BS, .block_count = NB, .cyclic = true };

	fx_open(&x, true);
	/* a stream of another context's device, this device's scheduler */
	CHECK(datax_context_create_from_uri(getenv("DATAX_TEST_URI"), &c2) == 0);
	CHECK(datax_context_get_device(c2, 0, &d2) == 0 && d2 != x.dev);
	CHECK(datax_stream_create(d2, &c, &s2) == 0);
	CHECK(datax_sched_queue_stream_gate(x.s, at(&x, 50), s2, false) == -EINVAL);
	CHECK(strstr(lasterr(&x), "another device") != NULL);
	datax_context_destroy(c2);
	fx_close(&x);
}

static void test_gate_no_cap_enotsup(void)
{
	struct fx x;
	struct datax_stream *s;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, true);
	x.dev->caps.flags &= ~(1u << DATAX_CAP_STREAM_GATE);
	CHECK(datax_sched_queue_stream_gate(x.s, at(&x, 50), s, false) == -ENOTSUP);
	CHECK(lasterr(&x) != NULL && strlen(lasterr(&x)) > 0);
	fx_close(&x);
}

static void test_gate_guard_margin_late(void)
{
	struct fx x;
	struct datax_stream *s;
	unsigned long long T;
	long g;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, true);
	CHECK(preload(s, BS) == (int)BS);
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T, 1 * MS);
	g = datax_sched_queue_stream_gate(x.s, now(&x) + 5 * MS, s, false);   /* inside the guard */
	CHECK(g > 0);
	CHECK(datax_sched_commit(x.s) == DATAX_ERR_LATE);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

static void test_gate_cancelled_by_stop(void)
{
	struct fx x;
	struct datax_stream *s;
	struct fake_dma_tx_segment seg[16];
	unsigned long long T, T2;
	size_t e0, n;
	long g;

	fx_open(&x, true);
	s = stream(&x, DATAX_TX, 0x1, true);
	CHECK(preload(s, BS) == (int)BS);
	e0 = fake_tick_exec_log(x.f, 0, NULL, 0);
	T = at(&x, 30);
	CHECK(datax_stream_start_at(s, T) == 0);
	g = datax_sched_queue_stream_gate(x.s, T + 20 * MS, s, true);
	CHECK(g > 0 && datax_sched_commit(x.s) == 0);
	settle(&x);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_device_sched(x.dev)->ncmds == 0);
	/* a later start_at on a fresh cyclic send: the old gate never fires */
	CHECK(preload(s, BS) == (int)BS);
	T2 = at(&x, 90);
	CHECK(datax_stream_start_at(s, T2) == 0);
	step_to(&x, T2 + 40 * MS, 1 * MS);
	CHECK(gate_execs(&x, e0, x.txl, T + 20 * MS, NULL, NULL) == 0);
	n = fake_dma_tx_segments(x.d, seg, 16);
	for (size_t i = 0; i < n; i++)
		if (seg[i].t_start >= T2)
			CHECK(!seg[i].muted || seg[i].t_end <= T2);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

/* ---- final review fixes ------------------------------------------------------ */

/* Fix 1: the commit's device-time read fails inside start_at (start_at's
 * own guard read is the first current_time_ticks access with the poller
 * paused, the commit's the second). The opening must not stay behind
 * uncommitted: destroying the stream (not started: stop() returns at once)
 * and then the scheduler and context must never run its hook on the freed
 * stream (ASan: heap-use-after-free in open_done). */
static void test_start_at_commit_fail_destroy(void)
{
	struct fx x;
	struct datax_stream *s;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_inject_error_nth(x.f, "current_time_ticks", -EIO, 2);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -EIO);
	CHECK(strstr(lasterr(&x), "commit: cannot read the device time") != NULL);
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	datax_stream_destroy(s);
	fx_close(&x);                       /* sched destroy flushes what is left */
}

/* Fix 1: after that failure nothing of the stream is left uncommitted, the
 * gate is forced open, and the next start_at is not refused -EBUSY. */
static void test_start_at_commit_fail_retry(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_inject_error_nth(x.f, "current_time_ticks", -EIO, 2);
	CHECK(datax_stream_start_at(s, at(&x, 50)) == -EIO);
	CHECK(strstr(lasterr(&x), "commit: cannot read the device time") != NULL);
	CHECK(sched_count(&x, CMD_BUILD) == 0);
	CHECK(!gate_bit(&x, x.rxm) && !gate_bit(&x, x.rxl));
	CHECK(!(attr_u(&x, "capture_tag_enable") >> x.rxl & 1u));
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 2 * BS * FT + BS * FT / 2, 1 * MS);
	CHECK(recv_block(&x, s, &info, true) == T / FT + 1);
	CHECK(info.timestamp_ticks == T + 9 * FT && !info.discontinuous);
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

/* Fix 2: on the legacy path a lost RX block (libiio no longer holds it:
 * -EPERM, re-armed as the newest) after block 0 makes the timestamps
 * unknown and the next block discontinuous, instead of labelling block 2
 * with block 1's time. */
static void test_lost_block_unknown_timestamps(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T;

	fx_open(&x, true);
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	step_to(&x, T + 3 * BS * FT + BS * FT / 2, 1 * MS);
	CHECK(recv_block(&x, s, &info, true) == T / FT + 1);
	CHECK(info.timestamp_ticks == T + 9 * FT && !info.discontinuous);
	/* block 1 is complete: take it behind the stream's back (the state a
	 * consumed legacy read leaves) */
	CHECK(s->cur == 1);
	CHECK(fake_dma_rx_ops.dequeue(x.d, 1, true) == 0);
	/* one block per recv: info describes the call's first block */
	CHECK(recv_block(&x, s, &info, false) == T / FT + 1 + 2 * BS);
	CHECK(info.discontinuous && info.timestamp_ticks == 0);
	step_to(&x, now(&x) + 2 * BS * FT, 1 * MS);
	CHECK(recv_block(&x, s, &info, false) == T / FT + 1 + 3 * BS);
	CHECK(info.discontinuous && info.timestamp_ticks == 0);   /* sticky */
	CHECK(datax_stream_stop(s) == 0);
	fx_close(&x);
}

/* Fix 3: tag attributes present but no STREAM_TIMESTAMP capability (tron's
 * bitmap 0x9f): start_at must not enable the capture tag, since nothing
 * would strip it. Block 0 carries samples from its first frame. */
static void test_no_timestamp_cap_no_tag(void)
{
	struct fx x;
	struct datax_stream *s;
	struct datax_block_info info;
	unsigned long long T;

	g_bitmap = 0x9f;
	fx_open(&x, true);
	g_bitmap = 0xdf;
	CHECK(x.dev->caps.tag_attrs);
	CHECK(!(x.dev->caps.flags & (1u << DATAX_CAP_TIMESTAMPS)));
	s = stream(&x, DATAX_RX, 0x1, false);
	T = at(&x, 20);
	CHECK(datax_stream_start_at(s, T) == 0);
	CHECK(!(attr_u(&x, "capture_tag_enable") >> x.rxl & 1u));
	step_to(&x, T + 2 * BS * FT + BS * FT / 2, 1 * MS);
	CHECK(recv_block(&x, s, &info, false) == T / FT + 1);   /* no tag frames */
	CHECK(info.valid_samples == BS && info.timestamp_ticks == 0);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(!(attr_u(&x, "capture_tag_enable") >> x.rxl & 1u));
	fx_close(&x);
}

/* The stream's level-bit STREAM_GATE commands in the table. */
static size_t gate_cmds(struct fx *x, unsigned int bit)
{
	struct datax_sched *s = datax_device_sched(x->dev);
	size_t n = 0;

	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->ncmds; i++)
		n += s->cmds[i].command_id == DATAX_TICK_COMMAND_ID_STREAM_GATE &&
		     s->cmds[i].target_id == (DATAX_TICK_TARGET_FAMILY_STREAM_GATE | bit);
	pthread_mutex_unlock(&s->lock);
	return n;
}

/* Fix 4: the same-T re-feed fails (flush_queue write error). start_at
 * returns the error naming the re-feed, and the stream is stopped and
 * undone: not started (a retry is not -EALREADY), its opening cancelled,
 * the RX gate forced open with the tag bit clear. */
static void test_start_at_refeed_fail_undone(void)
{
	struct fx x;
	struct datax_stream *rx, *tx;
	unsigned long long T;
	int16_t b[8];

	fx_open(&x, true);
	rx = stream(&x, DATAX_RX, 0x1, false);
	tx = stream(&x, DATAX_TX, 0x1, false);
	CHECK(preload(tx, NB * BS) == (int)(NB * BS));
	T = at(&x, 40);
	CHECK(datax_stream_start_at(tx, T) == 0);
	settle(&x);
	CHECK(sched_count(&x, CMD_WRITTEN) == 1);
	CHECK(datax_internal_sched_pause(x.s, true) == 0);
	fake_tick_inject_error(x.f, "flush_queue", -EIO);
	CHECK(datax_stream_start_at(rx, T) == -EIO);
	CHECK(strstr(lasterr(&x), "re-feed") != NULL);
	CHECK(gate_cmds(&x, x.rxl) == 0);
	CHECK(!gate_bit(&x, x.rxm) && !gate_bit(&x, x.rxl));
	CHECK(!(attr_u(&x, "capture_tag_enable") >> x.rxl & 1u));
	CHECK(datax_stream_recv(rx, b, 1, NULL, 0) == -EINVAL);   /* not started */
	CHECK(datax_internal_sched_pause(x.s, false) == 0);
	/* the retry re-feeds and starts */
	CHECK(datax_stream_start_at(rx, T) == 0);
	CHECK(datax_stream_stop(rx) == 0);
	(void)datax_stream_stop(tx);
	fx_close(&x);
}

static const struct { const char *name; void (*fn)(void); } tests[] = {
#define T_(n) { #n, n }
	T_(test_start_at_guard_margin_late),
	T_(test_gate_tx_burst),
	T_(test_gate_rx_enotsup),
	T_(test_gate_untimed_einval),
	T_(test_gate_before_open_einval),
	T_(test_gate_other_device_einval),
	T_(test_gate_no_cap_enotsup),
	T_(test_gate_guard_margin_late),
	T_(test_gate_cancelled_by_stop),
	T_(test_start_at_commit_fail_destroy), T_(test_start_at_commit_fail_retry),
	T_(test_lost_block_unknown_timestamps), T_(test_no_timestamp_cap_no_tag),
	T_(test_start_at_refeed_fail_undone),
	T_(test_start_at_order),
	T_(test_start_at_rx_first_frame_at_T),
	T_(test_start_at_late_at_dispatch_fails),
	T_(test_start_at_tx_late_on_submit),
	T_(test_start_at_tx_host_late),
	T_(test_held_slot_open_never_delivers_shifted),
	T_(test_refeed_keeps_near_edge),
	T_(test_stop_cancels_open),
	T_(test_cancel_keeps_others),
	T_(test_stop_armed_rx_bounded),
	T_(test_start_after_aborted_start_at_open),
	T_(test_start_forces_open),
	T_(test_start_no_map_no_tick_writes),
	T_(test_start_at_enotsup_without_map),
	T_(test_start_at_ealready),
	T_(test_start_at_refuses_uncommitted_batch),
	T_(test_start_at_report_spacing_restores_gate),
	T_(test_spacing_ignores_unreported),
	T_(test_start_at_tx_partial_mask_enotsup),
	T_(test_start_at_clears_idle_overflow),
	T_(test_same_t_rx_open_before_tx),
	T_(test_start_at_tx_cyclic),
	T_(test_start_at_tx_stream_preload),
	T_(test_tx_preload_rules),
	T_(test_sched_flush_fails_armed),
	T_(test_sched_destroy_while_armed),
	T_(test_stop_armed_tx_bounded),
	T_(test_start_at_tx_failed_commit_transmits_nothing),
	T_(test_held_slot_tx_open_fails),
	T_(test_stop_after_tx_open_ran_no_flush),
	T_(test_destroy_from_event_callback_refused),
	T_(test_tag_vectors),
	T_(test_mode_bit_tag_ignored),
	T_(test_frames_to_ticks_no_overflow),
	T_(test_block0_tag_parsed_stripped),
	T_(test_strip_by_channel_count),
	T_(test_derived_timestamps),
	T_(test_overflow_zeroes_timestamps),
	T_(test_tag_bad_magic_unanchored),
	T_(test_tag_flags_rejected),
	T_(test_untimed_ts_zero),
	T_(test_zero_copy_block0),
	T_(test_soft_offset_timestamps),
#undef T_
};

int main(int argc, char **argv)
{
	size_t n = sizeof(tests) / sizeof(tests[0]);

	if (argc == 2 && !strcmp(argv[1], "list")) {
		for (size_t i = 0; i < n; i++)
			printf("%s\n", tests[i].name);
		return 0;
	}
	for (size_t i = 0; argc == 2 && i < n; i++)
		if (!strcmp(argv[1], tests[i].name)) {
			tests[i].fn();
			printf("%s: ok\n", tests[i].name);
			return 0;
		}
	fprintf(stderr, "usage: test_stream_timed <name>|list\n");
	return 2;
}
