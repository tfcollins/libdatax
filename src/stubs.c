// SPDX-License-Identifier: MIT
/*
 * P1 stubs: stream, scheduler, hop staging, MCS and PPS time-set are not
 * implemented yet. They return -ENOTSUP and tolerate NULL handles, so no
 * last-error is recorded.
 */
#include <errno.h>

#include <datax/datax.h>

int datax_device_stage_hop_table(struct datax_device *dev,
				 const unsigned long long *freqs_hz, size_t count)
{
	(void)dev; (void)freqs_hz; (void)count;
	return -ENOTSUP;
}

int datax_device_set_time_next_pps(struct datax_device *dev,
				   unsigned long long epoch_ticks)
{
	(void)dev; (void)epoch_ticks;
	return -ENOTSUP;
}

int datax_device_run_mcs(struct datax_device *dev)
{
	(void)dev;
	return -ENOTSUP;
}

int datax_stream_create(struct datax_device *dev,
			const struct datax_stream_config *cfg,
			struct datax_stream **stream)
{
	(void)dev; (void)cfg; (void)stream;
	return -ENOTSUP;
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
	(void)dev; (void)sched;
	return -ENOTSUP;
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
