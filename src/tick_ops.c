// SPDX-License-Identifier: MIT
/* Tick ops seam: the libiio implementation, binding, and attribute helpers. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <iio/iio.h>

#include <datax/datax_internal.h>

#include "error.h"
#include "internal.h"
#include "tick_ops.h"

/* ---- real ops: attributes of the Tick struct iio_device ---- */

static int iio_tick_read(void *h, const char *attr, char *buf, size_t len)
{
	const struct iio_attr *a;
	ssize_t n;

	if (!h || !attr || !buf || !len)
		return -EINVAL;
	a = iio_device_find_attr(h, attr);
	if (!a)
		return -ENOENT;
	n = iio_attr_read_raw(a, buf, len - 1);
	if (n < 0)
		return (int)n;
	if ((size_t)n > len - 1)
		n = (ssize_t)(len - 1);
	buf[n] = '\0';
	return (int)n;
}

static int iio_tick_write(void *h, const char *attr, const char *val)
{
	const struct iio_attr *a;
	ssize_t n;

	if (!h || !attr || !val)
		return -EINVAL;
	a = iio_device_find_attr(h, attr);
	if (!a)
		return -ENOENT;
	n = iio_attr_write_string(a, val);
	return n < 0 ? (int)n : 0;
}

const struct datax_tick_ops datax_tick_iio_ops = { iio_tick_read, iio_tick_write };

/* ---- binding ---- */

int datax_tick_bind(struct datax_device *dev, const struct datax_tick_ops *ops, void *h)
{
	bool busy;

	if (!dev || (ops && (!ops->read || !ops->write)))
		return -EINVAL;
	/* The poller reads tick_ops/tick_h unlocked: never rebind under it. */
	pthread_mutex_lock(&dev->ctx->lock);
	busy = dev->sched != NULL;
	pthread_mutex_unlock(&dev->ctx->lock);
	if (busy)
		return -EBUSY;
	dev->tick_ops = ops;
	dev->tick_h = ops ? h : NULL;
	return datax_caps_build(dev);
}

int datax_internal_tick_bind_fake(struct datax_device *dev,
				  const struct datax_tick_ops *ops, void *h)
{
	int ret = datax_tick_bind(dev, ops, h);

	if (ret == -EBUSY)
		return datax_error_ret(dev->ctx, EBUSY, "tick",
				       "tick_bind_fake: the device has a scheduler; destroy it first");
	if (ret && dev)
		return datax_error_ret(dev->ctx, -ret, "tick",
				       "tick_bind_fake: ops need read and write");
	return ret;
}

bool datax_tick_present(const struct datax_device *dev)
{
	return dev && dev->tick_ops;
}

/* ---- helpers ---- */

int datax_tick_read_str(const struct datax_device *dev, const char *attr,
			char *buf, size_t len)
{
	if (!datax_tick_present(dev))
		return -ENODEV;
	return dev->tick_ops->read(dev->tick_h, attr, buf, len);
}

static int read_base(const struct datax_device *dev, const char *attr, int base,
		     unsigned long long *v)
{
	char buf[64];
	char *end;
	unsigned long long x;
	int n = datax_tick_read_str(dev, attr, buf, sizeof(buf));

	if (n < 0)
		return n == -ENOENT ? -ENODEV : n;
	errno = 0;
	x = strtoull(buf, &end, base);
	if (errno || end == buf || buf[0] == '-')
		return -EINVAL;
	while (*end == ' ' || *end == '\n' || *end == '\t' || *end == '\r')
		end++;
	if (*end)
		return -EINVAL;
	*v = x;
	return 0;
}

int datax_tick_read_u64(const struct datax_device *dev, const char *attr,
			unsigned long long *v)
{
	if (!v)
		return -EINVAL;
	return read_base(dev, attr, 10, v);
}

int datax_tick_write_str(const struct datax_device *dev, const char *attr,
			 const char *val)
{
	if (!datax_tick_present(dev))
		return -ENODEV;
	return dev->tick_ops->write(dev->tick_h, attr, val);
}

int datax_tick_write_u64(const struct datax_device *dev, const char *attr,
			 unsigned long long v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%llu", v);
	return datax_tick_write_str(dev, attr, buf);
}

int datax_tick_read_limits(const struct datax_device *dev, unsigned long long *tick_hz,
			   unsigned *queue_depth, unsigned *capability_bitmap)
{
	unsigned long long v;
	int ret;

	if (!datax_tick_present(dev))
		return -ENODEV;
	if (tick_hz) {
		ret = read_base(dev, "time_tick_hz", 10, &v);
		if (ret)
			return ret;
		*tick_hz = v;
	}
	if (queue_depth) {
		ret = read_base(dev, "queue_depth", 10, &v);
		if (ret)
			return ret;
		if (v > UINT_MAX)
			return -ERANGE;
		*queue_depth = (unsigned)v;
	}
	if (capability_bitmap) {
		/* Hex text such as 0x0000001f; base 16 accepts the prefix. */
		ret = read_base(dev, "capability_bitmap", 16, &v);
		if (ret)
			return ret;
		if (v > UINT_MAX)
			return -ERANGE;
		*capability_bitmap = (unsigned)v;
	}
	return 0;
}
