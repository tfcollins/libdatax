// SPDX-License-Identifier: MIT
/*
 * Stubs: hop staging and MCS are not implemented yet and return -ENOTSUP.
 * Streams live in stream.c, the scheduler in sched.c and poller.c.
 *
 * Device-taking stubs record "<verb>: arrives in P<n>" as the context's
 * last error when dev is non-NULL; with a NULL dev they return the bare code.
 */
#include <errno.h>

#include <datax/datax.h>

#include "error.h"

static int dev_stub(const struct datax_device *dev, const char *msg)
{
	if (dev)
		return datax_error_ret(dev->ctx, ENOTSUP, "stub", "%s", msg);
	return -ENOTSUP;
}

int datax_device_stage_hop_table(struct datax_device *dev,
				 const unsigned long long *freqs_hz, size_t count)
{
	(void)freqs_hz; (void)count;
	return dev_stub(dev, "datax_device_stage_hop_table: arrives in P3");
}

int datax_device_run_mcs(struct datax_device *dev)
{
	return dev_stub(dev, "datax_device_run_mcs: arrives in P3");
}
