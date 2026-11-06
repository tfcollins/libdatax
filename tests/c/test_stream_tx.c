// SPDX-License-Identifier: MIT
/*
 * TX streams (one-shot and cyclic) against iiod-emu.
 *
 * Run through tests/emu/emu_pattern.sh on a temporary copy of the ad936x
 * fixture (no RX file is needed). iiod-emu writes each TX block it
 * dequeues to <device name>_buf0.bin beside the XML, overwriting from the
 * start, so after stop() the file holds the last block sent. The server
 * writes asynchronously: files are read only after stop(), whose drain
 * waits for every block.
 *
 * Instant k of the counting pattern is I = k mod 32768, Q = -I (plus
 * 1000 * c on channel c), as for RX (tests/emu/make_pattern.py).
 *
 * Not covered here: a one-shot stop() whose drain times out
 * (test_tx_stop_drain_timeout_reported in the plan). iiod-emu cannot stall
 * a dequeue through the public API: its server writes each TX block as
 * soon as it arrives. The path is covered on hardware by
 * tests/hardware/test_hw_stream_smoke.py::test_tx_oneshot_drains.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <iio/iio.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "stream.h" /* the cyclic tests inspect the stream's one block */

#define TX_FILE "cf-ad9361-dds-core-lpc_buf0.bin"

static const char *uri;
static char tx_path[4096];

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static struct datax_context *open_ctx(struct datax_device **dev)
{
	struct datax_context *ctx = NULL;

	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, dev) == 0);
	return ctx;
}

static struct datax_stream_config cfg(enum datax_direction dir, unsigned int mask,
				      size_t block_samples, unsigned int block_count,
				      bool cyclic)
{
	struct datax_stream_config c;

	memset(&c, 0, sizeof(c));
	c.direction = dir;
	c.channel_mask = mask;
	c.block_samples = block_samples;
	c.block_count = block_count;
	c.cyclic = cyclic;
	return c;
}

static const char *err(struct datax_context *ctx)
{
	return datax_context_get_last_error(ctx);
}

static int16_t *make_pattern(size_t n, unsigned int nch, size_t k0, int offset)
{
	int16_t *buf = calloc(n * nch, 4);

	CHECK(buf != NULL);
	for (size_t j = 0; j < n; j++)
		for (unsigned int c = 0; c < nch; c++) {
			int16_t i = (int16_t)((k0 + j + (size_t)offset * c) % 32768);

			buf[(j * nch + c) * 2] = i;
			buf[(j * nch + c) * 2 + 1] = (int16_t)-i;
		}
	return buf;
}

/* Read the TX file; *n is its length in frames of frame_bytes. */
static int16_t *read_tx(size_t frame_bytes, size_t *n)
{
	FILE *f = fopen(tx_path, "rb");
	long len;
	int16_t *buf;

	CHECK(f != NULL);
	CHECK(fseek(f, 0, SEEK_END) == 0);
	len = ftell(f);
	CHECK(len >= 0 && (size_t)len % frame_bytes == 0);
	rewind(f);
	buf = malloc((size_t)len + 1);
	CHECK(buf != NULL);
	CHECK(fread(buf, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	*n = (size_t)len / frame_bytes;
	return buf;
}

static void check_pattern(const int16_t *buf, size_t n, unsigned int nch,
			  size_t k0, int offset)
{
	for (size_t j = 0; j < n; j++) {
		for (unsigned int c = 0; c < nch; c++) {
			int16_t i = (int16_t)((k0 + j + (size_t)offset * c) % 32768);
			const int16_t *f = buf + (j * nch + c) * 2;

			if (f[0] != i || f[1] != (int16_t)-i) {
				fprintf(stderr, "instant %zu ch %u: got (%d,%d) want (%d,%d)\n",
					k0 + j, c, f[0], f[1], i, -i);
				CHECK(0);
			}
		}
	}
}

static void check_zero(const int16_t *buf, size_t n, unsigned int nch)
{
	for (size_t j = 0; j < n * nch * 2; j++)
		CHECK(buf[j] == 0);
}

static bool tx_file_exists(void)
{
	return access(tx_path, F_OK) == 0;
}

/* Block 1024, send 2048: two full blocks; the file holds the second. */
static void test_tx_oneshot_full_blocks(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 0x1, 1024, 4, false);
	struct datax_internal_stream_stats st;
	int16_t *src = make_pattern(2048, 1, 0, 0), *got;
	size_t n;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.enabled);
	CHECK(datax_stream_send(s, src, 2048, 1000) == 2048);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	CHECK(st.blocks_done == 2);
	got = read_tx(4, &n);
	CHECK(n == 1024);
	check_pattern(got, 1024, 1, 1024, 0);
	free(got);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(src);
}

