// SPDX-License-Identifier: MIT
/*
 * Talise FHM timed tunes on the adrv9009 family (fhm.c): hop staging,
 * queue_tune and the one-behind staging pipeline the scheduler's hooks run.
 * api-c-core "Hop staging"; spec decisions 6-7 and ruling 1.
 */
#ifndef DATAX_FHM_H
#define DATAX_FHM_H

#include <datax/datax.h>

#include "internal.h"
#include "tick_ops.h"

/* The real phy ops: channel attributes; h is the struct iio_channel *
 * (adrv9009-phy output altvoltage0, the TRX LO). */
extern const struct datax_tick_ops datax_fhm_iio_ops;

/* Bind ops/h as dev's FHM phy seam (NULL ops detaches). 0, -EINVAL, or
 * -EBUSY while dev has a scheduler (its hooks use the seam unlocked). */
int datax_fhm_bind(struct datax_device *dev, const struct datax_tick_ops *ops, void *h);

/* Free dev->fhm (context destroy, after the schedulers are gone). */
void datax_fhm_free(struct datax_device *dev);

/* datax_sched_queue_tune's body. */
long datax_fhm_queue_tune(struct datax_sched *s, datax_time when, unsigned int ch,
			  unsigned long long hz);

/* Timed datax_device_set_frequency (when != DATAX_TIME_NOW): validate, then
 * queue_tune + commit on the device's scheduler, created if needed. */
int datax_fhm_timed_set_frequency(struct datax_device *dev, unsigned int ch,
				  unsigned long long hz, datax_time when);

#endif
