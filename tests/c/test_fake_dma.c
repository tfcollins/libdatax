// SPDX-License-Identifier: MIT
/*
 * The fake DMA endpoint (tests/c/fake_dma.h) driven directly through its
 * stream-seam ops, with the fake Tick's stream gate levels as the only gate.
 * No emulator. 1 MSPS against the Tick's 100 MHz is exactly 100 ticks per
 * frame; frame n has edge tick 100 n, and a gate change at tick T applies to
 * the first frame whose edge is >= T.
 *
 *   test_fake_dma [name]      run every test, or the one named
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "fake_dma.h"

#define F_DROP 0x2u
#define F_NOW  0x4u
#define HZ     100000000ULL
#define FB     4u                 /* one channel: int16 I, int16 Q */

struct rig {
	struct fake_tick *f;
	struct fake_dma *d;
};

static struct rig rig_new(uint64_t sample_hz, const char *tag_enable)
{
	struct rig r;
	struct fake_tick_config tc = { .capability_bitmap = 0xdf };
	struct fake_dma_config dc = fake_dma_default_config(sample_hz);

	r.f = fake_tick_new(&tc);
	CHECK(r.f != NULL);
	fake_tick_set_gate_map(r.f, "rx-dma,tx-dma,rx-mode,tx-mode");
	r.d = fake_dma_new(r.f, &dc);
	CHECK(r.d != NULL);
	if (tag_enable)
		CHECK(fake_tick_ops.write(r.f, "capture_tag_enable", tag_enable) == 0);
	return r;
}

static void rig_free(struct rig *r)
{
	fake_dma_free(r->d);
	fake_tick_free(r->f);
}

/* STREAM_GATE: bit `bit` to `level` at tick ts (a past ts runs at once with
 * flags F_NOW). Gate roles: 0 rx level, 1 tx level, 2 rx mode, 3 tx mode. */
static void gate(struct rig *r, uint32_t seq, uint64_t ts, uint32_t flags, uint32_t bit,
		 uint32_t level)
{
	char b[256];

	snprintf(b, sizeof(b),
		 "0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0,0,0,0",
		 1u | (48u << 16), 3u | ((0x200u | bit) << 16), (unsigned)(ts & 0xffffffffu),
		 (unsigned)(ts >> 32), flags, seq, bit, level);
	CHECK(fake_tick_ops.write(r->f, "command_descriptor", b) == 0);
}

static void to_tick(struct rig *r, uint64_t t)
{
	uint64_t now = fake_tick_now(r->f);

	CHECK(t >= now);
	fake_tick_advance(r->f, t - now);
}

/* Open a direction and (RX/TX) enqueue the first `queue` blocks. */
static void open_dir(const struct datax_stream_src_ops *o, struct rig *r, size_t frames,
		     unsigned int nblocks, unsigned int queue, bool cyclic)
{
	CHECK(o->open(r->d, frames * FB, nblocks, FB) == 0);
	for (unsigned int i = 0; i < queue; i++)
		CHECK(o->enqueue(r->d, i, cyclic) == 0);
}

/* The frame index in sample k of block i. */
static uint64_t frame(const struct datax_stream_src_ops *o, struct rig *r, unsigned int i,
		      unsigned int k)
{
	const uint8_t *p = o->data(r->d, i);

	CHECK(p != NULL);
	return fake_dma_frame_index(p + (size_t)k * FB);
}

static void expect_block(struct rig *r, unsigned int i, const uint64_t *want, unsigned int n)
{
	CHECK(fake_dma_rx_ops.dequeue(r->d, i, true) == 0);
	for (unsigned int k = 0; k < n; k++) {
		if (frame(&fake_dma_rx_ops, r, i, k) != want[k]) {
			fprintf(stderr, "block %u frame %u: got %llu want %llu\n", i, k,
				(unsigned long long)frame(&fake_dma_rx_ops, r, i, k),
				(unsigned long long)want[k]);
			exit(1);
		}
	}
}

