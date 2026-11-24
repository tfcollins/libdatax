// SPDX-License-Identifier: MIT
/*
 * Fake DMA endpoint for the timed-stream tests: the RX and TX data path
 * behind the stream block seam (src/stream_src.h), gated by the fake Tick's
 * stream gate levels. Built into the test-only library datax_fake_tick.
 *
 * Plug it into a device with
 *     datax_internal_stream_bind_source(dev, DATAX_RX, &fake_dma_rx_ops, d);
 *     datax_internal_stream_bind_source(dev, DATAX_TX, &fake_dma_tx_ops, d);
 * It installs itself as the fake Tick's gate hook (one hook per Tick).
 *
 * Time. Sample frame n exists at Tick tick edge(n) = round(n * tick_hz /
 * sample_hz), counted from tick 0. The fake does no work of its own: every
 * call first catches up to fake_tick_now(). A transition of the gate levels
 * at tick T takes effect for the first frame whose edge is >= T. Gate pass is
 * !mode | level, with the bit positions the config gives (from the Tick's
 * stream_gate_map).
 *
 * RX. A frame is stored only while the RX gate passes, and the first passing
 * frame after a gap is dropped (util_cpack2 does). A frame carries its own
 * index: every channel is I = n & 0xffff, Q = n >> 16, int16 little-endian.
 * Frames fill the enqueued blocks in order. A closed-to-open transition while
 * mode = 1 and capture_tag_enable has the level bit is a release: the first
 * stored frame after it starts a new block (a partial one is zero-padded and
 * delivered first), and the first 32 bytes of that block are overwritten with
 * the capture header, UNstripped, built from the RX level gate's latch:
 * VALID | gate << 4 | LATE? | STALE?, tick_hz = the Tick's, burst_index = the
 * number of earlier releases. While the gate passes and the DMA is not
 * running, frames are lost and a sticky overflow is raised (forced-open mode
 * with no buffer, as on stock).
 *
 * TX. Enqueued blocks form a FIFO (a cyclic block repeats). After start,
 * nothing is read until the gate first passes; from then on one frame is consumed per sample clock, gate open or not:
 * closed, the DAC gets zeros (muted). An empty FIFO while reading is an
 * underflow. Each continuous stretch is reported by fake_dma_tx_segments.
 *
 * Every function is thread-safe. Free the fake DMA before its fake Tick.
 */
#ifndef DATAX_FAKE_DMA_H
#define DATAX_FAKE_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fake_tick.h"
#include "stream_src.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fake_dma;

/* Gate bit positions (the Tick's stream_gate_map roles) and the sample rate. */
struct fake_dma_config {
	uint64_t sample_hz;                 /* 1 MSPS: exactly 100 ticks per frame at 100 MHz */
	unsigned int rx_level, rx_mode, tx_level, tx_mode;
};
/* sample_hz with the standard map "rx-dma,tx-dma,rx-mode,tx-mode": bits 0..3. */
struct fake_dma_config fake_dma_default_config(uint64_t sample_hz);

/* sample_hz with the bit positions of a parsed Tick stream_gate_map (the
 * device's dev->caps.gate_map); a role absent from the map keeps the default
 * position. */
struct fake_dma_config fake_dma_config_from_map(uint64_t sample_hz,
						const struct datax_gate_map *map);

/* NULL: invalid config (sample rate 0, a bit >= 8) or no memory. */
struct fake_dma *fake_dma_new(struct fake_tick *f, const struct fake_dma_config *cfg);
void fake_dma_free(struct fake_dma *d);

/* h is the struct fake_dma *. */
extern const struct datax_stream_src_ops fake_dma_rx_ops;
extern const struct datax_stream_src_ops fake_dma_tx_ops;

/* The tick of sample frame n, and the first frame at or after a tick. */
uint64_t fake_dma_frame_tick(const struct fake_dma *d, uint64_t n);
uint64_t fake_dma_frame_at(const struct fake_dma *d, uint64_t tick);
/* The frame index an RX frame carries (its first channel's I and Q). */
uint64_t fake_dma_frame_index(const void *frame);

/* A continuous stretch of TX consumption over frames [first_frame, ...):
 * edges t_start .. t_end (t_end is the tick of the first frame not in it).
 * muted: the gate was closed, the DAC got zeros. underflow: the FIFO was
 * empty, nothing was consumed (first_frame is where it ran out). */
struct fake_dma_tx_segment {
	uint64_t t_start, t_end, first_frame;
	bool muted, underflow;
};
/* Copy up to max segments from index 0; returns the total count. */
size_t fake_dma_tx_segments(struct fake_dma *d, struct fake_dma_tx_segment *out, size_t max);

/* How long a blocking dequeue waits before -ETIMEDOUT (default 50 ms): the
 * context timeout of a real buffer (libiio's default is 5 s). */
void fake_dma_set_wait_ms(struct fake_dma *d, unsigned int ms);

/* ---- faults ---- */

/* The next `lost` stored RX frames are dropped, and the next RX status reports
 * an overflow. */
void fake_dma_inject_overflow(struct fake_dma *d, uint64_t lost);
enum fake_dma_tag_fault { FAKE_DMA_TAG_BAD_MAGIC = 1, FAKE_DMA_TAG_BAD_FLAGS };
/* The next release writes a bad header: a wrong magic, or VALID cleared. */
void fake_dma_corrupt_next_tag(struct fake_dma *d, enum fake_dma_tag_fault what);

/* ---- the capture header codec (tests check it against capture_tags.json) ---- */

struct fake_dma_tag_fields {
	uint64_t capture_tick;
	uint8_t gate;
	uint32_t seq;
	uint16_t target, cmd;
	uint32_t tick_hz, burst;
	bool late, stale;
};
void fake_dma_encode_tag(uint8_t out[32], const struct fake_dma_tag_fields *t);

/* ---- op log ---- */

enum fake_dma_op_kind { FAKE_DMA_OPEN, FAKE_DMA_ENQUEUE, FAKE_DMA_START, FAKE_DMA_STOP };
struct fake_dma_op {
	enum fake_dma_op_kind kind;
	bool tx;
	unsigned int block;      /* ENQUEUE: the block */
	bool cyclic;             /* ENQUEUE */
	size_t block_bytes;      /* OPEN */
	unsigned int nblocks;    /* OPEN */
};
/* Copy up to max entries from index first; returns the total count. */
size_t fake_dma_log(struct fake_dma *d, size_t first, struct fake_dma_op *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif
