// SPDX-License-Identifier: MIT
#ifndef DATAX_INTERNAL_H
#define DATAX_INTERNAL_H

#include <pthread.h>

#include <datax/datax.h>

struct iio_device;

enum datax_family {
	DATAX_FAMILY_UNKNOWN,
	DATAX_FAMILY_AD936X,
	DATAX_FAMILY_ADRV9009
};

struct datax_caps {
	unsigned int flags;        /* bit per enum datax_cap this SDK can do here */
	unsigned int probe_flags;  /* bit per enum datax_cap the probe detected */
	unsigned long long limits[DATAX_LIMIT_COUNT];
};

struct datax_context {
	struct iio_context *iio;
	pthread_mutex_t lock;
	struct datax_device **devices;
	unsigned int ndevices;
	char last_error[256];
	char last_subsystem[32];
	datax_event_cb event_cb;
	void *event_user;
};

struct datax_device {
	struct datax_context *ctx;
	enum datax_family family;
	char name[64];
	struct iio_device *phy[2];
	unsigned int nphy;
	unsigned int rx_channels, tx_channels;
	struct datax_caps caps;
};

/* caps.c: fill dev->caps after folding (public flags stay 0 in P1). */
int datax_caps_build(struct datax_device *dev);
/* caps.c, diagnostic export (used by the Python package's caps.detected
 * and doctor): what the probe detected. */
unsigned int datax_internal_probe_flags(const struct datax_device *dev);

#endif
