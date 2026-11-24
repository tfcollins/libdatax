// SPDX-License-Identifier: MIT
/*
 * Capture tag parsing and block timestamp arithmetic (P3b S3 Task 4;
 * api-c-core "timestamp_ticks"). The tag is the 32-byte header the Tick
 * capture path writes at the front of the first block after a release
 * (tick capture.py CaptureHeader, little-endian):
 *   magic u32, version u16, header_words u8, flags u8, capture_tick u64,
 *   seq u32, target u16, command u16, tick_hz u32, burst u32.
 */
#include <errno.h>

#include "stream.h"

static uint64_t le(const uint8_t *p, unsigned int n)
{
	uint64_t v = 0;

	while (n--)
		v = v << 8 | p[n];
	return v;
}

int datax_tag_parse(const void *buf, size_t len, struct datax_tag *out)
{
	const uint8_t *b = buf;
	struct datax_tag t;

	if (!b || !out || len < 32)
		return -EINVAL;
	t.magic = (uint32_t)le(b, 4);
	t.version = (uint16_t)le(b + 4, 2);
	t.header_words = b[6];
	t.flags = b[7];
	t.capture_tick = le(b + 8, 8);
	t.seq = (uint32_t)le(b + 16, 4);
	t.target = (uint16_t)le(b + 20, 2);
	t.command = (uint16_t)le(b + 22, 2);
	t.tick_hz = (uint32_t)le(b + 24, 4);
	t.burst = (uint32_t)le(b + 28, 4);
	if (t.magic != DATAX_TAG_MAGIC || t.version != DATAX_TAG_VERSION)
		return -EINVAL;
	*out = t;
	return 0;
}

bool datax_tag_anchors(const struct datax_tag *t, int rx_level, unsigned long long tick_hz)
{
	return t && rx_level >= 0 && (t->flags & DATAX_TAG_VALID) &&
	       !(t->flags & (DATAX_TAG_STALE | DATAX_TAG_LATE_EXECUTED)) &&
	       DATAX_TAG_GATE(t->flags) == (unsigned int)rx_level &&
	       (t->tick_hz == 0 || t->tick_hz == tick_hz);
}

/* round-half-up(n * hz / fs) as (n / fs) * hz + round((n % fs) * hz / fs):
 * the product never exceeds fs * hz, so n up to 2^64 does not overflow. */
unsigned long long datax_frames_to_ticks(unsigned long long n, unsigned long long fs,
					 unsigned long long hz)
{
	if (!fs)
		return 0;
	return (n / fs) * hz + ((n % fs) * hz + fs / 2) / fs;
}

int datax_internal_tag_parse(const void *buf, size_t len, struct datax_tag *out)
{
	return datax_tag_parse(buf, len, out);
}

unsigned long long datax_internal_frames_to_ticks(unsigned long long n, unsigned long long fs,
						  unsigned long long hz)
{
	return datax_frames_to_ticks(n, fs, hz);
}
