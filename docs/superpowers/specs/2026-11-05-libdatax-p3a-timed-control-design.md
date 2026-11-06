# Design: libdatax P3a — timed control on the ADRV9009-ZU11EG

Date: 2026-11-05
Status: draft. The owner said "go ahead, start P3" and opted out of questions. Every decision
below is the implementer's call and is marked as such. A builder check and a hardware spike
(section "Spike") come before the plan.
Board: P3 on <https://github.com/users/tfcollins/projects/4>

## Intent

Roadmap P3: "The timed API surface (`start_at`, scheduler, FHM hops, MCS) on `tron`… P3 is the
phase that exposes through `libdatax` what tick and hopcap already prove." Exit: REQ-TEST-022
timed suite, REQ-TEST-031 hop settle and REQ-TEST-032 phase coherence, with evidence artifacts
(REQ-TEST-023), and the ZU11EG matrix row flipped to the P3 capabilities.

After P2, every scheduler symbol (`datax_sched_*`), `stage_hop_table`, `run_mcs`,
`set_time_next_pps` and any timed verb still returns `-ENOTSUP`. Their capability flags are
gated false even where the probe sees Tick. P3 makes an application able to say "at device time
T, do X". On tron, X is a GPIO or marker edge, or a Talise frequency hop. It also has to report
honestly whether X happened on time.

## The scope fork, and the call

Two of the timed capabilities need fabric that no tron image has today:

- `RX_ARM` and `TX_ARM` (`stream_start_at`) are built in Tick as a `CAPTURE_ARM` or
  `TRANSMIT_ARM` pulse into an ADI `data_offload` block's `sync_ext` input. Tick's AD9081
  timed-capture example shows the pattern. The stock ZU11EG design has **no `data_offload`**:
  RX goes straight to `axi_dmac`, and TX uses `axi_dacfifo` (ADI HDL `adrv9009zu11eg_bd.tcl`).
- `TIMESTAMPS` on the DMA path needs Tick's `tick_stream_timestamp_tag` in the RX datapath.
  hopcap's timestamps come from the vrt49 framer on the network path instead.
- `STREAM_GATE` exists on tron only as hopcap's `vrt49_fhm_hop_ctrl` gating the VRT framer, not
  the DMA stream.

