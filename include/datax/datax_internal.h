// SPDX-License-Identifier: MIT
/*
 * Test-only diagnostics. Not part of the documented API: these symbols carry
 * the datax_internal_ prefix and may change without notice.
 */
#ifndef DATAX_DATAX_INTERNAL_H
#define DATAX_DATAX_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include <datax/datax.h>

#ifdef __cplusplus
extern "C" {
#endif

struct datax_internal_stream_stats {
	bool enabled;                    /* the hardware buffer is enabled */
	unsigned long long blocks_done;  /* RX blocks dequeued / TX blocks enqueued */
	unsigned long long overflows, underflows;
	unsigned long long status_polls; /* status-register polls */
	bool overflow_detect;            /* the status register is readable */
};

/* Snapshot of a stream's counters. 0, or -EINVAL for a NULL argument. */
int datax_internal_stream_stats(const struct datax_stream *s,
				struct datax_internal_stream_stats *out);

/* The streaming IIO device name of a direction (written to name), and
 * whether DMA overflow/underflow detection works on it: 1 yes, 0 no (no
 * status attribute, or a poll failed), -1 unknown (not probed: probing
 * happens when a stream starts). 0, -ENOENT without such a device. */
int datax_internal_device_stream_info(const struct datax_device *dev,
				      enum datax_direction dir, char *name,
				      size_t len, int *detect);

/* Tick ops seam (layout in src/tick_ops.h: two function pointers, read then
 * write). Replace the Tick access of a live device with ops/h, for tests
 * driving a fake Tick; NULL ops detaches Tick. Capabilities and limits are
 * re-probed through the new ops. 0, or -EINVAL. */
struct datax_tick_ops;
int datax_internal_tick_bind_fake(struct datax_device *dev,
				  const struct datax_tick_ops *ops, void *h);

/* FHM phy seam (same layout as the Tick ops): replace the adrv9009-phy TRX
 * LO channel attributes (frequency_hopping_mode_enable,
 * frequency_hopping_mode) of chip A with ops/h, for tests driving a fake
 * Talise FHM; NULL ops detaches. 0, -EINVAL (ops missing a function), or
 * -EBUSY while the device has a scheduler. */
int datax_internal_fhm_bind_fake(struct datax_device *dev,
				 const struct datax_tick_ops *ops, void *h);

/* Pause (true) or resume (false) the scheduler's poller: while paused it
 * neither feeds the Tick queue nor drains completions (commit still accepts
 * batches). Pausing returns once the current poller iteration has finished.
 * 0, or -EINVAL for a NULL scheduler. */
int datax_internal_sched_pause(struct datax_sched *s, bool pause);

/* MCS diagnostics (doctor). Writes the live JESD204 FSM state of the phys
 * to state and the PHASE_COHERENT reason to reason. 1 when PHASE_COHERENT
 * holds, 0 when not, -ENOTSUP on a device without MCS. */
int datax_internal_mcs_info(const struct datax_device *dev, char *state, size_t slen,
			    char *reason, size_t rlen);

/* Test-only: set the RF PLL phase sync input of the PHASE_COHERENT rule
 * (always false in production on adrv9009; see src/mcs.c). */
void datax_internal_mcs_force_phase_sync(struct datax_device *dev, bool on);

/* Whether the Tick reports a fabric PPS: 1, 0, or -errno (-ENODEV without a
 * Tick). */
int datax_internal_pps_present(const struct datax_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* DATAX_DATAX_INTERNAL_H */