static bool rx_ovf(struct rig *r)
{
	bool o = false, u = true;

	CHECK(fake_dma_rx_ops.status(r->d, &o, &u) == 0);
	CHECK(!u);
	return o;
}

static bool tx_unf(struct rig *r)
{
	bool o = true, u = false;

	CHECK(fake_dma_tx_ops.status(r->d, &o, &u) == 0);
	CHECK(!o);
	return u;
}

/* ---- the vectors ----------------------------------------------------------- */

struct vec {
	char name[32];
	struct fake_dma_tag_fields t;
	uint8_t bytes[32];
};

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

static bool vbool(const char *line, const char *key)
{
	return !strncmp(vkey(line, key), "true", 4);
}

/* Every positive case of capture_tags.json (one case per line). */
static unsigned int load_vectors(struct vec *out, unsigned int max)
{
	FILE *fp = fopen(DATAX_TAG_VECTORS, "r");
	char line[1024];
	unsigned int n = 0;

	CHECK(fp != NULL);
	while (fgets(line, sizeof(line), fp)) {
		const char *hex, *nm;
		struct vec *v;

		if (!strstr(line, "\"positive\": true"))
			continue;
		CHECK(n < max);
		v = &out[n++];
		nm = vkey(line, "name");
		CHECK(*nm == '"');
		sscanf(nm + 1, "%31[^\"]", v->name);
		v->t = (struct fake_dma_tag_fields){
			.capture_tick = vnum(line, "capture_tick"), .gate = (uint8_t)vnum(line, "gate"),
			.seq = (uint32_t)vnum(line, "seq"), .target = (uint16_t)vnum(line, "target"),
			.cmd = (uint16_t)vnum(line, "cmd"), .tick_hz = (uint32_t)vnum(line, "tick_hz"),
			.burst = (uint32_t)vnum(line, "burst"), .late = vbool(line, "late"),
			.stale = vbool(line, "stale"),
		};
		hex = vkey(line, "bytes");
		CHECK(*hex == '"');
		for (unsigned int i = 0; i < 32; i++) {
			unsigned int b;

			CHECK(sscanf(hex + 1 + 2 * i, "%2x", &b) == 1);
			v->bytes[i] = (uint8_t)b;
		}
	}
	fclose(fp);
	return n;
}

static const struct vec *vfind(const struct vec *v, unsigned int n, const char *name)
{
	for (unsigned int i = 0; i < n; i++)
		if (!strcmp(v[i].name, name))
			return &v[i];
	CHECK(!"vector not found");
	return NULL;
}

/* ---- tests ----------------------------------------------------------------- */

static void rx_frames_only_while_pass(void)
{
	struct rig r = rig_new(1000000, NULL);
	const uint64_t b0[] = { 11, 12, 13, 14 }, b1[] = { 15, 16, 17, 18 };

	open_dir(&fake_dma_rx_ops, &r, 4, 8, 8, false);
	gate(&r, 1, 0, F_DROP, 2, 1);                     /* rx mode: closed from tick 0 */
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	CHECK(rx_ovf(&r) == false);                        /* closed: nothing passed */
	gate(&r, 2, 1000, F_DROP, 0, 1);                  /* open: frames 10.. */
	gate(&r, 3, 2000, F_DROP, 0, 0);                  /* closed again: frames 20.. */
	to_tick(&r, 999);
	CHECK(fake_dma_rx_ops.dequeue(r.d, 0, true) == -EBUSY);   /* nothing stored */
	to_tick(&r, 2999);
	/* frames 10..19 pass; the first, 10, is dropped; 11..19 are stored */
	expect_block(&r, 0, b0, 4);
	expect_block(&r, 1, b1, 4);
	CHECK(fake_dma_rx_ops.dequeue(r.d, 2, true) == -EBUSY);   /* 19 only: partial */
	CHECK(fake_dma_rx_ops.dequeue(r.d, 0, true) == -EPERM);   /* already taken */
	CHECK(rx_ovf(&r) == false);                        /* a running DMA never idles */
	rig_free(&r);
}

