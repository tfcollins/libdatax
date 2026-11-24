// SPDX-License-Identifier: MIT
/*
 * P3b capability probe: the Tick stream_gate_map parser and the RX_ARM,
 * TX_ARM, STREAM_GATE and TIMESTAMPS flags.
 *
 * Usage: test_caps_p3b parse    (no emulator: the gate-map grammar)
 *        test_caps_p3b matrix   (emulator, DATAX_TEST_URI: the fake Tick bound
 *                                to adrv9009-zu11eg, one row per input)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "fake_tick.h"
#include "internal.h"

#define BIT(c) (1u << (c))

static const unsigned int TIMED = BIT(DATAX_CAP_TIMED_COMMANDS);
static const unsigned int RX = BIT(DATAX_CAP_RX_ARM);
static const unsigned int TX = BIT(DATAX_CAP_TX_ARM);
static const unsigned int GATE = BIT(DATAX_CAP_STREAM_GATE);
static const unsigned int TS = BIT(DATAX_CAP_TIMESTAMPS);

/* {present, rx_level, tx_level, rx_mode, tx_mode, fhm_b, tx_fullmask_only} */
static void parse_is(const char *text, int ret, int p, int rl, int tl, int rm, int tm, int fb,
		     int tf)
{
	int o[7] = { 9, 9, 9, 9, 9, 9, 9 };
	const int want[7] = { p, rl, tl, rm, tm, fb, tf };

	CHECK(datax_internal_gate_map_parse(text, o) == ret);
	for (int i = 0; i < 7; i++) {
		if (o[i] != want[i])
			fprintf(stderr, "map \"%s\": field %d = %d, want %d\n", text, i, o[i],
				want[i]);
		CHECK(o[i] == want[i]);
	}
}

static void test_gate_map_parse(void)
{
	/* the standard map, every separator */
	parse_is("rx-dma,tx-dma,rx-mode,tx-mode", 0, 1, 0, 1, 2, 3, 0, 0);
	parse_is("rx-dma tx-dma rx-mode tx-mode", 0, 1, 0, 1, 2, 3, 0, 0);
	parse_is("rx-dma\ntx-dma\nrx-mode\ntx-mode\n", 0, 1, 0, 1, 2, 3, 0, 0);
	parse_is(" rx-dma, tx-dma ,\trx-mode,,tx-mode \r\n", 0, 1, 0, 1, 2, 3, 0, 0);
	/* a spare holds its position */
	parse_is("rx-dma,-,tx-dma,rx-mode,tx-mode", 0, 1, 0, 2, 3, 4, 0, 0);
	parse_is("-,tx-dma,-,tx-mode", 0, 1, -1, 1, -1, 3, 0, 0);
	/* an unknown token is ignored but advances the position */
	parse_is("rx-dma,bogus,tx-dma", 0, 1, 0, 2, -1, -1, 0, 0);
	/* empty text: a map that names nothing */
	parse_is("", 0, 1, -1, -1, -1, -1, 0, 0);
	parse_is(" \n", 0, 1, -1, -1, -1, -1, 0, 0);
	/* a duplicate rejects the whole map: absent */
	parse_is("rx-dma,tx-dma,rx-dma", -EINVAL, 0, -1, -1, -1, -1, 0, 0);
	parse_is("rx-dma,tx-mode,tx-mode", -EINVAL, 0, -1, -1, -1, -1, 0, 0);
	parse_is("rx-dma,fhm-b,fhm-b", -EINVAL, 0, -1, -1, -1, -1, 0, 0);
	/* presence tokens */
	parse_is("rx-dma,tx-dma,rx-mode,tx-mode,fhm-b", 0, 1, 0, 1, 2, 3, 1, 0);
	parse_is("rx-dma,tx-dma,rx-mode,tx-mode,tx-start-fullmask-only", 0, 1, 0, 1, 2, 3, 0, 1);
	parse_is("fhm-b,tx-start-fullmask-only", 0, 1, -1, -1, -1, -1, 1, 1);
	/* presence tokens before a gate role do not shift the bits */
	parse_is("fhm-b,rx-dma,tx-start-fullmask-only,tx-dma,rx-mode,tx-mode", 0, 1, 0, 1, 2, 3,
		 1, 1);
	/* text is required */
	CHECK(datax_internal_gate_map_parse(NULL, NULL) == -EINVAL);
}

