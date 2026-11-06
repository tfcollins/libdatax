// SPDX-License-Identifier: MIT
/*
 * run_mcs verification and the PHASE_COHERENT rule against the fsm fixture
 * (adrv9009+tick-fsm.xml: both phys complete). A failing FSM is produced by
 * writing the phy attributes through libiio, which the emulator stores.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iio/iio.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "internal.h"

#define BIT(c) (1u << (c))

static void set_attr(struct datax_context *ctx, const char *phy, const char *attr, const char *v)
{
	struct iio_device *d = iio_context_find_device(datax_context_get_iio_context(ctx), phy);
	const struct iio_attr *a = d ? iio_device_find_attr(d, attr) : NULL;

	CHECK(a != NULL);
	CHECK(iio_attr_write_string(a, v) >= 0);
}

int main(void)
{
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	const struct datax_caps *caps;
	char state[128], reason[128];
	const char *err;

	CHECK(getenv("DATAX_TEST_URI") != NULL);
	CHECK(datax_context_create_from_uri(getenv("DATAX_TEST_URI"), &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
	caps = datax_device_get_caps(dev);

	/* test_run_mcs_ok */
	CHECK(datax_caps_has(caps, DATAX_CAP_MCS));
	CHECK(!datax_caps_has(caps, DATAX_CAP_PHASE_COHERENT));
	CHECK(datax_device_run_mcs(dev) == 0);
	CHECK(datax_internal_mcs_info(dev, state, sizeof(state), reason, sizeof(reason)) == 0);
	CHECK(!strcmp(state, "opt_post_running_stage"));

	/* test_phase_coherent_false_without_phase_sync: MCS verified, but no
	 * RF PLL phase sync (permanent on adrv9009 in P3a). */
	CHECK(!datax_caps_has(caps, DATAX_CAP_PHASE_COHERENT));
	CHECK(strstr(reason, "phase sync not enabled"));

	/* The rule itself: verified AND phase sync AND no single-chip tune. */
	datax_internal_mcs_force_phase_sync(dev, true);
	CHECK(datax_caps_has(caps, DATAX_CAP_PHASE_COHERENT));
	CHECK(datax_internal_mcs_info(dev, state, sizeof(state), reason, sizeof(reason)) == 1);
	dev->single_chip_tuned = true;     /* what fhm.c sets when chip A tunes */
	datax_caps_refresh_flags(dev);
	CHECK(!datax_caps_has(caps, DATAX_CAP_PHASE_COHERENT));
	CHECK(datax_internal_mcs_info(dev, state, sizeof(state), reason, sizeof(reason)) == 0);
	CHECK(strstr(reason, "single-chip tune"));
	dev->single_chip_tuned = false;
	datax_caps_refresh_flags(dev);
	CHECK(datax_caps_has(caps, DATAX_CAP_PHASE_COHERENT));
	/* A failed verification drops it, and MCS support itself stays. */
	set_attr(ctx, "adrv9009-phy", "jesd204_fsm_state", "opt_dummy");
	CHECK(datax_device_run_mcs(dev) == -EIO);
	CHECK(!datax_caps_has(caps, DATAX_CAP_PHASE_COHERENT));
	CHECK(datax_caps_has(caps, DATAX_CAP_MCS));
	datax_internal_mcs_force_phase_sync(dev, false);
	set_attr(ctx, "adrv9009-phy", "jesd204_fsm_state", "opt_post_running_stage");

	/* test_run_mcs_error_names_phy: each failing condition on chip B */
	CHECK(datax_device_run_mcs(dev) == 0);
	set_attr(ctx, "adrv9009-phy-b", "jesd204_fsm_state", "opt_pre_clock_stage");
	CHECK(datax_device_run_mcs(dev) == -EIO);
	err = datax_context_get_last_error(ctx);
	CHECK(strstr(err, "adrv9009-phy-b") && strstr(err, "opt_pre_clock_stage"));
	CHECK(!strstr(err, "adrv9009-phy:"));
	CHECK(datax_internal_mcs_info(dev, state, sizeof(state), reason, sizeof(reason)) == 0);
	CHECK(strstr(state, "opt_post_running_stage / opt_pre_clock_stage"));
	CHECK(strstr(reason, "not verified"));
	set_attr(ctx, "adrv9009-phy-b", "jesd204_fsm_state", "opt_post_running_stage");

	set_attr(ctx, "adrv9009-phy", "jesd204_fsm_error", "-5");
	CHECK(datax_device_run_mcs(dev) == -EIO);
	err = datax_context_get_last_error(ctx);
	CHECK(strstr(err, "adrv9009-phy") && strstr(err, "jesd204_fsm_error") && strstr(err, "-5"));
	set_attr(ctx, "adrv9009-phy", "jesd204_fsm_error", "0");

	set_attr(ctx, "adrv9009-phy-b", "jesd204_fsm_paused", "1");
	CHECK(datax_device_run_mcs(dev) == -EIO);
	err = datax_context_get_last_error(ctx);
	CHECK(strstr(err, "adrv9009-phy-b") && strstr(err, "paused"));
	set_attr(ctx, "adrv9009-phy-b", "jesd204_fsm_paused", "0");

	/* never writes FSM attributes: jesd204_fsm_ctrl is untouched */
	{
		struct iio_device *d = iio_context_find_device(datax_context_get_iio_context(ctx),
							       "adrv9009-phy");
		char buf[16] = "";

		CHECK(iio_attr_read_raw(iio_device_find_attr(d, "jesd204_fsm_ctrl"), buf, sizeof(buf)) > 0);
		CHECK(buf[0] == '1');
	}
	CHECK(datax_device_run_mcs(dev) == 0);
	datax_context_destroy(ctx);
	return 0;
}
