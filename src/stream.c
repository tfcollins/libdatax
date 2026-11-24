// SPDX-License-Identifier: MIT
/*
 * Stream lifecycle, copying I/O (recv/send) and zero-copy I/O
 * (get_block/put_block) over libiio v1 buffer streams and blocks.
 *
 * Block I/O goes through the source seam (stream_src.h): every buffer and
 * block call is an op of s->src, libiio's (datax_stream_iio_src, below) by
 * default, a fake DMA's when a test binds one. The mask, channel lookup and
 * sample-size check stay on the IIO streaming device.
 *
 * A stream owns one iio_buffer_stream on the device's streaming (DMA) IIO
 * device and block_count blocks of block_samples frames. The frame layout
 * is the DMA's: per instant, enabled radio channels ascending, I then Q,
 * int16 each, so copying is a memcpy. A stream is not thread-safe: one
 * thread drives it at a time (the device slot is guarded by the context lock).
 *
 * Waits. libiio's binary-protocol blocks snapshot the context timeout when
 * they are created and measure it from the enqueue, so changing the context
 * timeout around a dequeue does not bound it (measured against iiod-emu: a
 * dequeue still waited the creation-time 5 s after the timeout was set to
 * 300 ms), and changing it would race other users of the context. Nonblocking
 * dequeues cannot poll either: in the pinned libiio, iiod_io_command_is_done()
 * treats the write token that the first nonblocking call clears as "not
 * done", so every later nonblocking call returns -EBUSY until the block's
 * own timeout has passed. So timeout_ms == 0 makes one nonblocking attempt,
 * and any other value blocks, retrying libiio's per-block -ETIMEDOUT until
 * the deadline (> 0) or forever (< 0). A positive bound is therefore
 * approximate: about max(timeout_ms, the context timeout when the blocks
 * were created), on the binary path as on the legacy one.
 *
 * A block libiio no longer holds (-EPERM: on the legacy v0 path a failed
 * or timed-out read consumes the block's task) is re-armed by recv as the
 * newest block, and the cursor moves to the oldest queued one, so samples
 * stay in order (that block's data is lost) and the wait continues within
 * the deadline, or forever for timeout_ms < 0.
 *
 * Stop. RX stop drains every queued block while the buffer is still enabled
 * (they complete at the stream's rate), then disables. Cancelling instead
 * tears down the buffer's connection and leaves the blocks queued, so the
 * buffer could not restart without a reopen, and iiod-emu stalls ~5 s per
 * outstanding block on teardown. The drain stops at its first failure; the
 * buffer and blocks are then freed and the next start() reopens them. So
 * stop(), destroy() and context destroy inherit the context timeout (as it
 * was when the blocks were created) once per stalled operation: one drain
 * attempt, plus libiio's own teardown of a stalled buffer. An infinite
 * context timeout can block them indefinitely. Only stop() reports a drain
 * failure (-ETIMEDOUT); destroy() runs the same drain but discards the
 * result, so call stop() first when the drain outcome matters. start()
 * clears a pending discontinuity flag left from before the previous stop().
 *
 * Ring. Blocks are used in ring order: cur is the next block to read (RX)
 * or fill (TX), and also the oldest queued one, so a drain from cur is in
 * enqueue order. Every enqueue hands libiio the whole block (bytes_used 0):
 * the pinned iiod binary responder reads a TX block's full size whatever
 * bytes_used says, so a short enqueue stalls the buffer.
 *
 * One-shot TX. send() fills the block at cur, reclaiming it from libiio
 * first if it is still queued (-EPERM there means libiio no longer holds
 * it: it is free), and enqueues each full block. A partial last block is
 * held at cur until the next send() or stop(). stop() zero-pads and
 * enqueues it, drains every queued block (one context timeout each, first
 * failure ends the drain), disables, and returns -ETIMEDOUT if a block did
 * not drain (after disabling anyway). When it returns 0 every sample sent
 * has reached the device. TX blocks_done counts blocks handed to libiio.
 *
 * Cyclic TX. create() and start() open nothing. The single send() (or
 * get_block/put_block pair) opens the buffer with one block of exactly
 * the samples sent (block_samples for zero-copy), enqueues it cyclic and
 * only then enables the buffer: over the v0 iiod protocol cyclic is fixed
 * when the buffer opens, which libiio derives from the first enqueue.
 * stop() takes the block back (at once on binary iiod; on the legacy path
 * after its one write), disables and frees the buffer, so the next send
 * sizes a new one.
 *
 * Zero-copy. get_block lends the block at cur (RX: once filled; TX: once
 * free) and advances cur; put_block takes the oldest lent block back (RX:
 * re-enqueued; TX: enqueued whole). Blocks must be returned in the order
 * lent, which keeps the ring in enqueue order (-EINVAL otherwise). With
 * every block lent, get_block returns -EBUSY. stop() takes every lent
 * block back: their pointers are invalid afterwards, and a lent TX block
 * is discarded. A stream is copying or zero-copy for its whole life:
 * mixing returns -EINVAL. If an RX block must be re-armed (-EPERM, above)
 * while others are lent, the cursor moves past lent blocks to the oldest
 * queued one (-EBUSY only when every block is lent), and the re-armed
 * block can come back one place out of order; that only follows a failed
 * legacy read, whose data is lost.
 *
 * Status. After each RX dequeue and TX enqueue the stream polls the core
 * status register (stream_status.c), at most every 100 ms. An RX overflow
 * counts in stats.overflows and marks the next block discontinuous (recv
 * info, get_block info); a TX underflow counts in stats.underflows. A
 * failed poll (no direct_reg_access attribute, or a read error) switches
 * detection off for the stream's life.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <iio/iio.h>

#include "error.h"
#include "platform/family.h"
#include "stream.h"

#define SUB "stream"

static const char *dir_name(enum datax_direction dir)
{
	return dir == DATAX_RX ? "RX" : "TX";
}

static const char *iio_name(const struct datax_stream *s)
{
	const char *n = iio_device_get_name(s->iio_dev);

	return n ? n : iio_device_get_id(s->iio_dev);
}

int datax_stream_fail_iio(struct datax_stream *s, int ret, const char *what)
{
	char msg[128];

	iio_strerror(-ret, msg, sizeof(msg));
	return datax_error_ret(s->dev->ctx, -ret, SUB, "%s on %s failed: %s",
			       what, iio_name(s), msg);
}

static long long now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static size_t block_bytes(const struct datax_stream *s)
{
	return s->blk_samples * s->frame_bytes;
}

/* Take every lent block back (their pointers become invalid). */
static void unlend_all(struct datax_stream *s)
{
	for (unsigned int i = 0; s->lent && i < s->nblocks; i++)
		s->lent[i] = false;
	s->lend_head = 0;
	s->nlent = 0;
}

