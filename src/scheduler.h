// SPDX-License-Identifier: MIT
/*
 * Scheduler internals: the host-side batcher (sched.c) and its poller thread
 * (poller.c), per api-c-core "Scheduler semantics" and "Abstracting the Tick
 * submission bottleneck".
 *
 * Command life cycle (struct sched_cmd.state):
 *   BUILD    queued by a builder, not committed
 *   PENDING  committed, waiting for the feeder (the poller) to write it
 *   WRITTEN  descriptor written to Tick; reported ones wait for their record
 *   DONE     terminal result available (reported commands only), until
 *            wait() or get_completions() consumes it
 * Unreported commands leave the table once written and due, or at a
 * terminal outcome (host-late, write failure, flush).
 *
 * Locks. io serialises every Tick access libdatax makes while a scheduler
 * exists (a poller iteration, commit's time read, flush, pause); lock guards
 * the command table and flags. Order: io, then lock. The poller sleeps on
 * wake holding neither. Events are queued during an iteration and delivered
 * after io is released, on the poller thread, with no lock held.
 *
 * Time. API ticks (what callers pass and get back) and hardware ticks (Tick's
 * counter) differ by the device's soft offset (Task 3). Every timestamp
 * crosses through datax_devtime_to_hw / _from_hw, identity until then. The
 * table keeps both: requested_ticks echoes the API value the caller queued.
 */
#ifndef DATAX_SCHEDULER_H
#define DATAX_SCHEDULER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <datax/datax.h>

#include "internal.h"

#define DATAX_SCHED_DETAIL_LEN 128
#define DATAX_SCHED_DEFAULT_POLL_US 1000u
/* Accesses in one timed completion drain: seven completion_* reads,
 * completion_ack, and the current_time_ticks read. */
#define DATAX_SCHED_DRAIN_ACCESSES 9u
#define DATAX_SCHED_DRAIN_SAMPLES 5u

/* ---- hooks for later tasks (Task 3: time offset; Task 4: tunes) ---- */

/* API <-> hardware device time for dev. Identity until Task 3 adds the soft
 * offset (time.c). Every queue_* timestamp goes to_hw before it is packed;
 * every completion and event tick comes back from_hw. */
unsigned long long datax_devtime_to_hw(const struct datax_device *dev,
				       unsigned long long api_ticks);
unsigned long long datax_devtime_from_hw(const struct datax_device *dev,
					 unsigned long long hw_ticks);

/* Terminal result of one command, as the poller is about to publish it. */
struct datax_sched_result {
	enum datax_completion_status status;
	unsigned long long requested_ticks;  /* API ticks (as queued) */
	unsigned long long executed_ticks;   /* API ticks; 0 when it never ran */
	uint32_t tick_status;                /* Tick status code; UINT32_MAX: host-side */
	bool discarded;                      /* a rejected commit discarded it (never
					      * written; nothing is published) */
	char detail[DATAX_SCHED_DETAIL_LEN]; /* "" for OK */
};

/*
 * Per-command completion hook. Called once per terminal outcome (a drained
 * record, host-late, a failed write, flush, a late-PPS-edge drop, or the
 * batch being discarded by a rejected commit, with r->discarded set and
 * nothing published) before the result is published,
 * with the I/O lock held and the state lock not held, on the thread doing
 * the Tick I/O (the poller, or flush's caller). It may change r->status and
 * r->detail, do Tick reads/writes through the seam (io is already held),
 * call datax_device_get_time, do non-Tick I/O (phy attributes) and
 * datax_sched_emit, and datax_internal_sched_pause (it only sets the flag).
 * It must not block, and must not call wait, commit or flush (each returns
 * -EDEADLK while this thread holds io) or destroy (a no-op while this
 * thread holds io or is the poller; never call it from a hook), so
 * anything it datax_sched_enqueue()s waits in the builder for the caller's
 * next commit. Destroy also finishes every outstanding command FLUSHED
 * through this hook, on the destroying thread.
 * For an unreported command it fires only on host-late, write failure,
 * flush or discard (r is then informational; nothing is published).
 * Lock order with the FHM pipeline (fhm.c): io, then fhm->lock, then
 * s->lock, then ctx->lock.
 */
typedef void (*datax_sched_complete_fn)(struct datax_sched *s, long seq,
					struct datax_sched_result *r, void *user);

