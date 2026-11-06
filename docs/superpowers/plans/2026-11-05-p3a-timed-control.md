# libdatax P3a Timed Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement on the ADRV9009-ZU11EG (`tron`, hopcap image):
- the timed scheduler (GPIO and marker, sparse spaced reporting, completions, events);
- Talise FHM timed tunes on chip A;
- device time with a PPS or internal-clock fallback;
- MCS verification.

Proof is a fake-Tick Tier 1 suite and a tron hardware suite.

**Architecture:**
- **Tick seam.** All Tick access goes through an internal `struct datax_tick_ops` (`read_attr`, `write_attr`). The real implementation uses libiio attributes on `axi-timed-command-scheduler`; tests use a C fake that replays golden traces from `tick.hdl_model`.
- **Scheduler.** Each device has at most one scheduler, which owns one poller thread. The poller:
  - feeds the hardware queue in timestamp order, keeping at most depth − 1 entries;
  - drains the one-entry completion window into a ring;
  - stages the next FHM frequency;
  - watches the late, overflow and PPS counters;
  - raises events through the context callback.
- **Device time.** libdatax keeps a per-device soft offset (zero unless soft-aligned) and applies it wherever device time crosses the API.

**Tech Stack:** C11 + pthreads, libiio v1 (pin in `pins.json`), Python ctypes, pytest, ctest, the Tick Python model at the `pins.json` Tick revision (test-time, vendored per P1's rule), labgrid on `tron` through hopcap's runner pattern.

**Spec:** `docs/superpowers/specs/2026-11-05-libdatax-p3a-timed-control-design.md`, including its "Spike results" and "Rulings" sections. API contract: SDK `docs/api-c-core.md` at d187893 or later (Types, Device time and PPS, Hop staging, MCS, Scheduler semantics).

## Global Constraints

- P1 and P2 constraints hold: MIT SPDX headers, the C11 warning set with `-Werror`, last-error on every `-errno`, `git commit -s`, one worktree branch per task `task/p3a-<n>-<slug>`, no lab host names or IPs in the repo, and no skipped Tier 1 tests (CI runs with `DATAX_REQUIRE_EMU=1`).
- Public symbols are exactly those on the API page. Test-only symbols take the `datax_internal_` prefix and go in `src/libdatax.map` and `include/datax/datax_internal.h`.
- Tick facts (pinned RTL/driver):
  - 16-deep queue, 100 MHz, one completion slot.
  - A reported head command is held while that slot is unacked, and then records `executed = requested` (tick#38).
  - `edge_capture_*` latches only arm and gate commands.
  - Descriptor = 48 bytes, written as a CSV of twelve 32-bit words (P1 packer).
  - Default flags `LATE_DROP`; `REPORT_COMPLETION` only when marked.
- tron numbers (spike; libdatax measures its own at scheduler create):
  - completion drain about 1.76 ms, so report spacing = 3 × drain;
  - FHM stage write about 0.5 ms idle, about 1 ms while RX streams.
- GPIO bit 0 is reserved for tunes on the ZU11EG; bits 3:1 are the hop controller's slot index.
- Only chip A has a wired FHM trigger on this image. Chip B timed tunes return `-ENOTSUP`.
- `PHASE_COHERENT` stays false without RF PLL phase sync in the device tree; tron has none.
- tron runs hold nemo's `/tmp/vrt49-corundum-hardware.lock`. Never reprogram the PL at runtime.

## Review Focus

1. **Completion stall never reads as on time.** A reported command whose predecessor's completion was drained after its own timestamp must complete `LATE` with detail "poller lagged", never `OK`. Owner: Task 2, `test_poller_lag_marks_late` (the fake stalls the head exactly as the RTL does).
2. **Queue feeder.** A batch larger than the queue (40 commands) must execute in order without overflowing. A command the feeder could not write in time must complete `LATE` "host could not keep the queue fed" and must never be written late. Owner: Task 2, `test_commit_streams_large_batch`, `test_feeder_starved_marks_late`.
3. **Soft-aligned time is consistent.** After a soft alignment, `get_time`, `queue_*` timestamps and completion `requested`/`executed` ticks must all use the same offset: a command scheduled at aligned time T completes with `requested_ticks == T`. Owner: Task 3, `test_soft_offset_round_trips`.
4. **FHM pipeline.**
   - The first tune after `stage_hop_table` lands on the init frequency (the priming edge).
   - Each later edge commits the frequency staged before the previous edge.
   - A readback mismatch completes `DROPPED`, naming both frequencies.
   - A stage write that lands after its edge completes `LATE` "stage late".
   - Owner: Task 4, `test_fhm_one_behind_pipeline`, `test_fhm_readback_mismatch`, `test_fhm_stage_late`.
5. **Lifetime.** Destroying the context or the scheduler while the poller is mid-drain or mid-feed must join the thread before any handle is freed: no use-after-free, valgrind/ASan clean. Python `ctx.on_event` callbacks must not run after `ctx.close()`. Owner: Task 2, `test_destroy_while_polling`; Task 6, `test_event_after_close`.

---

### Task 0 (controller, done): API page and spec

SDK `docs/api-c-core.md` d187893 holds every new symbol and rule. The spec records the spike results and the rulings.

---

### Task 1: Header, Tick seam, golden traces, fake Tick

**Files:**
- Modify: `include/datax/datax.h`, `src/libdatax.map`, `src/internal.h`.
- Create:
  - `src/tick_ops.c`, `src/tick_ops.h`
  - `tests/c/fake_tick.c`, `tests/c/fake_tick.h`
  - `tests/c/test_fake_tick.c`
  - `tests/traces/` (golden JSON) and `tests/traces/gen_traces.py`
  - `tests/vendor/tick/hdl_model.py` (vendored, with a PROVENANCE row and hash test, P1 pattern)

**Interfaces:**
- **Header additions, verbatim from the API page:**
  - the two new `enum datax_limit` entries before `DATAX_LIMIT_COUNT`;
  - `enum datax_time_source`;
  - `datax_device_get_time_source`;
  - `datax_sched_report`.

  Each is stubbed to `-ENOTSUP` in this task, except `get_time_source`, which returns `DATAX_TIME_FREE_RUN` when a Tick device exists and `-ENOTSUP` otherwise.
- **Tick seam, `tick_ops.h`:**

  ```c
  struct datax_tick_ops {
      int (*read)(void *h, const char *attr, char *buf, size_t len);
      int (*write)(void *h, const char *attr, const char *val);
  };
  ```

  Plus `int datax_tick_bind(struct datax_device *dev, const struct datax_tick_ops *ops, void *h);`. The real ops wrap `iio_device_find_attr` / `iio_attr_read_raw` / `iio_attr_write_string` on the Tick device found at fold time. Test-only: `datax_internal_tick_bind_fake(dev, ops, h)` replaces the seam on a live device.
- **`gen_traces.py`** drives `tick.hdl_model` through named scenarios and writes `tests/traces/<name>.json`, each a sequence of `{t_ticks, op, attr, value}` steps with expected completions. Scenarios:
  - ordered queue
  - late-on-submit drop
  - late execute-now
  - flush
  - overflow (17th entry)
  - completion-slot hold (reported head held while unacked, then `executed = requested`)
  - repeat

  If the model lacks a behaviour the RTL has (check the completion-slot hold), add the case from the RTL line cited in the spec ruling, mark it `source: rtl` in the trace, and say so in the report.
- **`fake_tick.c`** implements the attribute surface (every attribute the driver exposes that libdatax uses) on a settable clock: `fake_tick_advance(f, ticks)`. Its behaviour must reproduce every trace. `test_fake_tick.c` replays each trace and asserts the fake matches step for step. A fake Talise FHM sits behind the same seam pattern:
  - `frequency_hopping_mode_enable`, `frequency_hopping_mode`;
  - an edge on GPIO bit 0 commits per the one-behind rule;
  - the init frequency is 2450 MHz.

- [ ] Write `test_fake_tick` (one test per trace) and a header test that the new symbols exist and are stubbed.
- [ ] Run `ctest -R 'test_fake_tick|test_stubs'`. Expect FAIL.
- [ ] Implement, regenerate the traces, and commit them with their provenance.
- [ ] Run the full `ctest` and `pytest`. Expect all to pass, with the docs-symbol gate green.
- [ ] Commit: `tick: internal ops seam, fake Tick replaying golden hdl_model traces`.

### Task 2: Scheduler core and poller

**Files:**
- Create: `src/sched.c`, `src/sched.h`, `src/poller.c`, `tests/c/test_sched.c`.
- Modify: `src/stubs.c`, `src/context.c` (destroy order: scheduler, then streams, then the IIO context), `src/device.c`.

**Interfaces:**
- Public: `datax_sched_create`, `_destroy`, `_queue_gpio`, `_queue_marker`, `_report`, `_commit`, `_flush`, `_wait`, `_get_completions`.
- `queue_tune` and `queue_stream_gate` stay `-ENOTSUP` (Task 4 / P3b).
- Rules, all from the API page "Scheduler semantics":
  - one per device (`-EBUSY`);
  - sorted builder;
  - streaming commit through the poller feeder (at most depth − 1 in hardware);
  - guard margin, giving `DATAX_ERR_LATE` with nothing written;
  - spacing check, `-EINVAL`;
  - `wait()` on an unreported seq gives `-EINVAL`;
  - lost report becomes `DROPPED` after the wait timeout, with detail naming `overflow_count`;
  - poller lag marks `LATE`;
  - `late_count` and `overflow_count` rises raise events;
  - `queue_gpio` with a mask touching a reserved bit gives `-EINVAL` (reserved set per family: adrv9009 = bit 0).
- `DATAX_LIMIT_REPORT_SPACING_US` = 3 × the median of 5 timed completion-window drains done at create time (8 reads + ack on an empty slot). Exposed through the device caps limits while the scheduler exists.
- `sched_create` sets a last-error warning (still returns 0) when `queue_level` ≠ 0 at create time.
- The poller runs every 1 ms (scheduler option `poll_us`, internal for now). It joins in `sched_destroy` and in context destroy before anything is freed.

- [ ] Write the failing tests in `test_sched.c`, all against the fake:
  - `test_one_scheduler_per_device`
  - `test_builder_sorts`
  - `test_commit_guard_margin_late`
  - `test_commit_streams_large_batch`
  - `test_feeder_starved_marks_late`
  - `test_report_spacing_refused`
  - `test_wait_unreported_einval`
  - `test_poller_lag_marks_late`
  - `test_lost_report_dropped`
  - `test_events_late_and_overflow`
  - `test_reserved_gpio_bit`
  - `test_flush_completes_flushed`
  - `test_destroy_while_polling` (run under ASan in CI if available, valgrind when on PATH)
- [ ] Run the tests. Expect FAIL.
- [ ] Implement.
- [ ] Run everything. Expect all to pass.
- [ ] Commit: `sched: batched streaming commit, spaced sparse reporting, poller with events`.

### Task 3: Device time, PPS and soft fallback

**Files:**
- Create: `src/devtime.c`, `tests/c/test_devtime.c`.
- Modify: `src/time.c`, `src/sched.c` (apply the offset), `src/poller.c` (PPS watch).

**Interfaces:**
- `datax_device_get_time_source` and `datax_device_set_time_next_pps`, per the API page "Device time and PPS":
  - With `pps_present`: write `time_epoch_ticks`, arm `arm_time_next_pps`, wait at most 2 s for `time_aligned`. The source becomes `PPS_ALIGNED`.
  - Otherwise, or on timeout, apply the soft offset so `get_time` reads `epoch` now. The source becomes `SOFT_ALIGNED`, the event fires with the RTT in its detail, and the call returns 0.
  - The poller handles `pps_present`/`time_aligned` transitions: holdover on loss; on appearance, an event and no jump.
- The offset is applied in `get_time`, in the `queue_*` timestamps (subtracted before descriptors are packed) and in completion ticks (added back).

- [ ] Write the failing tests:
  - `test_free_run_default`
  - `test_pps_hard_align`
  - `test_pps_timeout_soft_fallback`
  - `test_soft_offset_round_trips`
  - `test_pps_lost_holdover`
  - `test_pps_appears_no_jump`
- [ ] Run, implement, run, commit: `time: PPS alignment with internal-clock fallback`.

### Task 4: FHM timed tunes

**Files:**
- Create: `src/fhm.c`, `tests/c/test_fhm.c`.
- Modify: `src/platform/adrv9009.c`, `src/sched.c` (`queue_tune`), `src/device.c` (timed `set_frequency`), `src/stubs.c`.

**Interfaces:**
- `datax_device_stage_hop_table`:
  - band check per chip (the adrv9009 FHM band from the driver attributes, otherwise the hopcap default 2.4–2.5 GHz, cited);
  - at most 64 entries (`DATAX_LIMIT_HOP_TABLE_DEPTH` = 64 for adrv9009 with Tick);
  - enable FHM on chip A.
- `queue_tune(when, ch, hz)`:
  - `ch` on chip A only, otherwise `-ENOTSUP` "chip B FHM trigger is not wired on this image";
  - `hz` must be in the table (`-EINVAL` "frequency not staged");
  - emits a rise/fall pair on bit 0, the rise reported per the spacing rule (only every Nth rise when hops are closer than the spacing);
  - a priming edge before the first tune.
- The poller stages the next frequency after each edge's completion, or after the time of an unreported edge plus margin, and reads back the PLL on reported edges:
  - mismatch completes `DROPPED` "PLL <got> Hz, expected <want> Hz";
  - stage after the edge completes `LATE` "stage late".
- `DATAX_LIMIT_HOP_STAGE_US` is measured at `stage_hop_table` (median of 5 stage writes).
- Timed `set_frequency(when)` routes to the device's scheduler (created lazily, destroyed with the device).
- A chip A tune clears `PHASE_COHERENT` (moot on tron, where it is false).

- [ ] Write the failing tests:
  - `test_stage_table_band_and_depth`
  - `test_tune_requires_staged`
  - `test_tune_chip_b_enotsup`
  - `test_fhm_one_behind_pipeline`
  - `test_fhm_readback_mismatch`
  - `test_fhm_stage_late`
  - `test_dense_hops_report_every_nth`
  - `test_timed_set_frequency_uses_device_sched`
  - `test_reserved_bit_still_refused_for_gpio`
- [ ] Run, implement, run, commit: `fhm: timed Talise hops with one-behind staging and PLL readback`.

### Task 5: MCS, capability gates, doctor

**Files:**
- Create: `src/mcs.c`, `tests/c/test_mcs.c`, `tests/emu/adrv9009+tick-fsm.xml` (hand-written: the adrv9009 pair with `jesd204_fsm_state`/`_error`/`_paused`/`_ctrl` attributes, plus Tick).
- Modify: `src/caps.c`, `python/datax/doctor.py`, `tests/python/test_doctor.py`, `tests/c/test_caps.c`.

**Interfaces:**
- `run_mcs()` verifies, on both phys, `jesd204_fsm_state == opt_post_running_stage`, `jesd204_fsm_error == 0` and `jesd204_fsm_paused == 0`. It returns 0, or `-EIO` naming the phy and its state. It never writes FSM attributes.
- Caps ungated:
  - `TIMED_COMMANDS`: Tick present and a scheduler creatable.
  - `FHM_HOP`: adrv9009 + Tick + GPIO capability bit.
  - `MCS`: both phys + `jesd204_fsm_ctrl`.
  - `PHASE_COHERENT`: live; true only if MCS verified, RF PLL phase sync is enabled (read from the phy's device-tree-derived attribute if present, otherwise false) and no single-chip tune has happened since.
- Doctor lines:
  - `time:` source, and whether PPS is present;
  - `scheduler:` the report spacing and hop stage limits when measured, and an exclusivity note;
  - `mcs:` state, and why `PHASE_COHERENT` is false.

- [ ] Write the failing tests:
  - `test_run_mcs_ok` (emulator fixture)
  - `test_run_mcs_error_names_phy`
  - `test_caps_p3a_matrix` (flags per fixture)
  - `test_phase_coherent_false_without_phase_sync`
  - Python `test_doctor_p3a_lines`
- [ ] Run, implement, run, commit: `mcs: verify JESD MCS; P3a capability gates; doctor time/scheduler/mcs lines`.

### Task 6: Python surface

**Files:**
- Create: `python/datax/sched.py`, `tests/python/test_sched.py`, `examples/python/timed_staircase.py`, `examples/python/hop_schedule.py`.
- Modify: `python/datax/_lib.py`, `device.py`, `context.py`, `time.py`, `tests/python/test_examples.py`.

**Interfaces (per `api-python.md`):**
- `dev.scheduler` (lazy, refuses with `CapabilityError` when `TIMED_COMMANDS` is false);
- `sched.at(t).gpio(mask, value, report=False)`, `.marker(line, width, report=False)`, `.tune(hz, ch=0)`, each returning the seq;
- `sched.commit()`, `sched.wait(seq, timeout_ms)` returning a `Completion` dataclass;
- `sched.wait_all()`, `sched.flush()`;
- `dev.stage_hop_table(freqs)`, `dev.run_mcs()`;
- `dev.time.align_next_pps(epoch=0)`, `dev.time.source`;
- `ctx.on_event(fn)`: a trampoline kept alive by the context, callbacks delivered on the poller thread, never after `ctx.close()`;
- `LateError` for `DATAX_ERR_LATE`.

The tests run against the C fake through a test-only ctypes hook `datax_internal_tick_bind_fake`, exposed by `tests/python/conftest.py`.

- [ ] Write the failing tests:
  - `test_sched_builder_and_wait`
  - `test_report_flag`
  - `test_late_error`
  - `test_tune_flow`
  - `test_align_soft_fallback_event`
  - `test_event_after_close`
  - the two examples run
- [ ] Run, implement, run, commit: `python: scheduler builder, hops, time source, events`.

### Task 7: Hardware suite (collects without a board)

**Files:**
- Create: `tests/hardware/test_hw_timed.py`, `tests/hardware/test_hw_mcs.py`.
- Modify: `tests/hardware/conftest.py` (`DATAX_HW_IMAGE` = `hopcap-sfp`/`stock`, skipping P3a tests on `stock`), `tests/hardware/README.md`, `docs/hardware.md`.

**Tests:**
- `test_staircase`: 8 GPIO edges on bits 3:1, each reported, spaced at 2 × `REPORT_SPACING`; every completion `OK` with `executed == requested`. Evidence records the spacing and states that there is no independent scope.
- `test_hop_schedule`: stage {2420, 2440, 2460, 2480} MHz, then a priming tune and four tunes 200 ms apart; every reported tune `OK`; PLL readback equals the plan; the priming edge lands on 2450 MHz.
- `test_late_dropped`: a reported GPIO 1 ms in the past is refused at commit with `DATAX_ERR_LATE`. A command queued for now + guard margin + 1 ms while the poller is paused (internal hook) completes `LATE` and never executes.
- `test_pps_alignment`: on the selector image, `align_next_pps` gives source `PPS_ALIGNED`, Tick `time_aligned` 1 and `get_time` advancing whole seconds from the epoch. Without a PPS: `SOFT_ALIGNED` and the event.
- `test_run_mcs_verifies`.
- `test_phase_coherent_false`.
- P2's `test_hw_stream_smoke.py` RX tests run unchanged as the regression (TX is skipped on `hopcap-sfp`: its TX DMA feeds vrt49's deframer).

Evidence goes to `DATAX_HW_EVIDENCE`: completion logs, measured limits, Tick counters, image hashes.

- [ ] Write the Tier 1 checks for the pure helpers (spacing math, evidence redaction reuse).
- [ ] Run `pytest tests/hardware --collect-only`. Expect it to collect.
- [ ] Commit: `tests: P3a hardware suite`.

### Task 8 (controller, lab): image and tron proof

1. Merge hopcap's `tick_pps_select` PR once its CI is green and it has had review.
2. Build the hopcap SFP bitstream on hdl-dev-2 (`--port sfp`) and verify the evidence. The boot set's kernel and DTB are reused, with their hashes unchanged. Stage the result on nemo.
3. Run hopcap's hw-test on it (4/4, so hopcap does not regress).
4. Run the libdatax hardware suite on tron with the new image. Fix what the hardware finds, at the root.
5. Commit the evidence under `docs/evidence/p3a/`.

### Task 9 (controller): phase exit

- Update the support rows for the ZU11EG:
  - `timed-gpio`, `fhm-hop` and the new `sdk-timed` (libdatax) as integration artifacts, per the freeze rule until `hw-test.yml` runs;
  - `mcs` as integration artifacts with a note that `PHASE_COHERENT` is unreachable without RF PLL phase sync.
- Update the platform page, the roadmap (P3a done; P3b next) and the spec status.
- Pin the SDK `sources.json`.
- Open a P3b issue: timed DMA streams on the ZU11EG (data_offload gate, tagger, chip B FHM trigger).
