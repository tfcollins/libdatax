// SPDX-License-Identifier: MIT
/*
 * Stream lifecycle and RX copying against iiod-emu.
 *
 * Run through tests/emu/emu_pattern.sh, which serves a temporary copy of the
 * fixture with the RX counting pattern (tests/emu/make_pattern.py) beside it:
 * instant k has I = k mod 32768, Q = -I on every enabled channel (plus
 * 1000 * c on channel c in the two-channel mode). DATAX_TEST_MODE picks the
 * fixture-specific group:
 *   ad936x          1-channel pattern on cf-ad9361-lpc
 *   ad936x-2ch      2-channel pattern, channel offset 1000
 *   adrv9009        1-channel pattern on axi-adrv9009-rx-hpc (4R4T pair)
 *   adrv9009-single single-phy fixture without streaming devices
 *   nofile          ad936x with no RX buffer file (start fails cleanly)
 *   destroy         the destroy/teardown group alone (run under valgrind)
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <iio/iio.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "stream.h" /* the rearm test reaches the stream's blocks */

static const char *uri;

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

static struct datax_stream_config rx_cfg(unsigned int mask, size_t block_samples,
					 unsigned int block_count)
{
	struct datax_stream_config c;

	memset(&c, 0, sizeof(c));
	c.direction = DATAX_RX;
	c.channel_mask = mask;
	c.block_samples = block_samples;
	c.block_count = block_count;
	c.cyclic = false;
	return c;
}

static const char *err(struct datax_context *ctx)
{
	return datax_context_get_last_error(ctx);
}

/* Check n frames of the pattern for nch channels starting at instant k0. */
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

static void test_create_rejects_bad_mask(unsigned int nrx)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = (struct datax_stream *)(uintptr_t)1;
	struct datax_stream_config c;

	c = rx_cfg(0, 1024, 4);
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL);
	CHECK(s == NULL);
	CHECK(strstr(err(ctx), "channel_mask"));

	c = rx_cfg(1u << nrx, 1024, 4);
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL);
	CHECK(strstr(err(ctx), "channel_mask"));

	c = rx_cfg(0x1, 0, 4);
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL);
	CHECK(strstr(err(ctx), "block_samples"));

	c = rx_cfg(0x1, 1024, 4);
	c.cyclic = true;
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL);
	CHECK(strstr(err(ctx), "cyclic"));

	c = rx_cfg(0x1, 1024, 0);
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL);
	CHECK(strstr(err(ctx), "block_count"));

	c = rx_cfg(0x1, 1024, 4);
	c.direction = (enum datax_direction)7;
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL);
	CHECK(strstr(err(ctx), "direction"));

	CHECK(datax_stream_create(dev, NULL, &s) == -EINVAL);
	CHECK(strstr(err(ctx), "config"));
	c = rx_cfg(0x1, 1024, 4);
	CHECK(datax_stream_create(dev, &c, NULL) == -EINVAL);
	CHECK(datax_stream_create(NULL, &c, &s) == -EINVAL);

	/* The highest valid channel is accepted. */
	c = rx_cfg(1u << (nrx - 1), 1024, 4);
	CHECK(datax_stream_create(dev, &c, &s) == 0 && s != NULL);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_one_stream_per_direction(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *a = NULL, *b = NULL, *t = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1024, 2);
	struct datax_stream_config tc = rx_cfg(0x1, 1024, 2);

	CHECK(datax_stream_create(dev, &c, &a) == 0);
	CHECK(datax_stream_create(dev, &c, &b) == -EBUSY);
	CHECK(b == NULL);
	CHECK(strstr(err(ctx), "RX stream"));
	/* The other direction is independent. */
	tc.direction = DATAX_TX;
	CHECK(datax_stream_create(dev, &tc, &t) == 0);
	datax_stream_destroy(a);
	CHECK(datax_stream_create(dev, &c, &b) == 0);
	datax_stream_destroy(b);
	datax_stream_destroy(t);
	datax_context_destroy(ctx);
}

