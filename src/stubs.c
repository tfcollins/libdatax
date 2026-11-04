// SPDX-License-Identifier: MIT
/*
 * P1 stubs: stream, scheduler, hop staging, MCS and PPS time-set are not
 * implemented yet and return -ENOTSUP.
 *
 * Device-taking stubs record "<verb>: arrives in P<n>" as the context's
 * last error when dev is non-NULL; with a NULL dev they return the bare code.
 * Stream/sched handle-taking stubs have no handle layout yet, so they return
 * the bare code only (no last-error).
 */
#include <errno.h>

#include <datax/datax.h>

#include "error.h"

static int dev_stub(const struct datax_device *dev, const char *msg)
{
	if (dev)
		return datax_error_ret(dev->ctx, ENOTSUP, "stub", "%s", msg);
	return -ENOTSUP;
}

int datax_device_stage_hop_table(struct datax_device *dev,
				 const unsigned long long *freqs_hz, size_t count)
{
	(void)freqs_hz; (void)count;
	return dev_stub(dev, "datax_device_stage_hop_table: arrives in P3");
}

int datax_device_run_mcs(struct datax_device *dev)
{
	return dev_stub(dev, "datax_device_run_mcs: arrives in P3");
}

int datax_stream_create(struct datax_device *dev,
			const struct datax_stream_config *cfg,
			struct datax_stream **stream)
{
	(void)cfg; (void)stream;
	return dev_stub(dev, "datax_stream_create: arrives in P2");
}

void datax_stream_destroy(struct datax_stream *stream)
{
	(void)stream;
}

int datax_stream_start(struct datax_stream *stream)
{
	(void)stream;
	return -ENOTSUP;
}

int datax_stream_start_at(struct datax_stream *stream, datax_time when)
{
	(void)stream; (void)when;
	return -ENOTSUP;
}

int datax_stream_stop(struct datax_stream *stream)
{
	(void)stream;
	return -ENOTSUP;
}

int datax_stream_recv(struct datax_stream *stream, void *dst, size_t samples,
		      struct datax_block_info *info, int timeout_ms)
{
	(void)stream; (void)dst; (void)samples; (void)info; (void)timeout_ms;
	return -ENOTSUP;
}

int datax_stream_send(struct datax_stream *stream, const void *src, size_t samples,
		      int timeout_ms)
{
	(void)stream; (void)src; (void)samples; (void)timeout_ms;
	return -ENOTSUP;
}

int datax_stream_get_block(struct datax_stream *stream, void **data,
			   struct datax_block_info *info, int timeout_ms)
{
	(void)stream; (void)data; (void)info; (void)timeout_ms;
	return -ENOTSUP;
}

int datax_stream_put_block(struct datax_stream *stream, void *data)
{
	(void)stream; (void)data;
	return -ENOTSUP;
}

int datax_sched_create(struct datax_device *dev, struct datax_sched **sched)
{
	(void)sched;
	return dev_stub(dev, "datax_sched_create: arrives in P3");
}

void datax_sched_destroy(struct datax_sched *sched)
{
	(void)sched;
}

long datax_sched_queue_gpio(struct datax_sched *sched, datax_time when,
			    unsigned int mask, unsigned int value)
{
	(void)sched; (void)when; (void)mask; (void)value;
	return -ENOTSUP;
}

long datax_sched_queue_marker(struct datax_sched *sched, datax_time when,
			      unsigned int line, unsigned int width_ticks)
{
	(void)sched; (void)when; (void)line; (void)width_ticks;
	return -ENOTSUP;
}

long datax_sched_queue_tune(struct datax_sched *sched, datax_time when,
			    unsigned int ch, unsigned long long hz)
{
	(void)sched; (void)when; (void)ch; (void)hz;
	return -ENOTSUP;
}

long datax_sched_queue_stream_gate(struct datax_sched *sched, datax_time when,
				   struct datax_stream *stream, bool enable)
{
	(void)sched; (void)when; (void)stream; (void)enable;
	return -ENOTSUP;
}

int datax_sched_commit(struct datax_sched *sched)
{
	(void)sched;
	return -ENOTSUP;
}

int datax_sched_flush(struct datax_sched *sched)
{
	(void)sched;
	return -ENOTSUP;
}

int datax_sched_wait(struct datax_sched *sched, long seq, int timeout_ms,
		     struct datax_completion *out)
{
	(void)sched; (void)seq; (void)timeout_ms; (void)out;
	return -ENOTSUP;
}

int datax_sched_get_completions(struct datax_sched *sched,
				struct datax_completion *out, size_t max,
				size_t *count)
{
	(void)sched; (void)out; (void)max; (void)count;
	return -ENOTSUP;
}
