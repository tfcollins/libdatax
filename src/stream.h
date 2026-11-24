// SPDX-License-Identifier: MIT
#ifndef DATAX_STREAM_H
#define DATAX_STREAM_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "internal.h"
#include "stream_src.h"

struct iio_channels_mask;
struct iio_buffer_stream;
struct iio_block;

struct datax_stream {
	struct datax_device *dev;
	struct datax_stream_config cfg;
	struct iio_channels_mask *mask;
	/* The block source (stream_src.h) and its handle, copied from the
	 * device at create. hw: it is open (false after a failed start/stop
	 * reset). */
	const struct datax_stream_src_ops *src;
	void *src_h;
	bool hw;
	/* State of the libiio source only (stream.c): the buffer (NULL when
	 * closed) and blocks. */
	struct iio_buffer_stream *bs;
	struct iio_block **blocks;
	unsigned int nblocks;
	size_t frame_bytes;                 /* 4 * popcount(channel_mask) */
	bool started, enabled, zero_copy_used, copy_used;
	/* Ring cursor: the next block to read (RX) or fill (TX), which is
	 * also the oldest queued one; byte offset into it for copying I/O. */
	unsigned int cur; size_t cur_off;
	struct datax_internal_stream_stats stats;
	/* Additions to the plan's layout: per-block "handed to libiio" flags
	 * (drain, reset and destroy must know which blocks are outstanding),
	 * and the IIO device the stream runs on. */
	bool *queued;
	struct iio_device *iio_dev;
	/* Task 3. Frames per block: cfg.block_samples, except a cyclic
	 * stream's one block, sized to the samples of its send. */
	size_t blk_samples;
	/* Zero-copy: per-block "lent to the caller" flags, and the lent
	 * blocks in lend order (lend_fifo[lend_head], nlent of them). A lent
	 * block is not queued but must not be re-armed or reclaimed. */
	bool *lent;
	unsigned int *lend_fifo;
	unsigned int lend_head, nlent;
	bool cyclic_sent;                   /* cyclic TX: its block is enqueued */
	/* Task 4 (stream_status.c): rate-limited status polling. */
	bool polled_once, detect_off;
	long long last_poll_us;
	bool disc_pending;                  /* RX: report discontinuous next */
	/* RX, timed: an overflow seen while the pre-T blocks are taken,
	 * attributed to block nblocks (stream.c, rx_block_defer) */
	bool disc_next;

	/* P3b Task 3 (stream_timed.c): timed start. timed: started by
	 * start_at (stop() then cancels its gate commands); stopping: stop()
	 * is cancelling them, so the hook does not fail the stream. fail: 0,
	 * DATAX_ERR_LATE or -ECANCELED, set by the opening command's hook
	 * (fail_detail its cause, under tmu). open_state: 0 unresolved, 1 the
	 * opening ran on time, -1 it did not (RX holds block 0 until it is
	 * resolved; tcv signals a change). open_seq: the opening command.
	 * t_open: T in API ticks; fs: the sample rate read at start_at (0:
	 * unknown). held: TX preload, filled and not enqueued (copy-mode send
	 * on an unstarted stream). blk0_held: RX block 0 is dequeued at cur and
	 * kept back until open_state is resolved. */
	bool timed;
	_Atomic bool stopping;
	_Atomic int fail;
	_Atomic int open_state;
	_Atomic long open_seq;
	datax_time t_open;
	unsigned long long fs;
	bool *held;
	bool blk0_held;
	pthread_mutex_t tmu;
	pthread_cond_t tcv;
	char fail_detail[160];

	/* P3b Task 4 (stream_tag.c, stream.c): RX block timestamps of a timed
	 * stream. strip_frames: whole frames of the capture tag, ceil(tag
	 * bytes / frame_bytes), 0 when the device cannot tag. tick_hz: the
	 * Tick's, read at start_at. anchored / anchor_hw: block 0's tag gave
	 * the hardware tick of the release. blk: blocks taken since the
	 * release. unknown: an overflow was seen; sticky until the next
	 * start_at. blk_*: the block at cur: frames stripped from its front,
	 * its first delivered sample's API time (0 unknown) and its valid
	 * frames. lent_ptr: what each lent block's get_block returned (past
	 * its tag). */
	unsigned int strip_frames;
	unsigned long long tick_hz, anchor_hw;
	bool anchored, unknown;
	unsigned long long blk;
	unsigned int blk_strip;
	unsigned long long blk_ts;
	size_t blk_valid;
	void **lent_ptr;
};

/* stream.c helpers shared with stream_timed.c. Each sets the last error
 * on failure except enqueue (libiio's code is returned bare). */