static void test_start_stop_rules(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1024, 4);
	struct datax_internal_stream_stats st;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	CHECK(datax_stream_stop(s) == 0); /* stopping a stopped stream */
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.enabled);
	CHECK(datax_stream_start(s) == -EALREADY);
	CHECK(strstr(err(ctx), "already started"));
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(datax_stream_start_at(s, 1000) == -ENOTSUP);
	CHECK(strcmp(err(ctx), "timed start arrives with the scheduler (P3)") == 0);
	CHECK(datax_internal_stream_stats(NULL, &st) == -EINVAL);
	CHECK(datax_internal_stream_stats(s, NULL) == -EINVAL);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_recv_block(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 4096, 4);
	struct datax_block_info info;
	struct datax_internal_stream_stats st;
	int16_t *buf = calloc(4096, 4);

	CHECK(buf != NULL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	memset(&info, 0xa5, sizeof(info));
	CHECK(datax_stream_recv(s, buf, 4096, &info, 1000) == 4096);
	check_pattern(buf, 4096, 1, 0, 0);
	CHECK(info.valid_samples == 4096);
	CHECK(info.timestamp_ticks == 0);
	CHECK(info.discontinuous == false);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.blocks_done == 1);
	CHECK(datax_stream_stop(s) == 0);
	/* The three blocks drained at stop were not delivered: not counted. */
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.blocks_done == 1);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(buf);
}

static void test_recv_spans_blocks(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1000, 4);
	struct datax_block_info info;
	int16_t *buf = calloc(4500, 4);

	CHECK(buf != NULL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 1500, &info, 1000) == 1500);
	CHECK(info.valid_samples == 1000);
	CHECK(datax_stream_recv(s, buf + 1500 * 2, 700, NULL, 1000) == 700);
	CHECK(datax_stream_recv(s, buf + 2200 * 2, 2300, &info, -1) == 2300);
	check_pattern(buf, 4500, 1, 0, 0);
	datax_stream_destroy(s); /* destroy stops */
	datax_context_destroy(ctx);
	free(buf);
}

/* stop() mid-block, then start(): the stream begins afresh (the emulator
 * reopens its file at enable, so the pattern restarts at instant 0). */
static void test_restart_begins_afresh(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1000, 3);
	int16_t *buf = calloc(2500, 4);

	CHECK(buf != NULL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	for (int cycle = 0; cycle < 3; cycle++) {
		CHECK(datax_stream_start(s) == 0);
		CHECK(datax_stream_recv(s, buf, 2500, NULL, 1000) == 2500);
		check_pattern(buf, 2500, 1, 0, 0);
		CHECK(datax_stream_stop(s) == 0);
	}
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(buf);
}

/*
 * A block libiio no longer holds. On the legacy (v0 iiod) path a failed or
 * timed-out read consumes the block, and the next dequeue gives -EPERM.
 * The emulator's binary path never does that by itself, so the test takes
 * the block from libiio behind the stream's back (the same state), then
 * checks that recv re-arms it and continues in ring order (that block's
 * samples are lost, the rest stay contiguous), and that stop/start still
 * work. The real timed-out read is covered on hardware by
 * tests/hardware/test_hw_stream_smoke.py::test_recv_timeout_recovers.
 */
static void test_recv_rearms_unqueued_block(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1000, 4);
	struct datax_internal_stream_stats st;
	int16_t *buf = calloc(4000, 4);

	CHECK(buf != NULL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 1000, NULL, 1000) == 1000);
	check_pattern(buf, 1000, 1, 0, 0);
	CHECK(s->cur == 1 && s->queued[1]);
	/* Consume block 1 (instants 1000..1999) outside the stream. */
	CHECK(iio_block_dequeue(s->blocks[1], false) == 0);
	/* Block 1 is re-armed as the newest; recv continues with block 2. */
	CHECK(datax_stream_recv(s, buf, 4000, NULL, 1000) == 4000);
	check_pattern(buf, 4000, 1, 2000, 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && st.blocks_done == 5);
	/* Steal again, then stop: the drain skips a block libiio no longer
	 * holds, and the stream restarts from instant 0. */
	CHECK(iio_block_dequeue(s->blocks[(s->cur + 1) % 4], false) == 0);
	CHECK(datax_stream_stop(s) == 0);
	CHECK(s->bs != NULL); /* no reset was needed */
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 2500, NULL, 1000) == 2500);
	check_pattern(buf, 2500, 1, 0, 0);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(buf);
}