/* The libiio block source: h is the stream, whose iio_dev and mask it
 * opens. Failures set the last error here. */
static void iio_src_close(void *h)
{
	struct datax_stream *s = h;

	for (unsigned int i = 0; s->blocks && i < s->nblocks; i++)
		if (s->blocks[i])
			iio_block_destroy(s->blocks[i]);
	free(s->blocks);
	s->blocks = NULL;
	if (s->bs)
		iio_buffer_close(s->bs);
	s->bs = NULL;
}

static int iio_src_open(void *h, size_t bytes, unsigned int nblocks, size_t frame_bytes)
{
	struct datax_stream *s = h;
	struct iio_buffer *buf = iio_device_get_buffer(s->iio_dev, 0);
	struct iio_buffer_stream *bs;
	int ret;

	(void)frame_bytes;
	if (!buf)
		return datax_error_ret(s->dev->ctx, ENOTSUP, SUB,
				       "%s has no buffer", iio_name(s));
	bs = iio_buffer_open(buf, s->mask);
	ret = iio_err(bs);
	if (ret)
		return datax_stream_fail_iio(s, ret, "opening the buffer");
	s->bs = bs;
	s->blocks = calloc(nblocks, sizeof(*s->blocks));
	if (!s->blocks) {
		iio_src_close(s);
		return datax_error_ret(s->dev->ctx, ENOMEM, SUB, "out of memory");
	}
	for (unsigned int i = 0; i < nblocks; i++) {
		struct iio_block *b = iio_buffer_stream_create_block(bs, bytes);

		ret = iio_err(b);
		if (ret) {
			iio_src_close(s);
			return datax_stream_fail_iio(s, ret, "creating a block");
		}
		s->blocks[i] = b;
	}
	return 0;
}

static void *iio_src_data(void *h, unsigned int i)
{
	struct datax_stream *s = h;
	void *p = iio_block_start(s->blocks[i]);

	return iio_err(p) ? NULL : p;
}

static int iio_src_enqueue(void *h, unsigned int i, bool cyclic)
{
	struct datax_stream *s = h;

	return iio_block_enqueue(s->blocks[i], 0, cyclic);
}

static int iio_src_dequeue(void *h, unsigned int i, bool nonblock)
{
	struct datax_stream *s = h;

	return iio_block_dequeue(s->blocks[i], nonblock);
}

static int iio_src_start(void *h)
{
	struct datax_stream *s = h;

	return iio_buffer_stream_start(s->bs);
}

static int iio_src_stop(void *h)
{
	struct datax_stream *s = h;

	return iio_buffer_stream_stop(s->bs);
}

static void iio_src_cancel(void *h)
{
	struct datax_stream *s = h;

	iio_buffer_stream_cancel(s->bs);
}

const struct datax_stream_src_ops datax_stream_iio_src = {
	.open = iio_src_open, .close = iio_src_close, .data = iio_src_data,
	.enqueue = iio_src_enqueue, .dequeue = iio_src_dequeue,
	.start = iio_src_start, .stop = iio_src_stop, .cancel = iio_src_cancel,
	.status = NULL,
};

/* Free the blocks and close the buffer. Queued blocks are cancelled first. */
void datax_stream_hw_close(struct datax_stream *s)
{
	bool any_queued = false;

	if (!s->hw)
		return;
	for (unsigned int i = 0; i < s->nblocks; i++)
		any_queued |= s->queued && s->queued[i];
	if (any_queued)
		s->src->cancel(s->src_h);
	s->src->close(s->src_h);
	free(s->queued);
	free(s->lent);
	free(s->lend_fifo);
	free(s->held);
	s->queued = NULL;
	s->held = NULL;
	s->lent = NULL;
	s->lend_fifo = NULL;
	s->lend_head = 0;
	s->nlent = 0;
	s->hw = false;
	s->enabled = false;
	s->cyclic_sent = false;
	s->cur = 0;
	s->cur_off = 0;
}

/* Open the buffer and create the blocks; on failure nothing is left open. */
int datax_stream_hw_open(struct datax_stream *s)
{
	int ret = s->src->open(s->src_h, block_bytes(s), s->nblocks, s->frame_bytes);

	if (ret)
		/* The libiio source sets the last error itself. */
		return s->src == &datax_stream_iio_src ? ret :
			datax_stream_fail_iio(s, ret, "opening the block source");
	s->hw = true;
	s->queued = calloc(s->nblocks, sizeof(*s->queued));
	s->lent = calloc(s->nblocks, sizeof(*s->lent));
	s->lend_fifo = calloc(s->nblocks, sizeof(*s->lend_fifo));
	s->held = calloc(s->nblocks, sizeof(*s->held));
	if (!s->queued || !s->lent || !s->lend_fifo || !s->held) {
		datax_stream_hw_close(s);
		return datax_error_ret(s->dev->ctx, ENOMEM, SUB, "out of memory");
	}
	s->cur = 0;
	s->cur_off = 0;
	return 0;
}