static void rx_drops_first_after_gap(void)
{
	struct rig r = rig_new(1000000, NULL);
	const uint64_t b0[] = { 1, 2, 3, 4 }, b1[] = { 11, 12, 13, 14 }, b2[] = { 21, 22, 23, 24 };

	open_dir(&fake_dma_rx_ops, &r, 4, 8, 8, false);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	gate(&r, 1, 500, F_DROP, 2, 1);                   /* mode: closed from frame 5 */
	gate(&r, 2, 1000, F_DROP, 0, 1);                  /* open from frame 10 */
	gate(&r, 3, 1500, F_DROP, 0, 0);                  /* closed from frame 15 */
	gate(&r, 4, 2000, F_DROP, 0, 1);                  /* open from frame 20 */
	to_tick(&r, 2499);
	/* frame 0 is the first after the initial gap, 10 and 20 follow closures */
	expect_block(&r, 0, b0, 4);
	expect_block(&r, 1, b1, 4);
	expect_block(&r, 2, b2, 4);
	rig_free(&r);
}

static void release_tags_block0(void)
{
	struct vec v[16];
	unsigned int n = load_vectors(v, 16);
	const struct vec *on_time, *late;
	struct rig r;
	uint8_t buf[32];

	CHECK(n >= 6);
	/* the encoder reproduces every positive vector byte for byte */
	for (unsigned int i = 0; i < n; i++) {
		fake_dma_encode_tag(buf, &v[i].t);
		if (memcmp(buf, v[i].bytes, 32)) {
			fprintf(stderr, "vector %s differs\n", v[i].name);
			exit(1);
		}
	}
	on_time = vfind(v, n, "on_time_gate0");
	late = vfind(v, n, "late_executed");

	/* on_time_gate0 end to end: the release at 1e6 ticks tags block 0 */
	r = rig_new(1000000, "1");
	open_dir(&fake_dma_rx_ops, &r, 32, 4, 4, false);
	gate(&r, 99, 0, F_DROP, 2, 1);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	gate(&r, 7, 1000000, F_DROP, 0, 1);
	to_tick(&r, 1004000);
	CHECK(fake_dma_rx_ops.dequeue(r.d, 0, true) == 0);
	CHECK(!memcmp(fake_dma_rx_ops.data(r.d, 0), on_time->bytes, 32));
	/* released sample 0 (frame 10000) is dropped; stored frame k holds 10001 + k; the
	 * tag overwrote the first 8 (32 bytes), unstripped */
	CHECK(frame(&fake_dma_rx_ops, &r, 0, 8) == 10009);
	CHECK(frame(&fake_dma_rx_ops, &r, 0, 31) == 10032);
	CHECK(fake_dma_rx_ops.dequeue(r.d, 1, true) == -EBUSY);
	rig_free(&r);

	/* late_executed: the second release (burst 1), executed late at tick 2500;
	 * the partial block of the first release is delivered, tagged burst 0 */
	r = rig_new(1000000, "1");
	open_dir(&fake_dma_rx_ops, &r, 32, 4, 4, false);
	gate(&r, 99, 0, F_DROP, 2, 1);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	gate(&r, 1, 1000, F_DROP, 0, 1);
	gate(&r, 2, 1500, F_DROP, 0, 0);
	to_tick(&r, 2500);
	gate(&r, 8, 100, F_NOW, 0, 1);                    /* past ts: runs now, late */
	to_tick(&r, 5900);
	CHECK(fake_dma_rx_ops.dequeue(r.d, 0, true) == 0);   /* the partial one */
	memcpy(buf, fake_dma_rx_ops.data(r.d, 0), 32);
	CHECK(buf[0] == 'T' && buf[16] == 1 && buf[24] == 0x00 && buf[28] == 0);  /* seq 1, burst 0 */
	CHECK(frame(&fake_dma_rx_ops, &r, 0, 8) == 0);    /* 11..14 sit under the tag; zero padding after */
	CHECK(fake_dma_rx_ops.dequeue(r.d, 1, true) == 0);
	CHECK(!memcmp(fake_dma_rx_ops.data(r.d, 1), late->bytes, 32));
	rig_free(&r);

	/* injected faults: a wrong magic, a cleared VALID */
	for (int what = FAKE_DMA_TAG_BAD_MAGIC; what <= FAKE_DMA_TAG_BAD_FLAGS; what++) {
		r = rig_new(1000000, "1");
		open_dir(&fake_dma_rx_ops, &r, 32, 4, 4, false);
		gate(&r, 99, 0, F_DROP, 2, 1);
		CHECK(fake_dma_rx_ops.start(r.d) == 0);
		fake_dma_corrupt_next_tag(r.d, (enum fake_dma_tag_fault)what);
		gate(&r, 7, 1000, F_DROP, 0, 1);
		to_tick(&r, 5000);
		CHECK(fake_dma_rx_ops.dequeue(r.d, 0, true) == 0);
		memcpy(buf, fake_dma_rx_ops.data(r.d, 0), 32);
		if (what == FAKE_DMA_TAG_BAD_MAGIC)
			CHECK(buf[0] != 'T' && (buf[7] & 1));
		else
			CHECK(buf[0] == 'T' && !(buf[7] & 1));
		rig_free(&r);
	}
}

