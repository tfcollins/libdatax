// SPDX-License-Identifier: MIT
/*
 * libdatax - DataX SDR SDK core over libiio v1.
 * Contract: datax_sdk_sdr/docs/api-c-core.md. All functions return 0 or a
 * negative errno unless a handle or count is the documented return.
 */
#ifndef DATAX_DATAX_H
#define DATAX_DATAX_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DATAX_VERSION_MAJOR 0
#define DATAX_VERSION_MINOR 1

struct iio_context;

/* Opaque handles. */
struct datax_context;
struct datax_device;
struct datax_caps;
struct datax_stream;
struct datax_sched;

/* ---- Types ---- */

typedef unsigned long long datax_time;       /* device ticks */
#define DATAX_TIME_NOW ((datax_time)0)       /* immediate, software path */

enum datax_direction { DATAX_RX = 0, DATAX_TX = 1 };

#define DATAX_ERR_LATE (-ETIME)              /* deadline unmeetable at submit */

enum datax_cap {                             /* order matches the capability matrix */
	DATAX_CAP_TIMED_COMMANDS, DATAX_CAP_RX_ARM, DATAX_CAP_TX_ARM,
	DATAX_CAP_STREAM_GATE, DATAX_CAP_TIMESTAMPS, DATAX_CAP_FASTLOCK_HOP,
	DATAX_CAP_FHM_HOP, DATAX_CAP_MCS, DATAX_CAP_PHASE_COHERENT,
	DATAX_CAP_NET_OFFLOAD_NIC, DATAX_CAP_NET_OFFLOAD_SAMPLES, DATAX_CAP_COUNT
};

enum datax_limit {
	DATAX_LIMIT_TICK_HZ, DATAX_LIMIT_QUEUE_DEPTH, DATAX_LIMIT_HOP_TABLE_DEPTH,
	DATAX_LIMIT_GUARD_MARGIN_MS, DATAX_LIMIT_MAX_CHANNELS, DATAX_LIMIT_COUNT
};

bool datax_caps_has(const struct datax_caps *caps, enum datax_cap cap);
int  datax_caps_get_limit(const struct datax_caps *caps, enum datax_limit which,
			  unsigned long long *value);

enum datax_event_type {
	DATAX_EVT_LATE_COMMAND,
	DATAX_EVT_QUEUE_OVERFLOW,
	DATAX_EVT_STREAM_OVERFLOW,
	DATAX_EVT_STREAM_UNDERFLOW,
	DATAX_EVT_PPS_LOCK_CHANGE,
};

struct datax_event {
	enum datax_event_type type;
	unsigned long long ticks;                /* device time of the event, 0 if unknown */
	long seq;                                /* scheduler sequence id, -1 if none */
	const char *detail;                      /* human-readable, valid during the callback */
};

/* ---- Context ---- */

int  datax_context_create_from_uri(const char *uri, struct datax_context **ctx);
int  datax_context_create_local(struct datax_context **ctx);
void datax_context_destroy(struct datax_context *ctx);

unsigned int datax_context_get_device_count(const struct datax_context *ctx);
int  datax_context_get_device(struct datax_context *ctx, unsigned int index,
			      struct datax_device **dev);

struct iio_context *datax_context_get_iio_context(struct datax_context *ctx);
const char *datax_context_get_last_error(const struct datax_context *ctx);
int  datax_context_get_version(const struct datax_context *ctx,
			       unsigned int *major, unsigned int *minor);

/* ---- Device ---- */

const char *datax_device_get_name(const struct datax_device *dev);
const struct datax_caps *datax_device_get_caps(const struct datax_device *dev);

/* Radio functions - 'when' is a device time; DATAX_TIME_NOW = immediate. */
int datax_device_set_frequency(struct datax_device *dev, unsigned int ch,
			       unsigned long long hz, datax_time when);
int datax_device_get_frequency(const struct datax_device *dev, unsigned int ch,
			       unsigned long long *hz);
int datax_device_set_gain(struct datax_device *dev, unsigned int ch,
			  double db, datax_time when);
int datax_device_get_gain(const struct datax_device *dev, unsigned int ch, double *db);
int datax_device_set_sample_rate(struct datax_device *dev,
				 unsigned long long sps);   /* never timed */
