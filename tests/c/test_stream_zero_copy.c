// SPDX-License-Identifier: MIT
/*
 * Zero-copy streaming (get_block/put_block) in both directions against
 * iiod-emu.
 *
 * Run through tests/emu/emu_pattern.sh with the 1-channel RX counting
 * pattern on cf-ad9361-lpc (I = k mod 32768, Q = -I). TX blocks are read
 * back from cf-ad9361-dds-core-lpc_buf0.bin, which iiod-emu overwrites with
 * each TX block it dequeues; files are read only after stop().
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <iio/iio.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "stream.h" /* the cyclic test inspects the stream's one block */

#define TX_FILE "cf-ad9361-dds-core-lpc_buf0.bin"

static const char *uri;
static char tx_path[4096];

static struct datax_context *open_ctx(struct datax_device **dev)
{
	struct datax_context *ctx = NULL;

	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, dev) == 0);
	return ctx;
}

static struct datax_stream_config cfg(enum datax_direction dir, size_t block_samples,
				      unsigned int block_count, bool cyclic)
{
	struct datax_stream_config c;

	memset(&c, 0, sizeof(c));
	c.direction = dir;
	c.channel_mask = 0x1;
	c.block_samples = block_samples;
	c.block_count = block_count;
	c.cyclic = cyclic;
	return c;
}

static const char *err(struct datax_context *ctx)
{
	return datax_context_get_last_error(ctx);
}

static void fill_pattern(int16_t *buf, size_t n, size_t k0)
{
	for (size_t j = 0; j < n; j++) {
		int16_t i = (int16_t)((k0 + j) % 32768);

		buf[2 * j] = i;
		buf[2 * j + 1] = (int16_t)-i;
	}
}

static void check_pattern(const int16_t *buf, size_t n, size_t k0)
{
	for (size_t j = 0; j < n; j++) {
		int16_t i = (int16_t)((k0 + j) % 32768);

		if (buf[2 * j] != i || buf[2 * j + 1] != (int16_t)-i) {
			fprintf(stderr, "instant %zu: got (%d,%d) want (%d,%d)\n",
				k0 + j, buf[2 * j], buf[2 * j + 1], i, -i);
			CHECK(0);
		}
	}
}

/* Read the TX file; *n is its length in 1-channel frames. */
static int16_t *read_tx(size_t *n)
{
	FILE *f = fopen(tx_path, "rb");
	long len;
	int16_t *buf;

	CHECK(f != NULL);
	CHECK(fseek(f, 0, SEEK_END) == 0);
	len = ftell(f);
	CHECK(len >= 0 && len % 4 == 0);
	rewind(f);
	buf = malloc((size_t)len + 1);
	CHECK(buf != NULL);
	CHECK(fread(buf, 1, (size_t)len, f) == (size_t)len);
	fclose(f);
	*n = (size_t)len / 4;
	return buf;
}

static void test_zero_copy_rx(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX, 4096, 3, false);
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	int16_t buf[8];
	void *a = NULL, *b = NULL, *d = NULL, *e = buf;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_get_block(s, &a, &info, 1000) == -EINVAL);
	CHECK(strstr(err(ctx), "not started"));
	CHECK(datax_stream_start(s) == 0);
	memset(&info, 0xa5, sizeof(info));
	CHECK(datax_stream_get_block(s, &a, &info, 1000) == 0);
	CHECK(a != NULL);
	CHECK(info.valid_samples == 4096);
	CHECK(info.timestamp_ticks == 0 && info.discontinuous == false);
	check_pattern(a, 4096, 0);
	CHECK(datax_stream_put_block(s, a) == 0);
	/* Several blocks lent at once, returned in the order lent. */
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	check_pattern(a, 4096, 4096);
	CHECK(datax_stream_get_block(s, &b, NULL, -1) == 0);
	check_pattern(b, 4096, 8192);
	CHECK(b != a);
	CHECK(datax_stream_get_block(s, &d, NULL, 1000) == 0);
	check_pattern(d, 4096, 12288);
	/* Every block is lent: nothing left to wait for. A failed call
	 * clears its out-pointer. */
	CHECK(datax_stream_get_block(s, &e, NULL, 1000) == -EBUSY);
	CHECK(e == NULL);
	CHECK(strstr(err(ctx), "lent"));
	CHECK(datax_stream_put_block(s, b) == -EINVAL); /* not the oldest */
	CHECK(strstr(err(ctx), "order"));
	CHECK(datax_stream_put_block(s, a) == 0);
	CHECK(datax_stream_put_block(s, b) == 0);
	CHECK(datax_stream_put_block(s, d) == 0);
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	check_pattern(a, 4096, 16384);
	CHECK(datax_stream_put_block(s, a) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.blocks_done == 5);
	/* No mixing: copying calls on a zero-copy stream. */
	CHECK(datax_stream_recv(s, buf, 2, NULL, 1000) == -EINVAL);
	CHECK(strstr(err(ctx), "zero-copy"));
	/* A block still lent at stop() is taken back; restart begins afresh. */
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_stream_put_block(s, a) == -EINVAL);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	check_pattern(a, 4096, 0);
	datax_stream_destroy(s); /* with a block still lent */
	datax_context_destroy(ctx);
}

