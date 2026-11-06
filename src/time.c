// SPDX-License-Identifier: MIT
/* Device time from the Tick scheduler (through the Tick ops seam); PPS
 * time-set arrives in P3a. */
#include <errno.h>

#include <datax/datax.h>

#include "error.h"
#include "scheduler.h"
#include "tick_ops.h"

/* API <-> hardware ticks. Identity until the soft offset (Task 3). */
unsigned long long datax_devtime_to_hw(const struct datax_device *dev,
				       unsigned long long api_ticks)
{
	(void)dev;
	return api_ticks;
}

unsigned long long datax_devtime_from_hw(const struct datax_device *dev,
					 unsigned long long hw_ticks)
{
	(void)dev;
	return hw_ticks;
}

int datax_device_get_time(const struct datax_device *dev,
			  unsigned long long *ticks, unsigned long long *tick_hz)
{
	struct datax_sched *s;
	unsigned long long t, hz;
	int ret;

	if (!dev || !ticks || !tick_hz)
		return dev ? datax_error_ret(dev->ctx, EINVAL, "time",
					     "get_time: NULL argument") : -EINVAL;
	if (!datax_tick_present(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "time",
				       "no Tick scheduler in this context");
	/* While a scheduler exists its io lock serialises every Tick access
	 * (not on the poller thread: from a hook io is already held, and from
	 * an event callback the poller is doing no I/O). */
	s = datax_device_sched(dev);
	if (s && datax_poller_is_self(s))
		s = NULL;
	if (s)
		pthread_mutex_lock(&s->io);
	ret = datax_tick_read_u64(dev, "current_time_ticks", &t);
	if (!ret)
		ret = datax_tick_read_limits(dev, &hz, NULL, NULL);
	if (s)
		pthread_mutex_unlock(&s->io);
	if (ret)
		return datax_error_ret(dev->ctx, -ret, "time",
				       "get_time: Tick read failed");
	*ticks = datax_devtime_from_hw(dev, t);
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
