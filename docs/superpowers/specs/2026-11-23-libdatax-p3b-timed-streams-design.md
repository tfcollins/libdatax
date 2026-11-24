# Design: libdatax P3b — timed DMA streams on the ADRV9009-ZU11EG

Date: 2026-11-23
Status: draft. Written under the owner's "start P3b. document stages and tasks in github
project", with questions opted out by standing preference. Every decision below is the
implementer's call and is marked as one. Two of them are **spike gates** (S1), not settled
decisions.
Tracking: datax_sdk_sdr#28 and its stage sub-issues on <https://github.com/users/tfcollins/projects/4>.
Predecessor: `2026-11-05-libdatax-p3a-timed-control-design.md` (P3a, decision 1 split P3).

## Intent

P3a gave applications "at device time T, do X" for GPIO edges, markers and Talise A frequency
hops. P3b covers the streams themselves:

- `datax_stream_start_at(stream, T)`: the first RX sample, or the first TX sample on the DAC, is
  the sample at T.
- `datax_sched_queue_stream_gate(sched, T, stream, enable)`: a running stream is gated on and off
  on tick boundaries.
- `datax_block_info.timestamp_ticks`: an RX block says when its first sample was taken.
- Talise B frequency hops, which P3a refused with `-ENOTSUP`.

Exit is the rest of REQ-TEST-022: timed capture with a tick-exact first-sample timestamp, and
the gated burst pattern, on `tron`, with REQ-TEST-023 evidence. The ZU11EG matrix cells
`RX_ARM`, `TX_ARM`, `STREAM_GATE` and `TIMESTAMPS` flip, and `FHM_HOP` covers both chips.

## What exists, and what does not (recon 2026-11-23)

- **No ADRV9009 design has a `data_offload`.** Tick's timed capture (`CAPTURE_ARM` into a
  `data_offload` `sync_ext`) exists only on AD9081/ZCU102, and no hardware run is recorded for it
  (tick `docs/example-timed-capture.md`, `hardware_tests/test_timed_capture_iio.py`).
- **ZU11EG RX path** (ADI `adrv9009zu11eg_bd.tcl` at the pinned revision):
  - RX data flows TPL → `util_som_rx_cpack` → `axi_adrv9009_som_rx_dma`.
  - The DMA has `DMA_TYPE_SRC 2`, the FIFO interface, which has no `tlast` and no `tready`.
  - `SYNC_TRANSFER_START 1` is set, and `sync` comes from cpack's `packed_sync`.
- **ZU11EG TX path:** `axi_adrv9009_som_tx_dma` (cyclic, AXIS out) → `axi_tx_fifo`
  (`axi_dacfifo`, PL DDR4) → `util_som_tx_upack` → DAC TPL.
