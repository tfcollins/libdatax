# Design: libdatax P3b — timed DMA streams on the ADRV9009-ZU11EG

Date: 2026-11-23
Status: approved to start 2026-11-23 (owner: "go go"). Written under the owner's "start P3b.
document stages and tasks in github project", with questions opted out by standing preference.
Every decision below is the implementer's call and is marked as one. Two of them are **spike
gates** (S1), not settled decisions. The builder check's three questions are answered in
"Builder check rulings" at the end.
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
     is the gate opening. A closing gate cannot end a DMA block (see decision 5).
   - The adapter (`tick_fifo_tag_adapter`) presents cpack's `packed_fifo_wr` to the tagger as AXIS
     with `tready` = 1. It asserts a synthetic `tlast` while the RX mode bit is 0, so the tagger
     re-arms for the next `start_at`. The tagger runs in oneshot mode, enabled per gate through
     Tick's `stream_tag_en`.
   - RX opens once per `start_at` and closes only at `stop`, so a release is always block 0 of an
     IIO buffer: every block is enqueued before T, and the DMA writes from block 0's start.
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
   - After any discontinuity (an overflow) a block reads `timestamp_ticks = 0` ("unknown") with
     `discontinuous = true` for the rest of the stream. The next `start_at` re-anchors.
   - `TIMESTAMPS` means "block 0 of each release is hardware-stamped". The API page says so.
   - The 32 overwritten bytes are lost samples, not shifted ones. That is Tick's tagger contract,
     since `axi_dmac` treats an inserted beat badly.
   - Cost if wrong: if the adapter cannot meet timing at the cpack width, the timestamp comes
     from the `edge_capture_*` registers (same value, read by the host per release) and
     `TIMESTAMPS` stays false.