struct row {
	const char *name;
	const char *map;          /* NULL: attribute absent */
	uint32_t bitmap;
	unsigned int depth;
	bool hide_tags;
	unsigned int want;        /* public flags */
};

#define FULL "rx-dma,tx-dma,rx-mode,tx-mode"

static void check_row(struct datax_device *dev, const struct row *r)
{
	struct fake_tick_config cfg = { .tick_hz = 100000000ULL, .queue_depth = r->depth,
					.capability_bitmap = r->bitmap };
	struct fake_tick *f = fake_tick_new(&cfg);
	const struct datax_caps *caps;
	unsigned int got = 0;
	char info[512];

	CHECK(f != NULL);
	fake_tick_set_gate_map(f, r->map);
	fake_tick_hide_tag_attrs(f, r->hide_tags);
	CHECK(datax_internal_tick_bind_fake(dev, &fake_tick_ops, f) == 0);
	caps = datax_device_get_caps(dev);
	for (int c = 0; c < DATAX_CAP_COUNT; c++)
		if (datax_caps_has(caps, (enum datax_cap)c))
			got |= BIT(c);
	if (got != r->want)
		fprintf(stderr, "row %s: flags 0x%x, want 0x%x\n", r->name, got, r->want);
	CHECK(got == r->want);
	CHECK(datax_internal_gate_info(dev, info, sizeof(info)) == 0);
	CHECK(strstr(info, "tag_bytes=") != NULL);
	/* every unset P3b flag has a reason in the doctor text */
	if (!(r->want & RX))
		CHECK(strstr(info, "rx_arm=no (") != NULL);
	if (!(r->want & TS))
		CHECK(strstr(info, "timestamps=no (") != NULL);
	if (r->want & GATE)
		CHECK(strstr(info, "stream_gate=yes") != NULL);
	CHECK(datax_internal_tick_bind_fake(dev, NULL, NULL) == 0);
	fake_tick_free(f);
}

static void test_caps_p3b_matrix(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	const struct row rows[] = {
		{ "full", FULL, 0xdf, 16, false, TIMED | RX | TX | GATE | TS },
		{ "no map", NULL, 0xdf, 16, false, TIMED },
		{ "no mode bits", "rx-dma,tx-dma", 0xdf, 16, false, TIMED | TS },
		{ "no 0x08", FULL, 0xd7, 16, false, TIMED | TS },
		{ "no 0x40", FULL, 0x9f, 16, false, TIMED | RX | TX | GATE },
		{ "hidden tag attrs", FULL, 0xdf, 16, true, TIMED | RX | TX | GATE },
		{ "tx-only map", "-,tx-dma,-,tx-mode", 0xdf, 16, false, TIMED | TX | GATE },
		{ "rx-only map", "rx-dma,-,rx-mode", 0xdf, 16, false, TIMED | RX | GATE | TS },
		{ "rx level without mode", "rx-dma,tx-dma,-,tx-mode", 0xdf, 16, false,
		  TIMED | TX | GATE | TS },
		{ "duplicate rejects map", "rx-dma,tx-dma,rx-mode,tx-mode,rx-dma", 0xdf, 16, false,
		  TIMED },
		{ "tick not usable", FULL, 0xdf, 1, false, 0 },
	};

	CHECK(uri != NULL);
	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device_count(ctx) == 1);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
		check_row(dev, &rows[i]);
	datax_context_destroy(ctx);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "parse"))
		test_gate_map_parse();
	else if (argc == 2 && !strcmp(argv[1], "matrix"))
		test_caps_p3b_matrix();
	else {
		fprintf(stderr, "usage: test_caps_p3b parse|matrix\n");
		return 2;
	}
	return 0;
}