- **On the hopcap-sfp image** (P3a's image), vrt49 removes the DMA → `axi_tx_fifo` nets. It
  inserts `vrt49_dac_tx` (`system_bd_vrt49.tcl:632-654`) unconditionally, with no build switch.
  That is why the P2 TX tests skip on that image.
- **Tick facts** (tick `axi_timed_command_scheduler.v`):
  - `STREAM_GATE` (0x0003, target 0x0200 | gate) sets or clears a latched level bit,
    `stream_gate_level[gate]`. The level resets to 0.
  - On enable it latches `capture_tag_ticks` and the `capture_tag_*` fields (:787-796). The
    `edge_capture_*` registers read those latched values.
  - `stream_gate_en = stream_gate_level | arm_pulse_active` (:406).
  - `LATE_EXECUTE_NOW` executes a late descriptor at once (:777).
- **Tick's `tick_stream_timestamp_tag`** is an AXIS pass-through. On the first beat of each
  burst it overwrites the first 32 bytes with a header: magic `TICK`, flags, 64-bit
  `capture_tick`, `tick_hz`, sequence and burst index (tick `src/tick/capture.py:84-92`). It
  delimits bursts with `tlast` and has a oneshot mode.
- **Chip B:**
  - `adrv9009_gpio_00_b` (pin AM11) sits mid-range in the 61-bit EMIO IOBUF, around bit 58.
  - Tick's chip-A patch could narrow the IOBUF because `00_a` is bit 32, at the edge.
  - Chip B needs the IOBUF split around its bit, plus chip B's FHM GPIO property in the device
    tree.
  - `tick_fhm_gpio[3:1]` are spare.
- **libdatax:**
  - The symbols exist and return `-ENOTSUP`: `stream.c:499`, `sched.c:644`.
  - `timestamp_ticks` is hard-coded to 0.
  - The fake Tick already models gates, arm and the capture-tag latch.
  - Hardware tests skip by image (`hwtimed.py:image_skip_reason`), not by capability.

## Decisions

1. **Gate the DMA paths in fabric with Tick's `STREAM_GATE` level. Not `data_offload`, not
   arm pulses.** (Implementer's call.)
   - A `data_offload` on RX is a bounded burst store: BRAM, or the PL DDR4 that `axi_dacfifo`
     already uses. It cannot start a continuous stream at T and keep it running.
   - Tick's own doc names the cheaper route: gate the cpack write side, which the DMA already
     syncs on with `SYNC_TRANSFER_START`. That wrapper was deferred and never built.
   - Arm pulses (`CAPTURE_ARM`/`TRANSMIT_ARM`, 16 ticks wide) exist only because `sync_ext` is
     level-sensitive. A latched level does the same job with one mechanism and is already
     hardware-proven on the VRT path.
   - So `start_at`, gating and the timestamp latch all come from `STREAM_GATE`. This design does
     not use the arm commands.
   - Cost if wrong: if the DMA's `sync` handling makes a mid-stream gate stall rather than drop
     (spike G2), `STREAM_GATE` ships as start-only and burst gating moves to a second design.
2. **One new Tick RTL block, `tick_dma_gate`, per direction, with a mode bit so the reset
   state is "open".** (Implementer's call.) Tick gate bits on this image:

   | Tick gate | Meaning |
   |---|---|
   | 0 | RX level (`1` = pass) |
   | 1 | TX level (`1` = pass) |
   | 2 | RX timed mode (`0` = forced open, the reset state) |
   | 3 | TX timed mode (`0` = forced open, the reset state) |
   | 4–7 | spare |

   - `pass = !mode | level`, evaluated in the device clock after a two-flop CDC of both bits.
   - At reset every bit is 0, so the image streams exactly like the stock design. A P2-era
     libdatax, iio-oscilloscope or pyadi-iio sees no difference.
   - The gate never needs the arm pulses, so gates 0–3 must never be targeted by
     `CAPTURE_ARM`/`TRANSMIT_ARM`. libdatax never emits them on this image.
   - **RX gate:** it sits between the TPL outputs and cpack. While closed it holds every
     channel's `adc_valid` low. All channels are gated on the same clock edge, so cpack's channel
     alignment and `packed_sync` are untouched. Samples during a closed gate are discarded in
     fabric; they are not an overflow.
   - **TX gate:** it sits between `axi_tx_fifo` and upack/TPL. While closed it holds the upack
     read enable low and drives zeros to the TPL, so the DAC outputs mid-scale zero. When it opens,
     the next sample read is the next sample in the FIFO.
   - **Latency:** fixed CDC latency plus or minus one device clock. The tick clock (100 MHz) and
     the device clock are asynchronous. The bound is stated in the API page and measured in S4.
   - Cost if wrong: gating `adc_valid` upsets a block that reads it outside cpack (spike G2 lists
     the consumers). The fallback is to gate cpack's `packed_fifo_wr_en` after packing, which
     moves the tagger too.
3. **RX timestamps: Tick's tagger, through a thin FIFO-interface adapter, oneshot per
   release.** (Implementer's call.)
   - The ZU11EG RX DMA takes FIFO input (no `tlast`), so the only burst boundary the fabric knows
     is the gate opening.
   - The adapter (`tick_fifo_tag_adapter`) presents cpack's `packed_fifo_wr` to the tagger as AXIS
     with `tready` = 1. It asserts a synthetic `tlast` on the last beat before the gate closes,
     so the next opening starts a new burst. The tagger runs in oneshot mode, enabled per gate
     through Tick's `stream_tag_en`.
   - The header carries `capture_tick`, the tick Tick latched when the opening `STREAM_GATE`
     executed. That equals T for an on-time command.
   - **Block 0 after each release carries the hardware tag.**
     - libdatax verifies the magic and the flags.
     - It strips the 32 bytes: 4 frames at 2 channels, 2 frames at 4 channels. It reports
       `valid_samples` reduced by that count and `timestamp_ticks = capture_tick` (API ticks, after
       the P3a epoch offset).
   - **Blocks after block 0 have derived timestamps:**
     `T + round(samples_since_release × tick_hz / sample_rate)`.
     - These are exact only at the device rate. libdatax documents the rounding.
   - After any discontinuity (overflow, or a gate close and reopen without a tag) a block reads
     `timestamp_ticks = 0` ("unknown") with `discontinuous = true`. The next tagged block
     re-anchors.
   - `TIMESTAMPS` means "block 0 of each release is hardware-stamped". The API page says so.
   - The 32 overwritten bytes are lost samples, not shifted ones. That is Tick's tagger contract,
     since `axi_dmac` treats an inserted beat badly.
   - Cost if wrong: if the adapter cannot meet timing at the cpack width, the timestamp comes
     from the `edge_capture_*` registers (same value, read by the host per release) and
     `TIMESTAMPS` stays false.
4. **`start_at` is "close, arm the DMA, open at T".** (Implementer's call.)
   - `start_at(T)` refuses T inside the guard margin with `DATAX_ERR_LATE`, as P3a commit does.
   - Before T, in this order:
     1. Write execute-now `STREAM_GATE(mode=1)` and `STREAM_GATE(level=0)` for the direction:
        timestamp 0, `LATE_EXECUTE_NOW`, through the scheduler. These "immediate" commands are
        exempt from the guard margin.
     2. Wait for their completions.
     3. Open the IIO buffer and enqueue every RX block, or load the TX data: the whole
        `block_count` for streaming TX, the one buffer for cyclic TX.
     4. Commit `STREAM_GATE(level=1)` at T with `LATE_DROP | REPORT_COMPLETION`.
   - The stream is `started` immediately. `recv` blocks until data arrives.
   - **Late at dispatch:**
     - The open completes `LATE`, raises `DATAX_EVT_LATE_COMMAND` and marks the stream failed.
     - `recv`/`send` return `DATAX_ERR_LATE`.
     - The capture never starts shifted.
   - **`stop()` on an armed stream:**
     - It flushes the pending open, then writes execute-now `mode=0` (forced open) and
       `level=0`, then tears down as P2 does.
     - A later `start()` (untimed) therefore finds the gate open.
     - A later `start_at` repeats the sequence above.
   - `start()` (untimed) on this image writes execute-now `mode=0` first, so a crashed earlier
     process cannot leave the gate closed.
   - TX `start_at` is tested with cyclic TX (a preloaded `axi_dacfifo`) and with block streaming
     sized to `block_count`. Over `ip:`, iiod cannot feed 245.76 MSPS, so non-cyclic TX underflows
     after the preloaded blocks. That is recorded, not asserted, as RX overflows are in P2.
   - Cost if wrong: if the execute-now round trips make `start_at` setup slower than a useful
     lead time, a `prepare()` split moves the close/arm half out of the timed call.
5. **`queue_stream_gate(when, stream, enable)` drives the level bit only.**
   - It requires the stream to have been started with `start_at`. Otherwise it returns `-EINVAL`
     with "stream is not in timed mode; use start_at". `start()` leaves mode 0, so a level
     command would do nothing.
   - A gated burst is `gate(t1, true)`, `gate(t2, false)`.
   - Each re-opening produces a new tagged block 0 on RX.
   - While the RX gate is closed, `recv` simply has no data. That is not an error and not an
     overflow.
   - The Python surface is `sched.at(t).gate_on(stream)` and `.gate_off(stream)`, as in
     `api-application-mapping.md`.
6. **Image: spike gate G1 decides the base. The RTL lives in tick either way.** (Implementer's
   call on the candidates and the order.) One image must carry everything:
   - P2: RX, TX to the DAC, loopback;
   - the P3a regression: Tick at its address, FHM on chip A, PPS with the hardware fallback, the
     MCS-capable JESD204-FSM kernel;
   - P3b: both gates, the tagger, chip B's FHM.

   Candidates, in order:
   - **A. hopcap-sfp plus a stock-TX switch** (preferred).
     - A vrt49 build parameter, `VRT_DAC_TX_ENABLE` (default 1, today's behaviour), guards the
       `vrt49_dac_tx` insertion the way `ROCE_ENABLE` guards RoCE.
     - With 0, the ADI DMA → `axi_tx_fifo` nets stay. vrt49's `test_adrv9009_tx_bd_contract.py`
       learns the switch.
     - hopcap gains a profile (`--profile timed-dma`) that sets it, sources the gate/tagger
       fragment and routes chip B.
     - This keeps the P3a-proven kernel, boot path, `TronSession` and the Corundum PPS fallback.
       The vrt49 RX tap stays; the vrt49 TX user path is absent on this profile.
     - Risk: hopcap-sfp closed timing at WNS +0.071 ns.
   - **B. Tick's own tron build** (`tron-repeat-hil.yml`, ADI `hdl_2026_r1`) plus the gate,
     tagger and chip-B route.
     - It has stock TX already.
     - It lacks the PPS selector and a fabric PPS source. Using it means upstreaming hopcap's
       `tick_pps_select` into tick (MIT, provenance row), with a fabric 1 Hz divider as the
       internal source.
     - Its kernel, device tree and boot set for libdatax are unverified.

   - **G1 passes** when the candidate builds with the P3b blocks, meets timing (WNS ≥ 0) and
     passes its netlist checks (hopcap `check_hop_netlist.py` plus new checks for both gates, the
     tagger and the chip-B route).
   - A is tried first. If A fails timing twice (one seed or strategy change), B is built.
   - Cost if wrong: B costs the PPS port and a kernel bring-up, about two extra days.
7. **Chip B FHM:** `tick_fhm_gpio[1]` → `adrv9009_gpio_00_b`.
   - The IOBUF is split around `00_b`'s bit (no narrowing), mirrored into `gpio_i`, and asserted
     by the netlist check.
   - The device tree gives chip B its FHM GPIO pin.
   - libdatax `queue_tune` on channels 2–3 emits on Tick GPIO bit 1 and runs the same
     one-stage-ahead pipeline per chip. The two chips' pipelines are independent.
   - A hop on one chip clears `PHASE_COHERENT` (P3a decision 7). Hopping both chips at the same T
     does not restore it, because that needs REQ-TEST-032.
   - Spike G4 confirms bit 58's identity, since vrt49's comments give both `00_b` and `01_b` as
     "58".
8. **Capabilities come from probing the image, not a board table.**
   - Tick's driver gains a read-only IIO attribute, `stream_gate_map`, populated from a new device
     tree property on the Tick node, for example `adi,stream-gate-map = "rx-dma", "tx-dma",
     "rx-mode", "tx-mode"`. Absent means "no gate wired".
   - **`RX_ARM` / `TX_ARM`:** the map names the direction's level and mode bits, the bitmap has
     `STREAM_GATE_TARGET` (0x08), and the scheduler works.
   - **`STREAM_GATE`:** `RX_ARM` or `TX_ARM`.
   - **`TIMESTAMPS`:** the bitmap has `STREAM_TIMESTAMP` (0x40), the `capture_tag_*` attributes
     exist, and the map names the RX gate.
   - **`FHM_HOP` for chip B:** a second FHM pin entry in the map (`"fhm-b"`). Until it is present,
     channels 2–3 keep P3a's `-ENOTSUP`.
   - hopcap-sfp has no map, so every P3b flag stays false there. No regression.
   - Cost if wrong: a driver change in tick, carried by a pin bump. The alternative was matching
     image hashes inside libdatax, which ties the library to one lab's builds.
9. **Tier 1 first, on the fake Tick and the emulator.**
   - **The fake Tick gains:**
     - `stream_gate_map`;
     - execute-now semantics (already in the RTL model);
     - per-gate pass state;
     - a hook the stream layer calls to ask "is RX passing at device time t?".
   - **A fake gated RX source** generates blocks only while the gate passes, stamps block 0 with a
     header byte-identical to `tick.capture` output (golden vectors from tick's Python codec,
     pinned like the P3a traces), and can inject an overflow.
   - The emulator's static RX pattern cannot gate, so the stream tests for P3b bind the fake
     source through the same internal seam style as `datax_tick_ops`.
   - **Tier 1 covers:**
     - guard-margin rejection;
     - the close/arm/open order;
     - late-at-dispatch failing the stream;
     - `stop` cancelling an arm;
     - `start` after an aborted `start_at` finding the gate open;
     - header parse and strip;
     - derived timestamps;
     - discontinuity resets;
     - gated bursts producing one tagged block per opening;
     - chip-B tune routing;
     - every flag false without the map.
10. **Hardware suite (S4):** a new `images.json` entry, `timed-dma`, and a `p3b` marker skipped on
    every other image.
    - **Timed capture:** `start_at(now + 200 ms)`. `timestamp_ticks == T` is asserted, but it is
      not the evidence: by construction it only proves the header was written.
    - **Independent checks for timed capture:**
      - Tick's `edge_capture_ticks` for the opening command equals T.
      - **Loopback alignment:** TX and RX `start_at` at the same T through the Talise framer
        loopback (P2's `loopback.py`). The pattern's first-sample offset in the RX block is
        recorded, and it must be identical across 10 runs within ±1 sample. That proves the
        release is deterministic.
    - **Gated burst:** gate pattern on/off/on at 20 ms spacing. Assert one tagged block per
      opening, `capture_tick` values equal to the command times, and no samples between bursts.
    - **Chip-B FHM:** timed tune on channel 2 with a PLL readback, same as chip A.
    - **Regression on the same image:** the full P2 suite (TX now runs) and the P3a suite.
    - **Evidence (REQ-TEST-023):** completion logs, edge-capture values, the recorded offsets and
      their spread, and image hashes. No scope on the rig, as in P3a.
    - Under REQ-TEST-024 the suite is release-pinned through `images.json`.

## Non-goals

- REQ-TEST-031 hop settle and REQ-TEST-032 phase coherence. Both need RF cabling on `tron`
  (datax_sdk_sdr#9, #8).
- Arm pulses (`CAPTURE_ARM`/`TRANSMIT_ARM`) and `data_offload` on this board (decision 1).
- TX timestamps: the DAC path has no tagger. `TX_ARM` gives the start time, and nothing reports
  back per block.
- P3b on the ZC706 or the Z7035. The gate is written generically, but only the ZU11EG gets an
  image here.
- The vrt49 network TX path on the `timed-dma` profile.
- Network rates: P3b runs over `ip:`, as P2 and P3a do.

## Stages

Each stage is a sub-issue of datax_sdk_sdr#28. Its tasks are sub-issues in the implementing
repo.

| Stage | What | Depends on | Needs tron |
|---|---|---|---|
| S0 Design | this spec, builder check, SDK doc amendments (REQ-DOC-001) | — | no |
| S1 Spike | G1 image base (synth and timing), G2 gate and DMA sync in simulation, G3 tagger adapter in simulation, G4 chip-B bit, G5 execute-now gate over `ip:` on the fake Tick | S0 | no (boot check deferred to S4) |
| S2 HDL and kernel | `tick_dma_gate`, `tick_fifo_tag_adapter`, `stream_gate_map` driver attribute (tick); `VRT_DAC_TX_ENABLE` (vrt49); `timed-dma` profile, chip-B route, device tree, netlist checks, build and boot set (hopcap) | S1 | no |
| S3 libdatax Tier 1 | fake gate and source, `start_at`, `queue_stream_gate`, timestamps, capability probe, chip-B FHM, Python | S0 (API amended); S2 only for exact attribute names | no |
| S4 Hardware | `images.json` entry, `tron-run.sh` and `hw-test.yml` image option, the `p3b` suite, the P2 and P3a regression | S2, S3 | **yes** |
| S5 Phase exit | evidence, `support.json` rows, matrix flip, ZU11EG runbook and platform page | S4 | no |

S2 and S3 run in parallel after S1. `tron` was off the network on 2026-11-23, so S4 is
Blocked-lab until someone checks it at the bench.

## Spike gates (S1)

- **G1 — image base.** Decision 6. Pass: WNS ≥ 0, netlist checks green.
- **G2 — gate and DMA sync.** In simulation with the ADI `axi_dmac` (FIFO source,
  `SYNC_TRANSFER_START 1`) and `util_cpack2`:
  - (a) with the gate closed before the DMA is enabled, the first word the DMA writes is the
    first sample after opening;
  - (b) later transfers, the next IIO blocks, do not wait for another `sync`;
  - (c) closing and reopening mid-transfer drops samples and does not stall;
  - (d) list every consumer of `adc_valid`, the vrt49 tap included.

  Fail on (c) → `STREAM_GATE` ships start-only (decision 1's cost).
- **G3 — tagger adapter.** In simulation, the adapter plus Tick's tagger at the cpack width:
  - the header lands in the first 32 bytes after each opening;
  - it matches `tick.capture.parse_capture_header`;
  - a reopening re-tags.
- **G4 — chip-B pin.** Confirm the EMIO bit for `adrv9009_gpio_00_b` from the pinned
  `system_top` and the constraint file, and write the split patch.
- **G5 — execute-now over the attribute path.**
  - On the fake Tick and in Tick's Python model, a `LATE_EXECUTE_NOW` descriptor with timestamp
    0 executes on the next scheduler pass and completes `EXECUTED`, not `LATE`.
  - Measure the `ip:` round trips `start_at` setup costs. That needs hardware, so it is
    estimated from P3a's per-attribute cost and confirmed in S4.

## API and doc amendments (S0, before any code; REQ-DOC-001)

- **`streaming-model.md`:** replace "`axi_dmac` has no trigger input… `data_offload` `sync_ext`"
  with the per-board mechanism. On ZU11EG it is the cpack-side gate and the oneshot tagger. The
  `data_offload` arm remains the AD9081 mechanism.
- **`api-c-core.md`:**
  - `start_at` and `queue_stream_gate` semantics and errors;
  - the stop/cancel rule;
  - "block 0 is hardware-stamped, later blocks derived, 0 after a discontinuity";
  - the `±1 device clock` latency statement.
- **`api-object-model.md`:** the definitions of `RX_ARM`, `TX_ARM`, `STREAM_GATE` and
  `TIMESTAMPS` as decision 8 probes them.
- **Event name:** the docs say `DATAX_EVT_OVERFLOW`; the code has `DATAX_EVT_STREAM_OVERFLOW`
  and `DATAX_EVT_STREAM_UNDERFLOW`. The docs follow the code.
- **`api-python.md`:**
  - `rx.start_at(t)`;
  - `samples, ts = rx.recv()`, with `ts` None when unknown;
  - `sched.at(t).gate_on/gate_off`;
  - `LateError`.
- **`platform-adrv9009-zu11eg.md`:** the `timed-dma` image in the runbook, when S4 has it.

## Open for the builder

- The exact `stream_gate_map` property and attribute spelling (tick driver conventions).
- The cpack output width on the pinned revision (G3), which fixes how many frames the header
  costs.
- Whether `tick_dma_gate` is one module with a direction parameter or two.
- hopcap profile mechanics (`--profile` versus a second `--port`-style flag).
