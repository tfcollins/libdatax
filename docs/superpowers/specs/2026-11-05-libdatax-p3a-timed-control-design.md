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
   - **Its TX DMA feeds vrt49's wide deframer**, which parses V49 packets
     (`test_adrv9009_tx_bd_contract.py`), so raw TX IQ from libdatax does not reach the DAC on this
     image. P3a needs no TX, so the P2 regression on this image is **RX only** and says so.
   - It boots over JTAG through hopcap's `TronSession`, the same path the P2 tron runs used, with
     hopcap's artifacts instead of the stock set.
   - P2's RX streaming tests must still pass on it, as a regression check.
   - The alternative was Tick's own tron build (`tron-repeat-hil.yml`). It has stock TX, but no FHM
     pins by default (`TICK_FHM_PINS=0`), so FHM would need a new bitstream build. It is the
     fallback if the spike rules out hopcap's image.
3. **Transport: libdatax on the host, Tick through its IIO attributes over `ip:`** (as P2).
   - Every attribute access is one network round trip.
   - Draining one completion costs about eight: seven `completion_*` reads plus the ack.
   - The FHM staging floor is therefore several round trips, measured in the spike rather than
     assumed.
   - Buffer I/O uses its own `iiod` connection: libiio's `network_open_buffer` sets up a separate
     client. So the poller's attribute traffic does not queue behind a blocked RX read. The spike
     confirms this under load.
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
   - `run_mcs()` verifies that the FSM reports its MCS steps completed, and returns 0 without
     touching the links.
   - Only when the FSM reports it has not completed does `run_mcs()` request a resync, through
     whatever the spike proves safe. It refuses with `-EBUSY` while any stream is live, and checks
     RX data after the resync.
   - `PHASE_COHERENT` means "the MCS sequence completed, and no single-chip tune has happened
     since". It is not a measured phase guarantee. The API page says so; REQ-TEST-032 is the
     measurement.
   - If the device tree enables Talise RF PLL phase sync, the doc notes it. The spike reads that
     setting. Without it, deterministic JESD latency alone does not make the two LOs coherent, and
     the page warns of this.
   - `MCS` (board ability) reads true when both phys and the FSM control attribute exist.
