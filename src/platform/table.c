// SPDX-License-Identifier: MIT
/*
 * Family detection and device folding. Only radio families become
 * datax_devices; every other IIO device stays reachable through
 * datax_context_get_iio_context().
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <iio/iio.h>

#include "table.h"
#include "../error.h"

#define ADRV9009_PHY   "adrv9009-phy"
#define ADRV9009_PHY_B "adrv9009-phy-b"

enum datax_family datax_family_of(const char *name)
{
	if (!name)
		return DATAX_FAMILY_UNKNOWN;
	if (!strcmp(name, "ad9361-phy"))
		return DATAX_FAMILY_AD936X;
	if (!strcmp(name, ADRV9009_PHY) || !strcmp(name, ADRV9009_PHY_B))
		return DATAX_FAMILY_ADRV9009;
	return DATAX_FAMILY_UNKNOWN;
}

static struct datax_device *new_device(struct datax_context *ctx,
				       enum datax_family fam, const char *name)
{
	struct datax_device *dev = calloc(1, sizeof(*dev));

	if (!dev)
		return NULL;
	dev->ctx = ctx;
	dev->family = fam;
	strncpy(dev->name, name, sizeof(dev->name) - 1);
	return dev;
}

static void free_devices(struct datax_context *ctx)
{
	for (unsigned int i = 0; i < ctx->ndevices; i++)
		free(ctx->devices[i]);
	free(ctx->devices);
	ctx->devices = NULL;
	ctx->ndevices = 0;
}

int datax_fold_devices(struct datax_context *ctx)
{
	unsigned int n = iio_context_get_devices_count(ctx->iio);
	bool have_adrv = false;

	if (n)
		ctx->devices = calloc(n, sizeof(*ctx->devices));
	if (n && !ctx->devices)
		return datax_error_ret(ctx, ENOMEM, "context", "out of memory");

	for (unsigned int i = 0; i < n; i++) {
		struct iio_device *iod = iio_context_get_device(ctx->iio, i);
		const char *name = iio_device_get_name(iod);
		enum datax_family fam = datax_family_of(name);
		struct datax_device *dev;

		if (fam == DATAX_FAMILY_UNKNOWN)
			continue;
		if (fam == DATAX_FAMILY_ADRV9009 && have_adrv)
			continue; /* already folded via its sibling */

		dev = new_device(ctx, fam, fam == DATAX_FAMILY_ADRV9009 ?
				 "adrv9009" : name);
		if (!dev) {
			free_devices(ctx);
			return datax_error_ret(ctx, ENOMEM, "context", "out of memory");
		}

		if (fam == DATAX_FAMILY_AD936X) {
			dev->phy[0] = iod;
			dev->nphy = 1;
			dev->caps.limits[DATAX_LIMIT_MAX_CHANNELS] = 2;
			dev->caps.limits[DATAX_LIMIT_HOP_TABLE_DEPTH] = 8;
			dev->rx_channels = 2;
			dev->tx_channels = 2;
		} else {
			struct iio_device *a = iio_context_find_device(ctx->iio, ADRV9009_PHY);
			struct iio_device *b = iio_context_find_device(ctx->iio, ADRV9009_PHY_B);

			if (a)
				dev->phy[dev->nphy++] = a;
			if (b)
				dev->phy[dev->nphy++] = b;
			dev->caps.limits[DATAX_LIMIT_MAX_CHANNELS] = 2ull * dev->nphy;
			dev->caps.limits[DATAX_LIMIT_HOP_TABLE_DEPTH] = 0;
			dev->rx_channels = 2 * dev->nphy;
			dev->tx_channels = 2 * dev->nphy;
			have_adrv = true;
		}
		ctx->devices[ctx->ndevices++] = dev;
	}
	return 0;
}
