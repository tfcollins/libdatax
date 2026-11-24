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

/*
 * The Tick's stream_gate_map: which stream gate bit does which job. -1 means
 * the role is absent from the map. present: the attribute was read and
 * parsed (an empty map is present and names nothing).
 */
struct datax_gate_map {
	bool present;
	int rx_level, tx_level, rx_mode, tx_mode;
	bool fhm_b;            /* chip B's trigger is Tick GPIO bit 1 */
	bool tx_fullmask_only; /* TX start_at only on a mask of every TX channel */
};

/*
 * Parse the stream_gate_map text. 0, or -EINVAL for a NULL argument or a
 * duplicated role (out then reads as absent: present false, every bit -1).
 * Grammar and role table: src/tick_ops.c.
 */
int datax_tick_parse_gate_map(const char *text, struct datax_gate_map *out);

/*
 * Read and parse the bound Tick's gate map. 0 (out filled), -ENODEV without a
 * Tick, -ENOENT when the attribute is absent, -EINVAL when the map is
 * rejected. out is the absent map on every error.
 */
int datax_tick_read_gate_map(const struct datax_device *dev, struct datax_gate_map *out);

#endif