/*
 * Dequeue block i. timeout_ms == 0 tries once without blocking; otherwise
 * the dequeue blocks (bounded by the block's own timeout) and, with retry,
 * a libiio -ETIMEDOUT is retried until deadline (timeout_ms > 0) or forever
 * (< 0). -EPERM means libiio no longer holds the block (on the legacy v0
 * path a failed read consumes it); the queued flag is cleared and the
 * caller decides. *waited is set when a libiio timeout elapsed first.
 * Returns 0, -ETIMEDOUT, -EPERM or libiio's error (no last-error is set
 * here). blocks_done is counted by recv, not here (drained blocks at stop
 * are not delivered).
 */
static int dequeue(struct datax_stream *s, unsigned int i, int timeout_ms,
		   long long deadline, bool retry, bool *waited)
{
	int ret;

	*waited = false;
	for (;;) {
		ret = s->src->dequeue(s->src_h, i, timeout_ms == 0);
		if (timeout_ms == 0) {
			if (ret == -EBUSY)
				ret = -ETIMEDOUT;
			break;
		}
		if (ret != -ETIMEDOUT || !retry)
			break;
		*waited = true;
		/* A timed stream whose opening failed gets no data: give up
		 * after this one bounded wait (rx_take/tx_take report it). */
		if (atomic_load(&s->fail))
			break;
		if (timeout_ms > 0 && now_us() >= deadline)
			break;
	}
	if (!ret) {
		s->queued[i] = false;
	} else if (ret == -EPERM) {
		s->queued[i] = false;
	}
	return ret;
}

/* Enqueue the whole block i (cyclic on a cyclic stream). */
int datax_stream_enqueue(struct datax_stream *s, unsigned int i)
{
	int ret = s->src->enqueue(s->src_h, i, s->cfg.cyclic);

	if (!ret)
		s->queued[i] = true;
	return ret;
}

/* Dequeue every queued block, oldest first, one blocking attempt each,
 * never retried; each is bounded by the context timeout in effect when the
 * blocks were created (unbounded if that was infinite). The first failure
 * stops the drain (the caller frees the buffer), so a stalled stream costs
 * one such timeout, not one per block. A block libiio no longer holds
 * (-EPERM) needs no draining. 0, or the error. */
static int drain(struct datax_stream *s)
{
	for (unsigned int j = 0; j < s->nblocks; j++) {
		/* The cursor's block is the oldest (queued unless partly read). */
		unsigned int i = (s->cur + j) % s->nblocks;
		bool waited;
		int ret;

		if (!s->queued[i])
			continue;
		ret = dequeue(s, i, -1, 0, false, &waited);
		if (ret && ret != -EPERM)
			return ret;
	}
	return 0;
}

static void release_slot(struct datax_stream *s)
{
	struct datax_context *ctx = s->dev->ctx;

	pthread_mutex_lock(&ctx->lock);
	if (s->dev->stream[s->cfg.direction] == s)
		s->dev->stream[s->cfg.direction] = NULL;
	pthread_mutex_unlock(&ctx->lock);
}

static int init_timed_sync(struct datax_stream *s)
{
	pthread_condattr_t ca;
	int ret = pthread_condattr_init(&ca);

	if (ret)
		return ret;
	ret = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	if (!ret)
		ret = pthread_mutex_init(&s->tmu, NULL);
	if (!ret && (ret = pthread_cond_init(&s->tcv, &ca)))
		pthread_mutex_destroy(&s->tmu);
	pthread_condattr_destroy(&ca);
	return ret;
}

static void free_timed_sync(struct datax_stream *s)
{
	pthread_cond_destroy(&s->tcv);
	pthread_mutex_destroy(&s->tmu);
}

static int validate(struct datax_device *dev, const struct datax_stream_config *cfg)
{
	struct datax_context *ctx = dev->ctx;
	unsigned int nch;

	if (!cfg)
		return datax_error_ret(ctx, EINVAL, SUB, "stream_create: NULL config");
	if (cfg->direction != DATAX_RX && cfg->direction != DATAX_TX)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "stream_create: bad direction %d",
				       (int)cfg->direction);
	nch = cfg->direction == DATAX_RX ? dev->rx_channels : dev->tx_channels;
	if (!cfg->channel_mask ||
	    (nch < 32 && (cfg->channel_mask >> nch) != 0))
		return datax_error_ret(ctx, EINVAL, SUB,
				       "stream_create: channel_mask 0x%x invalid for %u %s channels",
				       cfg->channel_mask, nch, dir_name(cfg->direction));
	if (!cfg->block_samples)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "stream_create: block_samples must be > 0");
	if (cfg->cyclic && cfg->direction == DATAX_RX)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "stream_create: cyclic is TX only");
	if (!cfg->block_count)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "stream_create: block_count must be >= 1");
	return 0;
}

static unsigned int popcount(unsigned int v)
{
	unsigned int n = 0;

	for (; v; v &= v - 1)
		n++;
	return n;
}

/* Enable the IIO scan channels of every radio channel in the mask. */
static int build_mask(struct datax_stream *s, const struct datax_family_ops *ops)
{
	enum datax_direction dir = s->cfg.direction;
	ssize_t ss;

	s->mask = iio_create_channels_mask(iio_device_get_channels_count(s->iio_dev));
	if (!s->mask)
		return datax_error_ret(s->dev->ctx, ENOMEM, SUB, "out of memory");
	for (unsigned int ch = 0; ch < 32; ch++) {
		const char *ids[2];

		if (!(s->cfg.channel_mask & (1u << ch)))
			continue;
		if (ops->stream_channel_ids(ch, dir, &ids[0], &ids[1]))
			return datax_error_ret(s->dev->ctx, EINVAL, SUB,
					       "stream_create: no scan channels for %s channel %u",
					       dir_name(dir), ch);
		for (int k = 0; k < 2; k++) {
			const struct iio_channel *c =
				iio_device_find_channel(s->iio_dev, ids[k], dir == DATAX_TX);

			if (!c)
				return datax_error_ret(s->dev->ctx, ENOTSUP, SUB,
						       "stream_create: %s has no channel %s",
						       iio_name(s), ids[k]);
			iio_channel_enable(c, s->mask);
		}
	}
	ss = iio_device_get_sample_size(s->iio_dev, s->mask);
	if (ss < 0 || (size_t)ss != s->frame_bytes)
		return datax_error_ret(s->dev->ctx, ENOTSUP, SUB,
				       "stream_create: %s frame is %zd bytes, expected %zu (int16 I/Q)",
				       iio_name(s), ss, s->frame_bytes);
	return 0;
}

