// SPDX-License-Identifier: MIT
/*
 * Capability probe and gates. A public flag means "this SDK can do it here"
 * (P1 ruling); probe_flags keeps what the hardware detection found, for
 * tests and doctor.
 *
 * P3a gates:
 *   TIMED_COMMANDS  Tick present, GPIO target capability, a usable queue
 *                   (the scheduler can be created).
 *   FHM_HOP         adrv9009 + TIMED_COMMANDS + the FHM attributes on chip A's
 *                   TRX LO. A single-phy board qualifies too: only chip A's FHM
 *                   trigger is wired to Tick (spec decision 7) and that is the
 *                   chip FHM drives.
 *   MCS             both adrv9009 phys carry jesd204_fsm_ctrl.
 *   PHASE_COHERENT  live: MCS verified, RF PLL phase sync on, and no
 *                   single-chip tune since (mcs.c on the phase sync input).
 * Every other flag stays false until its phase ships.
 */
#include <errno.h>

#include <iio/iio.h>

#include "internal.h"
#include "tick_descriptor.h"
#include "tick_ops.h"

#define GUARD_MARGIN_MS 10

#define CAP_BIT(c) (1u << (c))

#define FHM_ENABLE_ATTR "frequency_hopping_mode_enable"

static bool fhm_attrs_present(const struct datax_device *dev)
{
	char buf[32];
	int n;

	if (dev->family != DATAX_FAMILY_ADRV9009 || !dev->fhm_ops)
		return false;
	n = dev->fhm_ops->read(dev->fhm_h, FHM_ENABLE_ATTR, buf, sizeof(buf));
	return n != -ENOENT && n != -ENODEV;
}

static bool mcs_attrs_present(const struct datax_device *dev)
{
	if (dev->family != DATAX_FAMILY_ADRV9009 || dev->nphy != 2)
		return false;
	for (unsigned int i = 0; i < 2; i++)
		if (!dev->phy[i] || !iio_device_find_attr(dev->phy[i], "jesd204_fsm_ctrl"))
			return false;
	return true;
}

/* Public flags from the stored inputs. The caller holds ctx->lock or owns
 * the device exclusively (build). */
static void compute_flags(struct datax_device *dev)
{
	unsigned int f = 0;
	bool timed = (dev->caps.probe_flags & CAP_BIT(DATAX_CAP_TIMED_COMMANDS)) &&
		     dev->caps.tick_usable;

	if (timed)
		f |= CAP_BIT(DATAX_CAP_TIMED_COMMANDS);
	if (timed && (dev->caps.probe_flags & CAP_BIT(DATAX_CAP_FHM_HOP)) && dev->caps.fhm_attrs)
		f |= CAP_BIT(DATAX_CAP_FHM_HOP);
	if ((dev->caps.probe_flags & CAP_BIT(DATAX_CAP_MCS)) && dev->caps.mcs_attrs) {
		f |= CAP_BIT(DATAX_CAP_MCS);
		if (dev->mcs_verified && dev->phase_sync && !dev->single_chip_tuned)
			f |= CAP_BIT(DATAX_CAP_PHASE_COHERENT);
	}
	dev->caps.flags = f;
}

void datax_caps_refresh_flags(struct datax_device *dev)
{
	if (!dev || !dev->ctx)
		return;
	pthread_mutex_lock(&dev->ctx->lock);
	compute_flags(dev);
	pthread_mutex_unlock(&dev->ctx->lock);
}

void datax_caps_fhm_rebound(struct datax_device *dev)
{
	bool present = fhm_attrs_present(dev);

	pthread_mutex_lock(&dev->ctx->lock);
	dev->caps.fhm_attrs = present;
	compute_flags(dev);
	pthread_mutex_unlock(&dev->ctx->lock);
}

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
	dev->caps.tick_usable = false;

	if (datax_tick_read_limits(dev, NULL, NULL, &bitmap) == 0) {
		probe |= CAP_BIT(DATAX_CAP_TIMED_COMMANDS);
		if (datax_tick_read_limits(dev, &hz, NULL, NULL) == 0)
			dev->caps.limits[DATAX_LIMIT_TICK_HZ] = hz;
		if (datax_tick_read_limits(dev, NULL, &depth, NULL) == 0)
			dev->caps.limits[DATAX_LIMIT_QUEUE_DEPTH] = depth;
		/* The same conditions sched_create enforces. */
		dev->caps.tick_usable = (bitmap & DATAX_TICK_CAPABILITY_GPIO_TARGET) &&
					depth >= 2 && hz != 0;
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
	dev->caps.fhm_attrs = fhm_attrs_present(dev);
	dev->caps.mcs_attrs = mcs_attrs_present(dev);
	compute_flags(dev);
	return 0;
}

/* Test-only: what the probe detected, independent of the public flags. */
unsigned int datax_internal_probe_flags(const struct datax_device *dev)
{
	return dev ? dev->caps.probe_flags : 0;
}