static void mode0_untagged(void)
{
	/* forced open (mode 0): level changes are no release, whatever the enable */
	struct rig r = rig_new(1000000, "1");
	uint64_t want[32];

	open_dir(&fake_dma_rx_ops, &r, 32, 4, 4, false);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	gate(&r, 1, 1000, F_DROP, 0, 1);
	gate(&r, 2, 1500, F_DROP, 0, 0);
	to_tick(&r, 4000);
	for (unsigned int k = 0; k < 32; k++)
		want[k] = k + 1;                          /* frame 0 only is dropped */
	expect_block(&r, 0, want, 32);
	rig_free(&r);

	/* mode 1 but the level bit is not in capture_tag_enable: untagged */
	r = rig_new(1000000, "2");
	open_dir(&fake_dma_rx_ops, &r, 32, 4, 4, false);
	gate(&r, 1, 0, F_DROP, 2, 1);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	gate(&r, 2, 1000, F_DROP, 0, 1);
	to_tick(&r, 5000);
	for (unsigned int k = 0; k < 32; k++)
		want[k] = k + 11;
	expect_block(&r, 0, want, 32);
	rig_free(&r);
}

static void overflow_drops_and_flags(void)
{
	struct rig r = rig_new(1000000, NULL);
	const uint64_t b0[] = { 1, 2, 3, 4 }, b1[] = { 8, 9, 10, 11 };

	open_dir(&fake_dma_rx_ops, &r, 4, 8, 8, false);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	CHECK(!rx_ovf(&r));                               /* frame 0 was dropped, not lost */
	to_tick(&r, 400);
	fake_dma_inject_overflow(r.d, 3);                 /* frames 5, 6, 7 are lost */
	CHECK(rx_ovf(&r));
	CHECK(!rx_ovf(&r));                               /* cleared by the read */
	to_tick(&r, 1299);
	CHECK(rx_ovf(&r));                                /* the three lost frames */
	expect_block(&r, 0, b0, 4);
	expect_block(&r, 1, b1, 4);
	rig_free(&r);

	/* a full ring drops too: the DMA has nowhere to put frames */
	r = rig_new(1000000, NULL);
	open_dir(&fake_dma_rx_ops, &r, 4, 2, 2, false);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	(void)rx_ovf(&r);
	to_tick(&r, 799);                                 /* frames 1..7 fill two blocks: 1..8 */
	CHECK(!rx_ovf(&r));
	to_tick(&r, 1299);                                /* nothing free: frames lost */
	CHECK(rx_ovf(&r));
	rig_free(&r);
}