int datax_stream_create(struct datax_device *dev,
			const struct datax_stream_config *cfg,
			struct datax_stream **stream)
{
	const struct datax_family_ops *ops;
	struct datax_context *ctx;
	struct datax_stream *s;
	enum datax_direction dir;
	bool busy;
	int ret;

	if (!dev)
		return -EINVAL;
	ctx = dev->ctx;
	if (!stream)
		return datax_error_ret(ctx, EINVAL, SUB, "stream_create: NULL stream pointer");
	*stream = NULL;
	ret = validate(dev, cfg);
	if (ret)
		return ret;
	dir = cfg->direction;
	ops = datax_family_ops(dev->family);
	if (!ops || !ops->stream_channel_ids)
		return datax_error_ret(ctx, ENOTSUP, SUB,
				       "stream_create: %s has no streaming support", dev->name);
	if (!dev->stream_dev[dir])
		return datax_error_ret(ctx, ENOTSUP, SUB,
				       "stream_create: %s streaming device %s not found",
				       dir_name(dir),
				       dir == DATAX_RX ? ops->rx_stream_dev : ops->tx_stream_dev);
	if (cfg->block_samples > SIZE_MAX / (4u * popcount(cfg->channel_mask)))
		return datax_error_ret(ctx, EINVAL, SUB,
				       "stream_create: block_samples too large");

	s = calloc(1, sizeof(*s));
	if (!s)
		return datax_error_ret(ctx, ENOMEM, SUB, "out of memory");
	s->dev = dev;
	s->cfg = *cfg;
	if (s->cfg.cyclic)
		s->cfg.block_count = 1;
	s->nblocks = s->cfg.block_count;
	s->blk_samples = cfg->block_samples;
	s->frame_bytes = 4u * popcount(cfg->channel_mask);
	s->iio_dev = dev->stream_dev[dir];
	if (init_timed_sync(s)) {
		free(s);
		return datax_error_ret(ctx, ENOMEM, SUB, "stream_create: cannot initialise locks");
	}

	pthread_mutex_lock(&ctx->lock);
	busy = dev->stream[dir] != NULL;
	if (!busy) {
		dev->stream[dir] = s;
		s->src = dev->src_ops[dir] ? dev->src_ops[dir] : &datax_stream_iio_src;
		s->src_h = dev->src_ops[dir] ? dev->src_h[dir] : s;
	}
	pthread_mutex_unlock(&ctx->lock);
	if (busy) {
		free_timed_sync(s);
		free(s);
		return datax_error_ret(ctx, EBUSY, SUB,
				       "stream_create: %s already has an %s stream",
				       dev->name, dir_name(dir));
	}

	ret = build_mask(s, ops);
	/* A cyclic TX stream sizes its one block at send (spec: create
	 * allocates no hardware buffer for it). */
	if (!ret && !s->cfg.cyclic)
		ret = datax_stream_hw_open(s);
	if (ret) {
		if (s->mask)
			iio_channels_mask_destroy(s->mask);
		release_slot(s);
		free_timed_sync(s);
		free(s);
		return ret;
	}
	*stream = s;
	return 0;
}

int datax_stream_start(struct datax_stream *s)
{
	const char *what = "stream_start: enabling the buffer";
	int ret;

	if (!s)
		return -EINVAL;
	if (s->started)
		return datax_error_ret(s->dev->ctx, EALREADY, SUB,
				       "stream_start: already started");
	/* Forced open first (P3b): an earlier start_at, stop() or crashed
	 * process may have left the gate closed. No Tick access without the
	 * direction's gate map roles. */
	ret = datax_stream_force_open(s, true);
	if (ret)
		return ret;
	datax_stream_timed_reset(s);
	/* An overflow seen before a stop() belongs to the old run: the first
	 * block after a restart must not carry it. */
	s->disc_pending = false;
	if (s->cfg.cyclic) {
		/* Cyclic TX enables at its single send (TX task), or now when
		 * send() preloaded the buffer. */
		if (s->hw && s->held && s->held[0]) {
			ret = datax_stream_cyclic_arm(s, "stream_start");
			if (ret)
				return ret;
			s->held[0] = false;
		}
		s->started = true;
		datax_stream_status_check(s, true);
		return 0;
	}
	if (!s->hw) {
		ret = datax_stream_hw_open(s);
		if (ret)
			return ret;
	}
	if (s->cfg.direction == DATAX_RX) {
		for (unsigned int i = 0; i < s->nblocks; i++) {
			ret = datax_stream_enqueue(s, i);
			if (ret) {
				what = "stream_start: enqueueing a block";
				goto err;
			}
		}
		s->cur = 0;
		s->cur_off = 0;
	}
	ret = s->src->start(s->src_h);
	if (ret)
		goto err;
	s->enabled = true;
	/* TX: the preload goes after enable, in ring order; the cursor stays
	 * on the partial block send() left (or the next free one). */
	if (s->cfg.direction == DATAX_TX) {
		ret = datax_stream_enqueue_held(s, "stream_start");
		if (ret) {
			datax_stream_hw_close(s);
			return ret;
		}
	}
	s->started = true;
	datax_stream_status_check(s, true); /* probe; clears stale flags */
	return 0;
err:
	ret = datax_stream_fail_iio(s, ret, what);
	/* Blocks may be queued on a buffer that never ran: free everything;
	 * the next start() reopens. */
	datax_stream_hw_close(s);
	return ret;
}

/* One-shot TX: zero-pad and enqueue the partial block held at cur (or a
 * full one whose enqueue failed in send). 0, or libiio's error. */
