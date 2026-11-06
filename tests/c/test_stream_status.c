// SPDX-License-Identifier: MIT
/*
 * DMA overflow/underflow status: parsing, the poll's register sequence,
 * rate limiting and the discontinuity flag, against iiod-emu.
 *
 * Run through tests/emu/emu_pattern.sh with the RX counting pattern on
 * cf-ad9361-lpc. DATAX_TEST_MODE: "attr" (the fixture has direct_reg_access
 * on both streaming devices) or "noattr" (a copy without them).
 *
 * The emulator stores whatever is written to a debug attribute and echoes
 * it back, so a poll reads back its own select word (0x80000088 for RX),
 * not a register: a flag raised in hardware cannot be injected, and the
 * test cannot show that the clear write clears. The hardware tests cover
 * detection end to end; here the bit decoding is tested on words, the
 * sequence on what the emulator holds afterwards.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <iio/iio.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "stream.h"

static const char *uri;

static struct datax_context *open_ctx(struct datax_device **dev)
{
	struct datax_context *ctx = NULL;

	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, dev) == 0);
	return ctx;
}

static struct datax_stream_config cfg(enum datax_direction dir)
{
	struct datax_stream_config c;

	memset(&c, 0, sizeof(c));
	c.direction = dir;
	c.channel_mask = 0x1;
	c.block_samples = 1024;
	c.block_count = 4;
	return c;
}

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&t, NULL);
}

static void test_status_parses_bits(void)
{
	bool o = true, u = true;

	datax_stream_status_parse(DATAX_RX, 0x4, &o, &u);
	CHECK(o && !u);
	datax_stream_status_parse(DATAX_RX, 0x0, &o, &u);
	CHECK(!o && !u);
	/* RX decodes bit 2 only; the other bits of the word are ignored. */
	datax_stream_status_parse(DATAX_RX, ~0x4u, &o, &u);
	CHECK(!o && !u);
	datax_stream_status_parse(DATAX_RX, 0xffffffffu, &o, &u);
	CHECK(o && !u);
	datax_stream_status_parse(DATAX_TX, 0x1, &o, &u);
	CHECK(!o && u);
	datax_stream_status_parse(DATAX_TX, ~0x1u, &o, &u);
	CHECK(!o && !u);
	datax_stream_status_parse(DATAX_TX, 0x4, &o, &u);
	CHECK(!o && !u);
}

static void read_attr(struct datax_context *ctx, const char *dev, char *dst, size_t n)
{
	struct iio_device *d = iio_context_find_device(datax_context_get_iio_context(ctx), dev);
	const struct iio_attr *a;

	CHECK(d != NULL);
	a = iio_device_find_debug_attr(d, "direct_reg_access");
	CHECK(a != NULL);
	CHECK(iio_attr_read_raw(a, dst, n) > 0);
}

/* The poll selects the register (0x80000000 | byte offset). */
static void test_poll_sequence(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL, *t = NULL;
	struct datax_stream_config rc = cfg(DATAX_RX), tc = cfg(DATAX_TX);
	char v[64];
	bool o = true, u = true;

	CHECK(datax_stream_create(dev, &rc, &s) == 0);
	CHECK(datax_stream_create(dev, &tc, &t) == 0);
	CHECK(datax_stream_status_poll(s, &o, &u) == 0);
	CHECK(!o && !u); /* the emulator echoed the select word */
	read_attr(ctx, "cf-ad9361-lpc", v, sizeof(v));
	CHECK(strcmp(v, "0x80000088") == 0);
	CHECK(datax_stream_status_poll(t, &o, &u) == 0);
	CHECK(!o && !u);
	read_attr(ctx, "cf-ad9361-dds-core-lpc", v, sizeof(v));
	CHECK(strcmp(v, "0x80004088") == 0);
	datax_stream_destroy(t);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_poll_rate_limited(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX);
	struct datax_internal_stream_stats st;
	struct datax_block_info info;
	static int16_t buf[2 * 1024];
	unsigned long long before;
	double t0;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_internal_stream_stats(s, &st) == 0);
	CHECK(st.overflow_detect);
	CHECK(st.status_polls == 1); /* the probe at start */
	before = st.status_polls;
	t0 = now_ms();
	for (int i = 0; i < 10; i++)
		CHECK(datax_stream_recv(s, buf, 1024, &info, 1000) == 1024);
	CHECK(datax_internal_stream_stats(s, &st) == 0);
	CHECK(st.blocks_done >= 10);
	if (now_ms() - t0 < 90.0) {
		CHECK(st.status_polls - before <= 1);
	} else {
		/* A slow machine: one poll per elapsed 100 ms at most. */
		CHECK(st.status_polls - before <= (unsigned long long)((now_ms() - t0) / 100.0) + 1);
	}
	before = st.status_polls;
	sleep_ms(150);
	CHECK(datax_stream_recv(s, buf, 1024, &info, 1000) == 1024);
	CHECK(datax_internal_stream_stats(s, &st) == 0);
	CHECK(st.status_polls == before + 1);
	CHECK(st.overflows == 0 && !info.discontinuous);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

