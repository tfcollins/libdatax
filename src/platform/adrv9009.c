// SPDX-License-Identifier: MIT
/*
 * adrv9009 ops. The 4R4T device folds up to two phys: phy[0] owns RX/TX
 * channels 0,1 and phy[1] owns 2,3. Each phy has one TRX LO
 * (output altvoltage0) shared by its RX and TX channels.
 */
#include <errno.h>
#include <stdio.h>

#include <iio/iio.h>

#include "../error.h"
#include "family.h"

#define CH_PER_PHY 2

/* Owning phy and in-phy channel index for a device channel (RX 0..rx-1,
 * then TX). Returns NULL (with last-error) if the phy is absent. */
static struct iio_device *owner(const struct datax_device *dev, unsigned int ch,
				bool *tx, unsigned int *idx)
{
	unsigned int n = ch;
	unsigned int p;

	*tx = ch >= dev->rx_channels;
	if (*tx)
		n = ch - dev->rx_channels;
	p = n / CH_PER_PHY;
	*idx = n % CH_PER_PHY;
	if (p >= dev->nphy || !dev->phy[p]) {
		datax_error_ret(dev->ctx, ENODEV, "adrv9009",
				"no phy for channel %u", ch);
		return NULL;
	}
	return dev->phy[p];
}

static int lookup(const struct datax_device *dev, struct iio_device *phy,
		  const char *chn_id, bool output, const char *attr,
		  const struct iio_attr **out)
{
	struct iio_channel *chn = iio_device_find_channel(phy, chn_id, output);

	*out = chn ? iio_channel_find_attr(chn, attr) : NULL;
	if (!*out)
		return datax_error_ret(dev->ctx, ENODEV, "adrv9009",
				       "%s %s/%s attribute not found",
				       output ? "output" : "input", chn_id, attr);
	return 0;
}

static int lo_attr(const struct datax_device *dev, unsigned int ch,
		   const struct iio_attr **out)
{
	bool tx;
	unsigned int idx;
	struct iio_device *phy = owner(dev, ch, &tx, &idx);

	if (!phy)
		return -ENODEV;
	return lookup(dev, phy, "altvoltage0", true, "frequency", out);
}

/* RX gain is input voltage<idx>, TX gain is output voltage<idx>. */
static int gain_attr(const struct datax_device *dev, unsigned int ch,
		     const struct iio_attr **out)
{
	char id[16];
	bool tx;
	unsigned int idx;
	struct iio_device *phy = owner(dev, ch, &tx, &idx);

	if (!phy)
		return -ENODEV;
	snprintf(id, sizeof(id), "voltage%u", idx);
	return lookup(dev, phy, id, tx, "hardwaregain", out);
}

static int adrv9009_set_frequency(struct datax_device *dev, unsigned int ch,
				  unsigned long long hz)
{
	const struct iio_attr *a;
	int ret = lo_attr(dev, ch, &a);

	if (ret)
		return ret;
	if (hz > 0x7fffffffffffffffULL)
		return datax_error_ret(dev->ctx, EINVAL, "adrv9009",
				       "frequency out of range");
	ret = iio_attr_write_longlong(a, (long long)hz);
	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "adrv9009",
				       "write of frequency failed");
	return 0;
}

static int adrv9009_get_frequency(const struct datax_device *dev,
				  unsigned int ch, unsigned long long *hz)
{
	const struct iio_attr *a;
	long long v;
	int ret = lo_attr(dev, ch, &a);

	if (ret)
		return ret;
	ret = iio_attr_read_longlong(a, &v);
	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "adrv9009",
				       "read of frequency failed");
	*hz = (unsigned long long)v;
	return 0;
}

static int adrv9009_set_gain(struct datax_device *dev, unsigned int ch, double db)
{
	const struct iio_attr *a;
	int ret = gain_attr(dev, ch, &a);

	if (ret)
		return ret;
	ret = iio_attr_write_double(a, db);
	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "adrv9009",
				       "write of hardwaregain failed");
	return 0;
}

