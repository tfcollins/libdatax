// SPDX-License-Identifier: MIT
#ifndef DATAX_INTERNAL_H
#define DATAX_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>

#include <datax/datax.h>

struct iio_device;
struct datax_stream;
struct datax_tick_ops;

enum datax_family {
	DATAX_FAMILY_UNKNOWN,
	DATAX_FAMILY_AD936X,
	DATAX_FAMILY_ADRV9009
};

struct datax_caps {
	unsigned int flags;        /* bit per enum datax_cap this SDK can do here */
	unsigned int probe_flags;  /* bit per enum datax_cap the probe detected */
	unsigned long long limits[DATAX_LIMIT_COUNT];
	/* bit per enum datax_limit that is measured at run time and has been
	 * (REPORT_SPACING_US while a scheduler exists); unmeasured ones read
	 * -ENOTSUP. */
	unsigned int measured;
	/* Inputs of the public flags, probed at build/bind time (caps.c): Tick
	 * has the GPIO target and a usable queue; the phy has the FHM
	 * attributes; both phys have jesd204_fsm_ctrl. */
	bool tick_usable, fhm_attrs, mcs_attrs;
};

struct datax_context {
	struct iio_context *iio;
	pthread_mutex_t lock;
	struct datax_device **devices;
	unsigned int ndevices;
	char last_error[256];
	char last_subsystem[32];
	datax_event_cb event_cb;
	void *event_user;
};

struct datax_device {
	struct datax_context *ctx;
	enum datax_family family;
	char name[64];
	struct iio_device *phy[2];
	unsigned int nphy;
	unsigned int rx_channels, tx_channels;
	struct datax_caps caps;
	/* Streaming (P2), indexed by enum datax_direction. stream_dev is the
	 * DMA device (NULL when the platform lacks it), filled at fold time;
	 * stream is the active stream or NULL. */
	struct iio_device *stream_dev[2];
	struct datax_stream *stream[2];
	/* Status-register detection seen by streams: 0 unknown, 1 yes, 2 no. */
	unsigned char status_detect[2];
	/* Tick seam (tick_ops.h): every Tick access goes through tick_ops with
	 * tick_h. Bound at fold time to the libiio ops on the context's Tick
	 * device; NULL ops when there is none. Tests rebind a fake. */
	const struct datax_tick_ops *tick_ops;
	void *tick_h;
	/* FHM phy seam (fhm.c): the adrv9009-phy (chip A) TRX LO channel
	 * attributes, through ops of the Tick seam's shape. Bound at fold time
	 * to the libiio ops on phy[0]'s output channel altvoltage0; NULL when
	 * there is none. Tests rebind a fake (datax_internal_fhm_bind_fake).
	 * Like tick_ops, never rebound while a scheduler exists. */
	const struct datax_tick_ops *fhm_ops;
	void *fhm_h;
	/* Hop staging and the tune pipeline (fhm.c), created by the first
	 * stage_hop_table, freed with the device. */
	struct datax_fhm *fhm;
	/* A single-chip (chip A) tune has executed: PHASE_COHERENT no longer
	 * holds (spec decision 7; Task 5 reads it). Under ctx->lock. */
	bool single_chip_tuned;
	/* The device's one scheduler (sched.c), or NULL; set and cleared under
	 * ctx->lock. While it exists the Tick seam cannot be rebound. */
	struct datax_sched *sched;
	/* Next scheduler sequence id. Per device, so ids never repeat across
	 * scheduler lifetimes (a destroyed scheduler can leave descriptors in
	 * Tick); seeded at the first scheduler, never 0 (an empty completion
	 * slot reads seq 0). */
	unsigned int next_seq;
	/* Device time (devtime.c). API time = hardware ticks + time_off (mod
	 * 2^64); nonzero only after a soft alignment. time_source is an enum
	 * datax_time_source. time_armed: a hard alignment timed out with the
	 * Tick still armed, so a late PPS edge will load the epoch. */
	_Atomic unsigned long long time_off;
	_Atomic int time_source;
	_Atomic bool time_armed;
	/* MCS (mcs.c), under ctx->lock. mcs_verified: the last run_mcs found
	 * both phys complete. phase_sync: RF PLL phase sync is enabled on the
	 * phys; always false on adrv9009 in P3a (mcs.c explains why). */
	bool mcs_verified;
	bool phase_sync;
};

/* caps.c: fill dev->caps after folding (public flags stay 0 in P1). */
int datax_caps_build(struct datax_device *dev);
/* caps.c: recompute the public flags from the stored inputs, the MCS state
 * and single_chip_tuned. Takes ctx->lock. */
void datax_caps_refresh_flags(struct datax_device *dev);
/* caps.c: the FHM phy seam was rebound; re-probe its attributes and refresh. */
void datax_caps_fhm_rebound(struct datax_device *dev);
/* caps.c, diagnostic export (used by the Python package's caps.detected
 * and doctor): what the probe detected. */
unsigned int datax_internal_probe_flags(const struct datax_device *dev);

#endif
