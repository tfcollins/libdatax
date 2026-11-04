// SPDX-License-Identifier: MIT
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iio/iio.h>

#include <datax/datax.h>

#include "check.h"

/* Read an attribute straight off one phy through the IIO escape hatch. */
static long long read_ll(struct datax_context *ctx, const char *phy,
			 const char *chn_id, int output, const char *attr)
{
	struct iio_context *ic = datax_context_get_iio_context(ctx);
	struct iio_device *d = iio_context_find_device(ic, phy);
	struct iio_channel *c = d ? iio_device_find_channel(d, chn_id, output) : NULL;
	const struct iio_attr *a = c ? iio_channel_find_attr(c, attr) : NULL;
	long long v = -1;

	CHECK(a != NULL);
	CHECK(iio_attr_read_longlong(a, &v) == 0);
	return v;
}

static double read_db(struct datax_context *ctx, const char *phy,
		      const char *chn_id, int output)
{
	struct iio_context *ic = datax_context_get_iio_context(ctx);
	struct iio_device *d = iio_context_find_device(ic, phy);
	struct iio_channel *c = d ? iio_device_find_channel(d, chn_id, output) : NULL;
	const struct iio_attr *a = c ? iio_channel_find_attr(c, "hardwaregain") : NULL;
	double v = 1e9;

	CHECK(a != NULL);
	CHECK(iio_attr_read_double(a, &v) == 0);
	return v;
}

static void test_pair(const char *uri)
{
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	unsigned long long hz = 0, v = 99;
	double db = 0;

	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
				   DATAX_LIMIT_MAX_CHANNELS, &v) == 0 && v == 4);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
				   DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 0);
	/* 4R4T: channels 0..7 valid, 8 is not */
	CHECK(datax_device_get_frequency(dev, 7, &hz) == 0);

	/* LO: ch 2 lands on phy-b, ch 0 on phy-a; ch 1 shares ch 0's LO */
	CHECK(datax_device_set_frequency(dev, 2, 2450000000ULL, DATAX_TIME_NOW) == 0);
	CHECK(datax_device_get_frequency(dev, 2, &hz) == 0 && hz == 2450000000ULL);
	CHECK(read_ll(ctx, "adrv9009-phy-b", "altvoltage0", 1, "frequency") == 2450000000LL);
	CHECK(read_ll(ctx, "adrv9009-phy", "altvoltage0", 1, "frequency") == 2400000000LL);
	CHECK(datax_device_set_frequency(dev, 1, 1900000000ULL, DATAX_TIME_NOW) == 0);
	CHECK(read_ll(ctx, "adrv9009-phy", "altvoltage0", 1, "frequency") == 1900000000LL);
	CHECK(datax_device_get_frequency(dev, 0, &hz) == 0 && hz == 1900000000ULL);
	CHECK(datax_device_get_frequency(dev, 3, &hz) == 0 && hz == 2450000000ULL);

	/* Gain: RX ch 3 -> phy-b input voltage1 */
	CHECK(datax_device_set_gain(dev, 3, 20.0, DATAX_TIME_NOW) == 0);
	CHECK(fabs(read_db(ctx, "adrv9009-phy-b", "voltage1", 0) - 20.0) <= 0.5);
	CHECK(fabs(read_db(ctx, "adrv9009-phy", "voltage1", 0) - 10.0) <= 0.5);
	CHECK(datax_device_get_gain(dev, 3, &db) == 0 && fabs(db - 20.0) <= 0.5);

	/* TX channels follow rx_channels (4..7): TX 2,3 -> phy-b output */
	CHECK(datax_device_set_gain(dev, 4 + 3, 5.0, DATAX_TIME_NOW) == 0);
	CHECK(fabs(read_db(ctx, "adrv9009-phy-b", "voltage1", 1) - 5.0) <= 0.5);
	CHECK(datax_device_set_gain(dev, 4 + 1, 7.0, DATAX_TIME_NOW) == 0);
	CHECK(fabs(read_db(ctx, "adrv9009-phy", "voltage1", 1) - 7.0) <= 0.5);
	CHECK(datax_device_set_frequency(dev, 4 + 2, 3000000000ULL, DATAX_TIME_NOW) == 0);
	CHECK(read_ll(ctx, "adrv9009-phy-b", "altvoltage0", 1, "frequency") == 3000000000LL);

	/* Sample rate is fixed by the JESD profile; bandwidth unsupported */
	CHECK(datax_device_set_sample_rate(dev, 122880000ULL) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(ctx), "JESD") != NULL);
	CHECK(read_ll(ctx, "adrv9009-phy", "voltage0", 0, "sampling_frequency") == 245760000LL);
	CHECK(datax_device_set_bandwidth(dev, 0, 100000000ULL) == -ENOTSUP);
	CHECK(read_ll(ctx, "adrv9009-phy", "voltage0", 0, "rf_bandwidth") == 200000000LL);

	/* Range errors use the folded channel count */
	CHECK(datax_device_set_frequency(dev, 8, 1000000000ULL, DATAX_TIME_NOW) == -EINVAL);

	datax_context_destroy(ctx);
}

int main(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	const char *mode = getenv("DATAX_TEST_MODE");
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	unsigned long long v = 99;
	unsigned long long hz = 0;

	CHECK(uri != NULL);
	if (!mode || strcmp(mode, "single")) {
		test_pair(uri);
		return 0;
	}

	/* Single-phy board: 2R2T on phy 0 only */
	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
				   DATAX_LIMIT_MAX_CHANNELS, &v) == 0 && v == 2);
	CHECK(datax_device_get_frequency(dev, 3, &hz) == 0); /* 2R2T: 0..3 */
	CHECK(datax_device_get_frequency(dev, 4, &hz) == -EINVAL);
	CHECK(datax_device_set_frequency(dev, 2, 1000000000ULL, DATAX_TIME_NOW) == 0); /* TX 0 */
	CHECK(read_ll(ctx, "adrv9009-phy", "altvoltage0", 1, "frequency") == 1000000000LL);
	datax_context_destroy(ctx);
	return 0;
}
