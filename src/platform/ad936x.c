// SPDX-License-Identifier: MIT
/* ad936x (AD9361/AD9363/AD9364) radio-control ops over libiio attributes. */
#include <errno.h>
#include <stdio.h>

#include <iio/iio.h>

#include "../error.h"
#include "family.h"

static const struct iio_attr *find_attr(const struct datax_device *dev,
					const char *chn_id, bool output,
					const char *attr)
{
	struct iio_channel *chn;

	if (dev->nphy < 1 || !dev->phy[0])
		return NULL;
	chn = iio_device_find_channel(dev->phy[0], chn_id, output);
	return chn ? iio_channel_find_attr(chn, attr) : NULL;
}

/* Look up attr or record -ENODEV. */
static int lookup(const struct datax_device *dev, const char *chn_id,
		  bool output, const char *attr, const struct iio_attr **out)
{
	*out = find_attr(dev, chn_id, output, attr);
	if (!*out)
		return datax_error_ret(dev->ctx, ENODEV, "ad936x",
				       "%s %s/%s attribute not found",
				       output ? "output" : "input", chn_id, attr);
	return 0;
}

static int rd_ll(const struct datax_device *dev, const struct iio_attr *a,
		 long long *v)
{
	int ret = iio_attr_read_longlong(a, v);

	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "ad936x",
				       "read of %s failed", iio_attr_get_name(a));
	return 0;
}

static int wr_ll(const struct datax_device *dev, const struct iio_attr *a,
		 long long v)
{
	int ret = iio_attr_write_longlong(a, v);

	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "ad936x",
				       "write of %s failed", iio_attr_get_name(a));
	return 0;
}

/* RX channels are input voltage<ch>; TX are output voltage<ch - rx>. */
static int voltage_attr(const struct datax_device *dev, unsigned int ch,
			const char *attr, const struct iio_attr **out)
{
	char id[16];
	bool tx = ch >= dev->rx_channels;

	snprintf(id, sizeof(id), "voltage%u", tx ? ch - dev->rx_channels : ch);
	return lookup(dev, id, tx, attr, out);
}

static const char *lo_id(const struct datax_device *dev, unsigned int ch)
{
	return ch < dev->rx_channels ? "altvoltage0" : "altvoltage1";
}

static int ad936x_set_frequency(struct datax_device *dev, unsigned int ch,
				unsigned long long hz)
{
	const struct iio_attr *a;
	int ret = lookup(dev, lo_id(dev, ch), true, "frequency", &a);

	if (ret)
		return ret;
	if (hz > 0x7fffffffffffffffULL)
		return datax_error_ret(dev->ctx, EINVAL, "ad936x",
				       "frequency out of range");
	return wr_ll(dev, a, (long long)hz);
}

static int ad936x_get_frequency(const struct datax_device *dev, unsigned int ch,
				unsigned long long *hz)
{
	const struct iio_attr *a;
	long long v;
	int ret = lookup(dev, lo_id(dev, ch), true, "frequency", &a);

	if (!ret)
		ret = rd_ll(dev, a, &v);
	if (ret)
		return ret;
	*hz = (unsigned long long)v;
	return 0;
}

static int ad936x_set_gain(struct datax_device *dev, unsigned int ch, double db)
{
	const struct iio_attr *a;
	int ret = voltage_attr(dev, ch, "hardwaregain", &a);

	if (ret)
		return ret;
	ret = iio_attr_write_double(a, db);
	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "ad936x",
				       "write of hardwaregain failed");
	return 0;
}

static int ad936x_get_gain(const struct datax_device *dev, unsigned int ch,
			   double *db)
{
	const struct iio_attr *a;
	int ret = voltage_attr(dev, ch, "hardwaregain", &a);

	if (ret)
		return ret;
	ret = iio_attr_read_double(a, db);
	if (ret < 0)
		return datax_error_ret(dev->ctx, -ret, "ad936x",
				       "read of hardwaregain failed");
	return 0;
}

static int ad936x_set_sample_rate(struct datax_device *dev,
				  unsigned long long sps)
{
	const struct iio_attr *a;
	int ret = lookup(dev, "voltage0", false, "sampling_frequency", &a);

	if (ret)
		return ret;
	if (sps > 0x7fffffffffffffffULL)
		return datax_error_ret(dev->ctx, EINVAL, "ad936x",
				       "sample rate out of range");
	return wr_ll(dev, a, (long long)sps);
}

static int ad936x_get_sample_rate(const struct datax_device *dev,
				  unsigned long long *sps)
{
	const struct iio_attr *a;
	long long v;
	int ret = lookup(dev, "voltage0", false, "sampling_frequency", &a);

	if (!ret)
		ret = rd_ll(dev, a, &v);
	if (ret)
		return ret;
	*sps = (unsigned long long)v;
	return 0;
}

static int ad936x_get_bandwidth(const struct datax_device *dev, unsigned int ch,
				unsigned long long *hz)
{
	const struct iio_attr *a;
	long long v;
	int ret = voltage_attr(dev, ch, "rf_bandwidth", &a);

	if (!ret)
		ret = rd_ll(dev, a, &v);
	if (ret)
		return ret;
	*hz = (unsigned long long)v;
	return 0;
}

static int ad936x_set_bandwidth(struct datax_device *dev, unsigned int ch,
				unsigned long long hz)
{
	const struct iio_attr *a;
	int ret = voltage_attr(dev, ch, "rf_bandwidth", &a);

	if (ret)
		return ret;
	if (hz > 0x7fffffffffffffffULL)
		return datax_error_ret(dev->ctx, EINVAL, "ad936x",
				       "bandwidth out of range");
	return wr_ll(dev, a, (long long)hz);
}

/* ad936x radio channel n is the scan pair voltage{2n}, voltage{2n+1}. */
static int ad936x_stream_channel_ids(unsigned int radio_ch,
				     enum datax_direction dir,
				     const char **id_i, const char **id_q)
{
	static const char *const ids[] = { "voltage0", "voltage1", "voltage2",
					   "voltage3" };

	(void)dir;
	if (radio_ch >= 2)
		return -EINVAL;
	*id_i = ids[2 * radio_ch];
	*id_q = ids[2 * radio_ch + 1];
	return 0;
}

const struct datax_family_ops datax_ad936x_ops = {
	.set_frequency = ad936x_set_frequency,
	.get_frequency = ad936x_get_frequency,
	.set_gain = ad936x_set_gain,
	.get_gain = ad936x_get_gain,
	.set_sample_rate = ad936x_set_sample_rate,
	.get_sample_rate = ad936x_get_sample_rate,
	.set_bandwidth = ad936x_set_bandwidth,
	.get_bandwidth = ad936x_get_bandwidth,
	.rx_channels_per_phy = 2,
	.tx_channels_per_phy = 2,
	.hop_table_depth = 8,
	.rx_stream_dev = "cf-ad9361-lpc",
	.tx_stream_dev = "cf-ad9361-dds-core-lpc",
	.stream_channel_ids = ad936x_stream_channel_ids,
};
