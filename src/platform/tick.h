// SPDX-License-Identifier: MIT
#ifndef DATAX_PLATFORM_TICK_H
#define DATAX_PLATFORM_TICK_H

struct iio_context;
struct iio_device;

#define DATAX_TICK_DEVICE_NAME "axi-timed-command-scheduler"

/* The Tick scheduler device in ctx, or NULL when the context has none. */
struct iio_device *datax_tick_find(struct iio_context *ctx);

#endif
