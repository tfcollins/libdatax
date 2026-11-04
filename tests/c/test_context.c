// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iio/iio.h>

#include <datax/datax.h>

#include "check.h"

int main(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	const char *xml = getenv("DATAX_TEST_XML");
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	unsigned int maj, min;
	unsigned long long v;

	CHECK(uri != NULL && xml != NULL);

	/* Bad URI: error, no context. */
	CHECK(datax_context_create_from_uri("ip:127.0.0.1:1", &ctx) < 0);
	CHECK(ctx == NULL);

	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(ctx != NULL);
	CHECK(datax_context_get_iio_context(ctx) != NULL);
	CHECK(datax_context_get_version(ctx, &maj, &min) == 0);

	if (!strcmp(xml, "ad936x")) {
		CHECK(datax_context_get_device_count(ctx) == 1);
		CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
		CHECK(!strcmp(datax_device_get_name(dev), "ad9361-phy"));
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_MAX_CHANNELS, &v) == 0 && v == 2);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 8);
		dev = NULL;
		CHECK(datax_context_get_device(ctx, 1, &dev) == -EINVAL);
		CHECK(strstr(datax_context_get_last_error(ctx), "index") != NULL);
		/* Non-radio devices are not datax devices but stay reachable. */
		CHECK(iio_context_find_device(datax_context_get_iio_context(ctx),
					      "xadc") != NULL);
		CHECK(iio_context_find_device(datax_context_get_iio_context(ctx),
					      "ad7291") != NULL);
		CHECK(iio_context_find_device(datax_context_get_iio_context(ctx),
					      "ad9517-3") != NULL);
	} else if (!strcmp(xml, "adrv9009-zu11eg")) {
		CHECK(datax_context_get_device_count(ctx) == 1);
		CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
		CHECK(!strcmp(datax_device_get_name(dev), "adrv9009"));
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_MAX_CHANNELS, &v) == 0 && v == 4);
		CHECK(datax_caps_get_limit(datax_device_get_caps(dev),
					   DATAX_LIMIT_HOP_TABLE_DEPTH, &v) == 0 && v == 0);
		CHECK(datax_context_get_device(ctx, 1, &dev) == -EINVAL);
	} else if (!strcmp(xml, "tick")) {
		CHECK(datax_context_get_device_count(ctx) == 0);
		CHECK(iio_context_find_device(datax_context_get_iio_context(ctx),
					      "axi-timed-command-scheduler") != NULL);
	} else {
		fprintf(stderr, "unknown DATAX_TEST_XML %s\n", xml);
		return 1;
	}

	CHECK(datax_context_set_event_callback(ctx, NULL, NULL) == 0);
	datax_context_destroy(ctx);
	datax_context_destroy(NULL);
	return 0;
}
