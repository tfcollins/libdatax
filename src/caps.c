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
 *   RX_ARM / TX_ARM  TIMED_COMMANDS + the STREAM_GATE_TARGET capability bit +
 *                   the Tick's stream_gate_map names the direction's level and
 *                   mode bits.
 *   STREAM_GATE     RX_ARM or TX_ARM.
 *   TIMESTAMPS      TIMED_COMMANDS + the STREAM_TIMESTAMP capability bit + the
 *                   capture_tag_* attributes + an RX level bit in the map.
 * Every other flag stays false until its phase ships.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <iio/iio.h>

#include <datax/datax_internal.h>

#include "internal.h"
#include "tick_descriptor.h"
#include "tick_ops.h"

#define GUARD_MARGIN_MS 10

#define CAP_BIT(c) (1u << (c))

#define TICK_CAP_STREAM_GATE DATAX_TICK_CAPABILITY_STREAM_GATE_TARGET
#define TICK_CAP_TIMESTAMP DATAX_TICK_CAPABILITY_STREAM_TIMESTAMP

#define FHM_ENABLE_ATTR "frequency_hopping_mode_enable"

static bool fhm_attrs_present(const struct datax_device *dev)
{
	char buf[32];
	int n;

	/* Chip A's seam: FHM_HOP needs at least chip A (fhm.c). */
	if (dev->family != DATAX_FAMILY_ADRV9009 || !dev->fhm_ops[0])
		return false;
	n = dev->fhm_ops[0]->read(dev->fhm_h[0], FHM_ENABLE_ATTR, buf, sizeof(buf));
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

static bool dir_arm_ok(const struct datax_device *dev, bool timed, int level, int mode)
{
	return timed && (dev->caps.tick_bitmap & TICK_CAP_STREAM_GATE) && level >= 0 && mode >= 0;
}

/* Why a P3b flag is not set, or NULL when it is. which: RX_ARM, TX_ARM or
 * TIMESTAMPS. */
static const char *p3b_why_not(const struct datax_device *dev, enum datax_cap which)
{
	const struct datax_gate_map *m = &dev->caps.gate_map;

	if (!(dev->caps.flags & CAP_BIT(DATAX_CAP_TIMED_COMMANDS)))
		return "no usable Tick scheduler";
	if (which == DATAX_CAP_TIMESTAMPS) {
		if (!(dev->caps.tick_bitmap & TICK_CAP_TIMESTAMP))
			return "no STREAM_TIMESTAMP in the capability bitmap";
		if (!dev->caps.tag_attrs)
			return "no capture_tag attributes";
		if (!m->present)
			return dev->caps.gate_map_rejected ? "gate map rejected" : "no gate map";
		return m->rx_level < 0 ? "gate map has no rx-dma" : NULL;
	}
	if (!(dev->caps.tick_bitmap & TICK_CAP_STREAM_GATE))
		return "no STREAM_GATE_TARGET in the capability bitmap";
	if (!m->present)
		return dev->caps.gate_map_rejected ? "gate map rejected" : "no gate map";
	if (which == DATAX_CAP_RX_ARM)
		return m->rx_level < 0 || m->rx_mode < 0 ? "gate map lacks rx-dma or rx-mode" : NULL;
	return m->tx_level < 0 || m->tx_mode < 0 ? "gate map lacks tx-dma or tx-mode" : NULL;
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
	if (dir_arm_ok(dev, timed, dev->caps.gate_map.rx_level, dev->caps.gate_map.rx_mode))
		f |= CAP_BIT(DATAX_CAP_RX_ARM);
	if (dir_arm_ok(dev, timed, dev->caps.gate_map.tx_level, dev->caps.gate_map.tx_mode))
		f |= CAP_BIT(DATAX_CAP_TX_ARM);
	if (f & (CAP_BIT(DATAX_CAP_RX_ARM) | CAP_BIT(DATAX_CAP_TX_ARM)))
		f |= CAP_BIT(DATAX_CAP_STREAM_GATE);
	if (timed && (dev->caps.tick_bitmap & TICK_CAP_TIMESTAMP) && dev->caps.tag_attrs &&
	    dev->caps.gate_map.rx_level >= 0)
		f |= CAP_BIT(DATAX_CAP_TIMESTAMPS);
	dev->caps.flags = f;
}

/* Read the P3b inputs through the Tick seam. */
static void probe_streams(struct datax_device *dev)
{
	char buf[64];
	unsigned long long hdr = 0;
	int ret;

	memset(&dev->caps.gate_map, 0, sizeof(dev->caps.gate_map));
	dev->caps.gate_map.rx_level = dev->caps.gate_map.tx_level = -1;
	dev->caps.gate_map.rx_mode = dev->caps.gate_map.tx_mode = -1;
	dev->caps.gate_map_rejected = false;
	dev->caps.tag_bytes = 0;
	dev->caps.tag_attrs = false;
	if (!datax_tick_present(dev))
		return;
	ret = datax_tick_read_gate_map(dev, &dev->caps.gate_map);
	dev->caps.gate_map_rejected = ret == -EINVAL;
	if (datax_tick_read_str(dev, "capture_tag_enable", buf, sizeof(buf)) >= 0 &&
	    datax_tick_read_str(dev, "capture_tag_magic", buf, sizeof(buf)) >= 0 &&
	    datax_tick_read_u64(dev, "capture_tag_header_bytes", &hdr) == 0) {
		dev->caps.tag_attrs = true;
		dev->caps.tag_bytes = hdr;
	}
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
	dev->caps.tick_bitmap = 0;

	if (datax_tick_read_limits(dev, NULL, NULL, &bitmap) == 0) {
		probe |= CAP_BIT(DATAX_CAP_TIMED_COMMANDS);
		if (datax_tick_read_limits(dev, &hz, NULL, NULL) == 0)
			dev->caps.limits[DATAX_LIMIT_TICK_HZ] = hz;
		if (datax_tick_read_limits(dev, NULL, &depth, NULL) == 0)
			dev->caps.limits[DATAX_LIMIT_QUEUE_DEPTH] = depth;
		/* The same conditions sched_create enforces. */
		dev->caps.tick_bitmap = bitmap;
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
	probe_streams(dev);
	compute_flags(dev);
	return 0;
}

/* Test-only: what the probe detected, independent of the public flags. */
unsigned int datax_internal_probe_flags(const struct datax_device *dev)
{
	return dev ? dev->caps.probe_flags : 0;
}

/* Doctor: the gate map as parsed, the timed flags and why not, tag_bytes. */
int datax_internal_gate_info(const struct datax_device *dev, char *buf, size_t len)
{
	static const struct {
		const char *name;
		enum datax_cap cap;
	} flags[] = {
		{ "rx_arm", DATAX_CAP_RX_ARM }, { "tx_arm", DATAX_CAP_TX_ARM },
		{ "stream_gate", DATAX_CAP_STREAM_GATE }, { "timestamps", DATAX_CAP_TIMESTAMPS },
	};
	const struct datax_gate_map *m;
	size_t n = 0;

	if (!dev || !buf || !len)
		return -EINVAL;
	m = &dev->caps.gate_map;
	if (m->present)
		n += snprintf(buf + n, len - n,
			      "gate-map rx-dma=%d tx-dma=%d rx-mode=%d tx-mode=%d fhm-b=%s "
			      "tx-start-fullmask-only=%s",
			      m->rx_level, m->tx_level, m->rx_mode, m->tx_mode,
			      m->fhm_b ? "yes" : "no", m->tx_fullmask_only ? "yes" : "no");
	else
		n += snprintf(buf + n, len - n, "gate-map %s",
			      dev->caps.gate_map_rejected ? "rejected" : "none");
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]) && n < len; i++) {
		bool set = dev->caps.flags & CAP_BIT(flags[i].cap);
		const char *why = NULL;

		if (!set) {
			if (flags[i].cap == DATAX_CAP_STREAM_GATE)
				why = "neither rx_arm nor tx_arm";
			else
				why = p3b_why_not(dev, flags[i].cap);
		}
		if (set)
			n += snprintf(buf + n, len - n, ", %s=yes", flags[i].name);
		else
			n += snprintf(buf + n, len - n, ", %s=no (%s)", flags[i].name,
				      why ? why : "unknown");
	}
	if (n < len)
		snprintf(buf + n, len - n, ", tag_bytes=%llu", dev->caps.tag_bytes);
	return 0;
}
