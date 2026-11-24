// SPDX-License-Identifier: MIT
/*
 * Fake DMA endpoint (see fake_dma.h). It models only what the timed-stream
 * tests need of the fabric between the JESD link and the DMA:
 *   RTL  hdl/library/axi_timed_command_scheduler (stream gate levels, capture
 *        tag), and util_cpack2 dropping the first sample after a gap.
 * Time is lazy: the Tick's gate hook records every gate transition in a
 * history, and each call replays the sample clock up to fake_tick_now()
 * against that history, in segments of constant gate state.
 *
 * Locking: the gate hook runs with the Tick's lock held and takes the DMA's;
 * so the DMA never calls the Tick while holding its own lock, except through
 * the *_locked accessors inside the hook.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fake_dma.h"

#define TAG_BYTES   32u
#define TAG_MAGIC   0x4b434954u
#define TAG_VERSION 1u
#define TAG_VALID   0x1u
#define TAG_STALE   0x2u
#define TAG_LATE    0x4u
#define GATE_BITS   8u
#define POLL_MS     1
#define WAIT_MS     50

/* A gate transition: from tick t the levels are gate. A release carries the
 * capture header the first stored frame after it will start a block with. */
struct hist {
	uint64_t t;
	uint32_t gate;
	bool release;
	uint8_t tag[TAG_BYTES];
};

enum bstate { B_IDLE, B_QUEUED, B_DONE };

struct blk {
	uint8_t *data;
	enum bstate st;
	bool cyclic, tagged;
	uint8_t hdr[TAG_BYTES];
};

struct dir {
	bool open, running;
	size_t bb, fb, fpb;           /* block bytes, frame bytes, frames per block */
	unsigned int nblocks;
	struct blk *blk;
	unsigned int *fifo;           /* queued blocks, oldest first */
	unsigned int head, count;
	uint64_t n_next;              /* next frame to process */
	size_t hi;                    /* history entry in effect for n_next */
};

struct fake_dma {
	pthread_mutex_t lock;
	struct fake_tick *f;
	struct fake_dma_config cfg;
	uint64_t hz;                  /* tick rate */

	struct hist *hist;
	size_t nhist, caphist;

	struct dir rx, tx;
	/* RX */
	bool prev_pass, tag_pending, ovf;
	uint8_t pending_tag[TAG_BYTES];
	size_t rx_fill;               /* bytes stored in the head block */
	uint64_t drop_pending;
	uint32_t releases, last_tag_count;
	enum fake_dma_tag_fault fault;
	/* TX */
	bool reading, have_data, unf;
	uint64_t cons;                /* frames consumed since reading began */
	size_t tx_off;                /* frames consumed of the head block */
	struct fake_dma_tx_segment *segs;
	size_t nsegs, capsegs;

	struct fake_dma_op *log;
	size_t nlog, caplog;
};

/* ---- the sample clock ---------------------------------------------------- */

static uint64_t edge(const struct fake_dma *d, uint64_t n)
{
	uint64_t sr = d->cfg.sample_hz, q = n / sr, r = n % sr;

	return q * d->hz + (r * d->hz + sr / 2) / sr;
}

static uint64_t edge_ge(const struct fake_dma *d, uint64_t t)
{
	uint64_t sr = d->cfg.sample_hz;
	uint64_t n = (t / d->hz) * sr + ((t % d->hz) * sr) / d->hz;

	while (edge(d, n) < t)
		n++;
	while (n && edge(d, n - 1) >= t)
		n--;
	return n;
}

uint64_t fake_dma_frame_tick(const struct fake_dma *d, uint64_t n)
{
	return edge(d, n);
}

uint64_t fake_dma_frame_at(const struct fake_dma *d, uint64_t tick)
{
	return edge_ge(d, tick);
}

uint64_t fake_dma_frame_index(const void *frame)
{
	const uint8_t *p = frame;

	return (uint64_t)(p[0] | (p[1] << 8)) | ((uint64_t)(p[2] | (p[3] << 8)) << 16);
}

static bool pass(uint32_t gate, unsigned int level, unsigned int mode)
{
	return !((gate >> mode) & 1u) || ((gate >> level) & 1u);
}

/* ---- the capture header --------------------------------------------------- */