static void idle_overflow_sticky(void)
{
	struct rig r = rig_new(1000000, NULL);

	open_dir(&fake_dma_rx_ops, &r, 4, 4, 4, false);
	gate(&r, 1, 0, F_DROP, 2, 1);                     /* closed */
	gate(&r, 2, 1000, F_DROP, 0, 1);                  /* open from frame 10 */
	to_tick(&r, 999);
	CHECK(!rx_ovf(&r));                               /* gate closed: no overflow */
	to_tick(&r, 2000);
	CHECK(rx_ovf(&r));                                /* passing, DMA not running */
	CHECK(!rx_ovf(&r));                               /* the read cleared it */
	to_tick(&r, 2200);                                /* sticky across calls that do not read */
	to_tick(&r, 2300);
	CHECK(rx_ovf(&r));
	CHECK(!rx_ovf(&r));
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	(void)rx_ovf(&r);                                 /* frames up to the start were idle */
	to_tick(&r, 2800);
	CHECK(!rx_ovf(&r));                               /* running: stored, not lost */
	CHECK(fake_dma_rx_ops.stop(r.d) == 0);
	to_tick(&r, 3300);
	CHECK(rx_ovf(&r));                                /* stopped: idle again */
	rig_free(&r);
}

static void tx_held_until_first_open(void)
{
	struct rig r = rig_new(1000000, NULL);
	struct fake_dma_tx_segment seg[4];

	open_dir(&fake_dma_tx_ops, &r, 8, 4, 2, false);
	gate(&r, 1, 0, F_DROP, 3, 1);                     /* tx mode: closed */
	CHECK(fake_dma_tx_ops.start(r.d) == 0);
	gate(&r, 2, 1000, F_DROP, 1, 1);                  /* first open */
	to_tick(&r, 999);
	CHECK(fake_dma_tx_segments(r.d, seg, 4) == 0);    /* nothing read yet */
	CHECK(fake_dma_tx_ops.dequeue(r.d, 0, true) == -EBUSY);
	CHECK(!tx_unf(&r));
	to_tick(&r, 2099);
	CHECK(fake_dma_tx_segments(r.d, seg, 4) == 1);
	CHECK(seg[0].t_start == 1000 && seg[0].t_end == 2100 && seg[0].first_frame == 0);
	CHECK(!seg[0].muted && !seg[0].underflow);
	CHECK(fake_dma_tx_ops.dequeue(r.d, 0, true) == 0);        /* 8 of 11 frames: block 0 done */
	CHECK(fake_dma_tx_ops.dequeue(r.d, 1, true) == -EBUSY);   /* block 1: 3 of 8 */
	rig_free(&r);
}

static void tx_mute_keeps_playing_cyclic(void)
{
	struct rig r = rig_new(1000000, NULL);
	struct fake_dma_tx_segment seg[8];

	open_dir(&fake_dma_tx_ops, &r, 8, 1, 1, true);
	gate(&r, 1, 0, F_DROP, 3, 1);
	CHECK(fake_dma_tx_ops.start(r.d) == 0);
	gate(&r, 2, 1000, F_DROP, 1, 1);                  /* play */
	gate(&r, 3, 1500, F_DROP, 1, 0);                  /* mute: reads continue */
	gate(&r, 4, 2000, F_DROP, 1, 1);                  /* play on, time aligned */
	to_tick(&r, 2499);
	CHECK(fake_dma_tx_segments(r.d, seg, 8) == 3);
	CHECK(seg[0].t_start == 1000 && seg[0].t_end == 1500 && seg[0].first_frame == 0 && !seg[0].muted);
	CHECK(seg[1].t_start == 1500 && seg[1].t_end == 2000 && seg[1].first_frame == 5 && seg[1].muted);
	CHECK(seg[2].t_start == 2000 && seg[2].t_end == 2500 && seg[2].first_frame == 10 && !seg[2].muted);
	CHECK(!seg[0].underflow && !seg[1].underflow && !seg[2].underflow);
	CHECK(!tx_unf(&r));                               /* a cyclic block never runs dry */
	CHECK(fake_dma_tx_ops.dequeue(r.d, 0, false) == 0);   /* taken back at once */
	rig_free(&r);
}