static int adrv9009_get_gain(const struct datax_device *dev, unsigned int ch,
			     double *db)
{
	const struct iio_attr *a;
	int ret = gain_attr(dev, ch, &a);

	if (ret)
		return ret;
	ret = iio_attr_read_double(a, db);
	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "adrv9009",
				       "read of hardwaregain failed");
	return 0;
}

static int adrv9009_set_sample_rate(struct datax_device *dev,
				    unsigned long long sps)
{
	(void)sps;
	return datax_error_ret(dev->ctx, ENOTSUP, "adrv9009",
			       "sample rate is fixed by the JESD profile");
}

static int adrv9009_get_sample_rate(const struct datax_device *dev,
				    unsigned long long *sps)
{
	const struct iio_attr *a;
	long long v;
	int ret;

	if (dev->nphy < 1 || !dev->phy[0])
		return datax_error_ret(dev->ctx, ENODEV, "adrv9009", "no phy");
	ret = lookup(dev, dev->phy[0], "voltage0", false, "sampling_frequency", &a);
	if (ret)
		return ret;
	ret = iio_attr_read_longlong(a, &v);
	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "adrv9009",
				       "read of sampling_frequency failed");
	*sps = (unsigned long long)v;
	return 0;
}

static int adrv9009_get_bandwidth(const struct datax_device *dev,
				  unsigned int ch, unsigned long long *hz)
{
	(void)ch;
	(void)hz;
	return datax_error_ret(dev->ctx, ENOTSUP, "adrv9009",
			       "bandwidth: not exposed by the adrv9009 profile");
}

static int adrv9009_set_bandwidth(struct datax_device *dev, unsigned int ch,
				  unsigned long long hz)
{
	(void)ch;
	(void)hz;
	return datax_error_ret(dev->ctx, ENOTSUP, "adrv9009",
			       "bandwidth is fixed by the profile");
}

/* adrv9009 radio channel n streaming scan pair. Observed on the board
 * (2026-11-05, stock Kuiper zynqmp-adrv9009-zu11eg, libiio v1 iio_info):
 * RX axi-adrv9009-rx-hpc names them voltage{n}_i / voltage{n}_q, but TX
 * axi-adrv9009-tx-hpc uses AD9361-style interleaved voltage0..voltage7, so
 * radio channel n is I = voltage{2n}, Q = voltage{2n+1}. */
static int adrv9009_stream_channel_ids(unsigned int radio_ch,
				       enum datax_direction dir,
				       const char **id_i, const char **id_q)
{
	static const char *const rx_i[] = { "voltage0_i", "voltage1_i",
					    "voltage2_i", "voltage3_i" };
	static const char *const rx_q[] = { "voltage0_q", "voltage1_q",
					    "voltage2_q", "voltage3_q" };
	static const char *const tx_ids[] = { "voltage0", "voltage1",
					      "voltage2", "voltage3",
					      "voltage4", "voltage5",
					      "voltage6", "voltage7" };

	if (radio_ch >= 4)
		return -EINVAL;
	if (dir == DATAX_TX) {
		*id_i = tx_ids[2 * radio_ch];
		*id_q = tx_ids[2 * radio_ch + 1];
	} else {
		*id_i = rx_i[radio_ch];
		*id_q = rx_q[radio_ch];
	}
	return 0;
}

const struct datax_family_ops datax_adrv9009_ops = {
	.set_frequency = adrv9009_set_frequency,
	.get_frequency = adrv9009_get_frequency,
	.set_gain = adrv9009_set_gain,
	.get_gain = adrv9009_get_gain,
	.set_sample_rate = adrv9009_set_sample_rate,
	.get_sample_rate = adrv9009_get_sample_rate,
	.set_bandwidth = adrv9009_set_bandwidth,
	.get_bandwidth = adrv9009_get_bandwidth,
	.rx_channels_per_phy = CH_PER_PHY,
	.tx_channels_per_phy = CH_PER_PHY,
	/* No Tick, no timed hops: 0. caps.c raises it to 64 (the FHM table,
	 * fhm.c) when a Tick is bound. */
	.hop_table_depth = 0,
	.rx_stream_dev = "axi-adrv9009-rx-hpc",
	.tx_stream_dev = "axi-adrv9009-tx-hpc",
	.stream_channel_ids = adrv9009_stream_channel_ids,
};
