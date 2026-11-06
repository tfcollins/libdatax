// SPDX-License-Identifier: MIT
/*
 * Fake Tick and fake Talise FHM (see fake_tick.h).
 *
 * Every behaviour cites what it mirrors, at the pins.json Tick revision:
 *   RTL  hdl/library/axi_timed_command_scheduler/axi_timed_command_scheduler.v
 *   DRV  kernel/axi_timed_command_scheduler.c
 * and, for FHM, hopcap docs/explanation/fhm-pipeline.md and
 * src/hopcap/{talise,plan}.py. The golden traces in tests/traces (from Tick's
 * hdl_model) pin the queue, completion and PPS behaviour; test_fake_tick
 * replays them.
 *
 * Time is event-driven: advancing steps to each due timestamp in turn, so a
 * command executes on the tick it comes due, as the RTL dispatches it.
 * Simplification: the RTL dispatches one command per clock, so commands
 * sharing a timestamp run on consecutive cycles; here they run on the same
 * tick. Their records are identical (a non-repeat records executed =
 * requested either way).
 *
 * Simplification: the RTL dispatches on the cycle after a register write
 * (a push, an ack, a flush), so a command executed late (LATE_EXECUTE_NOW
 * on submit, a late repeat, or a reported head released by the ack of a
 * held completion slot) runs and is stamped one cycle after that write;
 * here it is stamped with the tick of the write itself. Tests must never
 * assert the exact executed ticks of a late-executed command against
 * hardware, only against this fake (and on hardware, only that it is late).
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_tick.h"

/* Wire constants (Tick UAPI, RTL localparams lines 200-259). */
#define DESC_VERSION   1u
#define DESC_SIZE      48u
#define F_REPORT       0x1u
#define F_LATE_DROP    0x2u
#define F_LATE_NOW     0x4u
#define F_REPEAT       0x8u
#define F_SUPPORTED    0xfu
#define CMD_GPIO       0x0001u
#define CMD_MARKER     0x0002u
#define CMD_GATE       0x0003u
#define CMD_TDD        0x0004u
#define CMD_CAPTURE    0x0006u
#define CMD_TRANSMIT   0x0007u
#define CMD_SPI        0x0008u
#define ST_EXECUTED        0u
#define ST_LATE_EXECUTED   1u
#define ST_LATE_DROPPED    2u
#define ST_OVERFLOW        3u
#define ST_UNSUPPORTED     4u
#define ST_BAD_DESCRIPTOR  5u
#define ST_REPEAT_OVERFLOW 6u
#define CAP_ARM_RELEASE    0x20u
#define CAP_SPI            0x100u
#define STREAM_TAG_MAGIC   0x4b434954u   /* RTL line 257 */
#define STREAM_TAG_BYTES   32u           /* RTL line 259 */
#define MARKER_COUNT       8u            /* RTL parameter defaults */
#define STREAM_GATE_COUNT  8u

#define TRON_TICK_HZ 100000000ULL
#define TRON_DEPTH   16u
#define TRON_CAPS    0x9fu

struct q_entry {
	uint16_t cmd, target;
	uint64_t ts;
	uint32_t flags, seq;
	uint32_t a[4];
	uint32_t rep_idx;          /* RTL q_repeat_index */
	bool late_on_submit;       /* RTL q_late_on_submit */
};

struct attr_count {
	char name[48];
	unsigned long r, w;
};

struct fake_tick {
	pthread_mutex_t lock;
	uint64_t hz;
	unsigned int depth;
	uint32_t caps;

	uint64_t now;
	struct q_entry q[FAKE_TICK_MAX_DEPTH];
	unsigned int level;
	bool comp_valid;
	uint32_t comp[8];          /* RTL completion_words */
	uint32_t late, overflow;
	bool pps_seen, pps_arm, aligned;
	uint64_t epoch;
	uint32_t gpio, marker, gate;

	/* edge capture (RTL capture_tag_*) */
	uint64_t cap_ticks;
	uint32_t cap_seq, cap_count;
	uint16_t cap_target, cap_cmd;
	uint8_t cap_gate;
	bool cap_unread, cap_overrun;
	uint32_t tag_enable;

	fake_tick_gpio_hook hook;
	void *hook_user;
	uint64_t access_cost;
	bool inj_set;
	char inj_attr[48];         /* "" matches any attribute */
	int inj_err;

	struct fake_tick_exec *log;
	size_t nlog, caplog;
	struct attr_count *counts;
	size_t ncounts;
};

/* ---- small helpers ---------------------------------------------------------- */

static uint32_t sat_inc(uint32_t v)   /* RTL saturating_increment */
{
	return v == UINT32_MAX ? v : v + 1;
}

/* kstrtobool (DRV arm_time_next_pps/flush_queue/completion_ack/edge_capture_ack):
 * y/Y/1/on true, n/N/0/off false. 0 or -EINVAL. */