**Decision 1 (implementer's call): split P3.**
- **P3a** (this design) exposes everything the existing hardware already does: the scheduler
  (timed GPIO and marker, completions, events), FHM hops, PPS epoch alignment and MCS.
- **P3b** (separate design, later) adds the HDL for timed DMA streams on the ZU11EG: a gated
  `data_offload` (or an equivalent gate) on RX and TX driven by Tick's `CAPTURE_ARM` and
  `TRANSMIT_ARM`, plus the timestamp tagger. It then implements `stream_start_at`, stream gating
  and timestamps on top.
- `RX_ARM`, `TX_ARM`, `STREAM_GATE` and `TIMESTAMPS` stay false through P3a. REQ-TEST-022's
  timed-capture and gated-burst items move to P3b.
- Why: P3a ships useful, hardware-proven API now and needs no new bitstream design. P3b is
  multi-day HDL work with its own risk. Cost if wrong: P3's exit is two milestones, not one.

## Decisions (implementer's calls, 2026-11-05)

2. **Hardware image: hopcap's SFP image.**
   - Bitstream and boot set from hopcap `main` with `--port sfp`, built and 4/4 on tron
     2026-11-05: XSA `dc3f4265…`, Image `18e592b5…`, system.dtb `1acf636a…`. They are staged on
     nemo `~/hopcap-artifacts/{bitstream-sfp,boot-sfp}`.
   - It carries Tick at `0x8600_0000` (IIO device `axi-timed-command-scheduler`, driver in the
     kernel) and the FHM GPIO route `tick_fhm_gpio[0] → TAL_GPIO_0` (Talise A).
   - It keeps the stock RX DMA (hopcap's `check-sfp-rx-netlist`) and the dual-Talise JESD stack.
   - It boots over JTAG through hopcap's `TronSession`, the same path the P2 tron runs used, with
     hopcap's artifacts instead of the stock set.
   - P2's streaming suite must still pass on it, as a regression check.
3. **Transport: libdatax on the host, Tick through its IIO attributes over `ip:`** (as P2).
   - Every descriptor write and every completion poll is one network round trip (about 1 ms on
     the lab LAN, against 12–24 ms over hopcap's serial-console helper).
   - That latency is the floor on how fast an FHM hop can be staged.
   - No kernel or Tick change in P3a. Tick's future chardev fast path slots in behind `commit()`
     and the poller without an API change, as api-c-core already promises.
4. **The scheduler is a host-side batcher plus one poller thread per scheduler**, exactly as
   api-c-core's "Abstracting the Tick submission bottleneck" section describes.
   - `queue_*` builders append 48-byte descriptors client-side, kept sorted by timestamp.
   - `commit()` writes them back-to-back to `command_descriptor` (CSV of twelve 32-bit words, the
     P1 packer) within the queue-depth headroom read from `queue_level`/`queue_depth`. It returns
     `-EAGAIN` with a count if the batch exceeds the headroom: the caller commits the rest later,
     and nothing is silently held.
   - The poller drains the one-entry completion window (`completion_*` attributes, then
     `completion_ack`) into a ring sized to the queue depth.
   - `wait(seq)` and `get_completions()` read the ring.
   - The poller also delivers events through the context's callback (P1 stored it). Deliveries:
     - `DATAX_EVT_LATE_COMMAND` when a completion is `LATE` or Tick's `late_count` rises;
     - `DATAX_EVT_QUEUE_OVERFLOW` on an `overflow_count` rise;
     - `DATAX_EVT_PPS_LOCK_CHANGE` on a `pps_present` or `time_aligned` change.
5. **Late policy is explicit per descriptor.**
   - Builders default to `TICK_CMD_F_LATE_DROP | REPORT_COMPLETION`, so a late command never
     executes silently; it completes `DATAX_COMPLETION_LATE`.
   - `commit()` refuses a descriptor whose timestamp is already inside the guard margin
     (`DATAX_LIMIT_GUARD_MARGIN_MS`, 10 ms) with `DATAX_ERR_LATE`. Rejection happens before any
     write.
6. **FHM hops are hard-timed edges with soft-timed staging**, the pipeline hopcap proved on tron
   (hopcap `docs/explanation/fhm-pipeline.md`).
   - `stage_hop_table(freqs, n)` validates every frequency against the chip's FHM band, enables
     FHM (`frequency_hopping_mode_enable`), and records the table.
   - `queue_tune(when, ch, hz)` requires `hz` in the table, else `-EINVAL` "frequency not
     staged", as already specified. It emits the GPIO pulse descriptors for the edge.
   - Talise's engine is one stage deep. The first edge after enabling lands on
     `fhmInitFrequency`, and each edge commits the frequency staged before the previous edge. So
     the scheduler emits a priming edge and keeps one frequency staged ahead: the poller writes
     `frequency_hopping_mode` after each edge's completion.
   - A staging write that lands after its edge makes that tune complete `DATAX_COMPLETION_LATE`,
     with detail "stage late", and raises `DATAX_EVT_LATE_COMMAND`.
   - After each edge the poller reads the PLL back through `frequency_hopping_mode` and compares
     it with the expected frequency. A mismatch completes `DROPPED`, with detail naming both
     frequencies.
   - The hop period floor is the staging round trip. It is exposed as a measured limit, added to
     the API page as a new `DATAX_LIMIT_HOP_STAGE_US`.
7. **Only chip A hops in P3a.** Tick drives only Talise A's FHM GPIO on this image.
   - A timed tune on radio channels 0–1 (chip A) is supported.
   - On channels 2–3 (chip B) it returns `-ENOTSUP` with "chip B FHM trigger is not wired on
     this image".
   - A chip A hop clears `PHASE_COHERENT`, since only one chip moved.
   - Wiring chip B's GPIO to Tick is a P3b HDL item.
8. **MCS on a JESD204-FSM kernel.** Both images' kernels run the JESD204 FSM, which performs MCS
   at boot; pyadi-iio's `mcs_chips()` does nothing on such kernels.
   - `run_mcs()` re-runs the FSM's sync through the `jesd204_fsm_*` attributes, then waits for
     the links to report running.
   - `PHASE_COHERENT` reads true only while the FSM reports MCS complete and no single-chip tune
     has happened since.
   - The exact attribute sequence is unproven and comes from the spike.
   - `MCS` (board ability) reads true when both phys and the FSM control attribute exist.
9. **PPS epoch.** `set_time_next_pps(epoch)` writes `time_epoch_ticks`, then `arm_time_next_pps`.
   - It returns `-ENODEV` "no PPS" when `pps_present` reads 0, which may be the case on tron. The
     spike checks.
   - `get_time` is unchanged.
10. **Capability gates.**
    - P3a ungates `TIMED_COMMANDS` (the Tick probe plus a working scheduler), `FHM_HOP` (adrv9009
      family plus Tick plus the FHM GPIO capability bit), `MCS`, and `PHASE_COHERENT` (as live
      state).
    - `hop_table_depth` is 0 in P1. It becomes the table size libdatax accepts, 64 entries: the
      Talise engine needs only the next frequency, so the table is a host-side set.
    - All the rest stay false (Decision 1).
11. **Tier 1 tests use a C fake Tick, not the emulator.**
    - `iiod-emu` serves static attributes and cannot model a queue, a clock or completions.
      libdatax therefore routes every Tick access through an internal ops table (`read_attr`,
      `write_attr`).
    - Tests swap in a deterministic fake: a 16-deep queue, a settable clock, completion
      generation with late, drop and overflow, edge capture, and a fake Talise FHM pipeline.
    - REQ-TEST-002 and REQ-TEST-003 (batching and commit order, completion-ring drainage,
      guard-margin rejection, error mapping, late handling, flush, the FHM pipeline) run against
      it in CI.
    - The fake is cross-checked against Tick's vendored Python model where they overlap
      (descriptor bytes; P1's pattern).
12. **Python surface follows `api-python.md`.**
    - `dev.scheduler` (lazy, caps-gated) offers `sched.at(t).gpio(mask, value)`,
      `.marker(line, width)` and `.tune(hz, ch=0)`, plus `sched.commit()`, `sched.wait(seq)`,
      `sched.wait_all()` and `sched.flush()`.
    - Device calls: `dev.stage_hop_table(freqs)`, `dev.run_mcs()`, `dev.time.align_next_pps(epoch)`.
    - `ctx.on_event(fn)` delivers on the poller's thread through a Python-safe trampoline.

## Goals

1. `datax_sched_*`, `stage_hop_table`, timed `set_frequency(when)`, `run_mcs` and
   `set_time_next_pps` implemented for the adrv9009 family with Tick, per the API page as
   amended by this design.
2. Python scheduler, hop and MCS surface.
3. Tier 1: a fake-Tick suite covering REQ-TEST-002 and REQ-TEST-003 in CI, under five minutes.
4. Tier 2, on tron with hopcap's SFP image:
   - **timed GPIO staircase:** N edges at known ticks, every completion `OK` with
     `executed == requested`, cross-checked by Tick's `edge_capture_*` record;
   - **hop schedule:** a plan through `queue_tune`, every hop on time, PLL readback equal to the
     plan, the priming edge on `fhmInitFrequency`;
   - **late handling:** a deliberately late descriptor completes `LATE` and does not execute;
   - **MCS:** `run_mcs()` completes and `PHASE_COHERENT` follows the rules in decision 8;
   - **PPS:** behaviour as found;
   - P2's streaming suite still green on this image.
5. Evidence per run (REQ-TEST-023/024):
   - completion logs, Tick edge-capture logs, measured tick rate, image hashes;
   - committed under `docs/evidence/p3a/`, like P2's.

## Non-goals

- Timed streams: `stream_start_at`, gating, timestamps and their caps (P3b).
- Chip B timed hops (P3b HDL).
- REQ-TEST-031 settle and REQ-TEST-032 phase-coherence **measurements**: both need RF cabling
  tron does not have (a fixed reference for settle, a split common input for coherence).
  - P3a delivers the API and the Tier 2 control-path tests.
  - The measurements use hopcap's `settle` analysis (#9) and a coherence analysis once the
    cabling exists.
- A kernel fast path for Tick submission.
- Z7035 timed control (P4).

## Anti-goals

- A late command executing silently, or completing `OK` when it ran late.
- A hop reported `OK` without its PLL readback matching the plan.
- A capability flag true where the call it gates would fail on this image (chip B hops, timed
  streams).
- A Tier 1 suite that tests the fake's assumptions instead of Tick's documented behaviour. Every
  fake behaviour cites the Tick driver, RTL or doc it mirrors.

## Constraints

- P1 and P2 constraints unchanged.
- The API page (`datax_sdk_sdr/docs/api-c-core.md`) is amended before code with:
  - `DATAX_LIMIT_HOP_STAGE_US`;
  - the commit headroom rule (`-EAGAIN`);
  - the default late policy;
  - the chip A only rule;
  - the PPS `-ENODEV`;
  - the `PHASE_COHERENT` rules.
- One poller thread per scheduler. It stops on `sched_destroy` and on context destroy, before the
  IIO context is freed (P2's lifetime rule).
- Event callbacks fire on the poller thread and must not block (already specified).
- tron runs hold the shared nemo lock. Never reprogram the PL at runtime. The image boots by JTAG
  with the PL from the XSA under test.

## Spike (before the plan)

One read-only and one write session on tron with hopcap's SFP image, using hopcap's runner
pattern, to settle the unknowns:
1. The Tick IIO device and attributes are visible over `ip:` from a libiio v1 host, and a
   `command_descriptor` write plus completion poll round trip works. Time the round trip.
2. The FHM attributes on `adrv9009-phy`: one enable, stage and edge cycle driven from the host
   over `ip:`, timing the stage round trip.
3. `pps_present`, `time_aligned` and the `time_epoch_ticks`/`arm_time_next_pps` behaviour.
4. The JESD204 FSM attributes on both phys and the hmc7044: what reruns MCS on this kernel, and
   which state shows that it completed.
5. `edge_capture_*` behaviour after a GPIO edge.

## Left to the builder

- The fake Tick's internal structure.
- The poller interval: start at 1 ms, and make it a scheduler option if needed.
- How Python marshals events onto its thread.
- The CSV formatting details, which the P1 packer already pins.
