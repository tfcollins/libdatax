// SPDX-License-Identifier: MIT
/*
 * Tick ops seam. Every access libdatax makes to the Tick scheduler
 * (axi-timed-command-scheduler) goes through the ops bound to the device, so
 * Tier 1 tests can swap in the C fake Tick (tests/c/fake_tick.h).
 *
 * read:  copy attribute attr as text into buf (NUL-terminated, at most
 *        len - 1 bytes). Returns the text length (>= 0) or -errno.
 * write: write val to attribute attr. Returns 0 or -errno.
 * Both return -ENOENT for an attribute the device does not have.
 */
#ifndef DATAX_TICK_OPS_H
#define DATAX_TICK_OPS_H

#include <stdbool.h>
#include <stddef.h>

struct datax_device;
struct iio_device;

struct datax_tick_ops {
	int (*read)(void *h, const char *attr, char *buf, size_t len);
	int (*write)(void *h, const char *attr, const char *val);
};

/* The real ops: libiio device attributes; h is the Tick struct iio_device. */
extern const struct datax_tick_ops datax_tick_iio_ops;

/*
 * Bind ops/h as dev's Tick (NULL ops: no Tick) and re-probe the device's
 * capabilities through them. 0, or -EINVAL (NULL dev, or ops missing a
 * function).
 */
int datax_tick_bind(struct datax_device *dev, const struct datax_tick_ops *ops, void *h);

/* True when dev has a Tick bound. */
bool datax_tick_present(const struct datax_device *dev);

/* Attribute helpers over the bound ops. -ENODEV when no Tick is bound. */
int datax_tick_read_str(const struct datax_device *dev, const char *attr,
			char *buf, size_t len);
/* Parses a decimal value (trailing whitespace allowed). -EINVAL if the text
 * does not parse. */
int datax_tick_read_u64(const struct datax_device *dev, const char *attr,
			unsigned long long *v);
int datax_tick_write_str(const struct datax_device *dev, const char *attr,
			 const char *val);
int datax_tick_write_u64(const struct datax_device *dev, const char *attr,
			 unsigned long long v);

/*
 * The Tick limits through the seam. Any output may be NULL. 0, or -errno
 * (-ENODEV without a Tick or when an attribute is missing, -EINVAL when it
 * does not parse, -ERANGE when it does not fit).
 */
int datax_tick_read_limits(const struct datax_device *dev, unsigned long long *tick_hz,
			   unsigned *queue_depth, unsigned *capability_bitmap);

#endif
