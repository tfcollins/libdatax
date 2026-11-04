// SPDX-License-Identifier: MIT
#ifndef DATAX_PLATFORM_FAMILY_H
#define DATAX_PLATFORM_FAMILY_H

#include "../internal.h"

/* Per-family radio-control operations. device.c validates arguments (NULL
 * dev, channel range, timed commands) before dispatching here, so ops may
 * assume ch < rx_channels + tx_channels. All return 0 or -errno and record
 * a last-error on failure. */
struct datax_family_ops {
	int (*set_frequency)(struct datax_device *dev, unsigned int ch,
			     unsigned long long hz);
	int (*get_frequency)(const struct datax_device *dev, unsigned int ch,
			     unsigned long long *hz);
	int (*set_gain)(struct datax_device *dev, unsigned int ch, double db);
	int (*get_gain)(const struct datax_device *dev, unsigned int ch,
			double *db);
	int (*set_sample_rate)(struct datax_device *dev, unsigned long long sps);
	int (*set_bandwidth)(struct datax_device *dev, unsigned int ch,
			     unsigned long long hz);
	unsigned int rx_channels, tx_channels, hop_table_depth;
};

/* Ops for a family, or NULL if the family has none (unknown/unimplemented). */
const struct datax_family_ops *datax_family_ops(enum datax_family family);

/* Provided by the per-family translation units. */
extern const struct datax_family_ops datax_ad936x_ops;

#endif