int datax_device_get_sample_rate(const struct datax_device *dev,
				 unsigned long long *sps);
int datax_device_set_bandwidth(struct datax_device *dev, unsigned int ch,
			       unsigned long long hz);
int datax_device_get_bandwidth(const struct datax_device *dev, unsigned int ch,
			       unsigned long long *hz);

/* Hop staging - milliseconds-scale, never timed; stage before scheduling. */
int datax_device_stage_hop_table(struct datax_device *dev,
				 const unsigned long long *freqs_hz, size_t count);

/* Device time */
int datax_device_get_time(const struct datax_device *dev,
			  unsigned long long *ticks, unsigned long long *tick_hz);
int datax_device_set_time_next_pps(struct datax_device *dev,
				   unsigned long long epoch_ticks);

/* Multi-chip sync (DATAX_CAP_MCS) */
int datax_device_run_mcs(struct datax_device *dev);

/* ---- Stream ---- */

struct datax_stream_config {
	enum datax_direction direction;     /* DATAX_RX or DATAX_TX */
	unsigned int channel_mask;
	size_t block_samples;               /* samples per block, per channel */
	unsigned int block_count;           /* buffers in flight */
	bool cyclic;                        /* TX: repeat from fabric memory */
};

struct datax_block_info {
	unsigned long long timestamp_ticks; /* first-sample time; 0 if untagged */
	bool discontinuous;                 /* overflow occurred before this block */
	size_t valid_samples;
};

int  datax_stream_create(struct datax_device *dev,
			 const struct datax_stream_config *cfg,
			 struct datax_stream **stream);
void datax_stream_destroy(struct datax_stream *stream);

int datax_stream_start(struct datax_stream *stream);
int datax_stream_start_at(struct datax_stream *stream, datax_time when);
int datax_stream_stop(struct datax_stream *stream);

/* Copying I/O - interleaved int16 I/Q, host order. */
int datax_stream_recv(struct datax_stream *stream, void *dst, size_t samples,
		      struct datax_block_info *info, int timeout_ms);
int datax_stream_send(struct datax_stream *stream, const void *src, size_t samples,
		      int timeout_ms);

/* Zero-copy - borrow the mapped block, return it when done. */
int datax_stream_get_block(struct datax_stream *stream, void **data,
			   struct datax_block_info *info, int timeout_ms);
int datax_stream_put_block(struct datax_stream *stream, void *data);

/* ---- Scheduler ---- */

enum datax_completion_status {
	DATAX_COMPLETION_OK,
	DATAX_COMPLETION_LATE,
	DATAX_COMPLETION_DROPPED,
	DATAX_COMPLETION_FLUSHED
};

struct datax_completion {
	long seq;
	enum datax_completion_status status;  /* OK, LATE, DROPPED, FLUSHED */
	unsigned long long requested_ticks;
	unsigned long long executed_ticks;
};

int  datax_sched_create(struct datax_device *dev, struct datax_sched **sched);
void datax_sched_destroy(struct datax_sched *sched);

/* Queue builders - accumulate client-side, in timestamp order. Each
 * returns a sequence id for completion matching (negative errno on error). */
long datax_sched_queue_gpio(struct datax_sched *sched, datax_time when,
			    unsigned int mask, unsigned int value);
long datax_sched_queue_marker(struct datax_sched *sched, datax_time when,
			      unsigned int line, unsigned int width_ticks);
long datax_sched_queue_tune(struct datax_sched *sched, datax_time when,
			    unsigned int ch, unsigned long long hz);
long datax_sched_queue_stream_gate(struct datax_sched *sched, datax_time when,
				   struct datax_stream *stream, bool enable);

int datax_sched_commit(struct datax_sched *sched);
int datax_sched_flush(struct datax_sched *sched);

int datax_sched_wait(struct datax_sched *sched, long seq, int timeout_ms,
		     struct datax_completion *out);
int datax_sched_get_completions(struct datax_sched *sched,
				struct datax_completion *out, size_t max,
				size_t *count);

/* ---- Events ---- */

typedef void (*datax_event_cb)(const struct datax_event *evt, void *user);
int datax_context_set_event_callback(struct datax_context *ctx,
				     datax_event_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* DATAX_DATAX_H */
