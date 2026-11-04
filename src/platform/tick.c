// SPDX-License-Identifier: MIT
/* Tick (axi-timed-command-scheduler) attribute access. */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>

#include <iio/iio.h>

#include "tick.h"

struct iio_device *datax_tick_find(struct iio_context *ctx)
{
	return ctx ? iio_context_find_device(ctx, DATAX_TICK_DEVICE_NAME) : NULL;
}

static int read_u64(struct iio_device *tick, const char *name, int base,
		    unsigned long long *out)
{
	const struct iio_attr *a = iio_device_find_attr(tick, name);
	char buf[64];
	char *end;
	ssize_t n;

	if (!a)
		return -ENODEV;
	n = iio_attr_read_raw(a, buf, sizeof(buf) - 1);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	errno = 0;
	*out = strtoull(buf, &end, base);
	if (errno || end == buf)
		return -EINVAL;
	return 0;
}

int datax_tick_read_limits(struct iio_device *tick, unsigned long long *tick_hz,
			   unsigned *queue_depth, unsigned *capability_bitmap)
{
	unsigned long long v;
	int ret;

	if (!tick)
		return -EINVAL;
	if (tick_hz) {
		ret = read_u64(tick, "time_tick_hz", 10, &v);
		if (ret)
			return ret;
		*tick_hz = v;
	}
	if (queue_depth) {
		ret = read_u64(tick, "queue_depth", 10, &v);
		if (ret)
			return ret;
		if (v > UINT_MAX)
			return -ERANGE;
		*queue_depth = (unsigned)v;
	}
	if (capability_bitmap) {
		/* Hex text such as 0x0000001f; base 16 accepts the prefix. */
		ret = read_u64(tick, "capability_bitmap", 16, &v);
		if (ret)
			return ret;
		if (v > UINT_MAX)
			return -ERANGE;
		*capability_bitmap = (unsigned)v;
	}
	return 0;
}

int datax_tick_read_time(struct iio_device *tick, unsigned long long *ticks)
{
	if (!tick || !ticks)
		return -EINVAL;
	return read_u64(tick, "current_time_ticks", 10, ticks);
}