/* Block 1024, send 1500: stop() pads the held partial block with zeros. */
static void test_tx_partial_block_padded_on_stop(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 0x1, 1024, 4, false);
	int16_t *src = make_pattern(1500, 1, 0, 0), *got;
	size_t n;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 1500, 1000) == 1500);
	CHECK(datax_stream_stop(s) == 0);
	got = read_tx(4, &n);
	CHECK(n == 1024);
	check_pattern(got, 476, 1, 1024, 0);
	check_zero(got + 476 * 2, 548, 1);
	free(got);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(src);
}

/* Sends of any size against any block size, through more blocks than the
 * ring holds (each block is reclaimed from libiio before it is refilled),
 * then a restart that begins afresh; two channels keep their frame order. */
static void test_tx_send_spans_and_reuses_blocks(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 0x3, 1000, 2, false);
	struct datax_internal_stream_stats st;
	int16_t *src = make_pattern(9000, 2, 0, 1000), *got;
	size_t n;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 700, 1000) == 700);
	CHECK(datax_stream_send(s, src + 700 * 4, 4300, -1) == 4300);
	CHECK(datax_stream_send(s, src + 5000 * 4, 4000, 1000) == 4000);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.blocks_done == 9);
	got = read_tx(8, &n);
	CHECK(n == 1000);
	check_pattern(got, 1000, 2, 8000, 1000);
	free(got);

	/* Restart: nothing held over from the first run. */
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 300, 1000) == 300);
	CHECK(datax_stream_stop(s) == 0);
	got = read_tx(8, &n);
	CHECK(n == 1000);
	check_pattern(got, 300, 2, 0, 1000);
	check_zero(got + 300 * 4, 700, 2);
	free(got);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(src);
}

static void test_tx_arg_errors(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL, *r = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 0x1, 1024, 2, false);
	struct datax_stream_config rc = cfg(DATAX_RX, 0x1, 1024, 2, false);
	int16_t buf[8] = { 0 };

	CHECK(datax_stream_send(NULL, buf, 1, 0) == -EINVAL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_send(s, buf, 1, 0) == -EINVAL);
	CHECK(strstr(err(ctx), "not started"));
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, NULL, 1, 0) == -EINVAL);
	CHECK(datax_stream_send(s, buf, 0, 0) == -EINVAL);
	CHECK(datax_stream_recv(s, buf, 1, NULL, 0) == -EINVAL);
	CHECK(strstr(err(ctx), "not an RX stream"));
	CHECK(datax_stream_create(dev, &rc, &r) == 0);
	CHECK(datax_stream_send(r, buf, 1, 0) == -EINVAL);
	CHECK(strstr(err(ctx), "not a TX stream"));
	datax_stream_destroy(r);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

/*
 * Cyclic TX enables at its single send, not at start(), and the buffer is
 * sized to exactly the samples sent.
 *
 * The file check is limited by the pinned emulator: iiod's binary responder
 * (libiio iiod/responder.c buffer_enqueue_block) answers a cyclic block at
 * once and never dequeues it, and iiod-emu writes TX data only at dequeue,
 * so the file stays empty. What the emulator does show is the enable: it
 * opens (creates) the file when the buffer is enabled, so the file must be
 * absent after start() and present after send(). If the file has content
 * (an emulator that writes cyclic blocks), it must be exactly the 300
 * instants sent. The block itself is checked through src/stream.h: one
 * block of 300 frames holding the pattern. That the waveform repeats on
 * hardware is covered by
 * tests/hardware/test_hw_loopback.py::test_digital_loopback (Task 6).
 */