9. **PPS is optional; the internal clock is the default and the fallback** (owner, 2026-11-05:
   "1PPS might be disconnected in hardware but the system should still have a fallback to
   internal clocks").
   - Device time is always Tick's free-running counter on the fabric's internal 100 MHz clock. It
     runs, and every timed call works, with no PPS at all.
   - On hopcap's tron image Tick's `pps_in` is tied off (Synchrona option off), so `pps_present` reads
     0. The spike confirmed this, and it is the normal case today.
   - **New accessor** (API page first): `int datax_device_get_time_source(const struct datax_device
     *dev, enum datax_time_source *src)`, with these values:
     - `DATAX_TIME_FREE_RUN`: internal clock, never aligned;
     - `DATAX_TIME_PPS_ALIGNED`: epoch loaded on a real PPS edge;
     - `DATAX_TIME_SOFT_ALIGNED`: aligned to the host clock in software, accurate only to about one
       network round trip;
     - `DATAX_TIME_PPS_HOLDOVER`: was PPS-aligned, PPS since lost, running on the internal clock.
   - **`set_time_next_pps(epoch)` never fails just because PPS is missing.**
     - With `pps_present` = 1, it arms `arm_time_next_pps` after writing `time_epoch_ticks`. It then
       waits up to 2 s for `time_aligned`, and the source becomes `PPS_ALIGNED`.
     - With no PPS, or no edge within 2 s, it falls back to software alignment. libdatax records a
       per-device offset so that device time reads `epoch` at the moment of the call. It applies the
       offset everywhere device time crosses the API: `get_time`, every `queue_*` timestamp, and
       completion times. Tick's own counter is left untouched.
     - The fallback returns 0, sets the source to `SOFT_ALIGNED`, and raises
       `DATAX_EVT_PPS_LOCK_CHANGE` with detail "no PPS: aligned to the host clock (±<rtt> µs)".
       Callers that need a hard edge check the source.
   - The poller watches `pps_present` and `time_aligned`. Every transition raises
     `DATAX_EVT_PPS_LOCK_CHANGE`.
     - PPS lost after alignment: the source becomes `PPS_HOLDOVER` and time continues on the
       internal clock.
     - PPS appearing while free-running or soft-aligned: no automatic jump in time. The event says a
       hard alignment is now possible.
   - Relative scheduling never depends on PPS ("now + 500 ms"), so it is correct in every source.
   - **Hardware fallback on hopcap's image (owner, 2026-11-05: "wire Corundum's PPS into Tick as
     the fallback")**: hopcap adds `tick_pps_select` in fabric, driving Tick's `pps_in`.
     - It selects the external PPS (PMOD0 D4, `gpio_i[16]`, the pin vrt49's time base already uses
       as its alternate source) while that pin carries a plausible 1 Hz pulse. Otherwise it selects
       Corundum's internal PTP PPS (`ptp_pps_str`), and it switches only between pulses.
     - On that image `pps_present` = 1 with nothing connected, so `set_time_next_pps` hard-aligns on
       a fabric PPS edge.
     - `DATAX_TIME_PPS_ALIGNED` therefore means "aligned on a fabric PPS edge: external when
       connected, otherwise Corundum's free-running PTP second", not GNSS time.
     - The selector's status (external alive, external selected) goes to spare PS GPIO inputs,
       which the board can read. libdatax over `ip:` cannot see it. `doctor` says so, and exposing it
       over IIO is a follow-up.
     - `SOFT_ALIGNED` remains the fallback on images without the selector, where Tick sees no PPS
       at all.
     - Side effect: Tick time and vrt49's VRT timestamps share Corundum's second boundary on this
       image.
   - The `pps_present` / `time_aligned` / epoch behaviour on a live PPS stays unproven on tron until
     a PPS source is wired. Tier 1 covers it with the fake Tick.
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
    - REQ-TEST-003 asks for simulation against Tick's Python model, so the fake's behaviour is
      pinned to golden completion traces from `tick.hdl_model`, the model hopcap already uses:
      queue order, late drop, late execute-now, flush and overflow.
    - The traces are generated by a script in the repo, from the pinned Tick revision, and
      committed with their provenance. The C fake's tests replay them.
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
   - **timed GPIO staircase:** N edges on bits 3:1 at known ticks, spaced at least
     `REPORT_SPACING`, every reported completion `OK` with `executed == requested`. Evidence is
     completion records, since no independent scope is available (ruling 2);
   - **hop schedule:** a plan through `queue_tune`, every hop on time, PLL readback equal to the
     plan, the priming edge on `fhmInitFrequency`;
   - **late handling:** a deliberately late descriptor completes `LATE` and does not execute;
   - **MCS:** `run_mcs()` completes and `PHASE_COHERENT` follows the rules in decision 8;
   - **PPS:** on the image with `tick_pps_select` and no external PPS, `pps_present` reads 1 and
     `set_time_next_pps` hard-aligns (source `PPS_ALIGNED`, Tick `time_aligned` = 1, `get_time`
     equal to the epoch plus elapsed whole seconds of Corundum PPS). The soft fallback is tested
     with the fake Tick and on the previous image (`pps_present` = 0);
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
  - `datax_time_source` and `datax_device_get_time_source`, and the PPS fallback rules;
  - the `PHASE_COHERENT` rules.
- One poller thread per scheduler. It stops on `sched_destroy` and on context destroy, before the
  IIO context is freed (P2's lifetime rule).
- Event callbacks fire on the poller thread and must not block (already specified).
- tron runs hold the shared nemo lock. Never reprogram the PL at runtime. The image boots by JTAG
  with the PL from the XSA under test.

## Spike (before the plan)

One read-only and one write session on tron with hopcap's SFP image, using hopcap's runner
pattern, to settle the unknowns:
0. The IIO device list over `ip:` from a libiio v1 host. Confirm stock `iiod` serves it while
   hopcap's card units run, and what feeds the DAC TPL.
1. The Tick IIO device and attributes are visible over `ip:` from a libiio v1 host, and a
   `command_descriptor` write plus completion poll round trip works. Time the round trip.
2. The FHM attributes on `adrv9009-phy`: one enable, stage and edge cycle driven from the host
   over `ip:`, timing the stage round trip.
3. `pps_present`, `time_aligned` and the `time_epoch_ticks`/`arm_time_next_pps` behaviour.
4. The JESD204 FSM attributes on both phys and the hmc7044: what reruns MCS on this kernel, and
   which state shows that it completed.
5. (Answered from RTL: `edge_capture_*` latches only arm/gate commands, not GPIO.)
6. Round trips for a completion drain and a FHM stage, idle and while an RX stream is running.
7. The device tree's Talise RF PLL phase-sync setting.
8. Last, and only if step 4 shows a safe way: a resync, with RX data checked afterwards.

## Rulings from the builder check (2026-11-05)

Checked against the Tick RTL at the pinned revision (`axi_timed_command_scheduler.v`).

1. **Tick holds a reported command while its one completion slot is full, then reports it
   on time.** `due_command_blocked_by_completion` (line 351) stalls the queue head when it has
   `REPORT_COMPLETION` and the slot is unacked. When it finally fires, its record is `EXECUTED`
   with `executed = requested` unless it was late on submit (lines 1010-1030). Commands without
   `REPORT_COMPLETION` never stall.
   - **P3a needs no RTL change, because libdatax never lets the stall happen.** Each scheduler
     measures its completion-drain cost when it is created and exposes it as
     `DATAX_LIMIT_REPORT_SPACING_US`, the drain cost times 3. `commit()` returns `-EINVAL` "reported
     commands closer than the report spacing" when two reported descriptors (this batch or already
     queued) are closer than that.
   - The poller records the device time of every drain. If a completion is drained after the next
     reported command's timestamp, the poller lagged. That next command's completion is then marked
     `DATAX_COMPLETION_LATE` (detail "poller lagged; execution may have been held"), and
     `DATAX_EVT_LATE_COMMAND` fires.
   - **Default reporting is sparse.** `queue_gpio` and `queue_marker` do not report unless the
     caller asks (a `report` flag on the builder). `queue_tune` reports its rising edge only, which is
     hopcap's proven pattern. `wait(seq)` on an unreported sequence returns `-EINVAL`.
   - The real fix belongs upstream in Tick: record the actual dispatch tick, and report
     `LATE_EXECUTED` when the stall delayed it (tfcollins/tick#38). When it lands, the spacing
     rule relaxes without an API change.
   - A lost report (slot full when a late-drop or reject is reported) shows only as a rise in
     `overflow_count`. The poller turns any rise into `DATAX_EVT_QUEUE_OVERFLOW`. Any seq still
     waiting when `wait()` times out completes `DATAX_COMPLETION_DROPPED` with detail "no
     completion; see overflow_count".
2. **Tier 2 timing evidence is completion records plus PLL readback, with the limit stated.**
   - `edge_capture_*` latches only arm and gate commands, so it cannot cross-check GPIO.
   - With ruling 1, completion records are trustworthy: no reported command can have stalled.
   - Hops add PLL readback.
   - The independent scope staircase against PPS (REQ-TEST-023, issue #7) stays open. It needs a
     scope, and the evidence says so.
   - GPIO bit 0 is reserved for `queue_tune` (it drives `TAL_GPIO_0`). `queue_gpio` refuses a mask
     touching it with `-EINVAL`. Bits 3:1 are the hop controller's slot index, are safe while no tune
     is queued, and are what the staircase toggles.
3. **One scheduler per device, and Tick is exclusive.**
   - A second `datax_sched_create` on the same device returns `-EBUSY`.
   - A timed device call (`set_frequency(when)`) uses the device's scheduler, creating it lazily. It
     is then destroyed with the device.
   - The poller belongs to that one scheduler. It stops in `sched_destroy`, or in context destroy
     before streams and the IIO context are freed.
   - Other Tick users on the board (hopcap, `iio_attr`) must not run concurrently. The API page and
     `doctor` say so. `sched_create` warns through last-error if `queue_level` is non-zero at
     creation.
4. **`struct datax_completion` is unchanged.** Detail travels in the event (`struct datax_event`
   already has `detail`) and in the context's last error when `wait()` returns a non-OK status. No
   ABI change.

## Left to the builder

- The fake Tick's internal structure.
- The poller interval: start at 1 ms, and make it a scheduler option if needed.
- How Python marshals events onto its thread.
- The CSV formatting details, which the P1 packer already pins.