static void test_recv_arg_errors(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL, *t = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 256, 2);
	struct datax_stream_config tc = rx_cfg(0x1, 256, 2);
	int16_t buf[512];

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_recv(s, buf, 16, NULL, 100) == -EINVAL); /* not started */
	CHECK(strstr(err(ctx), "not started"));
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, NULL, 16, NULL, 100) == -EINVAL);
	CHECK(datax_stream_recv(s, buf, 0, NULL, 100) == -EINVAL);
	CHECK(datax_stream_recv(NULL, buf, 16, NULL, 100) == -EINVAL);
	tc.direction = DATAX_TX;
	CHECK(datax_stream_create(dev, &tc, &t) == 0);
	CHECK(datax_stream_recv(t, buf, 16, NULL, 100) == -EINVAL);
	CHECK(strstr(err(ctx), "RX"));
	datax_stream_destroy(t);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_recv_two_channels(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x3, 512, 4);
	int16_t *buf = calloc(1300, 8);

	CHECK(buf != NULL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 600, NULL, 1000) == 600);
	CHECK(datax_stream_recv(s, buf + 600 * 4, 700, NULL, 1000) == 700);
	/* 8-byte frames: ch0 I, ch0 Q, ch1 I, ch1 Q; ch1 counts 1000 ahead. */
	check_pattern(buf, 1300, 2, 0, 1000);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(buf);
}

static void test_destroy_started_stream(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1000, 4);
	int16_t buf[1500 * 2];

	datax_stream_destroy(NULL);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 1500, NULL, 1000) == 1500);
	datax_stream_destroy(s); /* started, mid-block */
	/* The slot is free again. */
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	datax_stream_destroy(s); /* started, nothing read */
	datax_context_destroy(ctx);
}

static void test_context_destroy_frees_streams(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL, *t = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1000, 4);
	struct datax_stream_config tc = rx_cfg(0x1, 1000, 2);
	int16_t buf[1500 * 2];

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	tc.direction = DATAX_TX;
	CHECK(datax_stream_create(dev, &tc, &t) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 1500, NULL, 1000) == 1500);
	datax_context_destroy(ctx); /* both streams still alive */
}

static void test_adrv9009_pair(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x10, 1024, 4);
	int16_t *buf = calloc(3000, 4);

	CHECK(buf != NULL);
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL); /* 4R: bit 4 */
	c = rx_cfg(0x8, 1024, 4); /* radio channel 3 (voltage3_i/_q) */
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 3000, NULL, 1000) == 3000);
	check_pattern(buf, 3000, 1, 0, 0);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
	free(buf);
}

static void test_adrv9009_single(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x4, 1024, 4);

	/* 2R: bit 2 is out of range even though the family maps radio 0-3;
	 * the mask is checked before the (absent) streaming device. */
	CHECK(datax_stream_create(dev, &c, &s) == -EINVAL);
	CHECK(strstr(err(ctx), "channel_mask"));
	c = rx_cfg(0x2, 1024, 4);
	CHECK(datax_stream_create(dev, &c, &s) == -ENOTSUP);
	CHECK(s == NULL);
	CHECK(strstr(err(ctx), "axi-adrv9009-rx-hpc"));
	datax_context_destroy(ctx);
}