static int tx_flush(struct datax_stream *s)
{
	size_t bb = block_bytes(s);
	int ret;

	if (!s->cur_off)
		return 0;
	if (s->cur_off < bb)
		memset((unsigned char *)s->src->data(s->src_h, s->cur) + s->cur_off,
		       0, bb - s->cur_off);
	ret = datax_stream_enqueue(s, s->cur);
	if (ret)
		return ret;
	s->stats.blocks_done++;
	s->cur = (s->cur + 1) % s->nblocks;
	s->cur_off = 0;
	return 0;
}

int datax_stream_stop(struct datax_stream *s)
{
	bool tx_oneshot;
	int ret = 0, dret = 0, fret = 0, cret = 0, tret = 0;

	if (!s)
		return -EINVAL;
	ret = datax_stream_on_poller(s, "stop");
	if (ret)
		return ret;
	if (!s->started)
		return 0;
	/* Timed: cancel the gate commands, then force open, so the drain
	 * below sees data (an armed stream that never opened would otherwise
	 * wait out the context timeout per block). */
	if (s->timed)
		tret = datax_stream_timed_stop(s);
	tx_oneshot = s->cfg.direction == DATAX_TX && !s->cfg.cyclic;
	unlend_all(s); /* lent blocks are not queued: the drain skips them */
	if (s->hw && tx_oneshot)
		fret = tx_flush(s);
	if (s->hw && !s->cfg.cyclic)
		dret = drain(s);
	if (s->hw && s->cfg.cyclic && s->queued[0]) {
		/* Cyclic: take the block back before disabling, so the buffer
		 * closes without a cancel. It must come first: on the legacy
		 * path the block's write runs on libiio's worker, which
		 * iio_buffer_stream_stop parks, and a dequeue of a write the
		 * worker had not yet run would then wait forever
		 * (iio_task_sync(token, -1)). Binary iiod answers at once.
		 * On the legacy path send()/put_block() only queued the write,
		 * so this is where a failed cyclic write surfaces. -EPERM:
		 * libiio no longer holds the block; nothing to report. */
		bool waited;

		cret = dequeue(s, 0, -1, 0, false, &waited);
		if (cret == -EPERM)
			cret = 0;
	}
	if (s->hw && s->enabled) {
		ret = s->src->stop(s->src_h);
		if (ret)
			ret = datax_stream_fail_iio(s, ret, "stream_stop: disabling the buffer");
	}
	s->enabled = false;
	s->started = false;
	s->cur = 0;
	s->cur_off = 0;
	for (unsigned int i = 0; s->held && i < s->nblocks; i++)
		s->held[i] = false;
	datax_stream_timed_reset(s);
	/* A block that did not come back leaves the buffer in an unknown
	 * state: free it; start() reopens. A cyclic stream always frees its
	 * buffer: the next send sizes a new one. */
	if (fret || dret || ret || s->cfg.cyclic)
		datax_stream_hw_close(s);
	if (cret)
		return datax_stream_fail_iio(s, cret, "stream_stop: writing the cyclic block");
	if (fret)
		return datax_stream_fail_iio(s, fret, "stream_stop: enqueueing the held TX block");
	if (tx_oneshot && dret) {
		if (dret == -ETIMEDOUT)
			return datax_error_ret(s->dev->ctx, ETIMEDOUT, SUB,
					       "stream_stop: a TX block did not drain to %s within the context timeout",
					       iio_name(s));
		return datax_stream_fail_iio(s, dret, "stream_stop: draining TX blocks");
	}
	return ret ? ret : tret;
}

/* Drain results are reported only by stop(), not here. */
void datax_stream_destroy(struct datax_stream *s)
{
	if (!s)
		return;
	/* Refused (last error set) on the poller thread: the stream stays
	 * valid; destroy it from another thread. */
	if (datax_stream_on_poller(s, "destroy"))
		return;
	datax_stream_stop(s);
	datax_stream_hw_close(s);
	if (s->mask)
		iio_channels_mask_destroy(s->mask);
	release_slot(s);
	free_timed_sync(s);
	free(s);
}

/*
 * RX: dequeue the block at the cursor, waiting as dequeue() describes. A
 * block libiio no longer holds (-EPERM: on the legacy path a failed or
 * timed-out read consumed it; its data is lost) is re-armed as the newest
 * block and the cursor moves to the oldest queued one, keeping ring order,
 * and the wait goes on. Re-arms with no wait in between are bounded so a
 * broken buffer cannot spin. 0 (blocks_done counted), or a negative errno
 * with the last error set unless quiet.
 */
static int rx_take(struct datax_stream *s, int timeout_ms, long long deadline,
		   const char *verb, bool quiet)
{
	struct datax_context *ctx = s->dev->ctx;
	unsigned int rearms = 0;
	char what[64];

	for (;;) {
		/* Past the deadline: one last nonblocking try. */
		int tmo = timeout_ms > 0 && now_us() >= deadline ? 0 : timeout_ms;
		bool waited;
		int ret;

		ret = datax_stream_timed_failed(s, verb, quiet);
		if (ret)
			return ret;
		if (s->blk0_held)
			goto hold;         /* dequeued by an earlier call */
		if (s->nlent == s->nblocks)
			return quiet ? -EBUSY :
				datax_error_ret(ctx, EBUSY, SUB,
						"%s: every block of %s is lent; put_block one back first",
						verb, iio_name(s));
		/* After a re-arm the cursor can reach the lent blocks: they
		 * are not queued, so move past them to the oldest queued one. */
		while (s->lent[s->cur])
			s->cur = (s->cur + 1) % s->nblocks;
		ret = dequeue(s, s->cur, tmo, deadline, true, &waited);
		if (ret == -EPERM) {
			rearms = waited ? 0 : rearms + 1;
			if (rearms > 2 * s->nblocks)
				return quiet ? -EIO :
					datax_error_ret(ctx, EIO, SUB,
							"%s: %s blocks keep coming back unqueued",
							verb, iio_name(s));
			ret = datax_stream_enqueue(s, s->cur);
			if (ret) {
				snprintf(what, sizeof(what), "%s: re-enqueueing a block", verb);
				return quiet ? ret : datax_stream_fail_iio(s, ret, what);
			}
			s->cur = (s->cur + 1) % s->nblocks;
			continue;
		}
		if (ret == -ETIMEDOUT && (ret = datax_stream_timed_failed(s, verb, quiet)))
			return ret;
		if (ret == -ETIMEDOUT)
			return quiet ? ret :
				datax_error_ret(ctx, ETIMEDOUT, SUB,
						"%s: no data from %s within %d ms",
						verb, iio_name(s), timeout_ms);
		if (ret) {
			snprintf(what, sizeof(what), "%s: dequeueing a block", verb);
			return quiet ? ret : datax_stream_fail_iio(s, ret, what);
		}
hold:
		/* Timed RX: block 0 is kept back until the opening command is
		 * resolved, so a capture that started late is never delivered
		 * (Review Focus 1); a timeout leaves it held for the next call. */
		if (s->timed && atomic_load(&s->open_state) != 1) {
			s->blk0_held = true;
			ret = datax_stream_wait_open(s, timeout_ms, deadline);
			if (ret == -ETIMEDOUT)
				return quiet ? ret :
					datax_error_ret(ctx, ETIMEDOUT, SUB,
							"%s: the timed start on %s has not opened within %d ms",
							verb, iio_name(s), timeout_ms);
			if (ret)
				return datax_stream_timed_failed(s, verb, quiet);
		}
		s->blk0_held = false;
		s->stats.blocks_done++;
		datax_stream_status_check(s, false);
		return 0;
	}
}

