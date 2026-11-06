// SPDX-License-Identifier: MIT
/* Per-family streaming device map: channel-id mapping (pure) and the
 * fold-time lookup of the DMA devices (needs the emulated context). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <iio/iio.h>

#include <datax/datax.h>

#include "check.h"
#include "internal.h"
#include "platform/family.h"

static void test_ad936x_map(void)
{
	const struct datax_family_ops *ops = datax_family_ops(DATAX_FAMILY_AD936X);
	const char *i = NULL, *q = NULL;

	CHECK(ops != NULL && ops->stream_channel_ids != NULL);
	CHECK(!strcmp(ops->rx_stream_dev, "cf-ad9361-lpc"));
	CHECK(!strcmp(ops->tx_stream_dev, "cf-ad9361-dds-core-lpc"));
	CHECK(ops->stream_channel_ids(0, DATAX_RX, &i, &q) == 0);
	CHECK(!strcmp(i, "voltage0") && !strcmp(q, "voltage1"));
	CHECK(ops->stream_channel_ids(1, DATAX_TX, &i, &q) == 0);
	CHECK(!strcmp(i, "voltage2") && !strcmp(q, "voltage3"));
	CHECK(ops->stream_channel_ids(2, DATAX_RX, &i, &q) == -EINVAL);
}

static void test_adrv9009_map(void)
{
	const struct datax_family_ops *ops = datax_family_ops(DATAX_FAMILY_ADRV9009);
	const char *i = NULL, *q = NULL;

	CHECK(ops != NULL && ops->stream_channel_ids != NULL);
	CHECK(!strcmp(ops->rx_stream_dev, "axi-adrv9009-rx-hpc"));
	CHECK(!strcmp(ops->tx_stream_dev, "axi-adrv9009-tx-hpc"));
	CHECK(ops->stream_channel_ids(0, DATAX_TX, &i, &q) == 0);
	CHECK(!strcmp(i, "voltage0") && !strcmp(q, "voltage1"));
	CHECK(ops->stream_channel_ids(3, DATAX_TX, &i, &q) == 0);
	CHECK(!strcmp(i, "voltage6") && !strcmp(q, "voltage7"));
	CHECK(ops->stream_channel_ids(3, DATAX_RX, &i, &q) == 0);
	CHECK(!strcmp(i, "voltage3_i") && !strcmp(q, "voltage3_q"));
	CHECK(ops->stream_channel_ids(4, DATAX_RX, &i, &q) == -EINVAL);
	CHECK(ops->stream_channel_ids(4, DATAX_TX, &i, &q) == -EINVAL);
}

static void test_fold_finds_stream_devs(const char *uri, const char *xml)
{
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	int expect = strcmp(xml, "adrv9009-single") != 0;

	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device_count(ctx) == 1);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);
	CHECK((dev->stream_dev[DATAX_RX] != NULL) == expect);
	CHECK((dev->stream_dev[DATAX_TX] != NULL) == expect);
	CHECK(dev->stream[DATAX_RX] == NULL && dev->stream[DATAX_TX] == NULL);
	if (expect) {
		struct iio_context *ic = datax_context_get_iio_context(ctx);
		const struct datax_family_ops *ops = datax_family_ops(dev->family);

		CHECK(dev->stream_dev[DATAX_RX] ==
		      iio_context_find_device(ic, ops->rx_stream_dev));
		CHECK(dev->stream_dev[DATAX_TX] ==
		      iio_context_find_device(ic, ops->tx_stream_dev));
	}
	datax_context_destroy(ctx);
}

int main(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	const char *xml = getenv("DATAX_TEST_XML");

	CHECK(uri != NULL && xml != NULL);
	test_ad936x_map();
	test_adrv9009_map();
	test_fold_finds_stream_devs(uri, xml);
	return 0;
}