/* Per-iteration hook: after the drain and the feed, with the latest
 * hardware time observed. Same rules as datax_sched_complete_fn (must not
 * block; no wait, commit, flush or destroy). */
typedef void (*datax_sched_poll_fn)(struct datax_sched *s, unsigned long long hw_now,
				    void *user);

struct datax_sched_cmd_spec {
	datax_time when;              /* API ticks */
	uint16_t command_id, target_id;
	uint32_t args[4];
	bool report;                  /* REPORT_COMPLETION; spacing rule applies */
	/* Reported, but the result is not kept: after the hook it leaves the
	 * table instead of becoming DONE (nobody can wait on it: timed
	 * set_frequency's tune on the device's own scheduler). */
	bool consume;
	datax_sched_complete_fn on_complete;  /* may be NULL */
	void *user;
};

/* Append a raw command to the builder. No reserved-line check (queue_tune
 * drives the reserved line through this). Returns the seq or -errno, with
 * last-error. */
long datax_sched_enqueue(struct datax_sched *s, const struct datax_sched_cmd_spec *spec);

/* Install the per-iteration hook (NULL fn removes it). 0 or -EINVAL. */
int datax_sched_set_poll_hook(struct datax_sched *s, datax_sched_poll_fn fn, void *user);

/* Queue an event for delivery on the poller thread after the current (or
 * next) iteration. ticks are hardware ticks (0 unknown), converted from_hw
 * at delivery. Thread-safe; never blocks on I/O. */
void datax_sched_emit(struct datax_sched *s, enum datax_event_type type,
		      unsigned long long hw_ticks, long seq, const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));

/* Deliver one event through the context callback now, on this thread. */
void datax_event_fire(struct datax_context *ctx, const struct datax_event *evt);

/* The device's scheduler, or NULL. Only valid while the caller knows no
 * other thread can destroy it; to use it across blocking (taking its io
 * lock), hold a reference: _get returns it with a reference (or NULL),
 * _put drops it. Destroy waits for every reference before freeing. */
struct datax_sched *datax_device_sched(const struct datax_device *dev);
struct datax_sched *datax_device_sched_get(const struct datax_device *dev);
void datax_sched_put(struct datax_sched *s);

/*
 * The io lock, with per-thread ownership tracking. Every holder pushes a
 * stack node (h) so the library can tell whether the calling thread already
 * holds s->io: then wait/commit/flush/pause return -EDEADLK instead of
 * deadlocking, and get_time skips the lock. Unlock in LIFO order.
 */
struct datax_io_hold {
	const struct datax_sched *s;
	struct datax_io_hold *prev;
};
void datax_sched_io_lock(struct datax_sched *s, struct datax_io_hold *h);
void datax_sched_io_unlock(struct datax_sched *s, struct datax_io_hold *h);
bool datax_sched_holds_io(const struct datax_sched *s);

/* ---- internals shared by sched.c and poller.c ---- */

enum sched_state { CMD_BUILD, CMD_PENDING, CMD_WRITTEN, CMD_DONE };

struct sched_cmd {
	long seq;
	enum sched_state state;
	uint64_t order;                    /* insertion order: ties at equal ticks */
	unsigned long long api_ts, hw_ts;
	uint16_t command_id, target_id;
	uint32_t args[4];
	bool report;
	bool consume;                      /* see datax_sched_cmd_spec */
	bool lagged;                       /* a drain came after hw_ts: may be held */
	datax_sched_complete_fn on_complete;
	void *user;
	/* CMD_DONE */
	uint64_t done_order;
	struct datax_sched_result res;
};

#define SCHED_EVQ_LEN 32

struct sched_event {
	enum datax_event_type type;
	unsigned long long hw_ticks;
	long seq;
	char detail[DATAX_SCHED_DETAIL_LEN];
};

struct datax_sched {
	struct datax_device *dev;
	struct datax_context *ctx;

	pthread_mutex_t io;
	pthread_mutex_t lock;
	pthread_cond_t wake;               /* poller: commit, unpause, stop, emit */
	pthread_cond_t done;               /* waiters: a result was published;
					    * destroy: the last waiter left */
	pthread_cond_t unref;              /* with ctx->lock: refs reached 0 */
	unsigned int nwaiters;             /* threads inside wait() (s->lock) */
	unsigned int refs;                 /* datax_device_sched_get refs (ctx->lock) */
	pthread_t thread;
	bool thread_started;
	bool stop, paused, kick;
	unsigned int poll_us;              /* poller period (internal option) */
	unsigned long idle_iters;

