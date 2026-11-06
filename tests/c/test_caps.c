// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <datax/datax.h>

#include "check.h"
#include "internal.h"

#define BIT(c) (1u << (c))

/* The public flags must be exactly `want` (a mask of BIT(cap)). */
static void check_public_flags(const struct datax_caps *caps, unsigned int want)
{
	for (int c = 0; c < DATAX_CAP_COUNT; c++)
		CHECK(datax_caps_has(caps, (enum datax_cap)c) == !!(want & BIT(c)));
}

int main(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	const char *xml = getenv("DATAX_TEST_XML");
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	unsigned long long v;

	CHECK(uri != NULL && xml != NULL);
	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);

	if (!strcmp(xml, "tick")) {
		/* test_caps_bare_context: Tick only, no radio devices. */
		CHECK(datax_context_get_device_count(ctx) == 0);
		datax_context_destroy(ctx);
		return 0;
	}

	CHECK(datax_context_get_device_count(ctx) == 1);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
				   DATAX_LIMIT_GUARD_MARGIN_MS, &v) == 0 && v == 10);

	if (!strcmp(xml, "ad936x")) {
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_MAX_CHANNELS, &v) == 0 && v == 2);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 8);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_TICK_HZ, &v) == 0 && v == 0);
		CHECK(datax_internal_probe_flags(dev) == 0);
		check_public_flags(datax_device_get_caps(dev), 0);
	} else if (!strcmp(xml, "ad936x+tick")) {
		CHECK(datax_internal_probe_flags(dev) ==
		      (BIT(DATAX_CAP_TIMED_COMMANDS) | BIT(DATAX_CAP_FASTLOCK_HOP)));
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_TICK_HZ, &v) == 0 && v == 100000000ULL);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_QUEUE_DEPTH, &v) == 0 && v == 16);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_MAX_CHANNELS, &v) == 0 && v == 2);
		/* fastlock hop ships later: only the scheduler is ungated */
		check_public_flags(datax_device_get_caps(dev), BIT(DATAX_CAP_TIMED_COMMANDS));
	} else if (!strcmp(xml, "adrv9009+tick")) {
		CHECK(datax_internal_probe_flags(dev) ==
		      (BIT(DATAX_CAP_TIMED_COMMANDS) | BIT(DATAX_CAP_FHM_HOP) |
		       BIT(DATAX_CAP_MCS)));
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_TICK_HZ, &v) == 0 && v == 100000000ULL);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_QUEUE_DEPTH, &v) == 0 && v == 16);
		/* FHM hop table: a host-side set of 64 (spec decision 10) */
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 64);
		/* This fixture has no FHM attributes and no jesd204_fsm_ctrl:
		 * detected, but the SDK cannot do FHM or MCS here. */
		check_public_flags(datax_device_get_caps(dev), BIT(DATAX_CAP_TIMED_COMMANDS));
	} else if (!strcmp(xml, "adrv9009+tick-fsm")) {
		/* test_caps_p3a_matrix, full board: Tick + FHM attributes + FSM
		 * attributes. PHASE_COHERENT stays false (no RF PLL phase sync). */
		CHECK(datax_internal_probe_flags(dev) ==
		      (BIT(DATAX_CAP_TIMED_COMMANDS) | BIT(DATAX_CAP_FHM_HOP) |
		       BIT(DATAX_CAP_MCS)));
		check_public_flags(datax_device_get_caps(dev),
				   BIT(DATAX_CAP_TIMED_COMMANDS) | BIT(DATAX_CAP_FHM_HOP) |
				   BIT(DATAX_CAP_MCS));
	} else if (!strcmp(xml, "adrv9009-zu11eg")) {
		check_public_flags(datax_device_get_caps(dev), 0);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 0);
		CHECK(datax_internal_probe_flags(dev) == BIT(DATAX_CAP_MCS));
	} else if (!strcmp(xml, "adrv9009-single")) {
		CHECK(datax_internal_probe_flags(dev) == 0);
		check_public_flags(datax_device_get_caps(dev), 0);
	} else {
		fprintf(stderr, "unknown DATAX_TEST_XML %s\n", xml);
		return 1;
	}

	datax_context_destroy(ctx);
	return 0;
}