static int parse_bool(const char *s, bool *out)
{
	if (!s || !s[0])
		return -EINVAL;
	switch (s[0]) {
	case 'y': case 'Y': case '1':
		*out = true;
		return 0;
	case 'n': case 'N': case '0':
		*out = false;
		return 0;
	case 'o': case 'O':
		if (s[1] == 'n' || s[1] == 'N') {
			*out = true;
			return 0;
		}
		if (s[1] == 'f' || s[1] == 'F') {
			*out = false;
			return 0;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

/* kstrtou64(buf, 0, ..): base 0, one optional trailing newline. */
static int parse_u64(const char *s, size_t n, uint64_t *out)
{
	char tmp[32];
	char *end;
	unsigned long long v;

	if (n && s[n - 1] == '\n')
		n--;
	if (!n || n >= sizeof(tmp) || s[0] == '-' || s[0] == ' ')
		return -EINVAL;
	memcpy(tmp, s, n);
	tmp[n] = '\0';
	errno = 0;
	v = strtoull(tmp, &end, 0);
	if (errno || *end)
		return -EINVAL;
	*out = v;
	return 0;
}

static int emit(char *buf, size_t len, const char *fmt, unsigned long long v)
{
	int n = snprintf(buf, len, fmt, v);

	if (n < 0)
		return -EIO;
	return (size_t)n >= len ? (int)(len - 1) : n;
}

static struct attr_count *count_slot(struct fake_tick *f, const char *attr)
{
	struct attr_count *c;

	for (size_t i = 0; i < f->ncounts; i++)
		if (!strcmp(f->counts[i].name, attr))
			return &f->counts[i];
	c = realloc(f->counts, (f->ncounts + 1) * sizeof(*c));
	if (!c)
		return NULL;
	f->counts = c;
	c = &f->counts[f->ncounts++];
	memset(c, 0, sizeof(*c));
	snprintf(c->name, sizeof(c->name), "%s", attr);
	return c;
}

static void log_exec(struct fake_tick *f, uint32_t seq, uint16_t cmd, uint16_t target,
		     uint64_t req, uint64_t exec, uint32_t status, uint32_t detail,
		     bool reported)
{
	if (f->nlog == f->caplog) {
		size_t cap = f->caplog ? 2 * f->caplog : 64;
		struct fake_tick_exec *l = realloc(f->log, cap * sizeof(*l));

		if (!l)
			return;
		f->log = l;
		f->caplog = cap;
	}
	f->log[f->nlog++] = (struct fake_tick_exec){
		.sequence_id = seq, .command_id = cmd, .target_id = target,
		.requested_ticks = req, .executed_ticks = exec, .status = status,
		.detail = detail, .reported = reported,
	};
}

/* Fill the completion slot (RTL completion_words[0..7] <= ...;
 * completion_valid <= 1). Callers check the slot is free first. */
static void record(struct fake_tick *f, uint32_t seq, uint16_t cmd, uint16_t target,
		   uint64_t req, uint64_t exec, uint32_t status, uint32_t detail)
{
	f->comp[0] = seq;
	f->comp[1] = (uint32_t)cmd | ((uint32_t)target << 16);
	f->comp[2] = (uint32_t)req;
	f->comp[3] = (uint32_t)(req >> 32);
	f->comp[4] = (uint32_t)exec;
	f->comp[5] = (uint32_t)(exec >> 32);
	f->comp[6] = status;
	f->comp[7] = detail;
	f->comp_valid = true;
}

/* A rejection or drop that wants the slot: record it, or lose it and count
 * an overflow when the slot is full (RTL lines 696-708, 710-722, 849-861). */
static void report_or_lose(struct fake_tick *f, const struct q_entry *e, uint32_t status)
{
	bool lost = f->comp_valid;

	if (lost)
		f->overflow = sat_inc(f->overflow);
	else
		record(f, e->seq, e->cmd, e->target, e->ts, 0, status, 0);
	log_exec(f, e->seq, e->cmd, e->target, e->ts, 0, status, 0, !lost);
}

static void set_gpio(struct fake_tick *f, uint32_t v)
{
	uint32_t old = f->gpio;

	f->gpio = v;
	if (old != v && f->hook)
		f->hook(f->hook_user, old, v, f->now);
}

/* Capture-tag latch: gate and arm commands only (RTL lines 790-801, 813-826,
 * 958-969, 983-995); GPIO never latches. */
static void latch_tag(struct fake_tick *f, const struct q_entry *e, uint64_t at)
{
	f->cap_ticks = at;
	f->cap_seq = e->seq;
	f->cap_target = e->target;
	f->cap_cmd = e->cmd;
	f->cap_gate = (uint8_t)(e->a[0] & 0xf);
	f->cap_count = (f->cap_count + 1) & 0xffffffu;   /* 24-bit counter */
	f->cap_overrun = f->cap_overrun || f->cap_unread;
	f->cap_unread = true;
}

/* Target side effects (RTL dispatch case, lines 947-1000; late execute-now
 * case, lines 779-835). at is the tick the effect is recorded with. */
static void apply(struct fake_tick *f, const struct q_entry *e, uint64_t at)
{
	switch (e->cmd) {
	case CMD_GPIO:
		set_gpio(f, (f->gpio & ~e->a[0]) | (e->a[1] & e->a[0]));
		break;
	case CMD_MARKER:
		/* The RTL pulse retires after its width; like hdl_model the fake
		 * keeps the bit set (marker_out is observation only). */
		f->marker |= 1u << (e->a[0] % MARKER_COUNT);
		break;
	case CMD_GATE:
		if (e->a[1] & 1) {
			f->gate |= 1u << (e->a[0] % STREAM_GATE_COUNT);
			latch_tag(f, e, at);
		} else {
			f->gate &= ~(1u << (e->a[0] % STREAM_GATE_COUNT));
		}
		break;
	case CMD_CAPTURE:
	case CMD_TRANSMIT:
		/* One-shot pulse; like hdl_model it only ever sets the bit. */
		f->gate |= 1u << (e->a[0] % STREAM_GATE_COUNT);
		latch_tag(f, e, at);
		break;
	default:   /* TDD sync, SPI: no state the attribute surface shows */
		break;
	}
}

/* Insert after every entry with an equal timestamp (RTL insert loop, lines
 * 727-733 and 880-886: first index whose timestamp is greater). */
static void insert(struct fake_tick *f, const struct q_entry *e)
{
	unsigned int i = 0;

	while (i < f->level && f->q[i].ts <= e->ts)
		i++;
	memmove(&f->q[i + 1], &f->q[i], (f->level - i) * sizeof(f->q[0]));
	f->q[i] = *e;
	f->level++;
}

/* Dispatch the queue head (RTL lines 941-1090). false: held, because the head
 * would produce a completion while the slot is full
 * (due_command_blocked_by_completion, line 351). */
static bool dispatch_head(struct fake_tick *f)
{
	struct q_entry e = f->q[0];
	bool repeat = e.flags & F_REPEAT;
	bool rlate = repeat && e.ts < f->now;                       /* q0_repeat_late */
	bool late = e.late_on_submit || rlate;
	bool drop = rlate && !(e.flags & F_LATE_NOW);
	bool cont = repeat && (e.a[2] == 0 || e.rep_idx + 1 < e.a[2]); /* q0_repeat_continues */
	uint64_t base = rlate ? f->now : e.ts;                       /* q0_repeat_base */
	bool rovf = cont && base > UINT64_MAX - e.a[3];              /* q0_repeat_overflow */
	bool produces = (e.flags & F_REPORT) || drop || rovf;
	uint64_t exec = drop ? 0 : (late ? f->now : e.ts);           /* words[4/5], 1023-1030 */
	uint32_t status;

	if (f->comp_valid && produces)
		return false;
	if (!drop)
		apply(f, &e, exec);
	if (rlate && !e.late_on_submit)                              /* line 1008 */
		f->late = sat_inc(f->late);
	status = rovf ? ST_REPEAT_OVERFLOW : drop ? ST_LATE_DROPPED :
		 late ? ST_LATE_EXECUTED : ST_EXECUTED;               /* lines 1016-1020 */
	if (produces)
		record(f, e.seq, e.cmd, e.target, e.ts, exec, status, e.rep_idx);
	log_exec(f, e.seq, e.cmd, e.target, e.ts, exec, status, e.rep_idx, produces);
	if (rovf)                                                    /* line 1034 */
		f->overflow = sat_inc(f->overflow);
	f->level--;
	memmove(&f->q[0], &f->q[1], f->level * sizeof(f->q[0]));
	if (cont && !rovf) {                                         /* lines 1037-1074 */
		e.ts = base + e.a[3];
		e.rep_idx++;
		e.late_on_submit = false;
		insert(f, &e);
	}
	return true;
}

/* Dispatch everything due now, in order, until the head is held. */
static void settle(struct fake_tick *f)
{
	while (f->level && f->q[0].ts <= f->now)
		if (!dispatch_head(f))
			break;
}

static void advance_locked(struct fake_tick *f, uint64_t ticks)
{
	uint64_t target = ticks > UINT64_MAX - f->now ? UINT64_MAX : f->now + ticks;

	settle(f);
	while (f->level && f->q[0].ts > f->now && f->q[0].ts <= target) {
		f->now = f->q[0].ts;
		settle(f);
	}
	f->now = target;
	settle(f);
}

static bool cmd_target_supported(const struct fake_tick *f, uint16_t cmd, uint16_t t)
{
	/* desc_cmd_target_supported, RTL lines 359-379 */
	switch (cmd) {
	case CMD_GPIO:     return t <= 0x00ff;
	case CMD_MARKER:   return t >= 0x0100 && t <= 0x01ff;
	case CMD_GATE:     return t >= 0x0200 && t <= 0x02ff;
	case CMD_TDD:      return t >= 0x0300 && t <= 0x03ff;
	case CMD_CAPTURE:  return (f->caps & CAP_ARM_RELEASE) && t >= 0x0500 && t <= 0x05ff;
	case CMD_TRANSMIT: return (f->caps & CAP_ARM_RELEASE) && t >= 0x0600 && t <= 0x06ff;
	case CMD_SPI:      return (f->caps & CAP_SPI) && t >= 0x0700 && t <= 0x07ff;
	default:           return false;
	}
}

/* REG_DESC_PUSH (RTL lines 676-890). */
static void push(struct fake_tick *f, const uint32_t w[12])
{
	struct q_entry e = {
		.cmd = (uint16_t)(w[1] & 0xffff), .target = (uint16_t)(w[1] >> 16),
		.ts = (uint64_t)w[2] | ((uint64_t)w[3] << 32),
		.flags = w[4], .seq = w[5], .a = { w[6], w[7], w[8], w[9] },
	};
	uint32_t spi = e.a[1];
	bool bad = (w[0] & 0xffff) != DESC_VERSION || (w[0] >> 16) != DESC_SIZE ||
		   (e.flags & ~F_SUPPORTED) ||
		   (e.cmd == CMD_SPI && ((spi & 0xff) >= 8 || ((spi >> 8) & 0xff) == 0 ||
					 ((spi >> 8) & 0xff) > 32 || (spi >> 16) == 0)) ||
		   ((e.flags & F_REPEAT) && e.a[3] == 0) ||
		   ((e.flags & F_LATE_DROP) && (e.flags & F_LATE_NOW));    /* lines 678-684 */
	/* late_report_completion_blocked (line 356), desc_repeat_needs_slot (349) */
	bool report_blocked = f->comp_valid && (e.flags & F_REPORT) && (e.flags & F_LATE_NOW);
	bool needs_slot = (e.flags & F_REPEAT) && (e.a[2] == 0 || e.a[2] > 1);

	if (bad) {
		report_or_lose(f, &e, ST_BAD_DESCRIPTOR);
	} else if (!cmd_target_supported(f, e.cmd, e.target)) {
		report_or_lose(f, &e, ST_UNSUPPORTED);
	} else if (e.ts < f->now) {                                  /* late on submit, 723 */
		f->late = sat_inc(f->late);
		if ((report_blocked || needs_slot) && f->level != f->depth) {
			e.late_on_submit = true;                             /* lines 724-762 */
			insert(f, &e);
		} else if (report_blocked || needs_slot) {
			/* lines 763-777: no slot to wait in; overflow counts either way */
			bool rep = !f->comp_valid;

			f->overflow = sat_inc(f->overflow);
			if (rep)
				record(f, e.seq, e.cmd, e.target, e.ts, 0, ST_OVERFLOW, 0);
			log_exec(f, e.seq, e.cmd, e.target, e.ts, 0, ST_OVERFLOW, 0, rep);
		} else if (e.flags & F_LATE_NOW) {                       /* lines 778-846 */
			apply(f, &e, f->now);
			if (e.flags & F_REPORT)
				record(f, e.seq, e.cmd, e.target, e.ts, f->now, ST_LATE_EXECUTED, 0);
			log_exec(f, e.seq, e.cmd, e.target, e.ts, f->now, ST_LATE_EXECUTED, 0,
				 e.flags & F_REPORT);
		} else {                                                 /* lines 847-861 */
			report_or_lose(f, &e, ST_LATE_DROPPED);
		}
	} else if (f->level == f->depth) {                          /* lines 863-876 */
		bool rep = !f->comp_valid;

		f->overflow = sat_inc(f->overflow);
		if (rep)
			record(f, e.seq, e.cmd, e.target, e.ts, 0, ST_OVERFLOW, 0);
		log_exec(f, e.seq, e.cmd, e.target, e.ts, 0, ST_OVERFLOW, 0, rep);
	} else {
		insert(f, &e);                                           /* lines 877-912 */
	}
}

/* DRV command_descriptor_store: strsep on " \t\r\n,", empty tokens skipped,
 * kstrtou32(token, 0), exactly TICK_DESC_WORDS (12) words. */
static int parse_descriptor(const char *val, uint32_t w[12])
{
	const char *p = val;
	int n = 0;

	while (*p) {
		const char *s;
		uint64_t v;

		while (*p && strchr(" \t\r\n,", *p))
			p++;
		if (!*p)
			break;
		s = p;
		while (*p && !strchr(" \t\r\n,", *p))
			p++;
		if (n >= 12 || parse_u64(s, (size_t)(p - s), &v) || v > UINT32_MAX)
			return -EINVAL;
		w[n++] = (uint32_t)v;
	}
	return n == 12 ? 0 : -EINVAL;
}

/* ---- attribute surface (DRV tick_scheduler_attrs) --------------------------- */

enum { A_RO = 1, A_WO = 2, A_RW = 3 };

static const struct {
	const char *name;
	int mode;
} attrs[] = {
	{ "current_time_ticks", A_RO }, { "time_tick_hz", A_RO },
	{ "queue_depth", A_RO }, { "queue_level", A_RO },
	{ "late_count", A_RO }, { "overflow_count", A_RO },
	{ "pps_present", A_RO }, { "time_aligned", A_RO },
	{ "time_epoch_ticks", A_RW }, { "arm_time_next_pps", A_WO },
	{ "flush_queue", A_WO }, { "capability_bitmap", A_RO },
	{ "command_descriptor", A_WO },
	{ "completion_sequence_id", A_RO }, { "completion_status", A_RO },
	{ "completion_command_id", A_RO }, { "completion_target_id", A_RO },
	{ "completion_requested_timestamp_ticks", A_RO },
	{ "completion_executed_timestamp_ticks", A_RO },
	{ "completion_detail", A_RO }, { "completion_ack", A_WO },
	{ "edge_capture_valid", A_RO }, { "edge_capture_overrun", A_RO },
	{ "edge_capture_count", A_RO }, { "edge_capture_ticks", A_RO },
	{ "edge_capture_target_id", A_RO }, { "edge_capture_command_id", A_RO },
	{ "edge_capture_ack", A_WO }, { "capture_tag_enable", A_RW },
	{ "capture_tag_header_bytes", A_RO }, { "capture_tag_magic", A_RO },
};

static int attr_mode(const char *name)
{
	for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
		if (!strcmp(attrs[i].name, name))
			return attrs[i].mode;
	return 0;
}

/* RTL REG_COMP_x reads completion_valid ? word : 0 (lines 624-631). */
static uint32_t comp_word(const struct fake_tick *f, int i)
{
	return f->comp_valid ? f->comp[i] : 0;
}

static int read_locked(struct fake_tick *f, const char *a, char *buf, size_t len)
{
	const char *u = "%llu\n";

	if (!strcmp(a, "current_time_ticks"))
		return emit(buf, len, u, f->now);
	if (!strcmp(a, "time_tick_hz"))
		return emit(buf, len, u, f->hz);
	if (!strcmp(a, "queue_depth"))
		return emit(buf, len, u, f->depth);
	if (!strcmp(a, "queue_level"))
		return emit(buf, len, u, f->level);
	if (!strcmp(a, "late_count"))
		return emit(buf, len, u, f->late);
	if (!strcmp(a, "overflow_count"))
		return emit(buf, len, u, f->overflow);
	if (!strcmp(a, "pps_present"))      /* PPS_STATUS bit 0 = pps_seen, line 608 */
		return emit(buf, len, u, f->pps_seen);
	if (!strcmp(a, "time_aligned"))
		return emit(buf, len, u, f->aligned);
	if (!strcmp(a, "time_epoch_ticks"))
		return emit(buf, len, u, f->epoch);
	if (!strcmp(a, "capability_bitmap"))
		return emit(buf, len, "0x%08llx\n", f->caps);
	if (!strcmp(a, "completion_sequence_id"))
		return emit(buf, len, u, comp_word(f, 0));
	if (!strcmp(a, "completion_status"))
		return emit(buf, len, u, comp_word(f, 6));
	if (!strcmp(a, "completion_command_id"))
		return emit(buf, len, u, comp_word(f, 1) & 0xffff);
	if (!strcmp(a, "completion_target_id"))
		return emit(buf, len, u, comp_word(f, 1) >> 16);
	if (!strcmp(a, "completion_requested_timestamp_ticks"))
		return emit(buf, len, u, comp_word(f, 2) | ((unsigned long long)comp_word(f, 3) << 32));
	if (!strcmp(a, "completion_executed_timestamp_ticks"))
		return emit(buf, len, u, comp_word(f, 4) | ((unsigned long long)comp_word(f, 5) << 32));
	if (!strcmp(a, "completion_detail"))
		return emit(buf, len, u, comp_word(f, 7));
	/* EDGE_CAPTURE_STATUS = {count, gate, 2'd0, overrun, unread}, line 634 */
	if (!strcmp(a, "edge_capture_valid"))
		return emit(buf, len, u, f->cap_unread);
	if (!strcmp(a, "edge_capture_overrun"))
		return emit(buf, len, u, f->cap_overrun);
	if (!strcmp(a, "edge_capture_count"))
		return emit(buf, len, u, f->cap_count);
	if (!strcmp(a, "edge_capture_ticks"))
		return emit(buf, len, u, f->cap_ticks);
	if (!strcmp(a, "edge_capture_target_id"))
		return emit(buf, len, u, f->cap_target);
	if (!strcmp(a, "edge_capture_command_id"))
		return emit(buf, len, u, f->cap_cmd);
	if (!strcmp(a, "capture_tag_enable"))
		return emit(buf, len, u, f->tag_enable);
	if (!strcmp(a, "capture_tag_header_bytes"))
		return emit(buf, len, u, STREAM_TAG_BYTES);
	if (!strcmp(a, "capture_tag_magic"))
		return emit(buf, len, "0x%08llx\n", STREAM_TAG_MAGIC);
	return -ENOENT;
}

static int write_locked(struct fake_tick *f, const char *a, const char *val)
{
	uint32_t w[12];
	uint64_t v;
	bool b;
	int ret;

	if (!strcmp(a, "command_descriptor")) {
		ret = parse_descriptor(val, w);
		if (ret)
			return ret;
		push(f, w);
		settle(f);
		return 0;
	}
	if (!strcmp(a, "time_epoch_ticks")) {        /* DRV: kstrtou64(buf, 0) */
		ret = parse_u64(val, strlen(val), &v);
		if (ret)
			return ret;
		f->epoch = v;
		return 0;
	}
	if (!strcmp(a, "capture_tag_enable")) {      /* DRV: kstrtou32, > 0xffff -EINVAL */
		ret = parse_u64(val, strlen(val), &v);
		if (ret || v > 0xffff)
			return -EINVAL;
		/* stream_tag_en <= wdata[STREAM_GATE_COUNT-1:0] (RTL REG_STREAM_TAG_CONTROL) */
		f->tag_enable = (uint32_t)v & ((1u << STREAM_GATE_COUNT) - 1);
		return 0;
	}
	ret = parse_bool(val, &b);
	if (ret)
		return ret;
	if (!b)
		return 0;
	if (!strcmp(a, "arm_time_next_pps")) {
		/* DRV writes CONTROL = ENABLE | ARM; RTL lines 666-669 */
		f->pps_arm = true;
		f->aligned = false;
	} else if (!strcmp(a, "flush_queue")) {
		f->level = 0;                            /* RTL line 659: queue_level <= 0 */
	} else if (!strcmp(a, "completion_ack")) {
		f->comp_valid = false;                   /* REG_COMP_POP, line 917 */
	} else if (!strcmp(a, "edge_capture_ack")) {
		f->cap_unread = false;                   /* write-1-to-clear, lines 922-926 */
		f->cap_overrun = false;
	} else {
		return -ENOENT;
	}
	settle(f);   /* the RTL dispatches on the cycle after the write */
	return 0;
}

/* Common entry: access cost, fault injection, direction, count. */
static int tick_access(struct fake_tick *f, const char *attr, bool write, char *buf, size_t len,
		  const char *val)
{
	struct attr_count *c;
	int mode, ret;

	if (!f || !attr || (write ? !val : (!buf || !len)))
		return -EINVAL;
	pthread_mutex_lock(&f->lock);
	if (f->access_cost)
		advance_locked(f, f->access_cost);
	if (f->inj_set && (!f->inj_attr[0] || !strcmp(f->inj_attr, attr))) {
		f->inj_set = false;
		ret = f->inj_err;
		goto out;
	}
	mode = attr_mode(attr);
	if (!mode) {
		ret = -ENOENT;
		goto out;
	}
	if (!(mode & (write ? A_WO : A_RO))) {
		ret = -EACCES;
		goto out;
	}
	ret = write ? write_locked(f, attr, val) : read_locked(f, attr, buf, len);
	if (ret >= 0) {
		c = count_slot(f, attr);
		if (c) {
			if (write)
				c->w++;
			else
				c->r++;
		}
	}
out:
	pthread_mutex_unlock(&f->lock);
	return ret;
}

int fake_tick_read(void *h, const char *attr, char *buf, size_t len)
{
	return tick_access(h, attr, false, buf, len, NULL);
}

int fake_tick_write(void *h, const char *attr, const char *val)
{
	return tick_access(h, attr, true, NULL, 0, val);
}

const struct datax_tick_ops fake_tick_ops = { fake_tick_read, fake_tick_write };

/* ---- lifecycle and test controls ---------------------------------------------- */

struct fake_tick *fake_tick_new(const struct fake_tick_config *cfg)
{
	struct fake_tick *f;

	if (cfg && cfg->queue_depth > FAKE_TICK_MAX_DEPTH)
		return NULL;
	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	if (pthread_mutex_init(&f->lock, NULL)) {
		free(f);
		return NULL;
	}
	f->hz = cfg && cfg->tick_hz ? cfg->tick_hz : TRON_TICK_HZ;
	f->depth = cfg && cfg->queue_depth ? cfg->queue_depth : TRON_DEPTH;
	f->caps = cfg && cfg->capability_bitmap ? cfg->capability_bitmap : TRON_CAPS;
	return f;
}

void fake_tick_free(struct fake_tick *f)
{
	if (!f)
		return;
	pthread_mutex_destroy(&f->lock);
	free(f->log);
	free(f->counts);
	free(f);
}

void fake_tick_advance(struct fake_tick *f, uint64_t ticks)
{
	pthread_mutex_lock(&f->lock);
	advance_locked(f, ticks);
	pthread_mutex_unlock(&f->lock);
}

uint64_t fake_tick_now(struct fake_tick *f)
{
	uint64_t t;

	pthread_mutex_lock(&f->lock);
	t = f->now;
	pthread_mutex_unlock(&f->lock);
	return t;
}

void fake_tick_pps_edge(struct fake_tick *f)
{
	pthread_mutex_lock(&f->lock);
	/* RTL lines 567-574: a rising pps_in edge sets pps_seen; when armed it
	 * loads the epoch, disarms and sets time_aligned. pps_seen is sticky:
	 * only reset clears it. */
	f->pps_seen = true;
	if (f->pps_arm) {
		f->now = f->epoch;
		f->pps_arm = false;
		f->aligned = true;
	}
	settle(f);
	pthread_mutex_unlock(&f->lock);
}

#define GETTER(name, field)                                \
	uint32_t name(struct fake_tick *f)                 \
	{                                                  \
		uint32_t v;                                \
		pthread_mutex_lock(&f->lock);              \
		v = f->field;                              \
		pthread_mutex_unlock(&f->lock);            \
		return v;                                  \
	}
GETTER(fake_tick_gpio, gpio)
GETTER(fake_tick_marker, marker)
GETTER(fake_tick_stream_gate, gate)

void fake_tick_set_gpio_hook(struct fake_tick *f, fake_tick_gpio_hook hook, void *user)
{
	pthread_mutex_lock(&f->lock);
	f->hook = hook;
	f->hook_user = user;
	pthread_mutex_unlock(&f->lock);
}

size_t fake_tick_exec_log(struct fake_tick *f, size_t first, struct fake_tick_exec *out,
			  size_t max)
{
	size_t n;

	pthread_mutex_lock(&f->lock);
	n = f->nlog;
	for (size_t i = 0; out && i < max && first + i < n; i++)
		out[i] = f->log[first + i];
	pthread_mutex_unlock(&f->lock);
	return n;
}

void fake_tick_set_access_cost(struct fake_tick *f, uint64_t ticks)
{
	pthread_mutex_lock(&f->lock);
	f->access_cost = ticks;
	pthread_mutex_unlock(&f->lock);
}

void fake_tick_inject_error(struct fake_tick *f, const char *attr, int err)
{
	pthread_mutex_lock(&f->lock);
	f->inj_set = true;
	f->inj_err = err;
	snprintf(f->inj_attr, sizeof(f->inj_attr), "%s", attr ? attr : "");
	pthread_mutex_unlock(&f->lock);
}

unsigned long fake_tick_access_count(struct fake_tick *f, const char *attr, bool write)
{
	unsigned long n = 0;

	pthread_mutex_lock(&f->lock);
	for (size_t i = 0; i < f->ncounts; i++)
		if (!strcmp(f->counts[i].name, attr))
			n = write ? f->counts[i].w : f->counts[i].r;
	pthread_mutex_unlock(&f->lock);
	return n;
}

/* ---- fake Talise FHM ------------------------------------------------------------ */

#define ATTR_FHM_ENABLE "frequency_hopping_mode_enable"
#define ATTR_FHM_HOP    "frequency_hopping_mode"

struct fake_fhm {
	pthread_mutex_t lock;
	uint64_t init_hz;
	bool enabled;
	uint64_t staged_hz;   /* latest frequency_hopping_mode write */
	uint64_t armed_hz;    /* what the next edge commits */
	uint64_t pll_hz;      /* live RF PLL */
	unsigned int edges;
	bool corrupt;
	uint64_t corrupt_hz;
	uint64_t access_cost;
	struct fake_tick *tick;
};

struct fake_fhm *fake_fhm_new(uint64_t init_hz)
{
	struct fake_fhm *m = calloc(1, sizeof(*m));

	if (!m)
		return NULL;
	if (pthread_mutex_init(&m->lock, NULL)) {
		free(m);
		return NULL;
	}
	m->init_hz = init_hz ? init_hz : FAKE_FHM_INIT_HZ;
	m->staged_hz = m->armed_hz = m->pll_hz = m->init_hz;
	return m;
}

void fake_fhm_free(struct fake_fhm *m)
{
	if (!m)
		return;
	fake_fhm_attach(m, NULL);
	pthread_mutex_destroy(&m->lock);
	free(m);
}

/* GPIO hook, called with the Tick's lock held: only the FHM lock is taken. */
static void fhm_on_gpio(void *user, uint32_t old_gpio, uint32_t new_gpio, uint64_t ticks)
{
	struct fake_fhm *m = user;

	(void)ticks;
	if ((old_gpio & 1u) || !(new_gpio & 1u))
		return;   /* not a rising edge on bit 0 (TAL_GPIO_0) */
	pthread_mutex_lock(&m->lock);
	if (m->enabled) {
		/* One stage deep (fhm-pipeline.md; plan.FhmPipelineModel.on_edge):
		 * commit what was armed, arm the latest staged frequency. */
		m->pll_hz = m->armed_hz;
		m->armed_hz = m->staged_hz;
		m->edges++;
	}
	pthread_mutex_unlock(&m->lock);
}

void fake_fhm_attach(struct fake_fhm *m, struct fake_tick *f)
{
	struct fake_tick *old;

	pthread_mutex_lock(&m->lock);
	old = m->tick;
	m->tick = f;
	pthread_mutex_unlock(&m->lock);
	if (old && old != f)
		fake_tick_set_gpio_hook(old, NULL, NULL);
	if (f)
		fake_tick_set_gpio_hook(f, fhm_on_gpio, m);
}

/* The access cost advances the Tick without the FHM lock held, since the
 * Tick may call back into fhm_on_gpio. */
static void fhm_cost(struct fake_fhm *m)
{
	struct fake_tick *t;
	uint64_t cost;

	pthread_mutex_lock(&m->lock);
	t = m->tick;
	cost = m->access_cost;
	pthread_mutex_unlock(&m->lock);
	if (t && cost)
		fake_tick_advance(t, cost);
}

int fake_fhm_read(void *h, const char *attr, char *buf, size_t len)
{
	struct fake_fhm *m = h;
	int ret;

	if (!m || !attr || !buf || !len)
		return -EINVAL;
	fhm_cost(m);
	pthread_mutex_lock(&m->lock);
	if (!strcmp(attr, ATTR_FHM_ENABLE)) {
		ret = emit(buf, len, "%llu\n", m->enabled);
	} else if (!strcmp(attr, ATTR_FHM_HOP)) {
		/* talise.py: reading frequency_hopping_mode returns the live PLL */
		ret = emit(buf, len, "%llu\n", m->corrupt ? m->corrupt_hz : m->pll_hz);
		m->corrupt = false;
	} else {
		ret = -ENOENT;
	}
	pthread_mutex_unlock(&m->lock);
	return ret;
}

int fake_fhm_write(void *h, const char *attr, const char *val)
{
	struct fake_fhm *m = h;
	uint64_t v;
	bool b;
	int ret = 0;

	if (!m || !attr || !val)
		return -EINVAL;
	fhm_cost(m);
	pthread_mutex_lock(&m->lock);
	if (!strcmp(attr, ATTR_FHM_ENABLE)) {
		ret = parse_bool(val, &b);
		if (!ret) {
			m->enabled = b;
			/* Enabling (re)primes: the next edge lands on fhmInitFrequency. */
			if (b)
				m->staged_hz = m->armed_hz = m->init_hz;
		}
	} else if (!strcmp(attr, ATTR_FHM_HOP)) {
		/* talise.py stage(): writing frequency_hopping_mode stages the next */
		ret = parse_u64(val, strlen(val), &v);
		if (!ret)
			m->staged_hz = v;
	} else {
		ret = -ENOENT;
	}
	pthread_mutex_unlock(&m->lock);
	return ret;
}

const struct datax_tick_ops fake_fhm_ops = { fake_fhm_read, fake_fhm_write };

uint64_t fake_fhm_pll_hz(struct fake_fhm *m)
{
	uint64_t v;

	pthread_mutex_lock(&m->lock);
	v = m->pll_hz;
	pthread_mutex_unlock(&m->lock);
	return v;
}

unsigned int fake_fhm_edges(struct fake_fhm *m)
{
	unsigned int v;

	pthread_mutex_lock(&m->lock);
	v = m->edges;
	pthread_mutex_unlock(&m->lock);
	return v;
}

bool fake_fhm_enabled(struct fake_fhm *m)
{
	bool v;

	pthread_mutex_lock(&m->lock);
	v = m->enabled;
	pthread_mutex_unlock(&m->lock);
	return v;
}

void fake_fhm_corrupt_next_readback(struct fake_fhm *m, uint64_t hz)
{
	pthread_mutex_lock(&m->lock);
	m->corrupt = true;
	m->corrupt_hz = hz;
	pthread_mutex_unlock(&m->lock);
}

void fake_fhm_set_access_cost(struct fake_fhm *m, uint64_t ticks)
{
	pthread_mutex_lock(&m->lock);
	m->access_cost = ticks;
	pthread_mutex_unlock(&m->lock);
}