/* An overflow marks the next block, once. Injected as the flag the poll
 * would set (the emulator cannot raise it). */
static void test_discontinuous_flag(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX);
	struct datax_block_info info;
	static int16_t buf[2 * 1024];
	void *blk = NULL;

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	s->disc_pending = true;
	CHECK(datax_stream_recv(s, buf, 1024, &info, 1000) == 1024);
	CHECK(info.discontinuous);
	CHECK(datax_stream_recv(s, buf, 1024, &info, 1000) == 1024);
	CHECK(!info.discontinuous);
	datax_stream_destroy(s);

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	s->disc_pending = true;
	CHECK(datax_stream_get_block(s, &blk, &info, 1000) == 0);
	CHECK(info.discontinuous);
	CHECK(datax_stream_put_block(s, blk) == 0);
	CHECK(datax_stream_get_block(s, &blk, &info, 1000) == 0);
	CHECK(!info.discontinuous);
	CHECK(datax_stream_put_block(s, blk) == 0);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

/* A poll that saw the overflow but failed to clear it still counts it. The
 * emulator cannot fail a write or raise a flag, so the poll's outcome is fed
 * to the fold step directly. */
static void test_seen_overflow_survives_failed_clear(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX);

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	datax_stream_status_apply(s, -EIO, true, false, false);
	CHECK(s->stats.overflows == 1 && s->disc_pending);
	CHECK(s->detect_off && !s->stats.overflow_detect);
	datax_stream_destroy(s);

	CHECK(datax_stream_create(dev, &c, &s) == 0);
	datax_stream_status_apply(s, -EIO, true, false, true); /* probe: discard */
	CHECK(s->stats.overflows == 0 && !s->disc_pending && s->detect_off);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_stream_info(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX);
	char name[64];
	int detect = 99;

	CHECK(datax_internal_device_stream_info(dev, DATAX_RX, name, sizeof(name), &detect) == 0);
	CHECK(strcmp(name, "cf-ad9361-lpc") == 0);
	CHECK(detect == -1); /* never probed */
	CHECK(datax_internal_device_stream_info(dev, DATAX_TX, name, sizeof(name), &detect) == 0);
	CHECK(strcmp(name, "cf-ad9361-dds-core-lpc") == 0);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	CHECK(datax_internal_device_stream_info(dev, DATAX_RX, name, sizeof(name), &detect) == 0);
	CHECK(detect == 1);
	CHECK(datax_internal_device_stream_info(dev, DATAX_TX, name, sizeof(name), &detect) == 0);
	CHECK(detect == -1);
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

static void test_no_debug_attr_disables_detect(void)
{
	struct datax_device *dev = NULL;
	struct datax_context *ctx = open_ctx(&dev);
	struct datax_stream *s = NULL;
	struct datax_stream_config c = cfg(DATAX_RX);
	struct datax_internal_stream_stats st;
	struct datax_block_info info;
	static int16_t buf[2 * 1024];
	char name[64];
	int detect = 99;

	CHECK(datax_internal_device_stream_info(dev, DATAX_RX, name, sizeof(name), &detect) == 0);
	CHECK(detect == 0);
	CHECK(datax_stream_create(dev, &c, &s) == 0);
	CHECK(datax_stream_start(s) == 0);
	for (int i = 0; i < 6; i++) {
		CHECK(datax_stream_recv(s, buf, 1024, &info, 1000) == 1024);
		CHECK(!info.discontinuous);
		if (i == 2)
			sleep_ms(150);
	}
	CHECK(datax_internal_stream_stats(s, &st) == 0);
	CHECK(!st.overflow_detect);
	CHECK(st.overflows == 0 && st.underflows == 0);
	CHECK(st.status_polls <= 1); /* the probe only: it stays off for the life */
	datax_stream_destroy(s);
	datax_context_destroy(ctx);
}

int main(void)
{
	const char *mode = getenv("DATAX_TEST_MODE");

	uri = getenv("DATAX_TEST_URI");
	CHECK(uri != NULL && mode != NULL);
	if (!strcmp(mode, "attr")) {
		test_status_parses_bits();
		test_poll_sequence();
		test_poll_rate_limited();
		test_discontinuous_flag();
		test_seen_overflow_survives_failed_clear();
		test_stream_info();
	} else if (!strcmp(mode, "noattr")) {
		test_no_debug_attr_disables_detect();
	} else {
		CHECK(0);
	}
	printf("test_stream_status %s: ok\n", mode);
	return 0;
}
