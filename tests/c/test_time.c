// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <datax/datax.h>

#include "check.h"

int main(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	const char *xml = getenv("DATAX_TEST_XML");
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	unsigned long long ticks = 99, hz = 99;
	enum datax_time_source src = DATAX_TIME_PPS_HOLDOVER;

	CHECK(uri != NULL && xml != NULL);
	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);

	CHECK(datax_device_get_time(NULL, &ticks, &hz) == -EINVAL);
	CHECK(datax_device_get_time(dev, NULL, &hz) == -EINVAL);
	CHECK(datax_device_get_time(dev, &ticks, NULL) == -EINVAL);
	CHECK(datax_device_get_time_source(dev, NULL) == -EINVAL);

	if (strstr(xml, "+tick")) {
		CHECK(datax_device_get_time(dev, &ticks, &hz) == 0);
		CHECK(ticks == 0);
		CHECK(hz == 100000000ULL);
		/* Tick present, nothing aligned yet: the free-running clock */
		CHECK(datax_device_get_time_source(dev, &src) == 0);
		CHECK(src == DATAX_TIME_FREE_RUN);
	} else {
		CHECK(datax_device_get_time_source(dev, &src) == -ENOTSUP);
		CHECK(strstr(datax_context_get_last_error(ctx),
			     "no Tick scheduler in this context") != NULL);
		CHECK(datax_device_get_time(dev, &ticks, &hz) == -ENOTSUP);
		CHECK(strstr(datax_context_get_last_error(ctx),
			     "no Tick scheduler in this context") != NULL);
	}
	datax_context_destroy(ctx);
	return 0;
}
