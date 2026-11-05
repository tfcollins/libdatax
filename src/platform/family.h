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
	int (*get_sample_rate)(const struct datax_device *dev,
			       unsigned long long *sps);
	int (*set_bandwidth)(struct datax_device *dev, unsigned int ch,
			     unsigned long long hz);
	int (*get_bandwidth)(const struct datax_device *dev, unsigned int ch,
			     unsigned long long *hz);
	/* Channel counts are per phy; datax_fold_devices multiplies by the
	 * number of phys folded into the device (1 for ad936x, 1-2 for adrv9009). */
	unsigned int rx_channels_per_phy, tx_channels_per_phy;
	unsigned int hop_table_depth;

	/* Streaming (P2). Names of the DMA-backed IIO devices that carry the
	 * RX and TX sample streams; NULL-safe lookup happens at fold time.
	 * Sources: ad936x, ADI Kuiper stock cf-ad9361-lpc (RX) and
	 * cf-ad9361-dds-core-lpc (TX), present in tests/emu/ad936x.xml;
	 * adrv9009, ADI Kuiper 2023_R2 zynqmp-adrv9009-zu11eg device tree
	 * (axi-adrv9009-rx-hpc / axi-adrv9009-tx-hpc). The adrv9009 names are
	 * not yet confirmed on hardware; the hardware task (plan Task 9) does. */
	const char *rx_stream_dev, *tx_stream_dev;
	/* Scan-channel ids of radio channel radio_ch in direction dir. The ids
	 * point at static strings. Returns 0 or -EINVAL if out of range. */
	int (*stream_channel_ids)(unsigned int radio_ch, enum datax_direction dir,
				  const char **id_i, const char **id_q);
};

/* Ops for a family, or NULL if the family has none (unknown/unimplemented). */
const struct datax_family_ops *datax_family_ops(enum datax_family family);

/* Provided by the per-family translation units. */
extern const struct datax_family_ops datax_ad936x_ops;
extern const struct datax_family_ops datax_adrv9009_ops;

#endif
