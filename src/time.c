// SPDX-License-Identifier: MIT
/* Device time from the Tick scheduler (through the Tick ops seam); PPS
 * time-set arrives in P3a. */
#include <errno.h>

#include <datax/datax.h>

#include "error.h"
#include "tick_ops.h"

int datax_device_get_time(const struct datax_device *dev,
			  unsigned long long *ticks, unsigned long long *tick_hz)
{
	unsigned long long t, hz;
	int ret;

	if (!dev || !ticks || !tick_hz)
		return dev ? datax_error_ret(dev->ctx, EINVAL, "time",
					     "get_time: NULL argument") : -EINVAL;
	if (!datax_tick_present(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "time",
				       "no Tick scheduler in this context");
	ret = datax_tick_read_u64(dev, "current_time_ticks", &t);
	if (!ret)
		ret = datax_tick_read_limits(dev, &hz, NULL, NULL);
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

/* P3a Task 1: Tick present means the free-running internal clock; PPS and
 * soft alignment (and their sources) arrive with set_time_next_pps. */
int datax_device_get_time_source(const struct datax_device *dev,
				 enum datax_time_source *src)
{
	if (!dev || !src)
		return dev ? datax_error_ret(dev->ctx, EINVAL, "time",
					     "get_time_source: NULL argument") : -EINVAL;
	if (!datax_tick_present(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "time",
				       "no Tick scheduler in this context");
	*src = DATAX_TIME_FREE_RUN;
	return 0;
}
