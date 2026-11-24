// SPDX-License-Identifier: MIT
/* Tick ops seam: the libiio implementation, binding, and attribute helpers. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ---- stream gate map ---- */

/* The attribute name is not final (Tick driver): a rename is this one line. */
#define DATAX_TICK_GATE_MAP_ATTR "stream_gate_map"

/*
 * Grammar: tokens separated by comma, space or newline. The position k of a
 * gate token among the gate tokens is gate bit k; "-" holds a spare position
 * and an unknown token is ignored but advances the position. The last two
 * roles are presence tokens: they are not gate bits and do not advance the
 * position. A duplicated role rejects the whole map.
 */
enum gate_role {
	ROLE_RX_LEVEL, ROLE_TX_LEVEL, ROLE_RX_MODE, ROLE_TX_MODE,
	ROLE_FHM_B, ROLE_TX_FULLMASK_ONLY,
	ROLE_COUNT, ROLE_FIRST_PRESENCE = ROLE_FHM_B
};

static const char *const gate_roles[ROLE_COUNT] = {
	"rx-dma", "tx-dma", "rx-mode", "tx-mode", "fhm-b", "tx-start-fullmask-only"
};

#define GATE_BITS 8

static void gate_map_absent(struct datax_gate_map *out)
{
	out->present = false;
	out->rx_level = out->tx_level = out->rx_mode = out->tx_mode = -1;
	out->fhm_b = out->tx_fullmask_only = false;
}

int datax_tick_parse_gate_map(const char *text, struct datax_gate_map *out)
{
	int bit_of[ROLE_COUNT];
	int pos = 0;
	const char *p = text;

	if (!out)
		return -EINVAL;
	gate_map_absent(out);
	if (!text)
		return -EINVAL;
	for (int i = 0; i < ROLE_COUNT; i++)
		bit_of[i] = -1;

	while (*p) {
		size_t n = strcspn(p, ", \t\r\n");
		int role = -1;

		for (int i = 0; i < ROLE_COUNT && n; i++)
			if (strlen(gate_roles[i]) == n && !strncmp(p, gate_roles[i], n))
				role = i;
		if (role >= 0) {
			if (bit_of[role] != -1)
				return -EINVAL;   /* duplicate: reject the map */
			if (role >= ROLE_FIRST_PRESENCE) {
				bit_of[role] = 1;
			} else {
				/* A position past the gate bits names no bit; keep
				 * the role seen so a later duplicate still rejects. */
				bit_of[role] = pos < GATE_BITS ? pos : -2;
				pos++;
			}
		} else if (n) {
			pos++;                   /* "-" and unknown tokens */
		}
		p += n;
		p += strspn(p, ", \t\r\n");
	}
	out->present = true;
	out->rx_level = bit_of[ROLE_RX_LEVEL] >= 0 ? bit_of[ROLE_RX_LEVEL] : -1;
	out->tx_level = bit_of[ROLE_TX_LEVEL] >= 0 ? bit_of[ROLE_TX_LEVEL] : -1;
	out->rx_mode = bit_of[ROLE_RX_MODE] >= 0 ? bit_of[ROLE_RX_MODE] : -1;
	out->tx_mode = bit_of[ROLE_TX_MODE] >= 0 ? bit_of[ROLE_TX_MODE] : -1;
	out->fhm_b = bit_of[ROLE_FHM_B] == 1;
	out->tx_fullmask_only = bit_of[ROLE_TX_FULLMASK_ONLY] == 1;
	return 0;
}

int datax_tick_read_gate_map(const struct datax_device *dev, struct datax_gate_map *out)
{
	char buf[256];
	int ret;

	if (!out)
		return -EINVAL;
	gate_map_absent(out);
	ret = datax_tick_read_str(dev, DATAX_TICK_GATE_MAP_ATTR, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	return datax_tick_parse_gate_map(buf, out);
}

/* Test export: the parse as seven ints in struct field order. */
int datax_internal_gate_map_parse(const char *text, int out[7])
{
	struct datax_gate_map m;
	int ret = datax_tick_parse_gate_map(text, &m);

	if (out) {
		out[0] = m.present;
		out[1] = m.rx_level;
		out[2] = m.tx_level;
		out[3] = m.rx_mode;
		out[4] = m.tx_mode;
		out[5] = m.fhm_b;
		out[6] = m.tx_fullmask_only;
	}
	return ret;
}
