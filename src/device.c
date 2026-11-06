// SPDX-License-Identifier: MIT
/* Public radio-control verbs: validate, then dispatch to the family ops. */
#include <errno.h>

#include <datax/datax.h>

#include "error.h"
#include "platform/family.h"

/* Common checks. On success *ops is the family's op table. */
static int prepare(const struct datax_device *dev, bool use_ch, unsigned int ch,
		   const struct datax_family_ops **ops)
{
	if (!dev)
		return -EINVAL;
	if (use_ch && ch >= dev->rx_channels + dev->tx_channels)
		return datax_error_ret(dev->ctx, EINVAL, "device",
				       "channel %u out of range (device has %u)",
				       ch, dev->rx_channels + dev->tx_channels);
	*ops = datax_family_ops(dev->family);
	if (!*ops)
		return datax_error_ret(dev->ctx, ENOTSUP, "device",
				       "unsupported device family");
	return 0;
}

static int check_timed(const struct datax_device *dev, datax_time when)
{
	if (when != DATAX_TIME_NOW)
		return datax_error_ret(dev->ctx, ENOTSUP, "device",
				       "timed radio calls arrive with FHM tunes (P3a); "
				       "use datax_sched_* for timed GPIO and markers");
	return 0;
}

int datax_device_set_frequency(struct datax_device *dev, unsigned int ch,
			       unsigned long long hz, datax_time when)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, true, ch, &ops);

	if (!ret)
		ret = check_timed(dev, when);
	return ret ? ret : ops->set_frequency(dev, ch, hz);
}

int datax_device_get_frequency(const struct datax_device *dev, unsigned int ch,
			       unsigned long long *hz)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, true, ch, &ops);

	if (!ret && !hz)
		return datax_error_ret(dev->ctx, EINVAL, "device", "NULL output");
	return ret ? ret : ops->get_frequency(dev, ch, hz);
}

int datax_device_set_gain(struct datax_device *dev, unsigned int ch, double db,
			  datax_time when)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, true, ch, &ops);

	if (!ret)
		ret = check_timed(dev, when);
	return ret ? ret : ops->set_gain(dev, ch, db);
}

int datax_device_get_gain(const struct datax_device *dev, unsigned int ch,
			  double *db)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, true, ch, &ops);

	if (!ret && !db)
		return datax_error_ret(dev->ctx, EINVAL, "device", "NULL output");
	return ret ? ret : ops->get_gain(dev, ch, db);
}

int datax_device_set_sample_rate(struct datax_device *dev,
				 unsigned long long sps)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, false, 0, &ops);

	return ret ? ret : ops->set_sample_rate(dev, sps);
}

int datax_device_get_sample_rate(const struct datax_device *dev,
				 unsigned long long *sps)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, false, 0, &ops);

	if (!ret && !sps)
		return datax_error_ret(dev->ctx, EINVAL, "device", "NULL output");
	return ret ? ret : ops->get_sample_rate(dev, sps);
}

int datax_device_get_bandwidth(const struct datax_device *dev, unsigned int ch,
			       unsigned long long *hz)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, true, ch, &ops);

	if (!ret && !hz)
		return datax_error_ret(dev->ctx, EINVAL, "device", "NULL output");
	return ret ? ret : ops->get_bandwidth(dev, ch, hz);
}

int datax_device_set_bandwidth(struct datax_device *dev, unsigned int ch,
			       unsigned long long hz)
{
	const struct datax_family_ops *ops;
	int ret = prepare(dev, true, ch, &ops);

	return ret ? ret : ops->set_bandwidth(dev, ch, hz);
}