int datax_stream_hw_open(struct datax_stream *s);
void datax_stream_hw_close(struct datax_stream *s);
int datax_stream_enqueue(struct datax_stream *s, unsigned int i);
int datax_stream_fail_iio(struct datax_stream *s, int ret, const char *what);
int datax_stream_cyclic_arm(struct datax_stream *s, const char *verb);

/* stream_timed.c. */
/* Untimed start() and stop(): with the direction's RX_ARM/TX_ARM, write
 * execute-now gate(mode, 0) and gate(level, 0) (forced open, the reset
 * state), and clear the RX tag bit when clear_tag. Holds the scheduler's io
 * while one exists. 0 (no Tick access without the flag) or -errno with
 * last-error. */
int datax_stream_force_open(struct datax_stream *s, bool clear_tag);
/* stop() on a timed stream, before the P2 teardown: cancel the stream's
 * gate commands on the device scheduler, then force the gate open. 0 or the
 * first -errno (with last-error); the teardown runs either way. */
int datax_stream_timed_stop(struct datax_stream *s);
/* -EDEADLK with a last error when the caller is the device scheduler's
 * poller thread or holds its io (an event callback or a hook): stop() and
 * destroy() refuse there, so a stream is never freed under its own pending
 * hook. 0 otherwise. */
int datax_stream_on_poller(struct datax_stream *s, const char *verb);
/* A failed timed stream: fail (DATAX_ERR_LATE or -ECANCELED) with a last
 * error naming the cause, unless quiet; 0 otherwise. */
int datax_stream_timed_failed(struct datax_stream *s, const char *verb, bool quiet);
/* RX block 0 of a timed stream (dequeued, at cur): wait until the opening
 * command is resolved, within timeout_ms (0: no wait; < 0: forever) and the
 * now_us deadline. 0 when it ran on time, -ETIMEDOUT, or the failure. */
int datax_stream_wait_open(struct datax_stream *s, int timeout_ms, long long deadline_us);
/* Reset the timed state (start(), stop()). */
void datax_stream_timed_reset(struct datax_stream *s);
/* TX: held blocks, and enqueueing them in ring order (start, start_at). */
unsigned int datax_stream_held_count(const struct datax_stream *s);
int datax_stream_enqueue_held(struct datax_stream *s, const char *verb);

/* stream_tag.c. Capture tag (struct datax_tag is in datax_internal.h): 0, or
 * -EINVAL for a short buffer, wrong magic or unsupported version (no last
 * error). */
#define DATAX_TAG_MAGIC 0x4b434954u
#define DATAX_TAG_VERSION 1u
#define DATAX_TAG_VALID (1u << 0)
#define DATAX_TAG_STALE (1u << 1)
#define DATAX_TAG_LATE_EXECUTED (1u << 2)
#define DATAX_TAG_GATE(flags) (((flags) >> 4) & 0xfu)
int datax_tag_parse(const void *buf, size_t len, struct datax_tag *out);
/* Whether a parsed tag anchors block timestamps: VALID, not STALE, not
 * LATE_EXECUTED, written by gate rx_level, and tick_hz 0 or equal to the
 * Tick's. */
bool datax_tag_anchors(const struct datax_tag *t, int rx_level, unsigned long long tick_hz);
unsigned long long datax_frames_to_ticks(unsigned long long n, unsigned long long fs,
					 unsigned long long hz);

/* stream_status.c. Poll the core status register: select it, read it, clear
 * the sticky bit by writing it back. 0, or a negative errno (no last error is
 * set: callers treat a failure as "no detection"). Only the stream's own
 * direction's flag is reported. */
int datax_stream_status_poll(struct datax_stream *s, bool *overflow, bool *underflow);
/* The flags a status word carries for a direction. */
void datax_stream_status_parse(enum datax_direction dir, uint32_t word,
			       bool *overflow, bool *underflow);
/* Fold a poll's result into the stream: flags seen count even if ret < 0;
 * ret < 0 then turns detection off. Split from _check for testing. */
void datax_stream_status_apply(struct datax_stream *s, int ret, bool ovf, bool unf,
			       bool discard);
/* At most one poll per 100 ms (monotonic); accounts overflows (and sets
 * disc_pending) and underflows. A failed poll switches detection off for
 * the stream's life. discard: probe only, ignore stale flags. */
void datax_stream_status_check(struct datax_stream *s, bool discard);
/* Poll now whatever the rate limit says and discard the result (the sticky
 * idle overflow of forced-open mode), restart the rate limit from now and
 * clear disc_pending (start_at, G2/G3 ruling 7). */
void datax_stream_status_clear(struct datax_stream *s);

#endif