static void test_copy_then_zero_copy_rejected(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL, *t = NULL;
	struct datax_stream_config c = cfg(DATAX_RX, 1024, 2, false);
	struct datax_stream_config tc = cfg(DATAX_TX, 1024, 2, false);
	int16_t buf[8] = { 0 };
	void *p = NULL;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 2, NULL, 1000) == 2);
	CHECK(datax_stream_get_block(s, &p, NULL, 1000) == -EINVAL);
	CHECK(strstr(err(ctx), "copy"));
	CHECK(datax_stream_create(dev, &tc, &t) == 0);
	CHECK(datax_stream_start(t) == 0);
	CHECK(datax_stream_send(t, buf, 2, 1000) == 2);
	CHECK(datax_stream_get_block(t, &p, NULL, 1000) == -EINVAL);
	CHECK(strstr(err(ctx), "copy"));
	datax_stream_destroy(t);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_zero_copy_tx(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 1024, 2, false);
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	int16_t buf[8] = { 0 }, *got;
	void *a = NULL, *b = NULL, *e = NULL;
	size_t n;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	memset(&info, 0xa5, sizeof(info));
	CHECK(datax_stream_get_block(s, &a, &info, 1000) == 0);
	CHECK(info.valid_samples == 1024);
	CHECK(info.timestamp_ticks == 0 && info.discontinuous == false);
	fill_pattern(a, 1024, 0);
	CHECK(datax_stream_put_block(s, a) == 0);
	/* Through more blocks than the ring holds: free blocks are reclaimed. */
	for (size_t k = 1; k < 5; k++) {
		CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
		fill_pattern(a, 1024, k * 1024);
		CHECK(datax_stream_put_block(s, a) == 0);
	}
	/* Both blocks lent: -EBUSY, and they must come back in order. */
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	CHECK(datax_stream_get_block(s, &b, NULL, 1000) == 0);
	CHECK(datax_stream_get_block(s, &e, NULL, 1000) == -EBUSY);
	CHECK(datax_stream_put_block(s, b) == -EINVAL);
	fill_pattern(a, 1024, 5 * 1024);
	fill_pattern(b, 1024, 6 * 1024);
	CHECK(datax_stream_put_block(s, a) == 0);
	CHECK(datax_stream_put_block(s, b) == 0);
	CHECK(datax_stream_send(s, buf, 2, 1000) == -EINVAL); /* no mixing */
	CHECK(strstr(err(ctx), "zero-copy"));
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.blocks_done == 7);
	got = read_tx(&n);
	CHECK(n == 1024);
	check_pattern(got, 1024, 6 * 1024);
	free(got);

	/* A block lent at stop() is discarded, not sent. */
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	fill_pattern(a, 1024, 100);
	CHECK(datax_stream_put_block(s, a) == 0);
	CHECK(datax_stream_get_block(s, &b, NULL, 1000) == 0);
	fill_pattern(b, 1024, 9999);
	CHECK(datax_stream_stop(s) == 0);
	got = read_tx(&n);
	CHECK(n == 1024);
	check_pattern(got, 1024, 100);
	free(got);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

/*
 * Cyclic zero-copy: get_block lends one block of block_samples (opened at
 * that call: create allocates no hardware buffer for a cyclic stream) and
 * put_block enqueues it cyclic, then enables. The pinned emulator never
 * writes a cyclic block to its file (see test_stream_tx.c,
 * test_cyclic_enables_at_send), so the file may stay empty; with content,
 * it must be the whole block. Repetition on hardware is covered by
 * tests/hardware/test_hw_loopback.py::test_digital_loopback (Task 6).
 */
