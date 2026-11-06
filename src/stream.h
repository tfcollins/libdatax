// SPDX-License-Identifier: MIT
#ifndef DATAX_STREAM_H
#define DATAX_STREAM_H

#include <stdbool.h>
#include <stddef.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "internal.h"

struct iio_channels_mask;
struct iio_buffer_stream;
struct iio_block;

struct datax_stream {
	struct datax_device *dev;
	struct datax_stream_config cfg;
	struct iio_channels_mask *mask;
	struct iio_buffer_stream *bs;       /* NULL after a failed start/stop reset */
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
};

#endif
