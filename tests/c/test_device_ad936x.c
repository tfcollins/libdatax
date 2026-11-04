// SPDX-License-Identifier: MIT
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iio/iio.h>

#include <datax/datax.h>

#include "check.h"

static long long read_attr(struct datax_context *ctx, const char *chn_id,
			   int output, const char *attr)
{
	struct iio_context *ic = datax_context_get_iio_context(ctx);
	struct iio_device *d = iio_context_find_device(ic, "ad9361-phy");
	struct iio_channel *c = d ? iio_device_find_channel(d, chn_id, output) : NULL;
	const struct iio_attr *a = c ? iio_channel_find_attr(c, attr) : NULL;
	long long v = -1;

	CHECK(a != NULL);
	CHECK(iio_attr_read_longlong(a, &v) == 0);
	return v;
}

int main(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	unsigned long long hz = 0, prev = 0;
	double db = 0;

	CHECK(uri != NULL);
	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);

	/* RX LO (ch 0) and TX LO (ch 2 = first TX) */
CHECK(datax_device_set_frequency(dev, 0, 2450000000ULL, DATAX_TIME_NOW) == 0);
	CHECK(datax_device_get_frequency(dev, 0, &hz) == 0 && hz == 2450000000ULL);
	CHECK(read_attr(ctx, "altvoltage0", 1, "frequency") == 2450000000LL);
	CHECK(datax_device_set_frequency(dev, 2, 2400000000ULL, DATAX_TIME_NOW) == 0);
	CHECK(datax_device_get_frequency(dev, 2, &hz) == 0 && hz == 2400000000ULL);
	CHECK(read_attr(ctx, "altvoltage1", 1, "frequency") == 2400000000LL);

	/* Gain: RX and TX (negative dB attenuation) */
	CHECK(datax_device_set_gain(dev, 0, 30.0, DATAX_TIME_NOW) == 0);
	CHECK(datax_device_get_gain(dev, 0, &db) == 0 && fabs(db - 30.0) <= 0.5);
	CHECK(datax_device_set_gain(dev, 2, -10.0, DATAX_TIME_NOW) == 0);
	CHECK(datax_device_get_gain(dev, 2, &db) == 0 && fabs(db + 10.0) <= 0.5);

	/* Sample rate via the escape hatch */
	CHECK(datax_device_set_sample_rate(dev, 30720000ULL) == 0);
	CHECK(read_attr(ctx, "voltage0", 0, "sampling_frequency") == 30720000LL);

	/* Bandwidth round trip */
	CHECK(datax_device_set_bandwidth(dev, 1, 18000000ULL) == 0);
	CHECK(read_attr(ctx, "voltage1", 0, "rf_bandwidth") == 18000000LL);
	CHECK(datax_device_set_bandwidth(dev, 3, 12000000ULL) == 0);
	CHECK(read_attr(ctx, "voltage1", 1, "rf_bandwidth") == 12000000LL);

	/* Argument errors */
	CHECK(datax_device_set_frequency(dev, 7, 1000000000ULL, DATAX_TIME_NOW) == -EINVAL);
	CHECK(strstr(datax_context_get_last_error(ctx), "channel") != NULL);
	CHECK(datax_device_get_frequency(dev, 4, &hz) == -EINVAL);
	CHECK(datax_device_set_gain(dev, 9, 0.0, DATAX_TIME_NOW) == -EINVAL);
	CHECK(datax_device_set_bandwidth(dev, 4, 1000000ULL) == -EINVAL);
	CHECK(datax_device_set_frequency(NULL, 0, 1ULL, DATAX_TIME_NOW) == -EINVAL);
	CHECK(datax_device_get_frequency(NULL, 0, &hz) == -EINVAL);
	CHECK(datax_device_get_gain(NULL, 0, &db) == -EINVAL);
	CHECK(datax_device_set_sample_rate(NULL, 1ULL) == -EINVAL);
	CHECK(datax_device_set_bandwidth(NULL, 0, 1ULL) == -EINVAL);

	/* test_timed_set_is_enotsup_and_no_write */
	CHECK(datax_device_get_frequency(dev, 0, &prev) == 0);
	CHECK(datax_device_set_frequency(dev, 0, 2500000000ULL, 1000) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(ctx), "P3") != NULL);
	CHECK(datax_device_get_frequency(dev, 0, &hz) == 0 && hz == prev);
	CHECK(datax_device_set_gain(dev, 0, 10.0, 1000) == -ENOTSUP);
	CHECK(datax_device_get_gain(dev, 0, &db) == 0 && fabs(db - 30.0) <= 0.5);

	datax_context_destroy(ctx);
	return 0;
}