static void test_zero_copy_tx_cyclic(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_TX, 512, 4, true);
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	void *a = NULL, *b = NULL;
	int16_t *got;
	size_t n;

	unlink(tx_path);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(s->bs == NULL);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_get_block(s, &a, &info, 1000) == 0);
	CHECK(info.valid_samples == 512);
	CHECK(s->nblocks == 1 &&
	      (size_t)((char *)iio_block_end(s->blocks[0]) - (char *)a) == 512 * 4);
	CHECK(datax_stream_get_block(s, &b, NULL, 1000) == -EBUSY);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	CHECK(access(tx_path, F_OK) != 0);
	fill_pattern(a, 512, 0);
	CHECK(datax_stream_put_block(s, a) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.enabled);
	CHECK(access(tx_path, F_OK) == 0);
	CHECK(datax_stream_put_block(s, a) == -EINVAL); /* already returned */
	CHECK(datax_stream_get_block(s, &b, NULL, 1000) == -EBUSY);
	CHECK(strstr(err(ctx), "cyclic"));
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	got = read_tx(&n);
	CHECK(n == 0 || n == 512);
	if (n)
		check_pattern(got, 512, 0);
	free(got);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

/*
 * A block libiio no longer holds (-EPERM, the legacy path's consumed read)
 * while other blocks are lent: get_block re-arms it, and the cursor must
 * skip the lent blocks it then reaches instead of reporting "every block
 * is lent". The emulator never consumes a block itself, so the test takes
 * it from libiio behind the stream's back (as test_stream_rx does).
 */
static void test_zero_copy_rx_rearm_skips_lent(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX, 4096, 3, false);
	void *a = NULL, *b = NULL, *d = NULL;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	CHECK(datax_stream_get_block(s, &b, NULL, 1000) == 0);
	CHECK(s->cur == 2 && s->queued[2]);
	/* Consume block 2 (instants 8192..12287) outside the stream. */
	CHECK(iio_block_dequeue(s->blocks[2], false) == 0);
	/* Re-armed; the cursor passes lent blocks 0 and 1 back to block 2. */
	CHECK(datax_stream_get_block(s, &d, NULL, 1000) == 0);
	CHECK(d == iio_block_start(s->blocks[2]));
	check_pattern(d, 4096, 12288);
	CHECK(datax_stream_put_block(s, a) == 0);
	CHECK(datax_stream_put_block(s, b) == 0);
	CHECK(datax_stream_put_block(s, d) == 0);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_put_foreign_pointer(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX, 1024, 2, false);
	int16_t buf[8];
	void *a = NULL;

	CHECK(datax_stream_put_block(NULL, buf) == -EINVAL);
	CHECK(datax_stream_get_block(NULL, &a, NULL, 0) == -EINVAL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_put_block(s, buf) == -EINVAL); /* not started */
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_get_block(s, NULL, NULL, 1000) == -EINVAL);
	CHECK(datax_stream_put_block(s, buf) == -EINVAL);
	CHECK(strstr(err(ctx), "not a lent block"));
	CHECK(datax_stream_put_block(s, NULL) == -EINVAL);
	CHECK(datax_stream_get_block(s, &a, NULL, 1000) == 0);
	CHECK(datax_stream_put_block(s, (char *)a + 4) == -EINVAL);
	CHECK(datax_stream_put_block(s, a) == 0);
	CHECK(datax_stream_put_block(s, a) == -EINVAL); /* double put */
	CHECK(strstr(err(ctx), "not a lent block"));
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

int main(void)
{
	const char *dir = getenv("DATAX_TEST_XML_DIR");

	uri = getenv("DATAX_TEST_URI");
	CHECK(uri != NULL && dir != NULL);
	CHECK((size_t)snprintf(tx_path, sizeof(tx_path), "%s/%s", dir, TX_FILE) <
	      sizeof(tx_path));
	test_zero_copy_rx();
	test_copy_then_zero_copy_rejected();
	test_zero_copy_tx();
	test_zero_copy_tx_cyclic();
	test_zero_copy_rx_rearm_skips_lent();
	test_put_foreign_pointer();
	return 0;
}
