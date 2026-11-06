// SPDX-License-Identifier: MIT
/* Tick (axi-timed-command-scheduler) discovery. Attribute access goes through
 * the ops seam (tick_ops.h), bound to this device at fold time. */
#include <stddef.h>

#include <iio/iio.h>

#include "tick.h"

struct iio_device *datax_tick_find(struct iio_context *ctx)
{
	return ctx ? iio_context_find_device(ctx, DATAX_TICK_DEVICE_NAME) : NULL;
}
