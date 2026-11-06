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

#ifdef __cplusplus
}
#endif

#endif /* DATAX_DATAX_INTERNAL_H */
