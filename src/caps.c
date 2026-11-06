// SPDX-License-Identifier: MIT
/*
 * Capability probe. Ruling (P1): the public caps.flags stays 0 because a flag
 * means "this SDK can do it here" and the timed paths arrive in P3. The probe
 * result is kept in caps.probe_flags and exposed to tests only.
 */
#include <errno.h>

#include "internal.h"
#include "tick_ops.h"

#define GUARD_MARGIN_MS 10

#define CAP_BIT(c) (1u << (c))

int datax_caps_build(struct datax_device *dev)
{
	unsigned long long hz = 0;
	unsigned int depth = 0, bitmap = 0;
	unsigned int probe = 0;

	if (!dev || !dev->ctx)
		return -EINVAL;

	dev->caps.flags = 0;
	dev->caps.limits[DATAX_LIMIT_GUARD_MARGIN_MS] = GUARD_MARGIN_MS;
	dev->caps.limits[DATAX_LIMIT_TICK_HZ] = 0;
	dev->caps.limits[DATAX_LIMIT_QUEUE_DEPTH] = 0;
	/* Measured when a scheduler is created (P3a); until then get_limit
	 * returns -ENOTSUP for them (caps_access.c). */
	dev->caps.limits[DATAX_LIMIT_REPORT_SPACING_US] = 0;
	dev->caps.limits[DATAX_LIMIT_HOP_STAGE_US] = 0;
	dev->caps.measured = 0;

	if (datax_tick_read_limits(dev, NULL, NULL, &bitmap) == 0) {
		probe |= CAP_BIT(DATAX_CAP_TIMED_COMMANDS);
		if (datax_tick_read_limits(dev, &hz, NULL, NULL) == 0)
			dev->caps.limits[DATAX_LIMIT_TICK_HZ] = hz;
		if (datax_tick_read_limits(dev, NULL, &depth, NULL) == 0)
			dev->caps.limits[DATAX_LIMIT_QUEUE_DEPTH] = depth;
	}

	if (probe & CAP_BIT(DATAX_CAP_TIMED_COMMANDS)) {
		if (dev->family == DATAX_FAMILY_ADRV9009)
			probe |= CAP_BIT(DATAX_CAP_FHM_HOP);
		else if (dev->family == DATAX_FAMILY_AD936X)
			probe |= CAP_BIT(DATAX_CAP_FASTLOCK_HOP);
	}
	/* The FHM hop table is a host-side set of 64 (spec decision 10; fhm.c),
	 * usable only with Tick to trigger the hops. */
	if (dev->family == DATAX_FAMILY_ADRV9009)
		dev->caps.limits[DATAX_LIMIT_HOP_TABLE_DEPTH] =
			(probe & CAP_BIT(DATAX_CAP_TIMED_COMMANDS)) ? 64 : 0;
	if (dev->family == DATAX_FAMILY_ADRV9009 && dev->nphy == 2)
		probe |= CAP_BIT(DATAX_CAP_MCS);

	dev->caps.probe_flags = probe;
	return 0;
}

/* Test-only: what the probe detected, independent of the public flags. */
unsigned int datax_internal_probe_flags(const struct datax_device *dev)
{
	return dev ? dev->caps.probe_flags : 0;
}
