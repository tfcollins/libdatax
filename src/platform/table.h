// SPDX-License-Identifier: MIT
#ifndef DATAX_PLATFORM_TABLE_H
#define DATAX_PLATFORM_TABLE_H

#include "../internal.h"

/* Family of a raw IIO device name; DATAX_FAMILY_UNKNOWN for anything that is
 * not a radio (cf-*, axi-*, ad7291, xadc, ad9517-*, Tick ...). */
enum datax_family datax_family_of(const char *iio_device_name);

/* Build ctx->devices/ndevices from ctx->iio: one device per ad936x phy, one
 * per adrv9009 phy/phy-b pair. Returns 0 or -errno. */
int datax_fold_devices(struct datax_context *ctx);

#endif