	/* limits, fixed at create */
	unsigned long long tick_hz;
	unsigned int depth;
	unsigned long long guard_ticks;
	unsigned long long spacing_us, spacing_ticks;
	unsigned long long access_ticks;   /* one Tick access, from the drain timing */
	unsigned int reserved_mask;        /* GPIO lines only queue_tune may drive */

	/* command table */
	struct sched_cmd *cmds;
	size_t ncmds, capcmds;
	uint64_t next_order, next_done;

	/* poller observations (hardware ticks) */
	unsigned long long last_hw;        /* latest device time read */
	unsigned long long last_hw_mono;   /* host monotonic ns of that read */
	unsigned long long late_seen, overflow_seen;
	int pps_seen_state;                /* devtime.c watch: 0 unknown, 1 absent, 2 present */
	unsigned long host_late;           /* commands the feeder could not write in time */

	datax_sched_poll_fn poll_fn;
	void *poll_user;

	struct sched_event evq[SCHED_EVQ_LEN];
	unsigned int nevq;
	unsigned long evq_lost;            /* dropped since the last delivery */
	enum datax_event_type evq_lost_type; /* type of the last dropped one */
};

/* sched.c, called with s->lock held */
struct sched_cmd *datax_sched_find(struct datax_sched *s, long seq);
void datax_sched_remove(struct datax_sched *s, struct sched_cmd *c);
/*
 * Terminal outcome res for c: run c's on_complete hook (dropping s->lock
 * around it), then publish: DONE for a reported command (waking waiters),
 * removal for an unreported one; a LATE result queues
 * DATAX_EVT_LATE_COMMAND. c is invalid afterwards (the table may move).
 * s->lock held on entry and on return; the caller holds s->io.
 */
void datax_sched_finish(struct datax_sched *s, struct sched_cmd *c,
			struct datax_sched_result *res);
/* Queue an event; s->lock held. */
void datax_sched_emit_locked(struct datax_sched *s, enum datax_event_type type,
			     unsigned long long hw_ticks, long seq, const char *fmt,
			     ...) __attribute__((format(printf, 5, 6)));
/* Map a Tick record to a result (status and detail). */
void datax_sched_map_status(uint32_t tick_status, struct datax_sched_result *r);
/* Pack c as Tick's CSV of twelve 32-bit words. */
void datax_sched_format_desc(const struct sched_cmd *c, char *buf, size_t len);

/* devtime.c: the PPS watch, run by the poller every IDLE_COUNTER_EVERY
 * iterations (and on every active one) with io held and s->lock not held. */
void datax_devtime_poll(struct datax_sched *s);

/* devtime.c: if a timed-out hard alignment is still armed and its edge has
 * landed, adopt it (clear the soft offset, drop written commands, event).
 * s may be NULL (no scheduler); otherwise io must be held. */
/* Returns true when it adopted an edge (the queue was flushed and PENDING
 * commands re-based): a caller holding cached queue state must re-read it. */
bool datax_devtime_check_late_edge(struct datax_device *dev, struct datax_sched *s);

/* poller.c */
/* At create, before the poller starts: time DATAX_SCHED_DRAIN_SAMPLES
 * completion drains in device ticks and set access_ticks, spacing_ticks
 * and spacing_us. 0 or a Tick error. */
int datax_poller_measure(struct datax_sched *s);
/* Read the device time into last_hw/last_hw_mono (io held). */
int datax_poller_read_time(struct datax_sched *s);
int datax_poller_start(struct datax_sched *s);
void datax_poller_stop(struct datax_sched *s);        /* join; idempotent */
bool datax_poller_is_self(const struct datax_sched *s);
/* One drain pass (io held): every pending record, until the slot reads
 * empty. Returns 0 or the first Tick error. */
int datax_poller_drain(struct datax_sched *s);
/* Ack the completion slot until it reads empty, discarding records (io
 * held; destroy, after flush_queue). 0 or the first Tick error. */
int datax_poller_clear_slot(struct datax_sched *s);
/* Deliver queued events (no lock held). */
void datax_poller_deliver(struct datax_sched *s);
/* Monotonic host time in nanoseconds. */
unsigned long long datax_mono_ns(void);

#endif
