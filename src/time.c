// SPDX-License-Identifier: MIT
/* Device time from the Tick scheduler (through the Tick ops seam). The
 * offset, PPS alignment and the time source live in devtime.c. */
#include <errno.h>

#include <datax/datax.h>

#include "error.h"
#include "scheduler.h"
#include "tick_ops.h"

int datax_device_get_time(const struct datax_device *dev,
			  unsigned long long *ticks, unsigned long long *tick_hz)
{
	struct datax_sched *s;
	struct datax_io_hold h;
	bool locked;
	unsigned long long t, hz;
	int ret;

	if (!dev || !ticks || !tick_hz)
		return dev ? datax_error_ret(dev->ctx, EINVAL, "time",
					     "get_time: NULL argument") : -EINVAL;
	if (!datax_tick_present(dev))
		return datax_error_ret(dev->ctx, ENOTSUP, "time",
				       "no Tick scheduler in this context");
	/*
	 * While a scheduler exists its io lock serialises every Tick access.
	 * Hold a reference so a concurrent destroy cannot free it while we wait
	 * for io. Skip the lock when this thread already holds it (a hook, on
	 * the poller or on flush's or destroy's caller).
	 */
	s = datax_device_sched_get(dev);
	locked = s && !datax_sched_holds_io(s);
	if (locked)
		datax_sched_io_lock(s, &h);
	ret = datax_tick_read_u64(dev, "current_time_ticks", &t);
	if (!ret)
		ret = datax_tick_read_limits(dev, &hz, NULL, NULL);
	if (locked)
		datax_sched_io_unlock(s, &h);
	datax_sched_put(s);
	if (ret)
		return datax_error_ret(dev->ctx, -ret, "time",
				       "get_time: Tick read failed");
	*ticks = datax_devtime_from_hw(dev, t);
	*tick_hz = hz;
	return 0;
}
