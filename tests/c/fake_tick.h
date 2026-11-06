// SPDX-License-Identifier: MIT
/*
 * Fake Tick (axi-timed-command-scheduler) and fake Talise FHM for Tier 1
 * tests. Built as the test-only library libdatax_fake_tick (never installed).
 *
 * The fake implements the Tick driver's attribute surface
 * (kernel/axi_timed_command_scheduler.c at the pins.json Tick revision) over
 * a register model that mirrors the RTL (hdl/library/axi_timed_command_
 * scheduler/axi_timed_command_scheduler.v): a sorted queue, one completion
 * slot, late/overflow counters, PPS epoch alignment and GPIO/marker/stream
 * gate side effects. Its behaviour is pinned by the golden hdl_model traces
 * in tests/traces (test_fake_tick replays every one).
 *
 * Plug it into libdatax with
 *     datax_internal_tick_bind_fake(dev, &fake_tick_ops, fake);
 * Every function is thread-safe (one mutex per fake).
 */
#ifndef DATAX_FAKE_TICK_H
#define DATAX_FAKE_TICK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tick_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fake_tick;
struct fake_fhm;

/* Build parameters. Zero fields take tron's values (hopcap SFP image):
 * 100 MHz, depth 16, capability bitmap 0x9f (GPIO, PPS epoch, marker,
 * stream gate, TDD, repeat; no arm-release, timestamp or SPI). */
struct fake_tick_config {
	uint64_t tick_hz;
	unsigned int queue_depth;          /* at most FAKE_TICK_MAX_DEPTH */
	uint32_t capability_bitmap;
};
#define FAKE_TICK_MAX_DEPTH 64

struct fake_tick *fake_tick_new(const struct fake_tick_config *cfg); /* NULL cfg: tron */
void fake_tick_free(struct fake_tick *f);

/* The seam: h is the struct fake_tick *. Attribute names, formats and
 * read/write directions follow the driver; an unknown attribute is
 * -ENOENT, the wrong direction -EACCES, a value that does not parse
 * -EINVAL. */
extern const struct datax_tick_ops fake_tick_ops;
int fake_tick_read(void *h, const char *attr, char *buf, size_t len);
int fake_tick_write(void *h, const char *attr, const char *val);

/* ---- clock and pins ---- */

/* Advance the clock by ticks. Commands execute on the tick they come due,
 * as the RTL dispatches them. */
void fake_tick_advance(struct fake_tick *f, uint64_t ticks);
uint64_t fake_tick_now(struct fake_tick *f);
/* A rising edge on pps_in (RTL lines 563-575): sets pps_present (sticky,
 * as the RTL's pps_seen); when armed, loads time_epoch_ticks and sets
 * time_aligned. */
void fake_tick_pps_edge(struct fake_tick *f);
uint32_t fake_tick_gpio(struct fake_tick *f);         /* gpio_out */
uint32_t fake_tick_marker(struct fake_tick *f);       /* marker_out (sticky bits) */
uint32_t fake_tick_stream_gate(struct fake_tick *f);  /* stream_gate_en level */

/* Called (with the fake's lock held: do not call back into this fake) each
 * time gpio_out changes, with the old and new value and the tick. */
typedef void (*fake_tick_gpio_hook)(void *user, uint32_t old_gpio, uint32_t new_gpio,
				    uint64_t ticks);
void fake_tick_set_gpio_hook(struct fake_tick *f, fake_tick_gpio_hook hook, void *user);

/* ---- ground truth and fault injection for tests ---- */

/* Every command the fake executed or dropped, reported or not. */
struct fake_tick_exec {
	uint32_t sequence_id;
	uint16_t command_id, target_id;
	uint64_t requested_ticks;
	uint64_t executed_ticks;   /* tick it actually ran; 0 when dropped */
	uint32_t status;           /* Tick status code (0 EXECUTED .. 6 REPEAT_OVERFLOW) */
	uint32_t detail;           /* repeat occurrence index */
	bool reported;             /* a completion was recorded in the slot */
};
/* Copy up to max entries from index first; returns the total count. */
size_t fake_tick_exec_log(struct fake_tick *f, size_t first, struct fake_tick_exec *out,
			  size_t max);

/* Each attribute access (read or write) advances the clock by ticks first,
 * standing in for the network round trip. Default 0. */
void fake_tick_set_access_cost(struct fake_tick *f, uint64_t ticks);
/* The next access to attr returns err (a negative errno) and does nothing.
 * attr NULL: the next access to any attribute. */
void fake_tick_inject_error(struct fake_tick *f, const char *attr, int err);
/* Number of successful reads or writes of attr since creation. */
unsigned long fake_tick_access_count(struct fake_tick *f, const char *attr, bool write);

/* ---- fake Talise FHM (adrv9009-phy TRX LO channel attributes) ---- */

#define FAKE_FHM_INIT_HZ 2450000000ULL   /* fhmInitFrequency default */

/*
 * The Talise FHM engine is one stage deep (hopcap docs/explanation/
 * fhm-pipeline.md, hopcap plan.FhmPipelineModel):
 * - frequency_hopping_mode_enable "1" enables GPIO-trigger mode; the next
 *   edge commits fhmInitFrequency;
 * - writing frequency_hopping_mode stages the next frequency;
 * - each rising edge on Tick GPIO bit 0 (TAL_GPIO_0) commits the frequency
 *   staged before the previous edge and arms the latest staged one;
 * - reading frequency_hopping_mode returns the live RF PLL frequency.
 * The PLL starts at init_hz (0: FAKE_FHM_INIT_HZ).
 */
struct fake_fhm *fake_fhm_new(uint64_t init_hz);
void fake_fhm_free(struct fake_fhm *m);
/* Follow f's GPIO bit 0. Replaces any gpio hook on f; NULL f detaches. */
void fake_fhm_attach(struct fake_fhm *m, struct fake_tick *f);

/* Same seam shape as the Tick ops; h is the struct fake_fhm *. */
extern const struct datax_tick_ops fake_fhm_ops;
int fake_fhm_read(void *h, const char *attr, char *buf, size_t len);
int fake_fhm_write(void *h, const char *attr, const char *val);

uint64_t fake_fhm_pll_hz(struct fake_fhm *m);
unsigned int fake_fhm_edges(struct fake_fhm *m);        /* edges that committed */
bool fake_fhm_enabled(struct fake_fhm *m);
/* The next frequency_hopping_mode read returns hz instead of the PLL. */
void fake_fhm_corrupt_next_readback(struct fake_fhm *m, uint64_t hz);
/* Each FHM attribute access advances the attached Tick's clock by ticks. */
void fake_fhm_set_access_cost(struct fake_fhm *m, uint64_t ticks);

#ifdef __cplusplus
}
#endif

#endif