/*
 * TX: make the block at the cursor free to fill, reclaiming it from libiio
 * if it is still queued (waiting as dequeue() describes). -EPERM there
 * means libiio no longer holds it, so it is free. 0, or a negative errno
 * with the last error set unless quiet.
 */
static int tx_take(struct datax_stream *s, int timeout_ms, long long deadline,
		   const char *verb, bool quiet)
{
	int tmo = timeout_ms > 0 && now_us() >= deadline ? 0 : timeout_ms;
	char what[64];
	bool waited;
	int ret;

	/* TX never re-arms, so the cursor's block is lent only when all are. */
	if (s->nlent == s->nblocks || s->lent[s->cur])
		return quiet ? -EBUSY :
			datax_error_ret(s->dev->ctx, EBUSY, SUB,
					"%s: every block of %s is lent; put_block one back first",
					verb, iio_name(s));
	if (!s->queued[s->cur])
		return 0;
	ret = dequeue(s, s->cur, tmo, deadline, true, &waited);
	if (!ret || ret == -EPERM)
		return 0;
	if (ret == -ETIMEDOUT && (ret = datax_stream_timed_failed(s, verb, quiet)))
		return ret;
	if (ret == -ETIMEDOUT)
		return quiet ? ret :
			datax_error_ret(s->dev->ctx, ETIMEDOUT, SUB,
					"%s: no free block on %s within %d ms",
					verb, iio_name(s), timeout_ms);
	snprintf(what, sizeof(what), "%s: reclaiming a block", verb);
	return quiet ? ret : datax_stream_fail_iio(s, ret, what);
}

/* Re-enqueue the fully drained block at the cursor and advance. */
static int requeue(struct datax_stream *s)
{
	int ret = datax_stream_enqueue(s, s->cur);

	if (ret)
		return datax_stream_fail_iio(s, ret, "recv: re-enqueueing a block");
	s->cur_off = 0;
	s->cur = (s->cur + 1) % s->nblocks;
	return 0;
}

int datax_stream_recv(struct datax_stream *s, void *dst, size_t samples,
		      struct datax_block_info *info, int timeout_ms)
{
	struct datax_context *ctx;
	long long deadline = 0;
	size_t bb, done = 0;
	bool first = true;
	int ret;

	if (!s)
		return -EINVAL;
	ctx = s->dev->ctx;
	if (s->cfg.direction != DATAX_RX)
		return datax_error_ret(ctx, EINVAL, SUB, "recv: not an RX stream");
	if (!dst || !samples)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "recv: NULL buffer or zero samples");
	if (s->zero_copy_used)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "recv: stream is in zero-copy mode (get_block/put_block)");
	if (!s->started || !s->hw)
		return datax_error_ret(ctx, EINVAL, SUB, "recv: stream not started");
	ret = datax_stream_timed_failed(s, "recv", false);
	if (ret)
		return ret;
	s->copy_used = true;
	if (samples > INT_MAX)
		samples = INT_MAX;
	if (timeout_ms > 0)
		deadline = now_us() + (long long)timeout_ms * 1000LL;
	bb = block_bytes(s);

	while (done < samples) {
		unsigned char *src;
		size_t n;

		if (s->cur_off == bb) {
			/* A previous re-enqueue failed: retry it first. */
			ret = requeue(s);
			if (ret)
				return done ? (int)done : ret;
		}
		if (s->cur_off == 0) {
			ret = rx_take(s, timeout_ms, deadline, "recv", done > 0);
			if (ret)
				return done ? (int)done : ret;
		}
		if (first) {
			if (info) {
				info->timestamp_ticks = 0;
				info->discontinuous = s->disc_pending;
				info->valid_samples = s->cfg.block_samples;
			}
			s->disc_pending = false;
		}
		first = false;
		src = s->src->data(s->src_h, s->cur);
		if (!src)
			return done ? (int)done : datax_stream_fail_iio(s, -EIO, "recv: mapping a block");
		n = (bb - s->cur_off) / s->frame_bytes;
		if (n > samples - done)
			n = samples - done;
		memcpy((unsigned char *)dst + done * s->frame_bytes,
		       src + s->cur_off, n * s->frame_bytes);
		done += n;
		s->cur_off += n * s->frame_bytes;
		if (s->cur_off == bb) {
			ret = requeue(s);
			if (ret)
				return (int)done; /* retried at the next call */
		}
	}
	return (int)done;
}

