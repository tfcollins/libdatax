// SPDX-License-Identifier: MIT
/* Device time from the Tick scheduler; PPS time-set arrives in P3. */
#include <errno.h>

#include <datax/datax.h>

#include "error.h"
#include "platform/tick.h"

int datax_device_get_time(const struct datax_device *dev,
			  unsigned long long *ticks, unsigned long long *tick_hz)
{
	struct iio_device *tick;
	unsigned long long t, hz;
	int ret;

	if (!dev || !ticks || !tick_hz)
		return dev ? datax_error_ret(dev->ctx, EINVAL, "time",
					     "get_time: NULL argument") : -EINVAL;
	tick = datax_tick_find(dev->ctx->iio);
	if (!tick)
		return datax_error_ret(dev->ctx, ENOTSUP, "time",
				       "no Tick scheduler in this context");
	ret = datax_tick_read_time(tick, &t);
	if (!ret)
		ret = datax_tick_read_limits(tick, &hz, NULL, NULL);
	if (ret)
		return datax_error_ret(dev->ctx, -ret, "time",
				       "get_time: Tick read failed");
	*ticks = t;
	*tick_hz = hz;
	return 0;
}

int datax_device_set_time_next_pps(struct datax_device *dev,
				   unsigned long long epoch_ticks)
{
	(void)epoch_ticks;
	if (dev)
		return datax_error_ret(dev->ctx, ENOTSUP, "stub", "%s",
				       "datax_device_set_time_next_pps: arrives in P3");
	return -ENOTSUP;
}