/* Write the 1-channel counting pattern for cf-ad9361-lpc into dir. */
static void write_pattern(const char *dir)
{
	char path[1024];
	FILE *f;

	snprintf(path, sizeof(path), "%s/cf-ad9361-lpc_buf0.bin", dir);
	f = fopen(path, "wb");
	CHECK(f != NULL);
	for (int k = 0; k < 32768; k++) {
		int16_t iq[2] = { (int16_t)k, (int16_t)-k };

		CHECK(fwrite(iq, sizeof(iq), 1, f) == 1);
	}
	CHECK(fclose(f) == 0);
}

/*
 * Observed iiod-emu behaviour (libiio emu.c emu_enable_buffer): a missing
 * RX buffer file is rejected when the buffer is ENABLED, so start() fails
 * with -ENOENT and no data ever flows; the stream is left stopped (blocks
 * freed, buffer closed), and recv on it fails fast with -EINVAL "not
 * started" well within 2x timeout_ms.
 *
 * This is not a recv timeout test: recv's -ETIMEDOUT branch and its
 * recovery need a started stream whose data stalls, which the emulator
 * cannot produce (it serves its file without pacing). The hardware test
 * tests/hardware/test_hw_stream_smoke.py::test_recv_timeout_recovers
 * (Task 6) covers it.
 *
 * The emulator's server side then waits up to 5 s per queued block for the
 * file; the short iio timeout keeps the client's teardown at ~0.5 s. After
 * the file appears, the next start() reopens the buffer lazily and streams.
 */
static void test_start_without_data_file_fails_cleanly(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct iio_context *ic = datax_context_get_iio_context(ctx);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = rx_cfg(0x1, 1024, 2);
	struct datax_internal_stream_stats st;
	const char *dir = getenv("DATAX_TEST_XML_DIR");
	int16_t buf[1024 * 2];
	double t0;
	int ret;

	CHECK(dir != NULL);
	CHECK(iio_context_set_timeout(ic, 500) == 0);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	t0 = now_s();
	ret = datax_stream_start(s);
	CHECK(ret == -ENOENT);
	CHECK(strstr(err(ctx), "cf-ad9361-lpc"));
	CHECK(now_s() - t0 < 5.0);
	CHECK(datax_internal_stream_stats(s, &st) == 0 && !st.enabled);
	t0 = now_s();
	CHECK(datax_stream_recv(s, buf, 1024, NULL, 200) == -EINVAL);
	CHECK(now_s() - t0 < 0.4);
	CHECK(strstr(err(ctx), "not started"));

	/* Recovery: the file appears; start() reopens the buffer. The server
	 * may still be busy with the failed blocks, so allow it time. */
	write_pattern(dir);
	CHECK(iio_context_set_timeout(ic, 15000) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_stream_recv(s, buf, 1024, NULL, 5000) == 1024);
	check_pattern(buf, 1024, 1, 0, 0);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

int main(void)
{
	const char *mode = getenv("DATAX_TEST_MODE");

	uri = getenv("DATAX_TEST_URI");
	CHECK(uri != NULL && mode != NULL);
	if (!strcmp(mode, "ad936x")) {
		test_create_rejects_bad_mask(2);
		test_one_stream_per_direction();
		test_start_stop_rules();
		test_recv_block();
		test_recv_spans_blocks();
		test_restart_begins_afresh();
		test_recv_rearms_unqueued_block();
		test_recv_arg_errors();
		test_destroy_started_stream();
		test_context_destroy_frees_streams();
	} else if (!strcmp(mode, "ad936x-2ch")) {
		test_recv_two_channels();
	} else if (!strcmp(mode, "adrv9009")) {
		test_create_rejects_bad_mask(4);
		test_adrv9009_pair();
	} else if (!strcmp(mode, "adrv9009-single")) {
		test_adrv9009_single();
	} else if (!strcmp(mode, "nofile")) {
		test_start_without_data_file_fails_cleanly();
	} else if (!strcmp(mode, "destroy")) {
		test_destroy_started_stream();
		test_context_destroy_frees_streams();
	} else {
		fprintf(stderr, "unknown DATAX_TEST_MODE %s\n", mode);
		return 2;
	}
	return 0;
}
