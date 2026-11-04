// SPDX-License-Identifier: MIT
#ifndef DATAX_PLATFORM_TICK_H
#define DATAX_PLATFORM_TICK_H

struct iio_context;
struct iio_device;

#define DATAX_TICK_DEVICE_NAME "axi-timed-command-scheduler"

/* The Tick scheduler device in ctx, or NULL when the context has none. */
struct iio_device *datax_tick_find(struct iio_context *ctx);

/*
 * Read the Tick limits. Any output may be NULL. Returns 0, or -errno
 * (-ENODEV when an attribute is missing, -EINVAL when it does not parse).
 */
int datax_tick_read_limits(struct iio_device *tick, unsigned long long *tick_hz,
			   unsigned *queue_depth, unsigned *capability_bitmap);

/* Read attr current_time_ticks. */
int datax_tick_read_time(struct iio_device *tick, unsigned long long *ticks);

#endif