static void tx_underflow_segment(void)
{
	struct rig r = rig_new(1000000, NULL);
	struct fake_dma_tx_segment seg[4];

	open_dir(&fake_dma_tx_ops, &r, 8, 2, 1, false);
	CHECK(fake_dma_tx_ops.start(r.d) == 0);           /* forced open */
	to_tick(&r, 1199);
	CHECK(fake_dma_tx_segments(r.d, seg, 4) == 2);
	/* frame 0's edge came with the start; frames 1..8 play, then the FIFO is dry */
	CHECK(seg[0].t_start == 100 && seg[0].t_end == 900 && seg[0].first_frame == 0);
	CHECK(!seg[0].muted && !seg[0].underflow);
	CHECK(seg[1].t_start == 900 && seg[1].t_end == 1200 && seg[1].first_frame == 8);
	CHECK(seg[1].underflow);
	CHECK(tx_unf(&r));
	CHECK(!tx_unf(&r));
	CHECK(fake_dma_tx_ops.dequeue(r.d, 0, true) == 0);
	rig_free(&r);

	/* nothing enqueued yet is not an underflow: reading begins with the data */
	r = rig_new(1000000, NULL);
	CHECK(fake_dma_tx_ops.open(r.d, 8 * FB, 2, FB) == 0);
	CHECK(fake_dma_tx_ops.start(r.d) == 0);
	to_tick(&r, 1000);
	CHECK(fake_dma_tx_segments(r.d, seg, 4) == 0 && !tx_unf(&r));
	CHECK(fake_dma_tx_ops.enqueue(r.d, 0, false) == 0);
	to_tick(&r, 2499);
	CHECK(fake_dma_tx_segments(r.d, seg, 4) == 2);
	CHECK(seg[0].t_start == 1100 && seg[0].t_end == 1900 && seg[0].first_frame == 0);
	CHECK(seg[1].underflow && seg[1].t_start == 1900 && seg[1].first_frame == 8);
	rig_free(&r);
}

static void op_log(void)
{
	struct rig r = rig_new(1000000, NULL);
	struct fake_dma_op op[8];

	open_dir(&fake_dma_rx_ops, &r, 4, 4, 2, false);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	CHECK(fake_dma_rx_ops.stop(r.d) == 0);
	CHECK(fake_dma_tx_ops.open(r.d, 8 * FB, 2, FB) == 0);
	CHECK(fake_dma_tx_ops.enqueue(r.d, 1, true) == 0);
	CHECK(fake_dma_log(r.d, 0, op, 8) == 7);
	CHECK(op[0].kind == FAKE_DMA_OPEN && !op[0].tx && op[0].block_bytes == 4 * FB && op[0].nblocks == 4);
	CHECK(op[1].kind == FAKE_DMA_ENQUEUE && op[1].block == 0 && !op[1].cyclic);
	CHECK(op[2].kind == FAKE_DMA_ENQUEUE && op[2].block == 1);
	CHECK(op[3].kind == FAKE_DMA_START && !op[3].tx && op[4].kind == FAKE_DMA_STOP);
	CHECK(op[4].kind == FAKE_DMA_STOP && op[5].kind == FAKE_DMA_OPEN && op[5].tx);
	CHECK(op[6].kind == FAKE_DMA_ENQUEUE && op[6].tx && op[6].cyclic && op[6].block == 1);
	CHECK(fake_dma_log(r.d, 4, op, 8) == 7 && op[0].kind == FAKE_DMA_STOP);
	CHECK(fake_dma_rx_ops.enqueue(r.d, 0, false) == -EBUSY);  /* already queued */
	CHECK(fake_dma_rx_ops.enqueue(r.d, 9, false) == -EINVAL);
	CHECK(fake_dma_rx_ops.open(r.d, 4 * FB, 4, FB) == -EBUSY);
	rig_free(&r);
}