4. **`start_at` is "close, arm the DMA, open at T".** (Implementer's call.)
   - `start_at(T)` refuses T inside the guard margin with `DATAX_ERR_LATE`, as P3a commit does.
   - Before T, in this order:
     1. Write execute-now `STREAM_GATE(mode=1)` and `STREAM_GATE(level=0)` for the direction.
        Each is timestamp 0, `LATE_EXECUTE_NOW` and **no** `REPORT_COMPLETION`, through a
        generalised `datax_tick_gpio_now` (`datax_tick_cmd_now`). These "immediate" commands are
        exempt from the guard margin.
     2. Nothing to wait for (G5, 2026-11-23). Tick applies a late execute-now descriptor at push
        time, so the effect is in place when the attribute write returns. It also raises
        `late_count`, so the helper re-baselines `late_seen` under the scheduler's I/O lock, as
        `datax_tick_gpio_now` already does. No spurious `DATAX_EVT_LATE_COMMAND` follows.
        - The mode-bit enable latches Tick's shared capture tag with its late flag set. The RX
          tagger keeps only its own gate's tag (gate 0), so the header written at T is the
          on-time one.
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
5. **`queue_stream_gate(when, stream, enable)` drives the level bit only, and on this image
   only for TX.** (Implementer's call, from the builder check.)
   - It requires the stream to have been started with `start_at`. Otherwise it returns `-EINVAL`
     with "stream is not in timed mode; use start_at". `start()` leaves mode 0, so a level
     command would do nothing.
   - **TX:** a gated burst is `gate(t1, true)`, `gate(t2, false)`. While closed the DAC outputs
     zero and the TX FIFO is not read, so a cyclic buffer resumes where it paused. There are no
     block boundaries to respect.
   - **RX mid-stream gating returns `-ENOTSUP`** with "RX gating needs a DMA with end-of-burst
     (tlast) on this image".
     - Why: the RX DMA's FIFO source has no `tlast`, so closing the gate cannot end a block. The
       tail of a burst would sit in a half-filled block until the next opening. The next header
       would then land at an arbitrary offset, and derived timestamps would be wrong.
     - Requiring whole-block bursts doesn't fix it either: the tick clock and the sample clock are
       asynchronous, so a time-based close cannot hit a block boundary exactly.
   - **G6 result (2026-11-23): fail end to end, so RX gating is out of P3b**
     (datax_sdk_sdr#39).
     - It passes in HDL and kernel: AXIS-source `axi_dmac` early `tlast` with length reporting,
       and the ADI 6.1 IIO dmaengine block `bytes_used`.
     - It fails in libiio v1: the local mmap dequeue discards `bytes_used` and refuses a short RX
       enqueue; iiod's dequeue response always sends the full block; `iio_block` has no
       `bytes_used` getter.
     - Fixing it is a libiio fork change plus an iiod protocol field, beyond P3b. The original
       text follows for the record.
   - **Spike G6** decides whether RX gating joins P3b. It reads the code path for an AXIS-source
     RX DMA whose `tlast` ends a transfer early, and whether that short block's `bytes_used`
     survives the ADI kernel's IIO block path, iiod and libiio v1.
     - If it does, S2 adds that route (`DMA_TYPE_SRC 1`, the adapter's synthetic `tlast` driven on
       gate close), and the stream layer handles short blocks. RX gating then ships with a tagged
       block per opening.
     - If not, RX gating moves to a follow-up issue. `STREAM_GATE` stays true, because TX gating
       works, and the API page states the RX restriction per platform.
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
    - **Timed capture:** `start_at(now + 200 ms)`. The suite asserts that block 0's
      `timestamp_ticks` equals `T + round((1 + stripped frames) × tick_hz / fs)` (G2/G3 ruling 6).
      That is not the evidence: by construction it only proves the header was written.
    - **Independent checks for timed capture:**
      - Tick's `edge_capture_ticks` for the opening command equals T.
      - **Loopback alignment:** TX and RX `start_at` at the same T through the Talise framer
        loopback (P2's `loopback.py`). The pattern's first-sample offset in the RX block is
        recorded, and it must be identical across 10 runs within ±1 sample. That proves the
        release is deterministic.
    - **Gated burst** (TX, observed on RX): RX runs continuously from a `start_at`, and TX gates
      a constant-tone cyclic buffer on/off/on at 20 ms spacing. The burst edges' sample positions
      in the RX stream (derived timestamps) must sit at the commanded times plus the loopback
      offset measured above, ±1 sample. Tick's completions for each gate command must be
      `EXECUTED` on time. RX gating gets the same test, with one tagged block per opening, if G6
      brings it in.
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
| S1 Spike | G1 image base (synth and timing), G2 gate and DMA sync in simulation, G3 tagger adapter in simulation, G4 chip-B bit, G5 execute-now gate over `ip:` on the fake Tick, G6 short RX blocks (code read) | S0 | no (boot check deferred to S4) |
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
  - (c) on TX, closing and reopening mid-buffer pauses `axi_tx_fifo` reads without an underflow
    or a stall, and on RX, closing at `stop` leaves no stuck transfer;
  - (d) list every consumer of `adc_valid`, the vrt49 tap included.

  Fail on (c) for TX → `STREAM_GATE` ships start-only (decision 1's cost).
- **G3 — tagger adapter.** In simulation, the adapter plus Tick's tagger at the cpack width:
  - the header lands in the first 32 bytes after each opening;
  - it matches `tick.capture.parse_capture_header`;
  - a reopening re-tags.
- **G4 — chip-B pin.** Result 2026-11-23: pass.
  - `adrv9009_gpio_00_b` is EMIO `gpio[58]` (the "58" comment on `01_b` is the wrong one) and
    pin AM11, LVCMOS18.
  - Linux GPIO number: 78 + 58 = 136.
  - The patch splits the IOBUF into `[92:59]` (34 pins) and `[57:33]` (25 pins). It adds
    `assign adrv9009_gpio_00_b = tick_fhm_gpio[1]` and `assign gpio_i[58] = tick_fhm_gpio[1]`,
    and keeps the chip-A edits.
  - `tick_fhm_gpio` is already 4 bits wide.
  - The device tree already sets `adi,fhm-config-fhm-gpio-pin = 0` (TAL_GPIO_0) for chip B.
    So chip B needs no device-tree change, only FHM enabled at run time.
- **G6 — short RX blocks.** A code read: does an AXIS-source `axi_dmac` transfer that ends
  early on `tlast` report its length through the ADI kernel's IIO dmaengine block path, iiod's
  block protocol and libiio v1's `iio_block` dequeue? Pass brings RX gating into P3b (decision 5).
- **G5 — execute-now over the attribute path.** Result 2026-11-23:
  - A descriptor with timestamp 0 and `LATE_EXECUTE_NOW` executes at push. If reported, it
    completes `LATE_EXECUTED`.
  - It raises `late_count`, and on a gate enable it latches a late-flagged capture tag. The RTL,
    Tick's Python model and libdatax's fake Tick all agree.
  - So: send it unreported and re-baseline `late_seen` (decision 4).
  - Original wording: "executes on the next scheduler pass and completes `EXECUTED`, not `LATE`".
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
  - `samples, info = rx.recv()` keeps returning `BlockInfo`. `info.timestamp_ticks` is 0 when
    unknown, the same as in C (`python/datax/stream.py` has no None path, and adding one would
    split the two APIs). The doc's `samples, ts` example changes to match;
  - `sched.at(t).gate_on/gate_off`;
  - `LateError`.
- **`platform-adrv9009-zu11eg.md`:** the `timed-dma` image in the runbook, when S4 has it.

## Open for the builder

- The exact `stream_gate_map` property and attribute spelling (tick driver conventions).
- The cpack output width on the pinned revision (G3), which fixes how many frames the header
  costs.
- Whether `tick_dma_gate` is one module with a direction parameter or two.
- hopcap profile mechanics (`--profile` versus a second `--port`-style flag).

## Builder check rulings (2026-11-23)

The builder check (a fresh reviewer, no context) asked three questions:

1. **How do gated RX bursts line up with DMA blocks?** They can't on a FIFO-source DMA.
   - Ruling: RX opens once per `start_at`, and mid-stream gating is TX only. RX gating depends on
     spike G6 (decisions 3 and 5).
   - The gated-burst item is tested on TX through the loopback (decision 10).
   - Cost if wrong: if the owner wants RX gating regardless, the AXIS route becomes mandatory
     work in S2 and S3.
2. **Does P3b exit if a fallback trips?**
   - Ruling: P3b stays open until each REQ-TEST-022 item is met, and its stages stay on the board.
     A tripped fallback becomes a new task under the stage it blocks, not a partial exit.
   - One equivalence is accepted. If G3 fails, the timed-capture item is still met by:
     - the gate release, plus the loopback determinism test (decision 10);
     - the host filling `timestamp_ticks` from `edge_capture_ticks`.

     `TIMESTAMPS` then stays false, because it means in-band.
   - Anything else partial needs the owner's sign-off.
3. **Does the Python `recv` change shape?** No. It keeps `(samples, BlockInfo)`, and
   `BlockInfo.timestamp_ticks` is 0 when unknown, as in C. The S0 doc pass found that the real
   field is `timestamp_ticks: int`. The earlier draft's "`.timestamp` is None" is withdrawn.

## Spike G2/G3 rulings (2026-11-24)

Source: tick branch `p3b-spike-gate` 3455531, with 18 simulation tests. The real ADI
`util_cpack2`, `axi_dmac_transfer` and `axi_dacfifo` run at the block design's parameters.

Results:
- G2(a) passes: release jitter is under one device clock.
- G2(b) passes.
- G2(c) RX passes.
- **G2(c) TX fails as specified:** each pause/resume behind `axi_dacfifo` loses 2 words and
  repeats 1.
- G2(d): gate the cpack `fifo_wr_en` **pin** only. The `adc_valid_0` net also feeds four vrt49
  blocks, the VRT time base among them.
- G3 passes at the real 128-bit width.

Rulings (implementer's calls; the owner is away and the standing instruction is to decide):

1. **TX gate closures mute; they do not pause.** This supersedes decisions 2 and 5.
   - While a TX gate is closed, reads continue and the DAC receives zeros. The cyclic buffer
     keeps playing and resumes time-aligned.
   - TX `start_at` still holds reads from its close until T, then mutes the first N reads.
     N is `axi_dacfifo`'s stale register plus duplicates of word 0: N = 3 at 8 converters.
   - Why: a true pause behind the DDR `axi_dacfifo` corrupts samples, and the bypass mode is no
     better. Mute keeps the waveform's phase tied to device time, which is what a gated burst
     pattern needs.
   - Cost if wrong: an application wanting pause and resume must reload its buffer.
2. **TX `start_at` with a partial channel mask** is one upack read early after a reload. That
   comes from `util_upack2`'s leftover half-word from the forced-open run.
   - S2 fixes it: reset upack well before the read release, then re-prime it, and make N a
     per-mask value.
   - If that fails, the image advertises a `tx-start-fullmask-only` role in `stream_gate_map`,
     and libdatax refuses partial-mask TX `start_at` with `-ENOTSUP`.
3. **RX insertion:** only `util_som_rx_cpack/fifo_wr_en` is re-routed, and only
   `util_som_rx_cpack/reset` gets the extra `pack_reset`. The `adc_valid_0` and `adc_rst` nets
   are untouched, and the netlist check asserts this.
   - `pack_reset` holds cpack in reset while the gate is timed and closed. Non-power-of-two
     channel counts need it.
4. **Adapter re-arm:** `tlast = !mode | !pass`, from the gate's synchronized outputs. Beats that
   carry the synthetic tlast pass unmodified. This replaces decision 3's "while mode is 0".
5. **Tag-versus-data race:**
   - The gate's `OPEN_DELAY` is 12 device clocks. 6 failed once when a TX open shared T.
   - S2 adds a tick-domain filter so the RX tagger only sees gate-0 tags.
   - libdatax also orders an RX open before a TX open at the same T.
   - The release latency is about 18 device clocks. It is fixed per image and measured in S4.
6. **Timestamp contract (decision 3, refined):**
   - `util_cpack2` drops the first sample after any gap, so the first stored sample is the
     second released one.
   - libdatax strips `ceil(32 / frame_bytes)` whole frames, not 32 bytes: 2, 4 or 8 frames at
     8, 4 or 2 converters, and 3 frames at 6.
   - Block 0's `timestamp_ticks` is `capture_tick + round((1 + dropped_frames) × tick_hz / fs)`.
   - The fixed fabric release latency is not added in S3. S4 measures it, and the API page
     states it per image.
7. **Overflow side effect:** with the gate forced open and no DMA running, cpack overflow drives
   TPL `adc_dovf` continuously. The stock design does the same with no buffer active.
   `start_at` clears any sticky overflow status after the DMA enable, so a stream does not start
   "discontinuous".
8. **The axi_dmac holds about 2 descriptors in hardware.** "Every block enqueued before T"
   holds at the IIO/kernel level. That is sufficient, and the simulation confirmed it.
9. **S2 integration bench:** one run with the real `axi_timed_command_scheduler` driving the
   gate and tagger. It covers same-T command spacing and the late-flagged tag on the
   execute-now path.
