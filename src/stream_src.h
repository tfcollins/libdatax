// SPDX-License-Identifier: MIT
/*
 * Stream block-I/O seam. stream.c moves every buffer and block call behind
 * these ops: the libiio implementation (datax_stream_iio_src, h = the
 * stream) is the default, and tests bind another source per device and
 * direction (datax_internal_stream_bind_source) to drive a fake DMA.
 *
 * Blocks are numbered 0..nblocks-1, as in the stream's ring. The block
 * lifecycle follows libiio: enqueue hands block i to the source, dequeue
 * takes it back once filled (RX) or consumed (TX).
 */
#ifndef DATAX_STREAM_SRC_H
#define DATAX_STREAM_SRC_H

#include <stdbool.h>
#include <stddef.h>

struct datax_stream_src_ops {
	/* Open the buffer and create nblocks blocks of block_bytes each
	 * (frame_bytes per sample instant). 0, or -errno with nothing left open. */
	int   (*open)(void *h, size_t block_bytes, unsigned int nblocks, size_t frame_bytes);
	/* Destroy the blocks and close the buffer. */
	void  (*close)(void *h);
	/* The bytes of block i, or NULL when it cannot be mapped. */
	void *(*data)(void *h, unsigned int i);
	/* Hand block i to the source (cyclic: transmit it repeatedly). */
	int   (*enqueue)(void *h, unsigned int i, bool cyclic);
	/* Take block i back. 0, -EBUSY (nonblock and not done), -ETIMEDOUT or
	 * -EPERM (the source no longer holds it). */
	int   (*dequeue)(void *h, unsigned int i, bool nonblock);
	int   (*start)(void *h);
	int   (*stop)(void *h);
	/* Abort every queued block. */
	void  (*cancel)(void *h);
	/* Sticky overflow / underflow flags, cleared by the read. NULL: the
	 * core status register (stream_status.c). */
	int   (*status)(void *h, bool *ovf, bool *unf);
};

/* The libiio implementation; bound with h = the struct datax_stream. */
extern const struct datax_stream_src_ops datax_stream_iio_src;

#endif
