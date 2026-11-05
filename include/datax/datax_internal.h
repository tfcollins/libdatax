// SPDX-License-Identifier: MIT
/*
 * Test-only diagnostics. Not part of the documented API: these symbols carry
 * the datax_internal_ prefix and may change without notice.
 */
#ifndef DATAX_DATAX_INTERNAL_H
#define DATAX_DATAX_INTERNAL_H

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif

#endif /* DATAX_DATAX_INTERNAL_H */
