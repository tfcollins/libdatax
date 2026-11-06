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

#include "family.h"
#include "table.h"
#include "tick.h"
#include "../error.h"
#include "../fhm.h"
#include "../tick_ops.h"

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
	struct iio_device *tick = datax_tick_find(ctx->iio);
	bool have_adrv = false;

	if (n)
		ctx->devices = calloc(n, sizeof(*ctx->devices));
	if (n && !ctx->devices)
		return datax_error_ret(ctx, ENOMEM, "context", "out of memory");

	for (unsigned int i = 0; i < n; i++) {
		struct iio_device *iod = iio_context_get_device(ctx->iio, i);
		const char *name = iio_device_get_name(iod);
		enum datax_family fam = datax_family_of(name);
		const struct datax_family_ops *ops;
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
			dev->phy[dev->nphy++] = iod;
		} else {
			struct iio_device *a = iio_context_find_device(ctx->iio, ADRV9009_PHY);
			struct iio_device *b = iio_context_find_device(ctx->iio, ADRV9009_PHY_B);

			if (a)
				dev->phy[dev->nphy++] = a;
			if (b)
				dev->phy[dev->nphy++] = b;
			have_adrv = true;
			/* FHM seam: chip A's TRX LO (only its FHM GPIO is wired to
			 * Tick on the ZU11EG image, spec decision 7). */
			if (a) {
				struct iio_channel *lo =
					iio_device_find_channel(a, "altvoltage0", true);

				if (lo) {
					dev->fhm_ops = &datax_fhm_iio_ops;
					dev->fhm_h = lo;
				}
			}
		}
		/* One source of truth: the family ops carry the per-phy counts. */
		ops = datax_family_ops(fam);
		dev->rx_channels = ops->rx_channels_per_phy * dev->nphy;
		dev->tx_channels = ops->tx_channels_per_phy * dev->nphy;
		dev->caps.limits[DATAX_LIMIT_MAX_CHANNELS] = dev->rx_channels;
		dev->caps.limits[DATAX_LIMIT_HOP_TABLE_DEPTH] = ops->hop_table_depth;
		/* Streaming DMA devices; absent on platforms without them. */
		if (ops->rx_stream_dev)
			dev->stream_dev[DATAX_RX] =
				iio_context_find_device(ctx->iio, ops->rx_stream_dev);
		if (ops->tx_stream_dev)
			dev->stream_dev[DATAX_TX] =
				iio_context_find_device(ctx->iio, ops->tx_stream_dev);
		/* The Tick seam: real libiio ops on the context's Tick device. */
		if (tick) {
			dev->tick_ops = &datax_tick_iio_ops;
			dev->tick_h = tick;
		}
		datax_caps_build(dev);
		ctx->devices[ctx->ndevices++] = dev;
	}
	return 0;
}
