// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <iio/iio.h>

#include "check.h"

int main(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct iio_context *ctx;

	CHECK(uri != NULL);
	ctx = iio_create_context(NULL, uri);
	CHECK(iio_err(ctx) == 0);
	CHECK(iio_context_get_devices_count(ctx) > 0);
	iio_context_destroy(ctx);
	return 0;
}
