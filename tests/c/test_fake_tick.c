// SPDX-License-Identifier: MIT
/*
 * The fake Tick against the golden hdl_model traces (REQ-TEST-003), plus the
 * fake's own surface, PPS and FHM behaviour, and the libdatax Tick seam.
 *
 *   test_fake_tick trace <file.json>   replay one trace step for step
 *   test_fake_tick unit                surface, PPS, FHM and fault hooks
 *   test_fake_tick seam                bind the fake to a live device
 *                                      (under emu.sh: needs DATAX_TEST_URI)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <datax/datax.h>
#include <datax/datax_internal.h>

#include "check.h"
#include "fake_tick.h"
#include "internal.h"

/* ---- minimal JSON reader (objects, arrays, strings, integers, literals) -- */

enum jtype { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct jval {
	enum jtype type;
	char *str;                 /* J_STR text, J_NUM digits */
	struct jval **items;       /* J_ARR elements, J_OBJ values */
	char **keys;               /* J_OBJ keys */
	size_t n;
	int b;
};

struct jparser {
	const char *p;
};

static void jws(struct jparser *j)
{
	while (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')
		j->p++;
}

static struct jval *jnew(enum jtype t)
{
	struct jval *v = calloc(1, sizeof(*v));

	CHECK(v != NULL);
	v->type = t;
	return v;
}

static char *jstring(struct jparser *j)
{
	size_t cap = 16, n = 0;
	char *s = malloc(cap);

	CHECK(s != NULL && *j->p == '"');
	j->p++;
	while (*j->p != '"') {
		char c = *j->p++;

		CHECK(c != '\0');
		if (c == '\\') {
			c = *j->p++;
			CHECK(c == '"' || c == '\\' || c == '/' || c == 'n' || c == 't');
			c = c == 'n' ? '\n' : c == 't' ? '\t' : c;
		}
		if (n + 2 > cap) {
			cap *= 2;
			s = realloc(s, cap);
			CHECK(s != NULL);
		}
		s[n++] = c;
	}
	j->p++;
	s[n] = '\0';
	return s;
}

static struct jval *jparse(struct jparser *j);

static void jpush(struct jval *v, char *key, struct jval *item)
{
	v->items = realloc(v->items, (v->n + 1) * sizeof(*v->items));
	CHECK(v->items != NULL);
	if (v->type == J_OBJ) {
		v->keys = realloc(v->keys, (v->n + 1) * sizeof(*v->keys));
		CHECK(v->keys != NULL);
		v->keys[v->n] = key;
	}
	v->items[v->n++] = item;
}

static struct jval *jparse(struct jparser *j)
{
	struct jval *v;

	jws(j);
	if (*j->p == '{' || *j->p == '[') {
		char close = *j->p == '{' ? '}' : ']';

		v = jnew(close == '}' ? J_OBJ : J_ARR);
		j->p++;
		jws(j);
		while (*j->p != close) {
			char *key = NULL;

			if (v->type == J_OBJ) {
				key = jstring(j);
				jws(j);
				CHECK(*j->p == ':');
				j->p++;
			}
			jpush(v, key, jparse(j));
			jws(j);
			if (*j->p == ',')
				j->p++;
			else
				CHECK(*j->p == close);
			jws(j);
		}
		j->p++;
		return v;
	}
	if (*j->p == '"') {
		v = jnew(J_STR);
		v->str = jstring(j);
		return v;
	}
	if (!strncmp(j->p, "null", 4) || !strncmp(j->p, "true", 4) ||
	    !strncmp(j->p, "false", 5)) {
		v = jnew(*j->p == 'n' ? J_NULL : J_BOOL);
		v->b = *j->p == 't';
		j->p += *j->p == 'f' ? 5 : 4;
		return v;
	}
	{
		const char *s = j->p;

		while ((*j->p >= '0' && *j->p <= '9') || *j->p == '-')
			j->p++;
		CHECK(j->p > s);
		v = jnew(J_NUM);
		v->str = strndup(s, (size_t)(j->p - s));
		CHECK(v->str != NULL);
		return v;
	}
}

static void jfree(struct jval *v)
{
	if (!v)
		return;
	for (size_t i = 0; i < v->n; i++) {
		jfree(v->items[i]);
		if (v->keys)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->str);
	free(v);
}

static const struct jval *jget(const struct jval *o, const char *key)
{
	CHECK(o && o->type == J_OBJ);
	for (size_t i = 0; i < o->n; i++)
		if (!strcmp(o->keys[i], key))
			return o->items[i];
	return NULL;
}

static unsigned long long jnum(const struct jval *v)
{
	CHECK(v && v->type == J_NUM);
	return strtoull(v->str, NULL, 10);
}

static struct jval *jload(const char *path)
{
	FILE *fp = fopen(path, "rb");
	struct jparser j;
	struct jval *v;
	char *text;
	long sz;

	CHECK(fp != NULL);
	CHECK(fseek(fp, 0, SEEK_END) == 0);
	sz = ftell(fp);
	CHECK(sz > 0);
	rewind(fp);
	text = malloc((size_t)sz + 1);
	CHECK(text != NULL);
	CHECK(fread(text, 1, (size_t)sz, fp) == (size_t)sz);
	text[sz] = '\0';
	fclose(fp);
	j.p = text;
	v = jparse(&j);
	free(text);
	return v;
}

/* ---- helpers ------------------------------------------------------------ */

static unsigned long long rd(struct fake_tick *f, const char *attr)
{
	char buf[64];
	char *end;
	unsigned long long v;
	int n = fake_tick_ops.read(f, attr, buf, sizeof(buf));

	if (n < 0) {
		fprintf(stderr, "read %s: %d\n", attr, n);
		exit(1);
	}
	CHECK((size_t)n == strlen(buf));
	v = strtoull(buf, &end, 0);
	CHECK(end != buf);
	return v;
}

static void wr(struct fake_tick *f, const char *attr, const char *val)
{
	int ret = fake_tick_ops.write(f, attr, val);

	if (ret) {
		fprintf(stderr, "write %s=%s: %d\n", attr, val, ret);
		exit(1);
	}
}

/* Twelve-word CSV for a descriptor (the driver's command_descriptor input). */
static void csv(char *out, size_t len, uint16_t cmd, uint16_t target, uint64_t ts,
		uint32_t flags, uint32_t seq, uint32_t a0, uint32_t a1, uint32_t a2,
		uint32_t a3)
{
	snprintf(out, len,
		 "0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,"
		 "0x%08x,0,0",
		 1u | (48u << 16), (unsigned)cmd | ((unsigned)target << 16),
		 (unsigned)(ts & 0xffffffffu), (unsigned)(ts >> 32), flags, seq,
		 a0, a1, a2, a3);
}

#define F_REPORT 0x1u
#define F_DROP   0x2u
#define F_NOW    0x4u
#define F_REPEAT 0x8u

static void push_gpio(struct fake_tick *f, uint32_t seq, uint64_t ts, uint32_t flags,
		      uint32_t mask, uint32_t value)
{
	char b[256];

	csv(b, sizeof(b), 1, 0, ts, flags, seq, mask, value, 0, 0);
	wr(f, "command_descriptor", b);
}

/* ---- trace replay -------------------------------------------------------- */

static int replay(const char *path)
{
	struct jval *t = jload(path);
	const struct jval *cfg = jget(t, "config"), *steps = jget(t, "steps");
	const struct jval *prov = jget(t, "provenance");
	struct fake_tick_config c = { 0 };
	struct fake_tick *f;

	CHECK(prov && jget(prov, "tick_revision") && jget(prov, "model_sha256"));
	c.tick_hz = jnum(jget(cfg, "tick_hz"));
	c.queue_depth = (unsigned)jnum(jget(cfg, "queue_depth"));
	f = fake_tick_new(&c);
	CHECK(f != NULL);
	CHECK(rd(f, "time_tick_hz") == c.tick_hz);
	CHECK(rd(f, "queue_depth") == c.queue_depth);
	CHECK(steps && steps->type == J_ARR && steps->n > 0);

	for (size_t i = 0; i < steps->n; i++) {
		const struct jval *s = steps->items[i];
		const struct jval *expect = jget(s, "expect");
		const char *op = jget(s, "op")->str;
		unsigned long long at = jnum(jget(s, "t_ticks"));
		unsigned long long now = fake_tick_now(f);

		/* t_ticks is when the step happens: never before the fake's now
		 * (an epoch load may have moved time; it is relative to that). */
		if (at < now) {
			fprintf(stderr, "%s step %zu: t_ticks %llu before now %llu\n",
				path, i, at, now);
			return 1;
		}
		fake_tick_advance(f, at - now);
		if (!strcmp(op, "advance")) {
			fake_tick_advance(f, jnum(jget(s, "value")));
		} else if (!strcmp(op, "write")) {
			wr(f, jget(s, "attr")->str, jget(s, "value")->str);
		} else if (!strcmp(op, "pps_edge")) {
			fake_tick_pps_edge(f);
		} else {
			fprintf(stderr, "%s step %zu: unknown op %s\n", path, i, op);
			return 1;
		}
		for (size_t k = 0; k < expect->n; k++) {
			const char *key = expect->keys[k];
			unsigned long long want = jnum(expect->items[k]);
			unsigned long long got = strcmp(key, "gpio_out") ?
				rd(f, key) : fake_tick_gpio(f);

			if (got != want) {
				fprintf(stderr, "%s step %zu (%s %s): %s = %llu, trace says %llu\n",
					path, i, op,
					jget(s, "attr")->type == J_STR ? jget(s, "attr")->str : "",
					key, got, want);
				return 1;
			}
		}
	}
	printf("%s: %zu steps match\n", path, steps->n);
	fake_tick_free(f);
	jfree(t);
	return 0;
}

/* ---- unit checks ----------------------------------------------------------- */

static void unit_surface(void)
{
	struct fake_tick *f = fake_tick_new(NULL);
	char buf[64];

	CHECK(f != NULL);
	/* tron build parameters and driver formats */
	CHECK(fake_tick_ops.read(f, "capability_bitmap", buf, sizeof(buf)) > 0);
	CHECK(!strcmp(buf, "0x0000009f\n"));
	CHECK(rd(f, "time_tick_hz") == 100000000ULL);
	CHECK(rd(f, "queue_depth") == 16);
	CHECK(rd(f, "queue_level") == 0);
	CHECK(fake_tick_ops.read(f, "current_time_ticks", buf, sizeof(buf)) == 2);
	CHECK(!strcmp(buf, "0\n"));
	/* a short buffer truncates and still terminates */
	CHECK(fake_tick_ops.read(f, "capability_bitmap", buf, 4) == 3);
	CHECK(!strcmp(buf, "0x0"));

	/* errors */
	CHECK(fake_tick_ops.read(f, "no_such_attr", buf, sizeof(buf)) == -ENOENT);
	CHECK(fake_tick_ops.write(f, "no_such_attr", "1") == -ENOENT);
	CHECK(fake_tick_ops.read(f, "command_descriptor", buf, sizeof(buf)) == -EACCES);
	CHECK(fake_tick_ops.read(f, "completion_ack", buf, sizeof(buf)) == -EACCES);
	CHECK(fake_tick_ops.write(f, "queue_level", "1") == -EACCES);
	CHECK(fake_tick_ops.write(f, "current_time_ticks", "5") == -EACCES);
	CHECK(fake_tick_ops.write(f, "completion_ack", "maybe") == -EINVAL);
	CHECK(fake_tick_ops.write(f, "time_epoch_ticks", "x") == -EINVAL);
	/* command_descriptor needs exactly twelve u32 words */
	CHECK(fake_tick_ops.write(f, "command_descriptor", "1,2,3") == -EINVAL);
	CHECK(fake_tick_ops.write(f, "command_descriptor",
				  "1,2,3,4,5,6,7,8,9,10,11,12,13") == -EINVAL);
	CHECK(fake_tick_ops.write(f, "command_descriptor",
				  "1,2,3,4,5,6,7,8,9,10,11,0x100000000") == -EINVAL);
	CHECK(rd(f, "queue_level") == 0);
	/* whitespace separators and decimal words are accepted, as kstrtou32(.., 0) */
	CHECK(fake_tick_ops.write(f, "command_descriptor",
				  "3145729 1\t1000 0\n2, 9 2 2 0 0 0 0") == 0);
	CHECK(rd(f, "queue_level") == 1);
	/* kstrtobool: "0"/"n" do nothing */
	wr(f, "flush_queue", "0");
	CHECK(rd(f, "queue_level") == 1);
	wr(f, "flush_queue", "y");
	CHECK(rd(f, "queue_level") == 0);

	/* epoch is read/write; time_tick_hz is not writable */
	wr(f, "time_epoch_ticks", "123456789");
	CHECK(rd(f, "time_epoch_ticks") == 123456789ULL);
	CHECK(fake_tick_ops.write(f, "time_tick_hz", "1") == -EACCES);
	fake_tick_free(f);
}

static void unit_bad_and_unsupported(void)
{
	struct fake_tick *f = fake_tick_new(NULL);
	char b[256];

	/* both late policies: BAD_DESCRIPTOR (RTL line 682) */
	push_gpio(f, 5, 100, F_REPORT | F_DROP | F_NOW, 1, 1);
	CHECK(rd(f, "completion_status") == 5 && rd(f, "completion_sequence_id") == 5);
	/* with the slot full a rejection only counts an overflow */
	csv(b, sizeof(b), 6, 0x500, 100, F_REPORT, 6, 0, 0, 0, 0);
	wr(f, "command_descriptor", b);
	CHECK(rd(f, "overflow_count") == 1 && rd(f, "completion_sequence_id") == 5);
	wr(f, "completion_ack", "1");
	/* CAPTURE_ARM without HAS_ARM_RELEASE (tron): UNSUPPORTED */
	wr(f, "command_descriptor", b);
	CHECK(rd(f, "completion_status") == 4 && rd(f, "completion_sequence_id") == 6);
	CHECK(rd(f, "completion_target_id") == 0x500 && rd(f, "completion_command_id") == 6);
	wr(f, "completion_ack", "1");
	/* REPEAT with period 0 is bad */
	push_gpio(f, 7, 100, F_REPORT | F_REPEAT, 1, 1);
	CHECK(rd(f, "completion_status") == 5);
	wr(f, "completion_ack", "1");
	CHECK(rd(f, "queue_level") == 0);
	fake_tick_free(f);
}

static void unit_stream_gate_capture(void)
{
	struct fake_tick *f = fake_tick_new(NULL);
	char b[256];

	CHECK(rd(f, "edge_capture_valid") == 0 && rd(f, "edge_capture_count") == 0);
	/* GPIO never latches the capture tag (RTL: only gate/arm paths do) */
	push_gpio(f, 1, 10, F_DROP, 0x2, 0x2);
	fake_tick_advance(f, 20);
	CHECK(rd(f, "edge_capture_count") == 0);
	/* STREAM_GATE enable latches at its scheduled tick */
	csv(b, sizeof(b), 3, 0x200, 50, F_DROP, 9, 2, 1, 0, 0);
	wr(f, "command_descriptor", b);
	fake_tick_advance(f, 100);
	CHECK(fake_tick_stream_gate(f) == 0x4);
	CHECK(rd(f, "edge_capture_valid") == 1 && rd(f, "edge_capture_count") == 1);
	CHECK(rd(f, "edge_capture_ticks") == 50);
	CHECK(rd(f, "edge_capture_target_id") == 0x200);
	CHECK(rd(f, "edge_capture_command_id") == 3);
	wr(f, "edge_capture_ack", "1");
	CHECK(rd(f, "edge_capture_valid") == 0 && rd(f, "edge_capture_count") == 1);
	/* disable clears the level and does not latch */
	csv(b, sizeof(b), 3, 0x200, 200, F_DROP, 10, 2, 0, 0, 0);
	wr(f, "command_descriptor", b);
	fake_tick_advance(f, 100);
	CHECK(fake_tick_stream_gate(f) == 0 && rd(f, "edge_capture_count") == 1);
	fake_tick_free(f);
}

static void unit_pps(void)
{
	struct fake_tick *f = fake_tick_new(NULL);

	CHECK(rd(f, "pps_present") == 0 && rd(f, "time_aligned") == 0);
	fake_tick_advance(f, 1000);
	wr(f, "time_epoch_ticks", "7000000");
	wr(f, "arm_time_next_pps", "0");            /* kstrtobool false: no arm */
	fake_tick_pps_edge(f);
	CHECK(rd(f, "pps_present") == 1 && rd(f, "time_aligned") == 0);
	CHECK(fake_tick_now(f) == 1000);
	wr(f, "arm_time_next_pps", "1");
	CHECK(rd(f, "time_aligned") == 0);
	fake_tick_advance(f, 5);
	CHECK(fake_tick_now(f) == 1005);            /* armed: time runs until the edge */
	fake_tick_pps_edge(f);
	CHECK(fake_tick_now(f) == 7000000ULL);
	CHECK(rd(f, "current_time_ticks") == 7000000ULL);
	CHECK(rd(f, "time_aligned") == 1 && rd(f, "pps_present") == 1);
	fake_tick_pps_edge(f);                      /* disarmed again: no reload */
	CHECK(fake_tick_now(f) == 7000000ULL);
	fake_tick_advance(f, 10);
	CHECK(rd(f, "current_time_ticks") == 7000010ULL);
	/* re-arm clears time_aligned (RTL lines 666-669) */
	wr(f, "arm_time_next_pps", "1");
	CHECK(rd(f, "time_aligned") == 0 && rd(f, "pps_present") == 1);
	fake_tick_free(f);
}

static void unit_hooks(void)
{
	struct fake_tick *f = fake_tick_new(NULL);
	struct fake_tick_exec ex[4];
	char buf[32];

	/* access cost: every attribute access moves the clock */
	fake_tick_set_access_cost(f, 25);
	CHECK(rd(f, "current_time_ticks") == 25);
	CHECK(rd(f, "current_time_ticks") == 50);
	push_gpio(f, 3, 60, F_DROP, 0x2, 0x2);       /* written at 75: late, dropped */
	CHECK(fake_tick_now(f) == 75);
	CHECK(rd(f, "late_count") == 1);
	fake_tick_set_access_cost(f, 0);

	/* exec log: ground truth, reported or not */
	CHECK(fake_tick_exec_log(f, 0, ex, 4) == 1);
	CHECK(ex[0].sequence_id == 3 && ex[0].status == 2 && ex[0].executed_ticks == 0);
	CHECK(ex[0].reported);
	wr(f, "completion_ack", "1");
	push_gpio(f, 4, 200, F_DROP, 0x2, 0x2);
	fake_tick_advance(f, 200);
	CHECK(fake_tick_exec_log(f, 1, ex, 4) == 2);
	CHECK(ex[0].sequence_id == 4 && ex[0].status == 0 && ex[0].executed_ticks == 200);
	CHECK(!ex[0].reported && fake_tick_gpio(f) == 0x2);

	/* fault injection is one-shot and does nothing */
	fake_tick_inject_error(f, "queue_level", -EIO);
	CHECK(fake_tick_ops.read(f, "queue_level", buf, sizeof(buf)) == -EIO);
	CHECK(fake_tick_ops.read(f, "queue_level", buf, sizeof(buf)) > 0);
	fake_tick_inject_error(f, NULL, -ETIMEDOUT);
	CHECK(fake_tick_ops.write(f, "flush_queue", "1") == -ETIMEDOUT);
	CHECK(fake_tick_access_count(f, "command_descriptor", true) == 2);
	CHECK(fake_tick_access_count(f, "flush_queue", true) == 0);
	CHECK(fake_tick_access_count(f, "queue_level", false) == 1);
	fake_tick_free(f);
}

static void unit_marker(void)
{
	struct fake_tick *f = fake_tick_new(NULL);
	char b[256];

	csv(b, sizeof(b), 2, 0x100, 10, F_DROP, 1, 3, 4, 0, 0);
	wr(f, "command_descriptor", b);
	fake_tick_advance(f, 10);
	CHECK(fake_tick_marker(f) == 0x8);
	fake_tick_free(f);
}

static void unit_fhm(void)
{
	struct fake_tick *f = fake_tick_new(NULL);
	struct fake_fhm *m = fake_fhm_new(0);
	char buf[64];

	CHECK(m != NULL);
	fake_fhm_attach(m, f);
	CHECK(fake_fhm_pll_hz(m) == FAKE_FHM_INIT_HZ);
	CHECK(fake_fhm_ops.read(m, "frequency_hopping_mode", buf, sizeof(buf)) > 0);
	CHECK(strtoull(buf, NULL, 10) == 2450000000ULL);
	CHECK(fake_fhm_ops.read(m, "frequency_hopping_mode_enable", buf, sizeof(buf)) > 0);
	CHECK(buf[0] == '0');
	CHECK(fake_fhm_ops.read(m, "nope", buf, sizeof(buf)) == -ENOENT);
	CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode", "abc") == -EINVAL);

	/* disabled: edges move nothing */
	CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode", "2410000000") == 0);
	push_gpio(f, 1, 10, F_DROP, 0x1, 0x1);
	fake_tick_advance(f, 10);
	CHECK(fake_fhm_edges(m) == 0 && fake_fhm_pll_hz(m) == FAKE_FHM_INIT_HZ);
	push_gpio(f, 2, 20, F_DROP, 0x1, 0x0);
	fake_tick_advance(f, 10);

	/* plan [A, B]: priming edge commits init, then one behind */
	CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode_enable", "1") == 0);
	CHECK(fake_fhm_enabled(m));
	CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode", "2420000000") == 0); /* A */
	push_gpio(f, 3, 30, F_DROP, 0x1, 0x1);     /* edge 0 (priming) */
	fake_tick_advance(f, 10);
	CHECK(fake_fhm_edges(m) == 1 && fake_fhm_pll_hz(m) == 2450000000ULL);
	CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode", "2430000000") == 0); /* B */
	/* falling edge and a write that keeps bit 0 high are not rising edges */
	push_gpio(f, 4, 40, F_DROP, 0x1, 0x0);
	push_gpio(f, 5, 45, F_DROP, 0x3, 0x2);
	fake_tick_advance(f, 20);
	CHECK(fake_fhm_edges(m) == 1);
	push_gpio(f, 6, 60, F_DROP, 0x1, 0x1);     /* edge 1 commits A */
	fake_tick_advance(f, 10);
	CHECK(fake_fhm_pll_hz(m) == 2420000000ULL);
	push_gpio(f, 7, 70, F_DROP, 0x1, 0x0);
	push_gpio(f, 8, 80, F_DROP, 0x1, 0x1);     /* edge 2 commits B */
	fake_tick_advance(f, 20);
	CHECK(fake_fhm_pll_hz(m) == 2430000000ULL);
	push_gpio(f, 9, 90, F_DROP, 0x1, 0x0);
	push_gpio(f, 10, 100, F_DROP, 0x1, 0x1);   /* edge 3: nothing new staged */
	fake_tick_advance(f, 20);
	CHECK(fake_fhm_pll_hz(m) == 2430000000ULL && fake_fhm_edges(m) == 4);

	/* corrupted readback is one-shot */
	fake_fhm_corrupt_next_readback(m, 2499000000ULL);
	CHECK(fake_fhm_ops.read(m, "frequency_hopping_mode", buf, sizeof(buf)) > 0);
	CHECK(strtoull(buf, NULL, 10) == 2499000000ULL);
	CHECK(fake_fhm_ops.read(m, "frequency_hopping_mode", buf, sizeof(buf)) > 0);
	CHECK(strtoull(buf, NULL, 10) == 2430000000ULL);

	/* re-enable re-primes: the next edge lands on init again */
	CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode_enable", "0") == 0);
	CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode_enable", "1") == 0);
	push_gpio(f, 11, 110, F_DROP, 0x1, 0x0);
	push_gpio(f, 12, 120, F_DROP, 0x1, 0x1);
	fake_tick_advance(f, 20);
	CHECK(fake_fhm_pll_hz(m) == FAKE_FHM_INIT_HZ);

	/* access cost moves the attached Tick's clock */
	fake_fhm_set_access_cost(m, 50);
	{
		uint64_t t0 = fake_tick_now(f);

		CHECK(fake_fhm_ops.write(m, "frequency_hopping_mode", "2440000000") == 0);
		CHECK(fake_tick_now(f) == t0 + 50);
	}
	fake_fhm_attach(m, NULL);
	fake_fhm_free(m);
	fake_tick_free(f);
}

static int unit(void)
{
	unit_surface();
	unit_bad_and_unsupported();
	unit_stream_gate_capture();
	unit_pps();
	unit_hooks();
	unit_marker();
	unit_fhm();
	printf("unit: ok\n");
	return 0;
}

/* ---- the libdatax seam on a live device ----------------------------------- */

static int seam(void)
{
	const char *uri = getenv("DATAX_TEST_URI");
	struct datax_context *ctx = NULL;
	struct datax_device *dev = NULL;
	struct fake_tick_config cfg = { .tick_hz = 100000000ULL, .queue_depth = 16 };
	struct fake_tick *f = fake_tick_new(&cfg);
	enum datax_time_source src = DATAX_TIME_PPS_HOLDOVER;
	unsigned long long ticks = 0, hz = 0, v = 0;
	struct datax_tick_ops half = { fake_tick_read, NULL };

	CHECK(uri != NULL && f != NULL);
	CHECK(datax_context_create_from_uri(uri, &ctx) == 0);
	CHECK(datax_context_get_device(ctx, 0, &dev) == 0);

	/* the fixture has no Tick */
	CHECK(datax_device_get_time_source(dev, &src) == -ENOTSUP);
	CHECK(datax_device_get_time(dev, &ticks, &hz) == -ENOTSUP);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev), DATAX_LIMIT_TICK_HZ, &v) == 0);
	CHECK(v == 0);
	CHECK(!(datax_internal_probe_flags(dev) & (1u << DATAX_CAP_TIMED_COMMANDS)));

	CHECK(datax_internal_tick_bind_fake(NULL, &fake_tick_ops, f) == -EINVAL);
	CHECK(datax_internal_tick_bind_fake(dev, &half, f) == -EINVAL);
	CHECK(datax_internal_tick_bind_fake(dev, &fake_tick_ops, f) == 0);

	/* every Tick access now goes through the fake */
	fake_tick_advance(f, 12345);
	CHECK(datax_device_get_time(dev, &ticks, &hz) == 0);
	CHECK(ticks == 12345 && hz == 100000000ULL);
	CHECK(fake_tick_access_count(f, "current_time_ticks", false) == 1);
	CHECK(datax_device_get_time_source(dev, &src) == 0 && src == DATAX_TIME_FREE_RUN);
	CHECK(datax_device_get_time_source(dev, NULL) == -EINVAL);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev), DATAX_LIMIT_TICK_HZ, &v) == 0);
	CHECK(v == 100000000ULL);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev), DATAX_LIMIT_QUEUE_DEPTH, &v) == 0);
	CHECK(v == 16);
	CHECK(datax_internal_probe_flags(dev) & (1u << DATAX_CAP_TIMED_COMMANDS));
	/* the two measured limits are not measured before a scheduler exists */
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev), DATAX_LIMIT_REPORT_SPACING_US,
				   &v) == -ENOTSUP);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev), DATAX_LIMIT_HOP_STAGE_US,
				   &v) == -ENOTSUP);

	/* a Tick read error reaches the caller with last-error */
	fake_tick_inject_error(f, "current_time_ticks", -EIO);
	CHECK(datax_device_get_time(dev, &ticks, &hz) == -EIO);
	CHECK(strstr(datax_context_get_last_error(ctx), "Tick read failed") != NULL);

	/* detach: back to no Tick */
	CHECK(datax_internal_tick_bind_fake(dev, NULL, NULL) == 0);
	CHECK(datax_device_get_time_source(dev, &src) == -ENOTSUP);
	CHECK(strstr(datax_context_get_last_error(ctx), "no Tick scheduler") != NULL);
	CHECK(datax_caps_get_limit(datax_device_get_caps(dev), DATAX_LIMIT_TICK_HZ, &v) == 0);
	CHECK(v == 0);

	datax_context_destroy(ctx);
	fake_tick_free(f);
	printf("seam: ok\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "trace"))
		return replay(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "unit"))
		return unit();
	if (argc == 2 && !strcmp(argv[1], "seam"))
		return seam();
	fprintf(stderr, "usage: %s trace <file.json> | unit | seam\n", argv[0]);
	return 2;
}
