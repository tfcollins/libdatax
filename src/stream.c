// SPDX-License-Identifier: MIT
/*
 * Stream lifecycle and RX copying over libiio v1 buffer streams and blocks.
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
 * context timeout can block them indefinitely.
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

static int fail_iio(struct datax_stream *s, int ret, const char *what)
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
	return s->cfg.block_samples * s->frame_bytes;
}

/* Free the blocks and close the buffer. Queued blocks are cancelled first. */
static void hw_close(struct datax_stream *s)
{
	bool any_queued = false;

	if (!s->bs)
		return;
	for (unsigned int i = 0; i < s->nblocks; i++)
		any_queued |= s->queued && s->queued[i];
	if (any_queued)
		iio_buffer_stream_cancel(s->bs);
	for (unsigned int i = 0; s->blocks && i < s->nblocks; i++)
		if (s->blocks[i])
			iio_block_destroy(s->blocks[i]);
	free(s->blocks);
	free(s->queued);
	s->blocks = NULL;
	s->queued = NULL;
	iio_buffer_close(s->bs);
	s->bs = NULL;
	s->enabled = false;
	s->cur = 0;
	s->cur_off = 0;
}

/* Open the buffer and create the blocks; on failure nothing is left open. */
static int hw_open(struct datax_stream *s)
{
	struct iio_buffer *buf = iio_device_get_buffer(s->iio_dev, 0);
	struct iio_buffer_stream *bs;
	int ret;

	if (!buf)
		return datax_error_ret(s->dev->ctx, ENOTSUP, SUB,
				       "%s has no buffer", iio_name(s));
	bs = iio_buffer_open(buf, s->mask);
	ret = iio_err(bs);
	if (ret)
		return fail_iio(s, ret, "opening the buffer");
	s->bs = bs;
	s->blocks = calloc(s->nblocks, sizeof(*s->blocks));
	s->queued = calloc(s->nblocks, sizeof(*s->queued));
	if (!s->blocks || !s->queued) {
		hw_close(s);
		return datax_error_ret(s->dev->ctx, ENOMEM, SUB, "out of memory");
	}
	for (unsigned int i = 0; i < s->nblocks; i++) {
		struct iio_block *b = iio_buffer_stream_create_block(bs, block_bytes(s));

		ret = iio_err(b);
		if (ret) {
			hw_close(s);
			return fail_iio(s, ret, "creating a block");
		}
		s->blocks[i] = b;
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
	struct iio_block *b = s->blocks[i];
	int ret;

	*waited = false;
	for (;;) {
		ret = iio_block_dequeue(b, timeout_ms == 0);
		if (timeout_ms == 0) {
			if (ret == -EBUSY)
				ret = -ETIMEDOUT;
			break;
		}
		if (ret != -ETIMEDOUT || !retry)
			break;
		*waited = true;
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

static int enqueue(struct datax_stream *s, unsigned int i)
{
	int ret = iio_block_enqueue(s->blocks[i], 0, false);

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
	s->frame_bytes = 4u * popcount(cfg->channel_mask);
	s->iio_dev = dev->stream_dev[dir];

	pthread_mutex_lock(&ctx->lock);
	busy = dev->stream[dir] != NULL;
	if (!busy)
		dev->stream[dir] = s;
	pthread_mutex_unlock(&ctx->lock);
	if (busy) {
		free(s);
		return datax_error_ret(ctx, EBUSY, SUB,
				       "stream_create: %s already has an %s stream",
				       dev->name, dir_name(dir));
	}

	ret = build_mask(s, ops);
	/* A cyclic TX stream sizes its one block at send (spec: create
	 * allocates no hardware buffer for it). */
	if (!ret && !s->cfg.cyclic)
		ret = hw_open(s);
	if (ret) {
		if (s->mask)
			iio_channels_mask_destroy(s->mask);
		release_slot(s);
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
	if (s->cfg.cyclic) {
		/* Cyclic TX enables at its single send (TX task). */
		s->started = true;
		return 0;
	}
	if (!s->bs) {
		ret = hw_open(s);
		if (ret)
			return ret;
	}
	if (s->cfg.direction == DATAX_RX) {
		for (unsigned int i = 0; i < s->nblocks; i++) {
			ret = enqueue(s, i);
			if (ret) {
				what = "stream_start: enqueueing a block";
				goto err;
			}
		}
	}
	ret = iio_buffer_stream_start(s->bs);
	if (ret)
		goto err;
	s->enabled = true;
	s->started = true;
	s->cur = 0;
	s->cur_off = 0;
	return 0;
err:
	ret = fail_iio(s, ret, what);
	/* Blocks may be queued on a buffer that never ran: free everything;
	 * the next start() reopens. */
	hw_close(s);
	return ret;
}

int datax_stream_start_at(struct datax_stream *s, datax_time when)
{
	(void)when;
	if (!s)
		return -ENOTSUP;
	return datax_error_ret(s->dev->ctx, ENOTSUP, SUB,
			       "timed start arrives with the scheduler (P3)");
}

int datax_stream_stop(struct datax_stream *s)
{
	int ret = 0, dret = 0;

	if (!s)
		return -EINVAL;
	if (!s->started)
		return 0;
	if (s->bs && s->cfg.direction == DATAX_RX)
		dret = drain(s);
	if (s->bs && s->enabled) {
		ret = iio_buffer_stream_stop(s->bs);
		if (ret)
			ret = fail_iio(s, ret, "stream_stop: disabling the buffer");
	}
	s->enabled = false;
	s->started = false;
	s->cur = 0;
	s->cur_off = 0;
	/* A block that did not come back leaves the buffer in an unknown
	 * state: free it; start() reopens. */
	if (dret || ret)
		hw_close(s);
	return ret;
}

void datax_stream_destroy(struct datax_stream *s)
{
	if (!s)
		return;
	datax_stream_stop(s);
	hw_close(s);
	if (s->mask)
		iio_channels_mask_destroy(s->mask);
	release_slot(s);
	free(s);
}

/* Re-enqueue the fully drained block at the cursor and advance. */
static int requeue(struct datax_stream *s)
{
	int ret = enqueue(s, s->cur);

	if (ret)
		return fail_iio(s, ret, "recv: re-enqueueing a block");
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
	unsigned int rearms = 0;
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
	if (!s->started || !s->bs)
		return datax_error_ret(ctx, EINVAL, SUB, "recv: stream not started");
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
			/* Past the deadline: one last nonblocking try. */
			int tmo = timeout_ms > 0 && now_us() >= deadline ? 0 : timeout_ms;
			bool waited;

			ret = dequeue(s, s->cur, tmo, deadline, true, &waited);
			if (ret == -EPERM) {
				/* libiio no longer holds the cursor's block (legacy
				 * path: a failed or timed-out read consumed it; its
				 * data is lost). Re-arm it as the newest block and
				 * move to the oldest queued one, keeping ring order,
				 * then keep waiting. Re-arms with no wait in between
				 * are bounded so a broken buffer cannot spin. */
				rearms = waited ? 0 : rearms + 1;
				if (rearms > 2 * s->nblocks)
					return done ? (int)done :
						datax_error_ret(ctx, EIO, SUB,
								"recv: %s blocks keep coming back unqueued",
								iio_name(s));
				ret = enqueue(s, s->cur);
				if (ret)
					return done ? (int)done :
						fail_iio(s, ret, "recv: re-enqueueing a block");
				s->cur = (s->cur + 1) % s->nblocks;
				continue;
			}
			if (ret) {
				if (done)
					break;
				if (ret == -ETIMEDOUT)
					return datax_error_ret(ctx, ETIMEDOUT, SUB,
							       "recv: no data from %s within %d ms",
							       iio_name(s), timeout_ms);
				return fail_iio(s, ret, "recv: dequeueing a block");
			}
			s->stats.blocks_done++;
		}
		if (first && info) {
			info->timestamp_ticks = 0;
			info->discontinuous = false;
			info->valid_samples = s->cfg.block_samples;
		}
		first = false;
		src = iio_block_start(s->blocks[s->cur]);
		ret = iio_err(src);
		if (ret)
			return done ? (int)done : fail_iio(s, ret, "recv: mapping a block");
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

int datax_internal_stream_stats(const struct datax_stream *s,
				struct datax_internal_stream_stats *out)
{
	if (!s || !out)
		return -EINVAL;
	*out = s->stats;
	out->enabled = s->enabled;
	return 0;
}