static void put_le(uint8_t *p, uint64_t v, unsigned int bytes)
{
	for (unsigned int i = 0; i < bytes; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

void fake_dma_encode_tag(uint8_t out[32], const struct fake_dma_tag_fields *t)
{
	unsigned int flags = TAG_VALID | ((unsigned int)(t->gate & 0xf) << 4);

	if (t->stale)
		flags |= TAG_STALE;
	if (t->late)
		flags |= TAG_LATE;
	/* magic, version u16, header_words u8, flags u8, tick u64, seq u32,
	 * target u16, command u16, tick_hz u32, burst u32 (capture.py). */
	put_le(out, TAG_MAGIC, 4);
	put_le(out + 4, TAG_VERSION, 2);
	out[6] = TAG_BYTES / 4;
	out[7] = (uint8_t)flags;
	put_le(out + 8, t->capture_tick, 8);
	put_le(out + 16, t->seq, 4);
	put_le(out + 20, t->target, 2);
	put_le(out + 22, t->cmd, 2);
	put_le(out + 24, t->tick_hz, 4);
	put_le(out + 28, t->burst, 4);
}

/* ---- gate history (the Tick's gate hook, lock held) ------------------------ */

static void gate_hook(void *user, uint32_t old_level, uint32_t new_level, uint64_t ticks)
{
	struct fake_dma *d = user;
	struct hist h = { .t = ticks, .gate = new_level };
	unsigned int lv = d->cfg.rx_level, md = d->cfg.rx_mode;

	pthread_mutex_lock(&d->lock);
	if (d->nhist && ticks < d->hist[d->nhist - 1].t)
		h.t = d->hist[d->nhist - 1].t;
	if (!pass(old_level, lv, md) && pass(new_level, lv, md) && ((new_level >> md) & 1u) &&
	    ((fake_tick_tag_enable_locked(d->f) >> lv) & 1u)) {
		struct fake_tick_tag lt;
		struct fake_dma_tag_fields t;

		fake_tick_gate_tag_locked(d->f, lv, &lt);
		t = (struct fake_dma_tag_fields){
			.capture_tick = lt.ticks, .gate = (uint8_t)lv, .seq = lt.seq,
			.target = lt.target, .cmd = lt.cmd, .tick_hz = (uint32_t)d->hz,
			.burst = d->releases++, .late = lt.late,
			.stale = lt.count == d->last_tag_count,
		};
		d->last_tag_count = lt.count;
		fake_dma_encode_tag(h.tag, &t);
		if (d->fault == FAKE_DMA_TAG_BAD_MAGIC)
			h.tag[0] ^= 0xff;
		else if (d->fault == FAKE_DMA_TAG_BAD_FLAGS)
			h.tag[7] &= (uint8_t)~TAG_VALID;
		d->fault = 0;
		h.release = true;
	}
	if (d->nhist == d->caphist) {
		size_t cap = d->caphist ? 2 * d->caphist : 16;
		struct hist *n = realloc(d->hist, cap * sizeof(*n));

		if (!n)
			goto out;
		d->hist = n;
		d->caphist = cap;
	}
	d->hist[d->nhist++] = h;
out:
	pthread_mutex_unlock(&d->lock);
}

/* ---- RX ------------------------------------------------------------------- */

static void rx_complete(struct fake_dma *d)
{
	struct dir *x = &d->rx;
	struct blk *b = &x->blk[x->fifo[x->head]];

	if (d->rx_fill < x->bb)
		memset(b->data + d->rx_fill, 0, x->bb - d->rx_fill);
	if (b->tagged)
		memcpy(b->data, b->hdr, x->bb < TAG_BYTES ? x->bb : TAG_BYTES);
	b->st = B_DONE;
	x->head = (x->head + 1) % x->nblocks;
	x->count--;
	d->rx_fill = 0;
}

/* Store released frame n; false when there was no room for it (the rest of
 * its segment is lost too). */
static bool rx_store(struct fake_dma *d, uint64_t n)
{
	struct dir *x = &d->rx;
	struct blk *b;
	uint8_t *p;

	if (d->drop_pending) {
		d->drop_pending--;
		d->ovf = true;
		d->tag_pending = false;
		return true;
	}
	if (d->tag_pending && d->rx_fill && x->count)
		rx_complete(d);          /* a release starts a block of its own */
	if (!x->count) {
		d->ovf = true;
		d->tag_pending = false;
		return false;
	}
	b = &x->blk[x->fifo[x->head]];
	if (d->tag_pending) {
		memcpy(b->hdr, d->pending_tag, TAG_BYTES);
		b->tagged = true;
		d->tag_pending = false;
	}
	p = b->data + d->rx_fill;
	for (size_t c = 0; c < x->fb / 4; c++) {
		put_le(p + 4 * c, n & 0xffff, 2);
		put_le(p + 4 * c + 2, (n >> 16) & 0xffff, 2);
	}
	d->rx_fill += x->fb;
	if (d->rx_fill + x->fb > x->bb)
		rx_complete(d);
	return true;
}

static void rx_seg(struct fake_dma *d, uint64_t na, uint64_t nb, uint32_t gate)
{
	if (!pass(gate, d->cfg.rx_level, d->cfg.rx_mode)) {
		d->prev_pass = false;
		d->tag_pending = false;
		return;
	}
	if (!d->prev_pass) {
		na++;                    /* util_cpack2 drops the first sample after a gap */
		d->prev_pass = true;
	}
	if (na >= nb)
		return;
	if (!d->rx.running) {
		d->ovf = true;           /* forced open (or open) with no buffer running */
		d->tag_pending = false;
		return;
	}
	for (uint64_t n = na; n < nb; n++)
		if (!rx_store(d, n))
			return;
}

/* ---- TX ------------------------------------------------------------------- */

static void tx_add(struct fake_dma *d, uint64_t na, uint64_t nb, bool muted, bool underflow)
{
	uint64_t ts = edge(d, na), te = edge(d, nb);
	struct fake_dma_tx_segment *l = d->nsegs ? &d->segs[d->nsegs - 1] : NULL;

	if (l && l->muted == muted && l->underflow == underflow && l->t_end == ts) {
		l->t_end = te;
		return;
	}
	if (d->nsegs == d->capsegs) {
		size_t cap = d->capsegs ? 2 * d->capsegs : 16;
		struct fake_dma_tx_segment *n = realloc(d->segs, cap * sizeof(*n));

		if (!n)
			return;
		d->segs = n;
		d->capsegs = cap;
	}
	d->segs[d->nsegs++] = (struct fake_dma_tx_segment){
		.t_start = ts, .t_end = te, .first_frame = d->cons,
		.muted = muted, .underflow = underflow,
	};
}

static void tx_seg(struct fake_dma *d, uint64_t na, uint64_t nb, uint32_t gate)
{
	struct dir *x = &d->tx;
	bool open = pass(gate, d->cfg.tx_level, d->cfg.tx_mode);

	if (!x->running)
		return;
	if (!d->reading) {
		if (!open || !d->have_data)
			return;          /* held until the first pass */
		d->reading = true;
	}
	while (na < nb) {
		uint64_t take = nb - na;
		struct blk *b;

		if (!x->count) {
			tx_add(d, na, nb, !open, true);
			d->unf = true;
			return;
		}
		b = &x->blk[x->fifo[x->head]];
		if (!b->cyclic && take > x->fpb - d->tx_off)
			take = x->fpb - d->tx_off;
		tx_add(d, na, na + take, !open, false);
		d->cons += take;
		d->tx_off = (size_t)((d->tx_off + take) % x->fpb);
		if (!b->cyclic && !d->tx_off) {
			b->st = B_DONE;
			x->head = (x->head + 1) % x->nblocks;
			x->count--;
		}
		na += take;
	}
}

/* ---- catching up ------------------------------------------------------------ */

static void walk(struct fake_dma *d, bool tx, uint64_t n_end)
{
	struct dir *x = tx ? &d->tx : &d->rx;

	while (x->n_next < n_end) {
		uint64_t nb = n_end;

		if (x->hi + 1 < d->nhist) {
			uint64_t ne = edge_ge(d, d->hist[x->hi + 1].t);

			if (ne <= x->n_next) {       /* the next transition is in effect */
				x->hi++;
				if (!tx && d->hist[x->hi].release) {
					memcpy(d->pending_tag, d->hist[x->hi].tag, TAG_BYTES);
					d->tag_pending = true;
				}
				continue;
			}
			if (ne < nb)
				nb = ne;
		}
		if (tx)
			tx_seg(d, x->n_next, nb, d->hist[x->hi].gate);
		else
			rx_seg(d, x->n_next, nb, d->hist[x->hi].gate);
		x->n_next = nb;
	}
}

/* Process every frame whose edge is <= now. */
static void catch_up(struct fake_dma *d, uint64_t now)
{
	uint64_t n_end = now == UINT64_MAX ? edge_ge(d, now) + 1 : edge_ge(d, now + 1);

	walk(d, false, n_end);
	walk(d, true, n_end);
}

/* Read the clock first (the Tick's lock), then take ours. */
static struct fake_dma *enter(void *h)
{
	struct fake_dma *d = h;
	uint64_t now = fake_tick_now(d->f);

	pthread_mutex_lock(&d->lock);
	catch_up(d, now);
	return d;
}

static void leave(struct fake_dma *d)
{
	pthread_mutex_unlock(&d->lock);
}

static void log_op(struct fake_dma *d, struct fake_dma_op op)
{
	if (d->nlog == d->caplog) {
		size_t cap = d->caplog ? 2 * d->caplog : 32;
		struct fake_dma_op *l = realloc(d->log, cap * sizeof(*l));

		if (!l)
			return;
		d->log = l;
		d->caplog = cap;
	}
	d->log[d->nlog++] = op;
}

/* ---- the seam ----------------------------------------------------------------- */

static int dma_open(void *h, bool tx, size_t bytes, unsigned int nblocks, size_t fb)
{
	struct fake_dma *d = enter(h);
	struct dir *x = tx ? &d->tx : &d->rx;
	int ret = 0;

	if (x->open || !bytes || !nblocks || !fb || bytes % fb) {
		ret = x->open ? -EBUSY : -EINVAL;
		goto out;
	}
	x->blk = calloc(nblocks, sizeof(*x->blk));
	x->fifo = calloc(nblocks, sizeof(*x->fifo));
	if (!x->blk || !x->fifo) {
		ret = -ENOMEM;
		goto fail;
	}
	for (unsigned int i = 0; i < nblocks; i++) {
		x->blk[i].data = calloc(1, bytes);
		if (!x->blk[i].data) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	x->open = true;
	x->running = false;
	x->bb = bytes;
	x->fb = fb;
	x->fpb = bytes / fb;
	x->nblocks = nblocks;
	x->head = x->count = 0;
	if (tx) {
		d->tx_off = 0;
	} else {
		d->rx_fill = 0;
	}
	log_op(d, (struct fake_dma_op){ .kind = FAKE_DMA_OPEN, .tx = tx,
					.block_bytes = bytes, .nblocks = nblocks });
	goto out;
fail:
	for (unsigned int i = 0; x->blk && i < nblocks; i++)
		free(x->blk[i].data);
	free(x->blk);
	free(x->fifo);
	x->blk = NULL;
	x->fifo = NULL;
out:
	leave(d);
	return ret;
}

static void dma_close(void *h, bool tx)
{
	struct fake_dma *d = enter(h);
	struct dir *x = tx ? &d->tx : &d->rx;

	for (unsigned int i = 0; x->blk && i < x->nblocks; i++)
		free(x->blk[i].data);
	free(x->blk);
	free(x->fifo);
	x->blk = NULL;
	x->fifo = NULL;
	x->open = x->running = false;
	x->nblocks = 0;
	x->head = x->count = 0;
	leave(d);
}

static void *dma_data(void *h, bool tx, unsigned int i)
{
	struct fake_dma *d = h;
	struct dir *x = tx ? &d->tx : &d->rx;
	void *p;

	pthread_mutex_lock(&d->lock);
	p = x->open && i < x->nblocks ? x->blk[i].data : NULL;
	pthread_mutex_unlock(&d->lock);
	return p;
}

static int dma_enqueue(void *h, bool tx, unsigned int i, bool cyclic)
{
	struct fake_dma *d = enter(h);
	struct dir *x = tx ? &d->tx : &d->rx;
	int ret = 0;

	if (!x->open || i >= x->nblocks) {
		ret = -EINVAL;
	} else if (x->blk[i].st != B_IDLE) {
		ret = -EBUSY;
	} else {
		x->blk[i].st = B_QUEUED;
		x->blk[i].cyclic = tx && cyclic;
		x->blk[i].tagged = false;
		x->fifo[(x->head + x->count) % x->nblocks] = i;
		x->count++;
		if (tx)
			d->have_data = true;
		log_op(d, (struct fake_dma_op){ .kind = FAKE_DMA_ENQUEUE, .tx = tx, .block = i,
						.cyclic = tx && cyclic });
	}
	leave(d);
	return ret;
}

/* Drop block i from the FIFO (a cyclic TX block taken back). */
static void fifo_remove(struct dir *x, unsigned int i)
{
	unsigned int kept = 0;

	for (unsigned int k = 0; k < x->count; k++) {
		unsigned int v = x->fifo[(x->head + k) % x->nblocks];

		if (v != i)
			x->fifo[(x->head + kept++) % x->nblocks] = v;
	}
	x->count = kept;
}

static int dma_dequeue(void *h, bool tx, unsigned int i, bool nonblock)
{
	struct fake_dma *d = h;
	struct dir *x = tx ? &d->tx : &d->rx;
	struct timespec t0, t;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		long waited_ms;
		int ret = -EBUSY;
		struct fake_dma *e = enter(h);

		if (!x->open || i >= x->nblocks) {
			ret = -EINVAL;
		} else if (x->blk[i].st == B_DONE) {
			x->blk[i].st = B_IDLE;
			ret = 0;
		} else if (x->blk[i].st == B_IDLE) {
			ret = -EPERM;
		} else if (tx && x->blk[i].cyclic) {
			bool was_head = x->count && x->fifo[x->head] == i;

			fifo_remove(x, i);
			if (was_head)
				d->tx_off = 0;
			x->blk[i].st = B_IDLE;
			ret = 0;
		}
		leave(e);
		if (ret != -EBUSY || nonblock)
			return ret;
		clock_gettime(CLOCK_MONOTONIC, &t);
		waited_ms = (t.tv_sec - t0.tv_sec) * 1000L + (t.tv_nsec - t0.tv_nsec) / 1000000L;
		if (waited_ms >= WAIT_MS)
			return -ETIMEDOUT;
		nanosleep(&(struct timespec){ .tv_nsec = POLL_MS * 1000000L }, NULL);
	}
}

static int dma_start(void *h, bool tx)
{
	struct fake_dma *d = enter(h);
	struct dir *x = tx ? &d->tx : &d->rx;
	int ret = x->open ? 0 : -EINVAL;

	if (!ret) {
		x->running = true;
		if (tx) {
			d->reading = false;
			d->have_data = x->count > 0;
		}
		log_op(d, (struct fake_dma_op){ .kind = FAKE_DMA_START, .tx = tx });
	}
	leave(d);
	return ret;
}

static int dma_stop(void *h, bool tx)
{
	struct fake_dma *d = enter(h);
	struct dir *x = tx ? &d->tx : &d->rx;

	x->running = false;
	if (tx) {
		d->reading = false;
		d->have_data = false;
	}
	log_op(d, (struct fake_dma_op){ .kind = FAKE_DMA_STOP, .tx = tx });
	leave(d);
	return 0;
}

static void dma_cancel(void *h, bool tx)
{
	struct fake_dma *d = enter(h);
	struct dir *x = tx ? &d->tx : &d->rx;

	for (unsigned int i = 0; i < x->nblocks; i++)
		x->blk[i].st = B_IDLE;
	x->head = x->count = 0;
	if (tx)
		d->tx_off = 0;
	else
		d->rx_fill = 0;
	leave(d);
}

static int dma_status(void *h, bool tx, bool *ovf, bool *unf)
{
	struct fake_dma *d = enter(h);

	*ovf = !tx && d->ovf;
	*unf = tx && d->unf;
	if (tx)
		d->unf = false;
	else
		d->ovf = false;
	leave(d);
	return 0;
}

#define DEFINE_OPS(name, tx)                                                               \
	static int name##_open(void *h, size_t bytes, unsigned int n, size_t fb)           \
	{ return dma_open(h, tx, bytes, n, fb); }                                          \
	static void name##_close(void *h) { dma_close(h, tx); }                            \
	static void *name##_data(void *h, unsigned int i) { return dma_data(h, tx, i); }   \
	static int name##_enqueue(void *h, unsigned int i, bool c)                         \
	{ return dma_enqueue(h, tx, i, c); }                                               \
	static int name##_dequeue(void *h, unsigned int i, bool nb)                        \
	{ return dma_dequeue(h, tx, i, nb); }                                              \
	static int name##_start(void *h) { return dma_start(h, tx); }                      \
	static int name##_stop(void *h) { return dma_stop(h, tx); }                        \
	static void name##_cancel(void *h) { dma_cancel(h, tx); }                          \
	static int name##_status(void *h, bool *o, bool *u) { return dma_status(h, tx, o, u); } \
	const struct datax_stream_src_ops fake_dma_##name##_ops = {                        \
		.open = name##_open, .close = name##_close, .data = name##_data,           \
		.enqueue = name##_enqueue, .dequeue = name##_dequeue, .start = name##_start, \
		.stop = name##_stop, .cancel = name##_cancel, .status = name##_status,     \
	}

DEFINE_OPS(rx, false);
DEFINE_OPS(tx, true);

/* ---- lifecycle and test controls ------------------------------------------------ */

struct fake_dma_config fake_dma_default_config(uint64_t sample_hz)
{
	return (struct fake_dma_config){ .sample_hz = sample_hz, .rx_level = 0, .rx_mode = 2,
					 .tx_level = 1, .tx_mode = 3 };
}

struct fake_dma *fake_dma_new(struct fake_tick *f, const struct fake_dma_config *cfg)
{
	struct fake_dma *d;
	char buf[32];

	if (!f || !cfg || !cfg->sample_hz || cfg->rx_level >= GATE_BITS ||
	    cfg->rx_mode >= GATE_BITS || cfg->tx_level >= GATE_BITS || cfg->tx_mode >= GATE_BITS)
		return NULL;
	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->f = f;
	d->cfg = *cfg;
	if (fake_tick_read(f, "time_tick_hz", buf, sizeof(buf)) < 0 ||
	    !(d->hz = strtoull(buf, NULL, 10)) || pthread_mutex_init(&d->lock, NULL)) {
		free(d);
		return NULL;
	}
	d->hist = calloc(16, sizeof(*d->hist));
	if (!d->hist) {
		pthread_mutex_destroy(&d->lock);
		free(d);
		return NULL;
	}
	d->caphist = 16;
	d->nhist = 1;
	d->hist[0].gate = fake_tick_stream_gate(f);
	d->rx.n_next = d->tx.n_next = edge_ge(d, fake_tick_now(f));
	fake_tick_set_gate_hook(f, gate_hook, d);
	return d;
}

void fake_dma_free(struct fake_dma *d)
{
	if (!d)
		return;
	fake_tick_set_gate_hook(d->f, NULL, NULL);
	for (int tx = 0; tx < 2; tx++) {
		struct dir *x = tx ? &d->tx : &d->rx;

		for (unsigned int i = 0; x->blk && i < x->nblocks; i++)
			free(x->blk[i].data);
		free(x->blk);
		free(x->fifo);
	}
	pthread_mutex_destroy(&d->lock);
	free(d->hist);
	free(d->segs);
	free(d->log);
	free(d);
}

size_t fake_dma_tx_segments(struct fake_dma *d, struct fake_dma_tx_segment *out, size_t max)
{
	size_t n;

	enter(d);
	n = d->nsegs;
	for (size_t i = 0; out && i < max && i < n; i++)
		out[i] = d->segs[i];
	leave(d);
	return n;
}

void fake_dma_inject_overflow(struct fake_dma *d, uint64_t lost)
{
	enter(d);                    /* frames before now are not affected */
	d->drop_pending += lost;
	d->ovf = true;
	leave(d);
}

void fake_dma_corrupt_next_tag(struct fake_dma *d, enum fake_dma_tag_fault what)
{
	pthread_mutex_lock(&d->lock);
	d->fault = what;
	pthread_mutex_unlock(&d->lock);
}

size_t fake_dma_log(struct fake_dma *d, size_t first, struct fake_dma_op *out, size_t max)
{
	size_t n;

	pthread_mutex_lock(&d->lock);
	n = d->nlog;
	for (size_t i = 0; out && i < max && first + i < n; i++)
		out[i] = d->log[first + i];
	pthread_mutex_unlock(&d->lock);
	return n;
}