struct advancer {
	struct fake_tick *f;
};

static void *advance_later(void *arg)
{
	struct advancer *a = arg;

	nanosleep(&(struct timespec){ .tv_nsec = 10000000L }, NULL);
	fake_tick_advance(a->f, 2000);
	return NULL;
}

static void dequeue_waits_for_the_clock(void)
{
	struct rig r = rig_new(1000000, NULL);
	struct advancer a = { r.f };
	pthread_t th;
	struct timespec t0, t1;
	long ms;

	open_dir(&fake_dma_rx_ops, &r, 4, 4, 4, false);
	CHECK(fake_dma_rx_ops.start(r.d) == 0);
	/* the clock stands still: a blocking dequeue gives up after about 50 ms */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(fake_dma_rx_ops.dequeue(r.d, 0, false) == -ETIMEDOUT);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
	CHECK(ms >= 45 && ms < 500);
	/* another thread moves the clock: the wait ends with the block */
	CHECK(pthread_create(&th, NULL, advance_later, &a) == 0);
	CHECK(fake_dma_rx_ops.dequeue(r.d, 0, false) == 0);
	pthread_join(th, NULL);
	rig_free(&r);
}

static void frame_clock_rounding(void)
{
	struct rig r = rig_new(30720000, NULL);
	uint64_t last = 0;

	CHECK(fake_dma_frame_tick(r.d, 0) == 0 && fake_dma_frame_tick(r.d, 1) == 3);
	CHECK(fake_dma_frame_tick(r.d, 3) == 10);         /* 9.77 rounds up */
	CHECK(fake_dma_frame_tick(r.d, 30720000) == HZ);
	for (uint64_t n = 1; n < 400; n++) {
		uint64_t t = fake_dma_frame_tick(r.d, n);

		CHECK(t >= last && fake_dma_frame_at(r.d, t) <= n);
		CHECK(fake_dma_frame_tick(r.d, fake_dma_frame_at(r.d, t)) >= t);
		last = t;
	}
	for (uint64_t t = 0; t < 700; t++) {
		uint64_t n = fake_dma_frame_at(r.d, t);

		CHECK(fake_dma_frame_tick(r.d, n) >= t);
		CHECK(n == 0 || fake_dma_frame_tick(r.d, n - 1) < t);
	}
	rig_free(&r);
	r = rig_new(1000000, NULL);
	CHECK(fake_dma_frame_tick(r.d, 7) == 700 && fake_dma_frame_at(r.d, 701) == 8);
	rig_free(&r);
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
		{ "rx_frames_only_while_pass", rx_frames_only_while_pass },
		{ "rx_drops_first_after_gap", rx_drops_first_after_gap },
		{ "release_tags_block0", release_tags_block0 },
		{ "mode0_untagged", mode0_untagged },
		{ "overflow_drops_and_flags", overflow_drops_and_flags },
		{ "idle_overflow_sticky", idle_overflow_sticky },
		{ "tx_held_until_first_open", tx_held_until_first_open },
		{ "tx_mute_keeps_playing_cyclic", tx_mute_keeps_playing_cyclic },
		{ "tx_underflow_segment", tx_underflow_segment },
		{ "op_log", op_log },
		{ "dequeue_waits_for_the_clock", dequeue_waits_for_the_clock },
		{ "frame_clock_rounding", frame_clock_rounding },
	};
	int ran = 0;

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (argc > 1 && strcmp(argv[1], tests[i].name))
			continue;
		tests[i].fn();
		printf("%s: ok\n", tests[i].name);
		ran++;
	}
	if (!ran) {
		fprintf(stderr, "no such test: %s\n", argc > 1 ? argv[1] : "");
		return 2;
	}
	return 0;
}