/* Cyclic: open the buffer with one block of exactly `samples` frames. */
static int cyclic_open(struct datax_stream *s, size_t samples)
{
	datax_stream_hw_close(s); /* stop() frees a cyclic buffer; nothing should be open */
	s->blk_samples = samples;
	return datax_stream_hw_open(s);
}

/* Cyclic: enqueue the block cyclic, then enable the buffer (the order the
 * v0 protocol needs). A failure frees the buffer. */
int datax_stream_cyclic_arm(struct datax_stream *s, const char *verb)
{
	char what[64];
	int ret = datax_stream_enqueue(s, 0);

	if (ret) {
		snprintf(what, sizeof(what), "%s: enqueueing the cyclic block", verb);
		goto err;
	}
	ret = s->src->start(s->src_h);
	if (ret) {
		snprintf(what, sizeof(what), "%s: enabling the buffer", verb);
		goto err;
	}
	s->enabled = true;
	s->cyclic_sent = true;
	s->stats.blocks_done++;
	return 0;
err:
	ret = datax_stream_fail_iio(s, ret, what);
	datax_stream_hw_close(s);
	return ret;
}

static int send_cyclic(struct datax_stream *s, const void *src, size_t samples)
{
	struct datax_context *ctx = s->dev->ctx;
	int ret;

	if (s->cyclic_sent)
		return datax_error_ret(ctx, EBUSY, SUB,
				       "send: the cyclic block is already transmitting; stop() first");
	if (samples > s->cfg.block_samples || samples > INT_MAX)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "send: a cyclic send takes at most block_samples (%zu) instants, got %zu",
				       s->cfg.block_samples, samples);
	s->copy_used = true;
	ret = cyclic_open(s, samples);
	if (ret)
		return ret;
	memcpy(s->src->data(s->src_h, 0), src, samples * s->frame_bytes);
	ret = datax_stream_cyclic_arm(s, "send");
	return ret ? ret : (int)samples;
}

/*
 * TX preload (api-c-core "Timed start"): copy-mode send on an unstarted
 * stream fills blocks from the cursor without enqueueing them (held), up
 * to block_count blocks, or the one cyclic buffer. Full: a partial count,
 * or -EBUSY when nothing was taken. start() or start_at() transmits them.
 */
static int send_preload(struct datax_stream *s, const void *src, size_t samples)
{
	struct datax_context *ctx = s->dev->ctx;
	size_t bb, done = 0;
	int ret;

	if (s->cfg.cyclic) {
		if (s->hw && s->held && s->held[0])
			return datax_error_ret(ctx, EBUSY, SUB,
					       "send: the cyclic buffer is already preloaded; start the stream");
		if (samples > s->cfg.block_samples || samples > INT_MAX)
			return datax_error_ret(ctx, EINVAL, SUB,
					       "send: a cyclic send takes at most block_samples (%zu) instants, got %zu",
					       s->cfg.block_samples, samples);
		ret = cyclic_open(s, samples);
		if (ret)
			return ret;
		s->copy_used = true;
		memcpy(s->src->data(s->src_h, 0), src, samples * s->frame_bytes);
		s->held[0] = true;
		return (int)samples;
	}
	if (!s->hw) {
		ret = datax_stream_hw_open(s);
		if (ret)
			return ret;
	}
	s->copy_used = true;
	if (samples > INT_MAX)
		samples = INT_MAX;
	bb = block_bytes(s);
	while (done < samples) {
		unsigned char *dst;
		size_t n;

		if (s->held[s->cur])
			break;                 /* every block is full */
		dst = s->src->data(s->src_h, s->cur);
		if (!dst)
			return done ? (int)done :
				datax_stream_fail_iio(s, -EIO, "send: mapping a block");
		n = (bb - s->cur_off) / s->frame_bytes;
		if (n > samples - done)
			n = samples - done;
		memcpy(dst + s->cur_off, (const unsigned char *)src + done * s->frame_bytes,
		       n * s->frame_bytes);
		done += n;
		s->cur_off += n * s->frame_bytes;
		if (s->cur_off == bb) {
			s->held[s->cur] = true;
			s->cur = (s->cur + 1) % s->nblocks;
			s->cur_off = 0;
		}
	}
	if (!done)
		return datax_error_ret(ctx, EBUSY, SUB,
				       "send: the TX preload is full (%u blocks); start the stream",
				       s->nblocks);
	return (int)done;
}

int datax_stream_send(struct datax_stream *s, const void *src, size_t samples,
		      int timeout_ms)
{
	struct datax_context *ctx;
	long long deadline = 0;
	size_t bb, done = 0;
	int ret;

	if (!s)
		return -EINVAL;
	ctx = s->dev->ctx;
	if (s->cfg.direction != DATAX_TX)
		return datax_error_ret(ctx, EINVAL, SUB, "send: not a TX stream");
	if (!src || !samples)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "send: NULL buffer or zero samples");
	if (s->zero_copy_used)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "send: stream is in zero-copy mode (get_block/put_block)");
	if (!s->started)
		return send_preload(s, src, samples);
	if (!s->cfg.cyclic && !s->hw)
		return datax_error_ret(ctx, EINVAL, SUB, "send: stream not started");
	ret = datax_stream_timed_failed(s, "send", false);
	if (ret)
		return ret;
	if (s->cfg.cyclic)
		return send_cyclic(s, src, samples);
	s->copy_used = true;
	if (samples > INT_MAX)
		samples = INT_MAX;
	if (timeout_ms > 0)
		deadline = now_us() + (long long)timeout_ms * 1000LL;
	bb = block_bytes(s);

	while (done < samples) {
		size_t n;

		if (s->cur_off == bb) {
			/* A previous enqueue failed: retry it first. */
			ret = datax_stream_enqueue(s, s->cur);
			if (ret)
				return done ? (int)done :
					datax_stream_fail_iio(s, ret, "send: enqueueing a block");
			s->stats.blocks_done++;
			datax_stream_status_check(s, false);
			s->cur = (s->cur + 1) % s->nblocks;
			s->cur_off = 0;
		}
		if (s->cur_off == 0) {
			ret = tx_take(s, timeout_ms, deadline, "send", done > 0);
			if (ret)
				return done ? (int)done : ret;
		}
		n = (bb - s->cur_off) / s->frame_bytes;
		if (n > samples - done)
			n = samples - done;
		memcpy((unsigned char *)s->src->data(s->src_h, s->cur) + s->cur_off,
		       (const unsigned char *)src + done * s->frame_bytes,
		       n * s->frame_bytes);
		done += n;
		s->cur_off += n * s->frame_bytes;
		if (s->cur_off == bb) {
			ret = datax_stream_enqueue(s, s->cur);
			if (ret) {
				/* Held: retried by the next send or stop(). */
				datax_stream_fail_iio(s, ret, "send: enqueueing a block");
				return (int)done;
			}
			s->stats.blocks_done++;
			datax_stream_status_check(s, false);
			s->cur = (s->cur + 1) % s->nblocks;
			s->cur_off = 0;
		}
	}
	return (int)done;
}