static void test_cyclic_enables_at_send(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 0x1, 1024, 4, true);
	struct datax_internal_stream_stats st;
	int16_t *src = make_pattern(1025, 1, 0, 0), *got;
	size_t n;
	double t0;

	unlink(tx_path);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(s->bs == NULL && s->nblocks == 1); /* no hardware buffer yet */
	for (int cycle = 0; cycle < 2; cycle++) {
		CHECK(datax_stream_start(s) == 0);
		CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
		CHECK(!tx_file_exists()); /* not enabled at start() */
		CHECK(datax_stream_send(s, src, 300, 1000) == 300);
		CHECK(datax_internal_stream_stats(s, &st) == 0 && st.enabled);
		CHECK(tx_file_exists()); /* enabled at send */
		CHECK(s->bs != NULL && s->nblocks == 1);
		CHECK((size_t)((char *)iio_block_end(s->blocks[0]) -
			       (char *)iio_block_start(s->blocks[0])) == 300 * 4);
		check_pattern(iio_block_start(s->blocks[0]), 300, 1, 0, 0);
		CHECK(datax_stream_send(s, src, 300, 1000) == -EBUSY);
		CHECK(strstr(err(ctx), "cyclic"));
		t0 = now_s();
		CHECK(datax_stream_stop(s) == 0);
		CHECK(now_s() - t0 < 1.0);
		CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
		CHECK(s->bs == NULL); /* the next send sizes a new buffer */
		got = read_tx(4, &n);
		CHECK(n == 0 || n == 300);
		if (n)
			check_pattern(got, 300, 1, 0, 0);
		free(got);
		unlink(tx_path);
	}
	t0 = now_s();
	datax_stream_destroy(s);
	CHECK(now_s() - t0 < 1.0);

	/* More than block_samples on a fresh cyclic stream. */
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 1025, 1000) == -EINVAL);
	CHECK(strstr(err(ctx), "block_samples"));
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	/* The rejected send left it unsent: a valid one still works. */
	CHECK(datax_stream_send(s, src, 1024, 1000) == 1024);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.enabled);
	/* Destroying a transmitting cyclic stream stops it. */
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(src);
}

/*
 * stop() cannot enqueue the held partial block: the error names that, not
 * the drain. The test enqueues the block from behind the stream (marking
 * it queued so the drain still takes it back), so the stream's own
 * enqueue gets libiio's -EPERM.
 */
static void test_tx_stop_reports_failed_flush(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 0x1, 1024, 2, false);
	int16_t *src = make_pattern(1500, 1, 0, 0);

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 1500, 1000) == 1500);
	CHECK(s->cur == 1 && s->cur_off == 476 * 4);
	CHECK(iio_block_enqueue(s->blocks[1], 0, false) == 0);
	s->queued[1] = true;
	CHECK(datax_stream_stop(s) == -EPERM);
	CHECK(strstr(err(ctx), "enqueueing the held TX block"));
	CHECK(s->bs == NULL); /* freed; start() reopens */
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 1024, 1000) == 1024);
	CHECK(datax_stream_stop(s) == 0);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(src);
}

/*
 * A cyclic write that failed surfaces at stop(). On the legacy path
 * send() only queues the write and its result arrives through the dequeue
 * that stop() makes. The test produces a failing result on the binary
 * path instead: it takes the cyclic block back from libiio behind the
 * stream's back and enqueues it again, which the emulator, still holding
 * the block, refuses (-EBUSY); that error comes back through the dequeue.
 */
static void test_cyclic_stop_reports_failed_write(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 0x1, 256, 1, true);
	struct datax_internal_stream_stats st;
	int16_t *src = make_pattern(256, 1, 0, 0);
	int ret;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 256, 1000) == 256);
	CHECK(iio_block_dequeue(s->blocks[0], false) == 0);
	CHECK(iio_block_enqueue(s->blocks[0], 0, true) == 0);
	CHECK(s->queued[0]);
	ret = datax_stream_stop(s);
	CHECK(ret == -EBUSY);
	CHECK(strstr(err(ctx), "writing the cyclic block on cf-ad9361-dds-core-lpc failed"));
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	CHECK(s->bs == NULL);
	/* The stream is usable again. */
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_send(s, src, 256, 1000) == 256);
	CHECK(datax_stream_stop(s) == 0);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(src);
}

int main(void)
{
	const char *dir = getenv("DATAX_TEST_XML_DIR");

	uri = getenv("DATAX_TEST_URI");
	CHECK(uri != NULL && dir != NULL);
	CHECK((size_t)snprintf(tx_path, sizeof(tx_path), "%s/%s", dir, TX_FILE) <
	      sizeof(tx_path));
	test_tx_oneshot_full_blocks();
	test_tx_partial_block_padded_on_stop();
	test_tx_send_spans_and_reuses_blocks();
	test_tx_arg_errors();
	test_cyclic_enables_at_send();
	test_tx_stop_reports_failed_flush();
	test_cyclic_stop_reports_failed_write();
	return 0;
}
