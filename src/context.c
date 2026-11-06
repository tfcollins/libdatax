// SPDX-License-Identifier: MIT
/* Context lifecycle and device/IIO accessors. Folding lives in platform/table.c. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <iio/iio.h>

#include "error.h"
#include "fhm.h"
#include "platform/table.h"
#include "scheduler.h"

static int create(const char *uri, struct datax_context **out)
{
	struct datax_context *ctx;
	struct iio_context *iio;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;

	iio = iio_create_context(NULL, uri);
	ret = iio_err(iio);
	if (ret)
		return ret < 0 ? ret : -ret;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		iio_context_destroy(iio);
		return -ENOMEM;
	}
	ctx->iio = iio;
	ret = pthread_mutex_init(&ctx->lock, NULL);
	if (ret) {
		iio_context_destroy(iio);
		free(ctx);
		return -ret;
	}
	ret = datax_fold_devices(ctx);
	if (ret) {
		datax_context_destroy(ctx);
		return ret;
	}
	*out = ctx;
	return 0;
}

int datax_context_create_from_uri(const char *uri, struct datax_context **ctx)
{
	if (!uri)
		return -EINVAL;
	return create(uri, ctx);
}

int datax_context_create_local(struct datax_context **ctx)
{
	return create("local:", ctx);
}

void datax_context_destroy(struct datax_context *ctx)
{
	if (!ctx)
		return;
	/* Called from an event callback, on a poller thread: that poller
	 * cannot join itself, so refuse (destroy it from another thread). */
	for (unsigned int i = 0; i < ctx->ndevices; i++)
		if (ctx->devices[i]->sched && datax_poller_is_self(ctx->devices[i]->sched))
			return;
	/* Schedulers first: each poller is joined before any stream, device or
	 * the IIO context it uses is freed. */
	for (unsigned int i = 0; i < ctx->ndevices; i++)
		datax_sched_destroy(ctx->devices[i]->sched);
	/* Streams still alive hold IIO buffers of this context. */
	for (unsigned int i = 0; i < ctx->ndevices; i++)
		for (int d = DATAX_RX; d <= DATAX_TX; d++)
			datax_stream_destroy(ctx->devices[i]->stream[d]);
	for (unsigned int i = 0; i < ctx->ndevices; i++) {
		datax_fhm_free(ctx->devices[i]);   /* after its scheduler's hooks */
		free(ctx->devices[i]);
	}
	free(ctx->devices);
	iio_context_destroy(ctx->iio);
	pthread_mutex_destroy(&ctx->lock);
	free(ctx);
}

static struct datax_context *mut(const struct datax_context *ctx)
{
	return (struct datax_context *)(uintptr_t)ctx;
}

unsigned int datax_context_get_device_count(const struct datax_context *ctx)
{
	unsigned int n;

	if (!ctx)
		return 0;
	pthread_mutex_lock(&mut(ctx)->lock);
	n = ctx->ndevices;
	pthread_mutex_unlock(&mut(ctx)->lock);
	return n;
}

int datax_context_get_device(struct datax_context *ctx, unsigned int index,
			     struct datax_device **dev)
{
	struct datax_device *d = NULL;
	unsigned int n;

	if (!ctx || !dev)
		return -EINVAL;
	pthread_mutex_lock(&ctx->lock);
	n = ctx->ndevices;
	if (index < n)
		d = ctx->devices[index];
	pthread_mutex_unlock(&ctx->lock);
	if (!d)
		return datax_error_ret(ctx, EINVAL, "context",
				       "device index %u out of range (count %u)",
				       index, n);
	*dev = d;
	return 0;
}

struct iio_context *datax_context_get_iio_context(struct datax_context *ctx)
{
	return ctx ? ctx->iio : NULL;
}

int datax_context_set_event_callback(struct datax_context *ctx,
				     datax_event_cb cb, void *user)
{
	if (!ctx)
		return -EINVAL;
	pthread_mutex_lock(&ctx->lock);
	ctx->event_cb = cb;
	ctx->event_user = user;
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

const char *datax_device_get_name(const struct datax_device *dev)
{
	return dev ? dev->name : NULL;
}

const struct datax_caps *datax_device_get_caps(const struct datax_device *dev)
{
	return dev ? &dev->caps : NULL;
}