int datax_stream_get_block(struct datax_stream *s, void **data,
			   struct datax_block_info *info, int timeout_ms)
{
	struct datax_context *ctx;
	long long deadline = 0;
	unsigned int i;
	int ret = 0;

	if (!s)
		return -EINVAL;
	ctx = s->dev->ctx;
	if (!data)
		return datax_error_ret(ctx, EINVAL, SUB, "get_block: NULL data pointer");
	*data = NULL;
	if (s->copy_used)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "get_block: stream is in copy mode (recv/send)");
	if (!s->started || (!s->cfg.cyclic && !s->hw))
		return datax_error_ret(ctx, EINVAL, SUB, "get_block: stream not started");
	ret = datax_stream_timed_failed(s, "get_block", false);
	if (ret)
		return ret;
	s->zero_copy_used = true;
	if (timeout_ms > 0)
		deadline = now_us() + (long long)timeout_ms * 1000LL;

	if (s->cfg.cyclic) {
		if (s->cyclic_sent)
			return datax_error_ret(ctx, EBUSY, SUB,
					       "get_block: the cyclic block is already transmitting; stop() first");
		if (s->hw && s->lent[0])
			return datax_error_ret(ctx, EBUSY, SUB,
					       "get_block: the cyclic block is already lent");
		if (!s->hw)
			ret = cyclic_open(s, s->cfg.block_samples);
	} else if (s->cfg.direction == DATAX_RX) {
		ret = rx_take(s, timeout_ms, deadline, "get_block", false);
	} else {
		ret = tx_take(s, timeout_ms, deadline, "get_block", false);
	}
	if (ret)
		return ret;
	i = s->cur;
	s->lent[i] = true;
	s->lend_fifo[(s->lend_head + s->nlent) % s->nblocks] = i;
	s->nlent++;
	s->cur = (i + 1) % s->nblocks;
	*data = s->src->data(s->src_h, i);
	if (info) {
		info->timestamp_ticks = 0;
		info->discontinuous = s->disc_pending;
		info->valid_samples = s->blk_samples;
	}
	s->disc_pending = false;
	return 0;
}

int datax_stream_put_block(struct datax_stream *s, void *data)
{
	struct datax_context *ctx;
	unsigned int i, k;
	int ret;

	if (!s)
		return -EINVAL;
	ctx = s->dev->ctx;
	ret = datax_stream_timed_failed(s, "put_block", false);
	if (ret)
		return ret;
	for (k = 0; data && k < s->nlent; k++)
		if (s->src->data(s->src_h, s->lend_fifo[(s->lend_head + k) % s->nblocks]) == data)
			break;
	if (!data || k == s->nlent)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "put_block: %p is not a lent block", data);
	if (k)
		return datax_error_ret(ctx, EINVAL, SUB,
				       "put_block: return blocks in the order get_block lent them");
	i = s->lend_fifo[s->lend_head];
	s->lent[i] = false;
	s->lend_head = (s->lend_head + 1) % s->nblocks;
	s->nlent--;
	if (s->cfg.cyclic)
		return datax_stream_cyclic_arm(s, "put_block");
	ret = datax_stream_enqueue(s, i);
	if (ret)
		/* Neither lent nor queued: RX re-arms it, TX finds it free. */
		return datax_stream_fail_iio(s, ret, "put_block: enqueueing a block");
	if (s->cfg.direction == DATAX_TX) {
		s->stats.blocks_done++;
		datax_stream_status_check(s, false);
	}
	return 0;
}

int datax_internal_stream_bind_source(struct datax_device *dev, enum datax_direction dir,
				      const struct datax_stream_src_ops *ops, void *h)
{
	int ret = 0;

	if (!dev)
		return -EINVAL;
	if (dir != DATAX_RX && dir != DATAX_TX)
		return datax_error_ret(dev->ctx, EINVAL, SUB,
				       "stream_bind_source: bad direction %d", (int)dir);
	if (ops && (!ops->open || !ops->close || !ops->data || !ops->enqueue ||
		    !ops->dequeue || !ops->start || !ops->stop || !ops->cancel))
		return datax_error_ret(dev->ctx, EINVAL, SUB,
				       "stream_bind_source: ops need open, close, data, enqueue, dequeue, start, stop and cancel");
	pthread_mutex_lock(&dev->ctx->lock);
	if (dev->stream[dir]) {
		ret = -EBUSY;
	} else {
		dev->src_ops[dir] = ops;
		dev->src_h[dir] = ops ? h : NULL;
	}
	pthread_mutex_unlock(&dev->ctx->lock);
	if (ret == -EBUSY)
		return datax_error_ret(dev->ctx, EBUSY, SUB,
				       "stream_bind_source: a %s stream exists; destroy it first",
				       dir_name(dir));
	return ret;
}

int datax_internal_stream_stats(const struct datax_stream *s,
				struct datax_internal_stream_stats *out)
{
	if (!s || !out)
		return -EINVAL;
	*out = s->stats;
	out->enabled = s->enabled;
	return 0;
}
